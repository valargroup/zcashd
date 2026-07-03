use std::mem;

use memuse::DynamicUsage;
use orchard::{
    bundle::{Authorized, BundleVersion},
    keys::OutgoingViewingKey,
    note_encryption::IronwoodDomain,
    primitives::redpallas::{Signature, SpendAuth},
};
use pasta_curves::group::{Group as _, GroupEncoding as _};
use pasta_curves::pallas;
use zcash_note_encryption::try_output_recovery_with_ovk;
use zcash_primitives::transaction::components::orchard as orchard_serialization;
use zcash_protocol::value::ZatBalance;

use crate::streams::CppStream;

/// An Ironwood-pool Action. The Ironwood pool uses the Orchard protocol, so this wraps the
/// same underlying type as `orchard_bundle::Action`; it is a distinct FFI type because the
/// two pools have separate note commitment trees, nullifier sets, and note plaintext formats.
pub struct Action(orchard::Action<Signature<SpendAuth>>);

impl Action {
    pub(crate) fn cv(&self) -> [u8; 32] {
        self.0.cv_net().to_bytes()
    }

    pub(crate) fn nullifier(&self) -> [u8; 32] {
        self.0.nullifier().to_bytes()
    }

    pub(crate) fn rk(&self) -> [u8; 32] {
        self.0.rk().into()
    }

    pub(crate) fn cmx(&self) -> [u8; 32] {
        self.0.cmx().to_bytes()
    }

    pub(crate) fn ephemeral_key(&self) -> [u8; 32] {
        self.0.encrypted_note().epk_bytes
    }

    pub(crate) fn enc_ciphertext(&self) -> [u8; 580] {
        self.0.encrypted_note().enc_ciphertext
    }

    pub(crate) fn out_ciphertext(&self) -> [u8; 80] {
        self.0.encrypted_note().out_ciphertext
    }

    pub(crate) fn spend_auth_sig(&self) -> [u8; 64] {
        self.0.authorization().into()
    }
}

/// The Ironwood component of an authorized transaction (`None` if absent). The inner bundle
/// always carries `BundleVersion::ironwood_v3()`.
#[derive(Clone)]
pub struct Bundle(Option<orchard::Bundle<Authorized, ZatBalance>>);

pub(crate) fn none_ironwood_bundle() -> Box<Bundle> {
    Box::new(Bundle(None))
}

/// Parses an authorized Ironwood bundle from the given stream, in the v6 transaction format.
pub(crate) fn parse_ironwood_bundle(reader: &mut CppStream<'_>) -> Result<Box<Bundle>, String> {
    match orchard_serialization::read_v6_bundle(reader, BundleVersion::ironwood_v3()) {
        Ok(parsed) => Ok(Box::new(Bundle(parsed))),
        Err(e) => Err(format!("Failed to parse Ironwood bundle: {}", e)),
    }
}

impl Bundle {
    /// Returns a copy of the value.
    pub(crate) fn box_clone(&self) -> Box<Self> {
        Box::new(self.clone())
    }

    /// Serializes an authorized Ironwood bundle to the given stream, in the v6 transaction
    /// format.
    ///
    /// If `bundle == None`, this serializes `nActionsIronwood = 0`.
    pub(crate) fn serialize(&self, writer: &mut CppStream<'_>) -> Result<(), String> {
        orchard_serialization::write_v6_bundle(self.inner(), writer)
            .map_err(|e| format!("Failed to serialize Ironwood bundle: {}", e))
    }

    pub(crate) fn inner(&self) -> Option<&orchard::Bundle<Authorized, ZatBalance>> {
        self.0.as_ref()
    }

    /// Returns the amount of dynamically-allocated memory used by this bundle.
    pub(crate) fn recursive_dynamic_usage(&self) -> usize {
        self.inner()
            // Bundles are boxed on the heap, so we count their own size as well as the size
            // of `Vec`s they allocate.
            .map(|bundle| mem::size_of_val(bundle) + bundle.dynamic_usage())
            // If the transaction has no Ironwood component, nothing is allocated for it.
            .unwrap_or(0)
    }

    /// Returns whether the Ironwood bundle is present.
    pub(crate) fn is_present(&self) -> bool {
        self.0.is_some()
    }

