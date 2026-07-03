// Copyright (c) 2026 The Zcash developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://www.opensource.org/licenses/mit-license.php .

#ifndef ZCASH_PRIMITIVES_IRONWOOD_H
#define ZCASH_PRIMITIVES_IRONWOOD_H

#include "streams.h"
#include "streams_rust.h"

#include <amount.h>

#include <rust/bridge.h>

class OrchardMerkleFrontier;

/**
 * The Ironwood component of an authorized transaction (ZIP 229).
 *
 * The Ironwood pool uses the Orchard protocol, but is a separate pool with its own
 * note commitment tree, nullifier set, anchors, and chain value pool. Ironwood
 * bundles exist only in v6 transactions.
 */
class IronwoodBundle
{
private:
    /// An optional Ironwood bundle.
    /// Memory is allocated by Rust.
    rust::Box<ironwood_bundle::Bundle> inner;

    friend class OrchardMerkleFrontier;
public:
    IronwoodBundle() : inner(ironwood_bundle::none()) {}

    IronwoodBundle(IronwoodBundle&& bundle) : inner(std::move(bundle.inner)) {}

    IronwoodBundle(const IronwoodBundle& bundle) :
        inner(bundle.inner->box_clone()) {}

    IronwoodBundle& operator=(IronwoodBundle&& bundle)
    {
        if (this != &bundle) {
            inner = std::move(bundle.inner);
        }
        return *this;
    }

    IronwoodBundle& operator=(const IronwoodBundle& bundle)
    {
        if (this != &bundle) {
            inner = bundle.inner->box_clone();
        }
        return *this;
    }

    const rust::Box<ironwood_bundle::Bundle>& GetDetails() const {
        return inner;
    }

    size_t RecursiveDynamicUsage() const {
        return inner->recursive_dynamic_usage();
    }

    template<typename Stream>
    void Serialize(Stream& s) const {
        try {
            inner->serialize(*ToRustStream(s));
        } catch (const std::exception& e) {
            throw std::ios_base::failure(e.what());
        }
    }

    template<typename Stream>
    void Unserialize(Stream& s) {
        try {
            inner = ironwood_bundle::parse(*ToRustStream(s));
        } catch (const std::exception& e) {
            throw std::ios_base::failure(e.what());
        }
    }

    /// Returns true if this contains an Ironwood bundle, or false if there is no
    /// Ironwood component.
    bool IsPresent() const { return inner->is_present(); }

    /// Returns the net value entering or exiting the Ironwood pool as a result of this
    /// bundle.
    CAmount GetValueBalance() const {
        return inner->value_balance_zat();
    }

    /// Queues this bundle's authorization for validation.
    ///
    /// `sighash` must be for the transaction this bundle is within.
    ///
    /// Ironwood bundles use the post-NU6.3 Orchard circuit, so the batch must have been
    /// constructed for the NU6.3 (or later) epoch.
    void QueueAuthValidation(
        orchard::BatchValidator& batch, const uint256& sighash) const
    {
        batch.add_ironwood_bundle(inner->box_clone(), sighash.GetRawBytes());
    }

    const size_t GetNumActions() const {
        return inner->num_actions();
    }

    const std::vector<uint256> GetNullifiers() const {
        const auto actions = inner->actions();
        std::vector<uint256> result;
        result.reserve(actions.size());
        for (const auto& action : actions) {
            result.push_back(uint256::FromRawBytes(action.nullifier()));
        }
        return result;
    }

    const std::optional<uint256> GetAnchor() const {
        if (IsPresent()) {
            return uint256::FromRawBytes(inner->anchor());
        } else {
            return std::nullopt;
        }
    }

    bool OutputsEnabled() const {
        return inner->enable_outputs();
    }

    bool SpendsEnabled() const {
        return inner->enable_spends();
    }

    /// Returns whether the bundle is present and its `enableCrossAddress` flag bit is set.
    /// Unlike the Orchard pool (for which this bit is reserved and MUST be 0 from NU6.3),
    /// the Ironwood pool may set this bit freely.
    bool CrossAddressEnabled() const {
        return inner->enable_cross_address();
    }

    /// Validates bundle fields that are not checked during proof verification but
    /// could cause crashes if malformed or violate consensus rules not enforced
    /// by the proof circuit. Returns true if all checks pass.
    bool ValidateWithoutProofVerification() const {
        return inner->validate_action_encodings();
    }

    bool CoinbaseOutputsAreValid() const {
        return inner->coinbase_outputs_are_valid();
    }
};

#endif // ZCASH_PRIMITIVES_IRONWOOD_H
