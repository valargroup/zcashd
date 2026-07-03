use std::convert::TryInto;

use rand_core::OsRng;
use tracing::{debug, error};

use crate::{
    bundlecache::{orchard_bundle_validity_cache, orchard_bundle_validity_cache_mut, CacheEntries},
    ironwood_bundle::Bundle as IronwoodBundle,
    orchard_bundle::Bundle,
};

struct BatchValidatorInner {
    // The verifying key against which this batch's proofs are checked is bound at
    // construction of the inner validator. NU6.2 changed the Orchard circuit (and thus the
    // verifying key), and NU6.3 changed it again (adding the cross-address restriction
    // constraints), so a batch is typed to a specific circuit at construction: the NU6.3
    // (restricted) key from NU6.3, the NU6.2 (fixed) key from NU6.2, or the pre-NU6.2
    // (insecure) key before that. A batch never mixes epochs (one batch per block, and a
    // fresh batch per transaction during mempool acceptance), so a single key per batch is
    // correct. Post-NU6.3 Ironwood bundles use the same circuit (and key) as post-NU6.3
    // Orchard-pool bundles, so they share the batch.
    validator: orchard::bundle::BatchValidator<'static>,
    queued_entries: CacheEntries,
    // Set when a bundle could not be added to the batch (e.g. its flags are unsupported by
    // the batch's verifying key); forces `validate` to fail.
    failed: bool,
}

pub(crate) struct BatchValidator(Option<BatchValidatorInner>);

/// Creates an Orchard bundle batch validation context for the circuit in force at the height
/// of the block being validated: the NU6.3 restricted circuit if `nu6_3_active`, else the
/// NU6.2 fixed circuit if `nu6_2_active`, else the pre-NU6.2 circuit.
pub(crate) fn orchard_batch_validation_init(
    cache_store: bool,
    nu6_2_active: bool,
    nu6_3_active: bool,
) -> Box<BatchValidator> {
    let vk: &'static orchard::circuit::VerifyingKey = if nu6_3_active {
        &crate::ORCHARD_VK_POST_NU6_3
    } else if nu6_2_active {
        &crate::ORCHARD_VK_FIXED
    } else {
        &crate::ORCHARD_VK_INSECURE
    };
    Box::new(BatchValidator(Some(BatchValidatorInner {
        validator: orchard::bundle::BatchValidator::new(vk),
        queued_entries: CacheEntries::new(cache_store),
        failed: false,
    })))
}

impl BatchValidator {
    /// Adds an Orchard bundle to this batch.
    pub(crate) fn add_bundle(&mut self, bundle: Box<Bundle>, sighash: [u8; 32]) {
        let batch = self.0.as_mut();
        let bundle = bundle.inner();

        match (batch, bundle) {
            (Some(batch), Some(bundle)) => {
                let cache = orchard_bundle_validity_cache();

                // Compute the cache entry for this bundle. The commitments are used only as a
                // cache key, so we always compute them in the v5 domain: this is defined for
                // every Orchard-pool bundle version, and uniquely identifies the bundle's
                // effecting and authorizing data.
                let cache_entry = {
                    let bundle_commitment = bundle
                        .commitment(orchard::bundle::TxVersion::V5)
                        .expect("v5 commitments are defined for all Orchard-pool bundles");
                    let bundle_authorizing_commitment = bundle
                        .authorizing_commitment(orchard::bundle::TxVersion::V5)
                        .expect("v5 commitments are defined for all Orchard-pool bundles");
                    cache.compute_entry(
                        bundle_commitment.0.as_bytes().try_into().unwrap(),
                        bundle_authorizing_commitment
                            .0
                            .as_bytes()
                            .try_into()
                            .unwrap(),
                        &sighash,
                    )
                };

                // Check if this bundle's validation result exists in the cache.
                if !cache.contains(cache_entry, &mut batch.queued_entries) {
                    // The bundle has been added to `inner.queued_entries` because it was not
                    // in the cache. We now add its authorization to the validation batch.
                    if let Err(e) = batch.validator.add_bundle(bundle, sighash) {
                        // The bundle's flags cannot be enforced by this batch's verifying
                        // key; the enclosing transaction is invalid for the batch's epoch.
                        error!("Failed to add Orchard bundle to batch: {}", e);
                        batch.failed = true;
                    }
                }
            }
            (Some(_), None) => debug!("Tx has no Orchard component"),
            (None, _) => error!("orchard::BatchValidator has already been used"),
        }
    }