    pub(crate) fn actions(&self) -> Vec<Action> {
        self.0
            .iter()
            .flat_map(|b| b.actions().iter())
            .cloned()
            .map(Action)
            .collect()
    }

    pub(crate) fn num_actions(&self) -> usize {
        self.inner().map(|b| b.actions().len()).unwrap_or(0)
    }

    /// Returns whether the Ironwood bundle is present and spends are enabled.
    pub(crate) fn enable_spends(&self) -> bool {
        self.inner()
            .map(|b| b.flags().spends_enabled())
            .unwrap_or(false)
    }

    /// Returns whether the Ironwood bundle is present and outputs are enabled.
    pub(crate) fn enable_outputs(&self) -> bool {
        self.inner()
            .map(|b| b.flags().outputs_enabled())
            .unwrap_or(false)
    }

    /// Returns whether the Ironwood bundle is present and cross-address transfers are
    /// enabled (the `enableCrossAddress` flag bit, which the Ironwood pool may set freely).
    pub(crate) fn enable_cross_address(&self) -> bool {
        self.inner()
            .map(|b| b.flags().cross_address_enabled())
            .unwrap_or(false)
    }

    /// Returns the value balance for this Ironwood bundle.
    ///
    /// A transaction with no Ironwood component has a value balance of zero.
    pub(crate) fn value_balance_zat(&self) -> i64 {
        self.inner().map(|b| b.value_balance().into()).unwrap_or(0)
    }

    /// Returns the anchor for the bundle.
    ///
    /// # Panics
    ///
    /// Panics if the bundle is not present.
    pub(crate) fn anchor(&self) -> [u8; 32] {
        self.inner()
            .expect("Bundle actions should have been checked to be non-empty")
            .anchor()
            .to_bytes()
    }

    /// Returns the proof for the bundle.
    ///
    /// # Panics
    ///
    /// Panics if the bundle is not present.
    pub(crate) fn proof(&self) -> Vec<u8> {
        self.inner()
            .expect("Bundle actions should have been checked to be non-empty")
            .authorization()
            .proof()
            .as_ref()
            .to_vec()
    }

    /// Returns the binding signature for the bundle.
    ///
    /// # Panics
    ///
    /// Panics if the bundle is not present.
    pub(crate) fn binding_sig(&self) -> [u8; 64] {
        self.inner()
            .expect("Bundle actions should have been checked to be non-empty")
            .authorization()
            .binding_signature()
            .into()
    }

    /// Checks action fields that are not validated by the proof circuit:
    /// - rk must not be the identity (causes a crash in proof verification)
    /// - epk must encode a valid, non-identity Pallas curve point (consensus
    ///   rule per protocol spec §5.4.9.4); this rejects the all-zeros identity
    ///   encoding, non-canonical x (x >= q_P), and canonical x for which no
    ///   curve point exists.
    pub(crate) fn validate_action_encodings(&self) -> bool {
        if let Some(bundle) = self.inner() {
            for action in bundle.actions() {
                let rk_bytes: [u8; 32] = action.rk().into();
                if rk_bytes == [0u8; 32] {
                    return false;
                }
                if pallas::Point::from_bytes(&action.encrypted_note().epk_bytes)
                    .into_option()
                    .into_iter()
                    .all(|p| p.is_identity().into())
                {
                    return false;
                }
            }
        }
        true
    }

    /// Returns whether all actions contained in the Ironwood bundle can be decrypted with
    /// the all-zeros OVK.
    ///
    /// Returns `true` if no Ironwood actions are present.
    ///
    /// This should only be called on an Ironwood bundle that is an element of a coinbase
    /// transaction. Ironwood-pool notes use the quantum-recoverable note plaintext format
    /// (ZIP 2005, lead byte 0x03), so decryption uses the Ironwood note encryption domain.
    pub(crate) fn coinbase_outputs_are_valid(&self) -> bool {
        if let Some(bundle) = self.inner() {
            for act in bundle.actions() {
                if try_output_recovery_with_ovk(
                    &IronwoodDomain::for_action(act),
                    &OutgoingViewingKey::from([0u8; 32]),
                    act,
                    act.cv_net(),
                    &act.encrypted_note().out_ciphertext,
                )
                .is_none()
                {
                    return false;
                }
            }
        }

        // Either there are no Ironwood actions, or all of the outputs
        // are decryptable with the all-zeros OVK.
        true
    }
}
