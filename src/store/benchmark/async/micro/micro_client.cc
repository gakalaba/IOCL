/***********************************************************************
 *
 * store/benchmark/async/micro/micro_client.cc:
 *
 * Copyright 2025 Anja Kalaba
 *
 * Permission is hereby granted, free of charge, to any person
 * obtaining a copy of this software and associated documentation
 * files (the "Software"), to deal in the Software without
 * restriction, including without limitation the rights to use, copy,
 * modify, merge, publish, distribute, sublicense, and/or sell copies
 * of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *
 **********************************************************************/
#include "store/benchmark/async/micro/micro_client.h"
#include "store/benchmark/async/retwis/add_user.h"

#include <iostream>

#include "store/benchmark/async/micro/big_transaction.h"
#include "store/benchmark/async/micro/app_request.h"
#include "store/strongstore/common.h"

namespace micro
{

    MicroClient::MicroClient(KeySelector *keySelector, const std::vector<Client *> &clients, uint32_t timeout,
                             Transport &transport, uint64_t id,
                             BenchmarkClientMode mode,
                             double switch_probability,
                             double arrival_rate, double think_time, double stay_probability,
                             int mpl,
                             int expDuration, int warmupSec, int cooldownSec, int tputInterval, uint32_t abortBackoff,
                             bool retryAborted, uint32_t maxBackoff, uint32_t maxAttempts, uint64_t fanout, bool issueConcurrent,
                             uint32_t read_percentage,
                             bool wo_replacement,
                             strongstore::LinearizableProtocol protocol)
        : BenchmarkClient(clients, timeout, transport, id,
                          mode,
                          switch_probability,
                          arrival_rate, think_time, stay_probability,
                          mpl,
                          expDuration, warmupSec, cooldownSec, abortBackoff,
                          retryAborted, maxBackoff, maxAttempts, fanout, issueConcurrent, "", protocol),
          keySelector(keySelector),
          fanout_{fanout},
          read_percentage_{read_percentage},
          wo_replacement_{wo_replacement},
          txn_idx_{0},
          max_txns_per_client_{100000}
    {
        ASSERT(fanout > 0);
        allKeyIdxs.reserve(fanout * max_txns_per_client_);
        std::unordered_set<int> seenKeys_;
        std::mt19937 &rand = GetRand();
        for (int i = 0; i < max_txns_per_client_; i++) {
            rand = GetRand();
            if (!wo_replacement_) {
                for (int i = 0; i < fanout; ++i)
                {
                    allKeyIdxs.push_back(keySelector->GetKey(rand));
                }
            } else {
                seenKeys_.clear();
                for (int i = 0; i < fanout; ++i)
                {
                    int ki = keySelector->GetKeyWOReplacement(rand, seenKeys_);
                    seenKeys_.insert(ki);
                    allKeyIdxs.push_back(ki);
                }
            }
        }
    }

    MicroClient::~MicroClient()
    {
    }

    AsyncTransaction *MicroClient::GetNextTransaction()
    {
        int this_txn_id = txn_idx_;
        if (this_txn_id >= max_txns_per_client_) {
            Panic("Exceeded max txns per client!");
        }
        txn_idx_++;
        return new BasicBigTransaction(keySelector, fanout_, read_percentage_, gsl::span<int>(allKeyIdxs).subspan(this_txn_id * fanout_, fanout_));
    }

    AsyncAppRequest *MicroClient::GetNextAppRequest()
    {
        int this_txn_id = txn_idx_;
        if (this_txn_id >= max_txns_per_client_) {
            Panic("Exceeded max txns per client!");
        }
        txn_idx_++;
        return new BasicAppRequest(keySelector, fanout_, read_percentage_, gsl::span<int>(allKeyIdxs).subspan(this_txn_id * fanout_, fanout_));
    }


} // namespace micro

