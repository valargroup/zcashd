// Copyright (c) 2026 The Zcash developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://www.opensource.org/licenses/mit-license.php .

#ifndef ZCASH_ZEBRA_COMPAT_METADATA_H
#define ZCASH_ZEBRA_COMPAT_METADATA_H

#include "serialize.h"
#include "uint256.h"

#include <string>

class CChainParams;

namespace unity {

struct TrustedBlockBoundary {
    int nHeight = -1;
    uint256 hash;
    std::string network;
    std::string genesisHash;
    std::string zebraEndpoint;

    ADD_SERIALIZE_METHODS;

    template <typename Stream, typename Operation>
    inline void SerializationOp(Stream& s, Operation ser_action) {
        READWRITE(nHeight);
        READWRITE(hash);
        READWRITE(network);
        READWRITE(genesisHash);
        READWRITE(zebraEndpoint);
    }

    bool IsSet() const { return nHeight >= 0 && !hash.IsNull(); }
};

bool WriteTrustedBlockBoundary(const TrustedBlockBoundary& boundary);
bool ReadTrustedBlockBoundary(TrustedBlockBoundary& boundary);
bool ClearTrustedBlockBoundary();
bool InitUnityMetadata();
void StopUnityMetadata();
bool GetCachedTrustedBlockBoundary(TrustedBlockBoundary& boundary);
std::string GetUnityMetadataLastError();
TrustedBlockBoundary MakeTrustedBlockBoundary(int nHeight, const uint256& hash, const CChainParams& chainparams);
bool TrustedBoundaryMatchesConfiguredSource(const TrustedBlockBoundary& boundary, const CChainParams& chainparams);

} // namespace unity

#endif // ZCASH_ZEBRA_COMPAT_METADATA_H
