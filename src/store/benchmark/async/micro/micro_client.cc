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
                             bool isTransformed,
                             const std::string &latencyFilename)
        : BenchmarkClient(clients, timeout, transport, id,
                          mode,
                          switch_probability,
                          arrival_rate, think_time, stay_probability,
                          mpl,
                          expDuration, warmupSec, cooldownSec, abortBackoff,
                          retryAborted, maxBackoff, maxAttempts, fanout, issueConcurrent, isTransformed, latencyFilename),
          keySelector(keySelector),
          read_percentage_{read_percentage}
    {
        ASSERT(fanout > 0);
    }

    MicroClient::~MicroClient()
    {
    }

    AsyncTransaction *MicroClient::GetNextTransaction()
    {
        return new BasicBigTransaction(keySelector, GetFanout(), GetRand(), read_percentage_);
    }

    AsyncAppRequest *MicroClient::GetNextAppRequest()
    {
        return new BasicAppRequest(keySelector, GetFanout(), GetRand(), read_percentage_);
    }


} // namespace micro