    /// Adds an Ironwood bundle to this batch.
    ///
    /// Ironwood bundles use the Orchard protocol with the post-NU6.3 circuit, so they are
    /// batch-validated together with post-NU6.3 Orchard-pool bundles; the batch must have
    /// been constructed with `nu6_3_active` (its verifying key rejects them otherwise).
    /// They also share the Orchard bundle validity cache: cache keys are derived from
    /// bundle commitments, whose personalization strings are distinct per pool, so entries
    /// for the two pools cannot collide.
    pub(crate) fn add_ironwood_bundle(&mut self, bundle: Box<IronwoodBundle>, sighash: [u8; 32]) {
        let batch = self.0.as_mut();
        let bundle = bundle.inner();

        match (batch, bundle) {
            (Some(batch), Some(bundle)) => {
                let cache = orchard_bundle_validity_cache();

                // Compute the cache entry for this bundle. Ironwood bundles exist only in
                // v6 transactions, so their commitments are computed in the v6 domain.
                let cache_entry = {
                    let bundle_commitment = bundle
                        .commitment(orchard::bundle::TxVersion::V6)
                        .expect("v6 commitments are defined for Ironwood bundles");
                    let bundle_authorizing_commitment = bundle
                        .authorizing_commitment(orchard::bundle::TxVersion::V6)
                        .expect("v6 commitments are defined for Ironwood bundles");
                    cache.compute_entry(
                        bundle_commitment.0.as_bytes().try_into().unwrap(),
                        bundle_authorizing_commitment
                            .0
                            .as_bytes()
                            .try_into()
                            .unwrap(),
                        &sighash,
                    )
                };

                // Check if this bundle's validation result exists in the cache.
                if !cache.contains(cache_entry, &mut batch.queued_entries) {
                    // The bundle has been added to `inner.queued_entries` because it was not
                    // in the cache. We now add its authorization to the validation batch.
                    if let Err(e) = batch.validator.add_bundle(bundle, sighash) {
                        // The bundle's flags cannot be enforced by this batch's verifying
                        // key; the enclosing transaction is invalid for the batch's epoch.
                        error!("Failed to add Ironwood bundle to batch: {}", e);
                        batch.failed = true;
                    }
                }
            }
            (Some(_), None) => debug!("Tx has no Ironwood component"),
            (None, _) => error!("orchard::BatchValidator has already been used"),
        }
    }

    /// Validates this batch.
    ///
    /// - Returns `true` if `batch` is null.
    /// - Returns `false` if any item in the batch is invalid.
    ///
    /// The batch validation context is freed by this function.
    ///
    /// ## Consensus rules
    ///
    /// [§4.6](https://zips.z.cash/protocol/protocol.pdf#actiondesc):
    /// - Canonical element encodings are enforced by [`orchard_bundle_parse`].
    /// - SpendAuthSig^Orchard validity is enforced here.
    /// - Proof validity is enforced here.
    ///
    /// [§7.1](https://zips.z.cash/protocol/protocol.pdf#txnencodingandconsensus):
    /// - `bindingSigOrchard` validity is enforced here.
    pub(crate) fn validate(&mut self) -> bool {
        if let Some(inner) = self.0.take() {
            if inner.failed {
                // A bundle could not be added to the batch (its flags are unsupported by
                // this batch's verifying key), so the batch is invalid.
                return false;
            }
            // The verifying key for this batch's circuit was fixed at construction
            // (`orchard_batch_validation_init`).
            if inner.validator.validate(OsRng) {
                // `BatchValidator::validate()` is only called if every
                // `BatchValidator::check_bundle()` returned `true`, so at this point
                // every bundle that was added to `inner.queued_entries` has valid
                // authorization.
                orchard_bundle_validity_cache_mut().insert(inner.queued_entries);
                true
            } else {
                false
            }
        } else {
            error!("orchard::BatchValidator has already been used");
            false
        }
    }
}
