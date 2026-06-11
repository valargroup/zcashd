// Copyright (c) 2026 The Zcash developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://www.opensource.org/licenses/mit-license.php .

#ifndef ZCASH_ZEBRA_COMPAT_METADATA_H
#define ZCASH_ZEBRA_COMPAT_METADATA_H

#include "serialize.h"
#include "uint256.h"

#include <string>

class CChainParams;

namespace zebra_compat {

struct TrustedBlockBoundary {
    int nHeight = -1;
    uint256 hash;
    std::string network;
    std::string genesisHash;
    std::string zebraEndpoint;

    ADD_SERIALIZE_METHODS;

    // Serializes the persisted boundary fields in stable LevelDB order.
    template <typename Stream, typename Operation>
    inline void SerializationOp(Stream& s, Operation ser_action) {
        READWRITE(nHeight);
        READWRITE(hash);
        READWRITE(network);
        READWRITE(genesisHash);
        READWRITE(zebraEndpoint);
    }

    // Returns true when this boundary names a concrete chain height and block.
    bool IsSet() const { return nHeight >= 0 && !hash.IsNull(); }
};

// Persists and caches the trusted validation boundary. Returns false and stores
// a metadata error if the LevelDB write fails.
bool WriteTrustedBlockBoundary(const TrustedBlockBoundary& boundary);

// Reads the trusted validation boundary from the metadata store, opening it on
// demand. Returns false when no boundary exists or the store cannot be opened.
bool ReadTrustedBlockBoundary(TrustedBlockBoundary& boundary);

// Removes the trusted validation boundary from disk and cache. Returns false
// when the metadata store cannot be opened or the erase fails.
bool ClearTrustedBlockBoundary();

// Opens the zebra-compat metadata store and primes the in-memory boundary cache.
bool InitZebraCompatMetadata();

// Closes the metadata store and clears cached metadata state.
void StopZebraCompatMetadata();

// Returns the cached trusted boundary without opening the metadata store.
bool GetCachedTrustedBlockBoundary(TrustedBlockBoundary& boundary);

// Returns the last metadata-layer error recorded by this process.
std::string GetZebraCompatMetadataLastError();

// Builds a boundary value for the active configured chain and Zebra endpoint.
TrustedBlockBoundary MakeTrustedBlockBoundary(int nHeight, const uint256& hash, const CChainParams& chainparams);

// Checks whether a persisted boundary belongs to the current network, genesis,
// and configured Zebra endpoint.
bool TrustedBoundaryMatchesConfiguredSource(const TrustedBlockBoundary& boundary, const CChainParams& chainparams);

} // namespace zebra_compat

#endif // ZCASH_ZEBRA_COMPAT_METADATA_H
