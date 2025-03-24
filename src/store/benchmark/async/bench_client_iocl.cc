/***********************************************************************
 *
 * store/benchmark/async/bench_client.cc:
 *
 * Copyright 2022 Jeffrey Helt, Matthew Burke, Amit Levy, Wyatt Lloyd
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
#include "store/benchmark/async/bench_client_iocl.h"

#include <sys/time.h>

#include <algorithm>
#include <sstream>
#include <string>
#include <utility>

#include "lib/latency.h"
#include "lib/message.h"
#include "lib/timeval.h"
#include "lib/transport.h"
#include "store/strongstore/iocl_client.h"

DEFINE_LATENCY(op);

BenchmarkClientIOCL::BenchmarkClientIOCL(const std::vector<Client *> &clients, uint32_t timeout,
                                         Transport &transport, uint64_t id,
                                         double switch_probability,
                                         double arrival_rate, double think_time, double stay_probability,
                                         int mpl,
                                         int expDuration, int warmupSec, int cooldownSec,
                                         uint32_t abortBackoff, bool retryAborted,
                                         uint32_t maxBackoff, uint32_t maxAttempts,
                                         uint64_t fanout,
                                         const std::string &latencyFilename)
    : BenchmarkClient(clients, timeout, transport, id, static_cast<BenchmarkClientMode>(UNKNOWN),
                      switch_probability,
                      arrival_rate, think_time, stay_probability,
                      mpl,
                      expDuration, warmupSec, cooldownSec,
                      abortBackoff, retryAborted,
                      maxBackoff, maxAttempts,
                      latencyFilename),
      fanout_{fanout}
{
    if (arrival_rate <= 0)
    {
        Panic("Arrival rate must be (strictly) positive!");
    }

    _Latency_Init(&latency, "txn");
}

BenchmarkClientIOCL::~BenchmarkClientIOCL()
{
    Debug("session_states_.size(): %lu", session_states_.size());
}

// Send next app level request
void BenchmarkClientIOCL::SendNext()
{
    Debug("[%lu] IOCL SendNext", n_sessions_started_);
    n_sessions_started_++;

    std::size_t client_index = n_sessions_started_ % clients_.size();
    auto &client = *clients_[client_index];

    auto &session = client.BeginSession();
    auto sid = session.id();

    Debug("session id: %lu", sid);

    auto ecb = std::bind(&BenchmarkClientIOCL::ExecuteCallback, this, sid, std::placeholders::_1);
    auto apprequest = GetNextAppRequest();
    stats.Increment("apprequest_attempts", 1);

    session_states_.emplace(sid, SessionState{session, apprequest, ecb, client_index, fanout_});

    auto &ss = session_states_.find(sid)->second;
    _Latency_StartRec(ss.lat());

    auto bcb = std::bind(&BenchmarkClientIOCL::ExecuteNextOperation, this, sid);

    client.Begin(session, bcb);

    if (!cooldownStarted)
    {
        bool send_next = (n_sessions_started_ < mpl_);
        uint64_t next_arrival_us = 0;

        if (send_next)
        {
            Debug("next app request arrival in %lu us; aka being put on event loop", next_arrival_us);
            transport_.TimerMicro(next_arrival_us, std::bind(&BenchmarkClientIOCL::SendNext, this));
        }
    }
}

void BenchmarkClientIOCL::ExecuteNextOperation(const uint64_t session_id)
{
    Debug("[%lu] IOCL ExecuteNextOperation of app level request", session_id);
    auto search = session_states_.find(session_id);
    ASSERT(search != session_states_.end());

    auto &ss = search->second;
    auto apprequest = ss.apprequest();
    auto op_index = ss.op_index();
    auto max_index = ss.get_fanout();
    auto &session = ss.session();

    Operation op = apprequest->GetNextOperation(op_index);
    ss.incr_op_index();

    auto gcb = std::bind(&BenchmarkClientIOCL::GetCallback, this, session_id, std::placeholders::_1, std::placeholders::_2, std::placeholders::_3, std::placeholders::_4);
    auto gtcb = std::bind(&BenchmarkClientIOCL::GetTimeout, this, session_id, std::placeholders::_1, std::placeholders::_2);
    auto pcb = std::bind(&BenchmarkClientIOCL::PutCallback, this, session_id, std::placeholders::_1, std::placeholders::_2, std::placeholders::_3);
    auto ptcb = std::bind(&BenchmarkClientIOCL::PutTimeout, this, session_id, std::placeholders::_1, std::placeholders::_2, std::placeholders::_3);

    auto client_index = ss.current_client_index();
    auto &client = *clients_[client_index];

    if (op_index = max_index)
    {
        client.Commit(session, ccb, ctcb, timeout_);
    }
    else
    {
        switch (op.type)
        {
        case GET:
            client.Get(session, op.key, gcb, gtcb, timeout_);
            break;
        case PUT:
            client.Put(session, op.key, op.value, pcb, ptcb, timeout_);
            break;
        }
    }
}

void BenchmarkClientIOCL::GetCallback(const uint64_t session_id, int status,
                                      const std::string &key, const std::string &val, Timestamp ts)
{
    Debug("[%lu] Get(%s) callback", session_id, key.c_str());
    auto search = session_states_.find(session_id);
    ASSERT(search != session_states_.end());

    auto &ss = search->second;

    if (status == REPLY_OK)
    {
        ExecuteNextOperation(session_id);
    }
    else if (status == REPLY_FAIL)
    {
        ExecuteAbort(session_id, ABORTED_SYSTEM);
    }
    else
    {
        Panic("Unknown status for Get %d.", status);
    }
}

void BenchmarkClientIOCL::GetTimeout(const uint64_t session_id,
                                     int status, const std::string &key)
{
    Warning("[%lu] Get(%s) timed out :(", session_id, key.c_str());
    auto search = session_states_.find(session_id);
    ASSERT(search != session_states_.end());

    auto &ss = search->second;
    auto &session = ss.session();

    auto client_index = ss.current_client_index();
    auto &client = *clients_[client_index];

    auto gcb = std::bind(&BenchmarkClientIOCL::GetCallback, this, session_id, std::placeholders::_1, std::placeholders::_2, std::placeholders::_3, std::placeholders::_4);
    auto gtcb = std::bind(&BenchmarkClientIOCL::GetTimeout, this, session_id, std::placeholders::_1, std::placeholders::_2);

    client.Get(session, key, gcb, gtcb, timeout_);
}

void BenchmarkClientIOCL::PutCallback(const uint64_t session_id, int status,
                                      const std::string &key, const std::string &val)
{
    Debug("[%lu] Put(%s,%s) callback.", session_id, key.c_str(), val.c_str());
    auto search = session_states_.find(session_id);
    ASSERT(search != session_states_.end());

    auto &ss = search->second;

    if (status == REPLY_OK)
    {
        ExecuteNextOperation(session_id);
    }
    else if (status == REPLY_FAIL)
    {
        ExecuteAbort(session_id, ABORTED_SYSTEM);
    }
    else
    {
        Panic("Unknown status for Put %d.", status);
    }
}

void BenchmarkClientIOCL::PutTimeout(const uint64_t session_id, int status,
                                     const std::string &key, const std::string &val)
{
    Warning("[%lu] Put(%s,%s) timed out :(", session_id, key.c_str(), val.c_str());
}

void BenchmarkClientIOCL::ExecuteCallback(uint64_t session_id,
                                          transaction_status_t result)
{
    Debug("[%lu] ExecuteCallback with result %d.", session_id, result);
    auto search = session_states_.find(session_id);
    ASSERT(search != session_states_.end());

    auto &ss = search->second;
    auto transaction = ss.transaction();
    auto &ttype = transaction->GetTransactionType();
    auto n_attempts = ss.n_attempts();

    if (result == COMMITTED || result == ABORTED_USER ||
        (maxAttempts != -1 && n_attempts >= static_cast<uint64_t>(maxAttempts)) ||
        !retryAborted)
    {
        bool erase_session = true;
        if (result == COMMITTED)
        {
            stats.Increment(ttype + "_committed", 1);

            if (!cooldownStarted)
            {
                bool send_next_in_session = false;
                uint64_t next_arrival_us = 0;
                switch (mode_)
                {
                case BenchmarkClientIOCLMode::OPEN:
                    send_next_in_session = stay_dist_(rand_);
                    next_arrival_us = static_cast<uint64_t>(think_time_dist_(rand_));
                    break;

                case BenchmarkClientIOCLMode::CLOSED:
                    send_next_in_session = true;
                    next_arrival_us = 0;
                    break;
                default:
                    Panic("Unexpected client mode!");
                }

                if (send_next_in_session)
                {
                    erase_session = false;
                    Debug("next arrival in session %lu us", next_arrival_us);

                    transport_.TimerMicro(next_arrival_us, std::bind(&BenchmarkClientIOCL::SendNextInSession, this, session_id));
                }
            }
            else
            {
                Debug("end of session");
            }
        }

        if (retryAborted)
        {
            stats.Add(ttype + "_attempts_list", n_attempts);
        }

        OnReply(session_id, result, erase_session);
    }
    else
    {
        stats.Increment(ttype + "_" + std::to_string(result), 1);
        BenchmarkClientIOCL::BenchState state = GetBenchState();
        Debug("Current bench state: %d.", state);
        if (state == DONE)
        {
            OnReply(session_id, ABORTED_SYSTEM, true);
        }
        else
        {
            uint64_t backoff = 0;
            if (abortBackoff > 0)
            {
                uint64_t exp = n_attempts - 1;
                backoff = static_cast<uint64_t>(1000 * 50 * (std::pow(1.3, exp)));
                backoff = std::min(backoff, 1000 * maxBackoff);
                // uint64_t exp = std::min(n_attempts - 1UL, 56UL);
                // Debug("Exp is %lu (min of %lu and 56.", exp, n_attempts - 1UL);
                // uint64_t upper = std::min((1UL << exp) * abortBackoff, maxBackoff);
                // Debug("Upper is %lu (min of %lu and %lu.", upper, (1UL << exp) * abortBackoff,
                //       maxBackoff);
                // backoff = std::uniform_int_distribution<uint64_t>(0UL, upper)(GetRand());
                // stats.Increment(ttype + "_backoff", backoff);
                Debug("Backing off for %lu us: %lu", backoff, n_attempts);
            }

            transport_.TimerMicro(backoff, [this, session_id]
                                  {
                auto search = session_states_.find(session_id);
                ASSERT(search != session_states_.end());

                auto &ss = search->second;
                ss.retry_transaction();

                stats.Increment(ss.transaction()->GetTransactionType() + "_attempts", 1);

                auto bcb = std::bind(&BenchmarkClientIOCL::ExecuteNextOperation, this, session_id);
                auto btcb = []() {};

                auto &client = *clients_[ss.current_client_index()];
                client.Retry(ss.session(), bcb, btcb, timeout_); });
        }
    }
}

void BenchmarkClientIOCL::OnReply(uint64_t transaction_id, int result, bool erase_session)
{
    Debug("[%lu] OnReply with result %d.", transaction_id, result);
    auto search = session_states_.find(transaction_id);
    ASSERT(search != session_states_.end());

    auto &ss = search->second;
    auto transaction = ss.transaction();
    auto lat = ss.lat();

    if (started)
    {
        // record latency
        if (!cooldownStarted)
        {
            _Latency_EndRec(&latency, lat);
            uint64_t ns = lat->accum;
            // TODO: use standard definitions across all clients for
            // success/commit and failure/abort
            if (result == 0)
            { // only record result if success
                struct timespec curr;
                clock_gettime(CLOCK_MONOTONIC, &curr);
                if (latencies.size() == 0UL)
                {
                    gettimeofday(&startMeasureTime, NULL);
                    startMeasureTime.tv_sec -= ns / 1000000000ULL;
                    startMeasureTime.tv_usec -= (ns % 1000000000ULL) / 1000ULL;
                    // std::cout << "#start," << startMeasureTime.tv_sec << ","
                    // << startMeasureTime.tv_usec << std::endl;
                }
                uint64_t currNanos = curr.tv_sec * 1000000000ULL + curr.tv_nsec;
                std::cout << transaction->GetTransactionType() << ',' << ns << ',' << currNanos << ','
                          << client_id_ << std::endl;
                latencies.push_back(ns);
            }
        }

        struct timeval diff;
        BenchState state = GetBenchState(diff);
        if ((state == COOL_DOWN || state == DONE) && !cooldownStarted)
        {
            Debug("Starting cooldown after %ld seconds.", diff.tv_sec);
            Finish();
        }
        else
        {
            Debug("Not done after %ld seconds.", diff.tv_sec);
        }
    }

    delete transaction;

    if (erase_session)
    {
        auto &client = *clients_[ss.current_client_index()];
        client.EndSession(ss.session());
        session_states_.erase(search);
    }

    n++;
}
