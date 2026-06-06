// Copyright (c) 2026 The Zcash developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://www.opensource.org/licenses/mit-license.php .

#include "unity/unity.h"

#include "rpc/server.h"
#include "util/strencodings.h"
#include "util/system.h"

#include <stdexcept>

#include <univalue.h>

namespace {

UniValue getunityinfo(const UniValue& params, bool fHelp)
{
    if (fHelp || params.size() != 0) {
        throw std::runtime_error(
            "getunityinfo\n"
            "\nReturns Unity node mode and source status.\n"
            "\nResult:\n"
            "{\n"
            "  \"enabled\": true|false,\n"
            "  \"service_state\": \"waiting|stopped|disabled\",\n"
            "  \"readiness\": \"ready|degraded|failed|disabled\",\n"
            "  \"blocksource\": \"p2p|zebra\",\n"
            "  \"p2p\": true|false,\n"
            "  \"blockvalidation\": \"full|trusted-zebra\",\n"
            "  \"zebra\": {\n"
            "    \"configured\": true|false,\n"
            "    \"reachable\": true|false,\n"
            "    \"identity_verified\": true|false,\n"
            "    \"streaming\": false,\n"
            "    \"url\": \"...\",\n"
            "    \"network\": \"main|test|regtest\",\n"
            "    \"genesis\": \"...\",\n"
            "    \"bestblockhash\": \"...\",\n"
            "    \"blocks\": n\n"
            "  },\n"
            "  \"local\": {\n"
            "    \"bestblockhash\": \"...\",\n"
            "    \"blocks\": n,\n"
            "    \"initial_block_download_complete\": true|false,\n"
            "    \"validation_notifications_caught_up\": true|false\n"
            "  },\n"
            "  \"ingestion\": {\n"
            "    \"last_success\": true|false,\n"
            "    \"last_hard_failure\": true|false,\n"
            "    \"last_height\": n|null,\n"
            "    \"last_hash\": \"...\"|null,\n"
            "    \"last_error\": \"...\"|null\n"
            "  },\n"
            "  \"trusted_boundary\": {\n"
            "    \"active\": true|false,\n"
            "    \"height\": n,\n"
            "    \"hash\": \"...\",\n"
            "    \"network\": \"main|test|regtest\",\n"
            "    \"genesis\": \"...\",\n"
            "    \"zebra_url\": \"...\"\n"
            "  },\n"
            "  \"mempool_mirror\": {\n"
            "    \"source\": \"zebra-poll\",\n"
            "    \"lag\": n,\n"
            "    \"last_update\": n|null,\n"
            "    \"last_failure\": n|null,\n"
            "    \"divergent\": n,\n"
            "    \"divergent_detail_sample_size\": n,\n"
            "    \"divergent_detail_overflow\": n,\n"
            "    \"zebra_size\": n|null,\n"
            "    \"local_size\": n,\n"
            "    \"last_error\": null|\"...\"\n"
            "  },\n"
            "  \"tx_forwarding\": {\n"
            "    \"last_success\": n|null,\n"
            "    \"last_error\": null|\"...\",\n"
            "    \"last_transport_error\": null|\"...\",\n"
            "    \"pending\": n\n"
            "  },\n"
            "  \"sync\": {\n"
            "    \"state\": \"synced|syncing|degraded|failed|disabled\",\n"
            "    \"detail\": \"...\",\n"
            "    \"last_error\": null|\"...\",\n"
            "    \"target_height\": n|null,\n"
            "    \"target_hash\": \"...\"|null,\n"
            "    \"last_synced_height\": n|null,\n"
            "    \"last_synced_hash\": \"...\"|null,\n"
            "    \"last_common_ancestor_height\": n|null,\n"
            "    \"last_common_ancestor_hash\": \"...\"|null,\n"
            "    \"lag\": n|null,\n"
            "    \"retry_count\": n,\n"
            "    \"current_backoff_seconds\": n,\n"
            "    \"next_retry\": n|null\n"
            "  },\n"
            "  \"metrics\": {\n"
            "    \"sync_lag\": n|null,\n"
            "    \"mempool_lag\": n,\n"
            "    \"mempool_ready\": true|false,\n"
            "    \"mempool_last_update_age_seconds\": n|null,\n"
            "    \"mempool_divergent\": n,\n"
            "    \"tx_forwarding_pending\": n,\n"
            "    \"tx_forwarding_transport_ready\": true|false,\n"
            "    \"validation_notifications_caught_up\": true|false,\n"
            "    \"retry_count\": n,\n"
            "    \"current_backoff_seconds\": n\n"
            "  },\n"
            "  \"limits\": {\n"
            "    \"poll_interval_seconds\": n,\n"
            "    \"max_retry_backoff_seconds\": n,\n"
            "    \"sync_batch_size\": n,\n"
            "    \"zebra_rpc_max_response_body_bytes\": n,\n"
            "    \"mempool_txids_per_poll\": n,\n"
            "    \"mempool_divergence_details\": n,\n"
            "    \"pending_forwarded_transactions\": n\n"
            "  }\n"
            "}\n"
            "\nExamples:\n"
            + HelpExampleCli("getunityinfo", "")
            + HelpExampleRpc("getunityinfo", "")
        );
    }

    return unity::GetUnityInfo();
}

static const CRPCCommand commands[] =
{ //  category              name                      actor (function)         okSafeMode
  //  --------------------- ------------------------  -----------------------  ----------
    { "unity",              "getunityinfo",           &getunityinfo,           true  },
};

} // namespace

namespace unity {

void RegisterUnityRPCCommands(CRPCTable& tableRPC)
{
    for (unsigned int vcidx = 0; vcidx < ARRAYLEN(commands); vcidx++) {
        tableRPC.appendCommand(commands[vcidx].name, &commands[vcidx]);
    }
}

} // namespace unity
