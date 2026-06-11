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

namespace zebra_compat {
namespace {

static const char DB_TRUSTED_BLOCK_BOUNDARY = 'B';
static const size_t ZEBRA_COMPAT_METADATA_CACHE_SIZE = 1 << 20;

CCriticalSection cs_zebra_compat_metadata;
std::unique_ptr<CDBWrapper> g_zebra_compat_metadata_db;
bool g_cached_trusted_boundary_present = false;
TrustedBlockBoundary g_cached_trusted_boundary;
std::string g_zebra_compat_metadata_last_error;

std::pair<char, std::string> TrustedBoundaryKey()
{
    return std::make_pair(DB_TRUSTED_BLOCK_BOUNDARY, std::string("trusted-block-boundary"));
}

bool EnsureZebraCompatMetadataDB()
{
    AssertLockHeld(cs_zebra_compat_metadata);

    if (g_zebra_compat_metadata_db) {
        return true;
    }

    try {
        g_zebra_compat_metadata_db.reset(new CDBWrapper(GetDataDir() / "zebra-compat", ZEBRA_COMPAT_METADATA_CACHE_SIZE, false, false));

        TrustedBlockBoundary boundary;
        g_cached_trusted_boundary_present =
            g_zebra_compat_metadata_db->Read(TrustedBoundaryKey(), boundary);
        if (g_cached_trusted_boundary_present) {
            g_cached_trusted_boundary = boundary;
        } else {
            g_cached_trusted_boundary = TrustedBlockBoundary();
        }
        g_zebra_compat_metadata_last_error.clear();
        return true;
    } catch (const std::exception& e) {
        g_zebra_compat_metadata_last_error = e.what();
        g_zebra_compat_metadata_db.reset();
        g_cached_trusted_boundary_present = false;
        g_cached_trusted_boundary = TrustedBlockBoundary();
        LogPrintf("zebra-compat metadata database error: %s\n", e.what());
        return false;
    }
}

} // namespace

bool WriteTrustedBlockBoundary(const TrustedBlockBoundary& boundary)
{
    LOCK(cs_zebra_compat_metadata);
    if (!EnsureZebraCompatMetadataDB()) {
        return false;
    }

    try {
        if (!g_zebra_compat_metadata_db->Write(TrustedBoundaryKey(), boundary, true)) {
            g_zebra_compat_metadata_last_error = "failed to write trusted block boundary";
            return false;
        }
        g_cached_trusted_boundary = boundary;
        g_cached_trusted_boundary_present = true;
        g_zebra_compat_metadata_last_error.clear();
        return true;
    } catch (const std::exception& e) {
        g_zebra_compat_metadata_last_error = e.what();
        LogPrintf("zebra-compat metadata write error: %s\n", e.what());
        return false;
    }
}

bool ReadTrustedBlockBoundary(TrustedBlockBoundary& boundary)
{
    LOCK(cs_zebra_compat_metadata);
    if (!EnsureZebraCompatMetadataDB() || !g_cached_trusted_boundary_present) {
        return false;
    }

    boundary = g_cached_trusted_boundary;
    return true;
}

bool ClearTrustedBlockBoundary()
{
    LOCK(cs_zebra_compat_metadata);
    if (!EnsureZebraCompatMetadataDB()) {
        return false;
    }

    try {
        if (!g_zebra_compat_metadata_db->Erase(TrustedBoundaryKey(), true)) {
            g_zebra_compat_metadata_last_error = "failed to clear trusted block boundary";
            return false;
        }
        g_cached_trusted_boundary = TrustedBlockBoundary();
        g_cached_trusted_boundary_present = false;
        g_zebra_compat_metadata_last_error.clear();
        return true;
    } catch (const std::exception& e) {
        g_zebra_compat_metadata_last_error = e.what();
        LogPrintf("zebra-compat metadata erase error: %s\n", e.what());
        return false;
    }
}

bool InitZebraCompatMetadata()
{
    LOCK(cs_zebra_compat_metadata);
    return EnsureZebraCompatMetadataDB();
}

void StopZebraCompatMetadata()
{
    LOCK(cs_zebra_compat_metadata);
    g_zebra_compat_metadata_db.reset();
    g_cached_trusted_boundary = TrustedBlockBoundary();
    g_cached_trusted_boundary_present = false;
    g_zebra_compat_metadata_last_error.clear();
}

bool GetCachedTrustedBlockBoundary(TrustedBlockBoundary& boundary)
{
    LOCK(cs_zebra_compat_metadata);
    if (!g_cached_trusted_boundary_present) {
        return false;
    }

    boundary = g_cached_trusted_boundary;
    return true;
}

std::string GetZebraCompatMetadataLastError()
{
    LOCK(cs_zebra_compat_metadata);
    return g_zebra_compat_metadata_last_error;
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

} // namespace zebra_compat
