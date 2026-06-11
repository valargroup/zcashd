// Copyright (c) 2026 The Zcash developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://www.opensource.org/licenses/mit-license.php .

#include "zebra_compat/zebra_compat.h"

#include "rpc/server.h"
#include "util/strencodings.h"
#include "util/system.h"

#include <stdexcept>

#include <univalue.h>

namespace {

// Implements `getzebracompatinfo`. Accepts no parameters and throws help text
// for help requests or invalid arity.
UniValue getzebracompatinfo(const UniValue& params, bool fHelp)
{
    if (fHelp || params.size() != 0) {
        throw std::runtime_error(
            "getzebracompatinfo\n"
            "\nReturns zebra-compat node mode and source status.\n"
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
            "    \"sticky_fault\": true|false,\n"
            "    \"retry_requested\": true|false,\n"
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
            + HelpExampleCli("getzebracompatinfo", "")
            + HelpExampleRpc("getzebracompatinfo", "")
        );
    }

    return zebra_compat::GetZebraCompatInfo();
}

// Implements `zebracompatretry`. Accepts no parameters and requests one worker
// pass only when a sticky fault is currently parked.
UniValue zebracompatretry(const UniValue& params, bool fHelp)
{
    if (fHelp || params.size() != 0) {
        throw std::runtime_error(
            "zebracompatretry\n"
            "\nRequests one fresh zebra-compat sync pass after a sticky fault parks the worker.\n"
            "If the underlying condition is still present, the worker will fail sticky again.\n"
            "\nResult:\n"
            "{\n"
            "  \"retry_requested\": true|false,\n"
            "  \"sticky_fault\": true|false,\n"
            "  \"worker_running\": true|false,\n"
            "  \"state\": \"...\",\n"
            "  \"detail\": \"...\"\n"
            "}\n"
            "\nExamples:\n"
            + HelpExampleCli("zebracompatretry", "")
            + HelpExampleRpc("zebracompatretry", "")
        );
    }

    zebra_compat::ZebraCompatRetryResult retry = zebra_compat::RetryZebraCompatStickyFault();
    UniValue obj(UniValue::VOBJ);
    obj.pushKV("retry_requested", retry.retryRequested);
    obj.pushKV("sticky_fault", retry.stickyFault);
    obj.pushKV("worker_running", retry.workerRunning);
    obj.pushKV("state", retry.state);
    obj.pushKV("detail", retry.detail);
    return obj;
}

static const CRPCCommand commands[] =
{ //  category              name                      actor (function)         okSafeMode
  //  --------------------- ------------------------  -----------------------  ----------
    { "zebra-compat",       "getzebracompatinfo",     &getzebracompatinfo,     true  },
    { "zebra-compat",       "zebracompatretry",       &zebracompatretry,       true  },
};

} // namespace

namespace zebra_compat {

void RegisterZebraCompatRPCCommands(CRPCTable& tableRPC)
{
    for (unsigned int vcidx = 0; vcidx < ARRAYLEN(commands); vcidx++) {
        tableRPC.appendCommand(commands[vcidx].name, &commands[vcidx]);
    }
}

} // namespace zebra_compat
