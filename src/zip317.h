// Copyright (c) 2023-2023 The Zcash developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://www.opensource.org/licenses/mit-license.php .

#ifndef ZCASH_ZIP317_H
#define ZCASH_ZIP317_H

#include "amount.h"
#include "primitives/transaction.h"

#include <cstdint>
#include <cstddef>
#include <vector>

// Constants for fee calculation.
// ZIP 317 specifies 5000 zatoshis per logical action. This wallet charges 1000, which still
// covers the 400 that Zakura, the node it runs behind, requires for mempool acceptance.
static const CAmount MARGINAL_FEE = 1000;
static const size_t GRACE_ACTIONS = 2;
static const size_t P2PKH_STANDARD_INPUT_SIZE = 150;
static const size_t P2PKH_STANDARD_OUTPUT_SIZE = 34;

// Constants for block template construction.
static const int64_t WEIGHT_RATIO_SCALE = INT64_C(10000000000000000);
static const int64_t WEIGHT_RATIO_CAP = 4;
static const size_t DEFAULT_BLOCK_UNPAID_ACTION_LIMIT = 0;

/// Limit on the number of unpaid actions a transaction can have to be accepted to the mempool.
static const size_t DEFAULT_TX_UNPAID_ACTION_LIMIT = DEFAULT_BLOCK_UNPAID_ACTION_LIMIT;

// The wallet refuses an explicit fee above this multiple of its conventional fee, as a likely
// mistake: 40,000 zatoshis per logical action, 8 times the ZIP 317 fee. Fees that pool software
// computes at ZIP 317's rate, or a little above it, are therefore accepted. `-maxtxfee` still
// bounds the total fee of a transaction.
static const int64_t EXPLICIT_FEE_RATIO_CAP = 40;

/// This is the lowest the conventional fee can be in ZIP 317.
static const CAmount MINIMUM_FEE = MARGINAL_FEE * GRACE_ACTIONS;

/// Return the conventional fee for the given `logicalActionCount` calculated according to
/// <https://zips.z.cash/zip-0317#fee-calculation>.
CAmount CalculateConventionalFee(size_t logicalActionCount);

/// Return the number of logical actions calculated according to
/// <https://zips.z.cash/zip-0317#fee-calculation>.
size_t CalculateLogicalActionCount(
        const std::vector<CTxIn>& vin,
        const std::vector<CTxOut>& vout,
        unsigned int joinSplitCount,
        unsigned int saplingSpendCount,
        unsigned int saplingOutputCount,
        unsigned int orchardActionCount);

#endif // ZCASH_ZIP317_H
