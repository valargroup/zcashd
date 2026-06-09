// Copyright (c) 2026 The Zcash developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://www.opensource.org/licenses/mit-license.php .

#include "zebra_compat/metadata.h"

#include "chainparams.h"
#include "dbwrapper.h"
#include "sync.h"
#include "util/system.h"

#include <exception>
#include <memory>
#include <utility>

namespace unity {
namespace {

static const char DB_TRUSTED_BLOCK_BOUNDARY = 'B';
static const size_t UNITY_METADATA_CACHE_SIZE = 1 << 20;

CCriticalSection cs_unity_metadata;
std::unique_ptr<CDBWrapper> g_unity_metadata_db;
bool g_cached_trusted_boundary_present = false;
TrustedBlockBoundary g_cached_trusted_boundary;
std::string g_unity_metadata_last_error;

std::pair<char, std::string> TrustedBoundaryKey()
{
    return std::make_pair(DB_TRUSTED_BLOCK_BOUNDARY, std::string("trusted-block-boundary"));
}

bool EnsureUnityMetadataDB()
{
    AssertLockHeld(cs_unity_metadata);

    if (g_unity_metadata_db) {
        return true;
    }

    try {
        g_unity_metadata_db.reset(new CDBWrapper(GetDataDir() / "unity", UNITY_METADATA_CACHE_SIZE, false, false));

        TrustedBlockBoundary boundary;
        g_cached_trusted_boundary_present =
            g_unity_metadata_db->Read(TrustedBoundaryKey(), boundary);
        if (g_cached_trusted_boundary_present) {
            g_cached_trusted_boundary = boundary;
        } else {
            g_cached_trusted_boundary = TrustedBlockBoundary();
        }
        g_unity_metadata_last_error.clear();
        return true;
    } catch (const std::exception& e) {
        g_unity_metadata_last_error = e.what();
        g_unity_metadata_db.reset();
        g_cached_trusted_boundary_present = false;
        g_cached_trusted_boundary = TrustedBlockBoundary();
        LogPrintf("Unity metadata database error: %s\n", e.what());
        return false;
    }
}

} // namespace

bool WriteTrustedBlockBoundary(const TrustedBlockBoundary& boundary)
{
    LOCK(cs_unity_metadata);
    if (!EnsureUnityMetadataDB()) {
        return false;
    }

    try {
        if (GetBoolArg("-zebra-compat-fail-trusted-boundary-write", false)) {
            g_unity_metadata_last_error = "trusted block boundary write failure injected";
            return false;
        }
        if (!g_unity_metadata_db->Write(TrustedBoundaryKey(), boundary, true)) {
            g_unity_metadata_last_error = "failed to write trusted block boundary";
            return false;
        }
        g_cached_trusted_boundary = boundary;
        g_cached_trusted_boundary_present = true;
        g_unity_metadata_last_error.clear();
        return true;
    } catch (const std::exception& e) {
        g_unity_metadata_last_error = e.what();
        LogPrintf("Unity metadata write error: %s\n", e.what());
        return false;
    }
}

bool ReadTrustedBlockBoundary(TrustedBlockBoundary& boundary)
{
    LOCK(cs_unity_metadata);
    if (!EnsureUnityMetadataDB() || !g_cached_trusted_boundary_present) {
        return false;
    }

    boundary = g_cached_trusted_boundary;
    return true;
}

bool ClearTrustedBlockBoundary()
{
    LOCK(cs_unity_metadata);
    if (!EnsureUnityMetadataDB()) {
        return false;
    }

    try {
        if (!g_unity_metadata_db->Erase(TrustedBoundaryKey(), true)) {
            g_unity_metadata_last_error = "failed to clear trusted block boundary";
            return false;
        }
        g_cached_trusted_boundary = TrustedBlockBoundary();
        g_cached_trusted_boundary_present = false;
        g_unity_metadata_last_error.clear();
        return true;
    } catch (const std::exception& e) {
        g_unity_metadata_last_error = e.what();
        LogPrintf("Unity metadata erase error: %s\n", e.what());
        return false;
    }
}

bool InitUnityMetadata()
{
    LOCK(cs_unity_metadata);
    return EnsureUnityMetadataDB();
}

void StopUnityMetadata()
{
    LOCK(cs_unity_metadata);
    g_unity_metadata_db.reset();
    g_cached_trusted_boundary = TrustedBlockBoundary();
    g_cached_trusted_boundary_present = false;
    g_unity_metadata_last_error.clear();
}

bool GetCachedTrustedBlockBoundary(TrustedBlockBoundary& boundary)
{
    LOCK(cs_unity_metadata);
    if (!g_cached_trusted_boundary_present) {
        return false;
    }

    boundary = g_cached_trusted_boundary;
    return true;
}

std::string GetUnityMetadataLastError()
{
    LOCK(cs_unity_metadata);
    return g_unity_metadata_last_error;
}

TrustedBlockBoundary MakeTrustedBlockBoundary(int nHeight, const uint256& hash, const CChainParams& chainparams)
{
    TrustedBlockBoundary boundary;
    boundary.nHeight = nHeight;
    boundary.hash = hash;
    boundary.network = chainparams.NetworkIDString();
    boundary.genesisHash = chainparams.GetConsensus().hashGenesisBlock.GetHex();
    boundary.zebraEndpoint = GetArg("-zebra-compat-url", "");
    return boundary;
}

bool TrustedBoundaryMatchesConfiguredSource(const TrustedBlockBoundary& boundary, const CChainParams& chainparams)
{
    return boundary.IsSet() &&
        boundary.network == chainparams.NetworkIDString() &&
        boundary.genesisHash == chainparams.GetConsensus().hashGenesisBlock.GetHex() &&
        !boundary.zebraEndpoint.empty() &&
        boundary.zebraEndpoint == GetArg("-zebra-compat-url", "");
}

} // namespace unity
