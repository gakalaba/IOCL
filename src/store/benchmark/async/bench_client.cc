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
#include "store/benchmark/async/bench_client.h"
#include <thread>

#include <sys/time.h>
#include <sys/eventfd.h>

#include <algorithm>
#include <sstream>
#include <string>
#include <utility>

#include "lib/latency.h"
#include "lib/message.h"
#include "lib/timeval.h"
#include "lib/transport.h"
#include "store/strongstore/client.h"
#include <fcntl.h>

DEFINE_LATENCY(op);

using request_utils::Value;

BenchmarkClient::BenchmarkClient(const std::vector<Client *> &clients, uint32_t timeout,
                                 Transport &transport, uint64_t id,
                                 BenchmarkClientMode mode,
                                 double switch_probability,
                                 double arrival_rate, double think_time, double stay_probability,
                                 int mpl,
                                 int expDuration, int warmupSec, int cooldownSec,
                                 uint32_t abortBackoff, bool retryAborted,
                                 uint32_t maxBackoff, uint32_t maxAttempts,
                                 uint64_t fanout, bool issueConcurrent,
                                 bool transformed,
                                 const std::string &latencyFilename)
    : transport_(transport),
      session_states_{},
      clients_{clients},
      client_id_{id},
      timeout_{timeout},
      rand_{id},
      next_arrival_dist_{arrival_rate * 1e-6},
      think_time_dist_{1 / think_time * 1e-6},
      stay_dist_{stay_probability},
      switch_dist_{switch_probability},
      mpl_{mpl},
      exp_duration_{expDuration},
      warmupSec{warmupSec},
      cooldownSec{cooldownSec},
      latencyFilename{latencyFilename},
      maxBackoff{maxBackoff},
      abortBackoff{abortBackoff},
      retryAborted{retryAborted},
      maxAttempts{maxAttempts},
      started{false},
      done{false},
      cooldownStarted{false},
      mode_{mode},
      fanout{fanout},
      issueConcurrent{issueConcurrent},
      isTransformed{transformed},
      replies_map_{}
{
    Notice("starting benchclient, issueConcurrent: %d; fanout: %lu", issueConcurrent, fanout);
    if (arrival_rate <= 0)
    {
        Panic("Arrival rate must be (strictly) positive!");
    }

    _Latency_Init(&latency, "txn");
}

BenchmarkClient::~BenchmarkClient()
{
    Debug("session_states_.size(): %lu", session_states_.size());
    auto search = session_states_.find(0);
    if (search == session_states_.end()) {
        //std::cout << "[AwaitAsynchResponse] ERROR: session_id " << 0 << " not found in session_states_!" << std::endl;
    }
    ASSERT(search != session_states_.end());

    auto &ss = search->second;
    auto client_index = ss.current_client_index();
    auto &client = *clients_[client_index];
}

void BenchmarkClient::StartTransformedEventLoop()
{
    Debug("PlsWork being called....");
    transport_.RunTransformed();
}

uint64_t BenchmarkClient::CustomInit()
{

    Debug("[%d] Starting Transformed App Client", n_sessions_started_);
    // std::cout << "Starting transformed app client" << std::endl;
    n_sessions_started_++;

    std::size_t client_index = n_sessions_started_ % clients_.size();
    auto &client = *clients_[client_index];

    auto &session = client.BeginSession();
    auto sid = session.id();

    Debug("session id: %lu", sid);
    // std::cout << "session id created" << std::endl;
    // std::cout.flush();
    // std::cerr << "session id created (stderr)" << std::endl;
    // std::cerr.flush();
    // //std::cout << "[CustomInit] created session" << std::endl;

    // don't need these two -> dummy values to call for emplace
    auto ecb = std::bind(&BenchmarkClient::ExecuteCallback, this, sid, std::placeholders::_1);
    // std::cout << "About to call GetNextAppRequest()" << std::endl;
    // std::cout.flush();
    auto appreq = GetNextAppRequest();
    // std::cout << "GetNextAppRequest() completed" << std::endl;
    // std::cout.flush();
    // don't need
    // stats.Increment(appreq->GetTransactionType() + "_attempts", 1);
    // move to sendAsync function, GetFanout() --> 0
    session_states_.emplace(sid, SessionState{session, appreq, ecb, client_index, GetFanout()});
    // std::cout << "[CustomInit] emplace called" << std::endl;
    // std::cout.flush();

    // auto &ss = session_states_.find(sid)->second;
    // don't need
    //_Latency_StartRec(ss.lat());

    auto bcb = []() {}; // Don't need to jump right into issueing requests, this will be done by the python app
    auto btcb = []() {};
    // remove
    // client.BeginIOCL(session, bcb, btcb, timeout_);
    // Debug("ANJAAAAAA we should be starting the event loop....");
    // Start event loop in a background thread
    // std::thread(transport_.RunTransformed).detach();
    // std::cout << "About to start event loop thread..." << std::endl;
    // std::cout.flush();
    // std::cerr << "About to start event loop thread (stderr)..." << std::endl;
    // std::cerr.flush();
    std::thread(std::bind(&BenchmarkClient::StartTransformedEventLoop, this)).detach();
    // std::cout << "Event loop thread started, about to return sid=" << sid << std::endl;
    // std::cout.flush();
    // std::cerr << "Event loop thread started (stderr), sid=" << sid << std::endl;
    // std::cerr.flush();
    return sid;
}

void BenchmarkClient::Start(bench_done_callback bdcb)
{
    n_sessions_started_ = 0;
    n = 0;
    curr_bdcb_ = bdcb;
    transport_.Timer(warmupSec * 1000, std::bind(&BenchmarkClient::WarmupDone, this));
    gettimeofday(&startTime, NULL);

    if (IsLinearizeable()) {
        transport_.TimerMicro(0, std::bind(&BenchmarkClient::SendNextAppRequest, this));
    } else {
        transport_.TimerMicro(0, std::bind(&BenchmarkClient::SendNext, this));
    }
}

void BenchmarkClient::SendNext()
{
    n_sessions_started_++;
    Debug("[%d] SendNext", n_sessions_started_);

    std::size_t client_index = n_sessions_started_ % clients_.size();
    auto &client = *clients_[client_index];

    auto &session = client.BeginSession();
    auto sid = session.id();

    Debug("session id: %lu", sid);

    auto ecb = std::bind(&BenchmarkClient::ExecuteCallback, this, sid, std::placeholders::_1);
    auto transaction = GetNextTransaction();
    stats.Increment(transaction->GetTransactionType() + "_attempts", 1);

    session_states_.emplace(sid, SessionState{session, transaction, ecb, client_index});

    auto &ss = session_states_.find(sid)->second;
    _Latency_StartRec(ss.lat());

    auto bcb = std::bind(&BenchmarkClient::ExecuteNextOperation, this, sid);
    auto btcb = []() {};

    Operation op = transaction->GetNextOperation(0);
    switch (op.type)
    {
    case BEGIN_RO:
    case BEGIN_RW:
        client.Begin(session, bcb, btcb, timeout_);
        break;

    default:
        NOT_REACHABLE();
    }

    if (!cooldownStarted)
    {
        bool send_next = false;
        uint64_t next_arrival_us = 0;
        switch (mode_)
        {
        case BenchmarkClientMode::OPEN:
            send_next = true;
            next_arrival_us = static_cast<uint64_t>(next_arrival_dist_(rand_));
            break;

        case BenchmarkClientMode::CLOSED:
            send_next = (n_sessions_started_ < mpl_);
            next_arrival_us = 0;
            break;
        default:
            Panic("Unexpected client mode!");
        }

        if (send_next)
        {
            Debug("next arrival in %lu us", next_arrival_us);
            transport_.TimerMicro(next_arrival_us, std::bind(&BenchmarkClient::SendNext, this));
        }
    }
}

void BenchmarkClient::SendNextAppRequest()
{
    n_sessions_started_++;
    Debug("[%d] SendNextAppRequest", n_sessions_started_);

    std::size_t client_index = n_sessions_started_ % clients_.size();
    auto &client = *clients_[client_index];

    auto &session = client.BeginSession();
    auto sid = session.id();

    Debug("session id: %lu", sid);

    auto ecb = std::bind(&BenchmarkClient::ExecuteCallback, this, sid, std::placeholders::_1);
    auto appreq = GetNextAppRequest();
    stats.Increment(appreq->GetTransactionType() + "_attempts", 1);

    session_states_.emplace(sid, SessionState{session, appreq, ecb, client_index, GetFanout()});

    auto &ss = session_states_.find(sid)->second;
    _Latency_StartRec(ss.lat());

    auto bcb = std::bind(&BenchmarkClient::ExecuteNextAppRequestOperation, this, sid);
    auto btcb = []() {};

    client.BeginAppRequest(session, bcb, btcb, timeout_);
}

void BenchmarkClient::SendNextInSession(const uint64_t session_id)
{
    Debug("[%lu] SendNextInSession", session_id);

    auto search = session_states_.find(session_id);
    ASSERT(search != session_states_.end());
    auto &ss = search->second;

    auto ecb = std::bind(&BenchmarkClient::ExecuteCallback, this, session_id, std::placeholders::_1);
    auto transaction = GetNextTransaction();
    stats.Increment(transaction->GetTransactionType() + "_attempts", 1);

    if (switch_dist_(rand_))
    {
        auto cur_client_index = ss.current_client_index();
        std::size_t next_client_index = (cur_client_index + 1) % clients_.size();

        auto &cur_client = *clients_[cur_client_index];
        rss::Session rss_session = cur_client.EndSession(ss.session());

        auto &next_client = *clients_[next_client_index];

        auto &session = next_client.ContinueSession(rss_session);
        ASSERT(session_id == session.id());

        ss.start_transaction(session, transaction, ecb, next_client_index);
    }
    else
    {
        ss.start_transaction(ss.session(), transaction, ecb, ss.current_client_index());
    }

    auto &session = ss.session();
    auto &client = *clients_[ss.current_client_index()];

    _Latency_StartRec(ss.lat());

    auto bcb = std::bind(&BenchmarkClient::ExecuteNextOperation, this, session_id);
    auto btcb = []() {};

    Operation op = transaction->GetNextOperation(0);
    switch (op.type)
    {
    case BEGIN_RW:
    case BEGIN_RO:
        client.Begin(session, bcb, btcb, timeout_);
        break;

    default:
        NOT_REACHABLE();
    }
}

void BenchmarkClient::SendNextAppRequestInSession(const uint64_t session_id)
{
    auto search = session_states_.find(session_id);
    ASSERT(search != session_states_.end());
    auto &ss = search->second;

    auto appreq = GetNextAppRequest();
    stats.Increment(appreq->GetTransactionType() + "_attempts", 1);

    // reset op_index!
    ss.start_apprequest(ss.session(), appreq, ss.current_client_index());

    auto &session = ss.session();
    auto sid = session.id();
    Debug("session id: %lu", sid);

    auto &client = *clients_[ss.current_client_index()];
    _Latency_StartRec(ss.lat());

    auto bcb = std::bind(&BenchmarkClient::ExecuteNextAppRequestOperation, this, sid);
    auto btcb = []() {};

    client.BeginAppRequest(session, bcb, btcb, timeout_);
}

void BenchmarkClient::ExecuteNextOperation(const uint64_t session_id)
{
    Debug("[%lu] ExecuteNextOperation", session_id);
    auto search = session_states_.find(session_id);
    ASSERT(search != session_states_.end());

    auto &ss = search->second;
    auto transaction = ss.transaction();
    auto op_index = ss.op_index();
    auto &session = ss.session();

    Operation op = transaction->GetNextOperation(op_index);
    ss.incr_op_index();
    Debug("Peeking next op");
    Operation peek_next_op = transaction->GetNextOperation(ss.op_index());
    bool nextOpCommit = (peek_next_op.type == COMMIT) || (peek_next_op.type == ROCOMMIT);
    Debug("nextOpCommit = %d", nextOpCommit);

    auto gcb = std::bind(&BenchmarkClient::GetCallback, this, session_id, std::placeholders::_1, std::placeholders::_2, std::placeholders::_3, std::placeholders::_4);
    auto gtcb = std::bind(&BenchmarkClient::GetTimeout, this, session_id, std::placeholders::_1, std::placeholders::_2);
    auto pcb = std::bind(&BenchmarkClient::PutCallback, this, session_id, std::placeholders::_1, std::placeholders::_2, std::placeholders::_3);
    auto ptcb = std::bind(&BenchmarkClient::PutTimeout, this, session_id, std::placeholders::_1, std::placeholders::_2, std::placeholders::_3);
    auto ccb = std::bind(&BenchmarkClient::CommitCallback, this, session_id, std::placeholders::_1);
    auto ctcb = std::bind(&BenchmarkClient::CommitTimeout, this);
    auto acb = std::bind(&BenchmarkClient::AbortCallback, this, session_id, ABORTED_USER);
    auto atcb = std::bind(&BenchmarkClient::AbortTimeout, this);

    auto client_index = ss.current_client_index();
    auto &client = *clients_[client_index];

    switch (op.type)
    {
    case GET:
        client.Get(session, op.key, gcb, gtcb, timeout_);
        break;

    case GET_FOR_UPDATE:
        client.GetForUpdate(session, op.key, gcb, gtcb, timeout_);
        break;

    case PUT:
        client.Put(session, op.key, op.value, pcb, ptcb, timeout_);
        break;

    case COMMIT:
        client.Commit(session, ccb, ctcb, timeout_);
        break;

    case ABORT:
        client.Abort(session, acb, atcb, timeout_);
        break;

    case ROCOMMIT:
        client.ROCommit(session, op.keys, ccb, ctcb, timeout_);
        break;

    case WAIT:
        break;

    default:
        NOT_REACHABLE();
    }

    Debug("isue Concurrent = %d, nextOpCommit %d, op.tpye = %d", issueConcurrent, nextOpCommit, op.type);
    if (issueConcurrent && !nextOpCommit && (op.type == GET || op.type == PUT || op.type == GET_FOR_UPDATE))
    {
        Debug("we're about to issue the next operation within this TRANSACTION without having gotten a response!!!");
        // TODO ANJA should these just be added to the event queue?? or actually issued next
        ExecuteNextOperation(session_id);
    }
    else
    {
        Debug("Not issueing next op from this fn");
    }

}

void BenchmarkClient::ExecuteNextAppRequestOperation(const uint64_t session_id)
{
    Debug("[%lu] ExecuteNextAppRequestOperation", session_id);
    auto search = session_states_.find(session_id);
    ASSERT(search != session_states_.end());

    auto &ss = search->second;
    auto appreq = ss.apprequest();
    auto op_index = ss.op_index();
    auto &session = ss.session();

    // Generic Operation Callback
    auto ocb = std::bind(&BenchmarkClient::ReceiveOperationResponse, this, session_id, std::placeholders::_1, std::placeholders::_2);
    auto otcb = std::bind(&BenchmarkClient::SendOperationTimeout, this, session_id, std::placeholders::_1, std::placeholders::_2);

    auto client_index = ss.current_client_index();
    auto &client = *clients_[client_index];

    Debug("opindex == %lu and ss.fanout() == %lu", op_index, ss.fanout());
    if (op_index == ss.fanout())
    {
        Debug("we've sent fanout number of requests, no longer sending more");
        return;
    }

    Operation op = appreq->GetNextOperation(op_index);
    ss.incr_op_index();
    std::string op_str;

    switch (op.type)
    {
    case GET:
        op_str = "get";
        break;

    case PUT:
        op_str = "put";
        break;

    default:
        Panic("unsupported opeartion type %d", op.type);
    }
    client.SendOperation(session, op_str, op.key, op.value, ocb, otcb, timeout_);

    if (issueConcurrent)
    {
        Debug("we're about to issue the next operation within this app request without having gotten a response!!!");
        // TODO ANJA should these just be added to the event queue?? or actually issued next
        ExecuteNextAppRequestOperation(session_id);
    } else {
        Debug("Not issueing next op from this fn");
    }
}

void BenchmarkClient::ExecuteAbort(const uint64_t session_id, transaction_status_t status)
{
    Debug("[%lu] ExecuteAbort", session_id);
    auto search = session_states_.find(session_id);
    ASSERT(search != session_states_.end());

    auto &ss = search->second;
    auto transaction = ss.transaction();
    auto op_index = ss.op_index();
    auto &session = ss.session();

    auto client_index = ss.current_client_index();
    auto &client = *clients_[client_index];

    auto acb = std::bind(&BenchmarkClient::AbortCallback, this, session_id, status);
    auto atcb = std::bind(&BenchmarkClient::AbortTimeout, this);

    client.Abort(session, acb, atcb, timeout_);
}

void BenchmarkClient::GetCallback(const uint64_t session_id, int status,
                                  const std::string &key, const std::string &val, Timestamp ts)
{
    Debug("[%lu] Get(%s) callback", session_id, key.c_str());
    auto search = session_states_.find(session_id);
    ASSERT(search != session_states_.end());

    auto &ss = search->second;
    ss.incr_responses();
    Debug("fanout = %d and responses = %lu", ss.transaction()->Fanout(), ss.responses());

    if (status == REPLY_OK)
    {
        if ((!issueConcurrent) || (issueConcurrent && (ss.responses() == ss.transaction()->Fanout())))
        {
            ExecuteNextOperation(session_id);
        }
        // ExecuteNextOperation(session_id);
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

void BenchmarkClient::GetTimeout(const uint64_t session_id,
                                 int status, const std::string &key)
{
    Warning("[%lu] Get(%s) timed out :(", session_id, key.c_str());
    auto search = session_states_.find(session_id);
    ASSERT(search != session_states_.end());

    auto &ss = search->second;
    auto &session = ss.session();

    auto client_index = ss.current_client_index();
    auto &client = *clients_[client_index];

    auto gcb = std::bind(&BenchmarkClient::GetCallback, this, session_id, std::placeholders::_1, std::placeholders::_2, std::placeholders::_3, std::placeholders::_4);
    auto gtcb = std::bind(&BenchmarkClient::GetTimeout, this, session_id, std::placeholders::_1, std::placeholders::_2);

    client.Get(session, key, gcb, gtcb, timeout_);
}

void BenchmarkClient::PutCallback(const uint64_t session_id, int status,
                                  const std::string &key, const std::string &val)
{
    Debug("[%lu] Put(%s,%s) callback.", session_id, key.c_str(), val.c_str());
    auto search = session_states_.find(session_id);
    ASSERT(search != session_states_.end());

    auto &ss = search->second;
    ss.incr_responses();
    Debug("fanout = %d and responses = %lu", ss.transaction()->Fanout(), ss.responses());


    if (status == REPLY_OK)
    {
        if ((!issueConcurrent) || (issueConcurrent && (ss.responses() == ss.transaction()->Fanout())))
        {
            ExecuteNextOperation(session_id);
        }
        // ExecuteNextOperation(session_id);
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

void BenchmarkClient::PutTimeout(const uint64_t session_id, int status,
                                 const std::string &key, const std::string &val)
{
    Warning("[%lu] Put(%s,%s) timed out :(", session_id, key.c_str(), val.c_str());
}


void BenchmarkClient::ReceiveOperationResponse(const uint64_t session_id,
                                             int status, const std::string &retval)
{
    Debug("session [%lu] running ReceiveOperationResponse callback in benchclient! status = %d and retval = %s", session_id, status, retval.c_str());
    auto search = session_states_.find(session_id);
    ASSERT(search != session_states_.end());

    auto &ss = search->second;
    ss.incr_responses();
    Debug("current number of responses recieved = %lu, looking for %lu", ss.responses(), ss.fanout());

    if (status == REPLY_OK)
    {
        if (ss.responses() == ss.fanout())
        {
            Debug("we're done with this app request! gonna send a new app request soon");
            auto appreq = ss.apprequest();
            auto &ttype = appreq->GetTransactionType();
            auto n_attempts = ss.n_attempts();

            stats.Increment(ttype + "_completed", 1);

            // Send Next App Request
            if (!cooldownStarted)
            {
                Debug("next arrival in session %d us", 0);
                transport_.TimerMicro(0, std::bind(&BenchmarkClient::SendNextAppRequestInSession, this, session_id));
                OnReply(session_id, 0, false);
            }
            else
            {
                Debug("end of session");
                OnReply(session_id, 0, true);
            }
        }
        else
        {
            if (!issueConcurrent)
            {

                Debug("we're gonna issue the next operation that's a part of this apprequest");
                ExecuteNextAppRequestOperation(session_id);
            }
        }
    }
    else
    {
        Panic("Received RequestResponse but the status wasn't OK! it was %d.", status);
    }
}

void BenchmarkClient::SendOperationTimeout(const uint64_t session_id,
                                         int status, const std::string &retval)
{
    Warning("[%lu] ExecuteNextAppRequestOperation timed out :(", session_id);
}

void BenchmarkClient::CommitCallback(const uint64_t session_id, transaction_status_t status)
{
    Debug("[%lu] Commit callback.", session_id);
    auto search = session_states_.find(session_id);
    ASSERT(search != session_states_.end());

    auto &ss = search->second;
    auto ecb = ss.ecb();

    ecb(status);
}

void BenchmarkClient::CommitTimeout()
{
    Warning("Commit timed out :(");
}

void BenchmarkClient::AbortCallback(const uint64_t session_id, transaction_status_t status)
{
    Debug("[%lu] Abort callback.", session_id);
    auto search = session_states_.find(session_id);
    ASSERT(search != session_states_.end());

    auto &ss = search->second;
    auto ecb = ss.ecb();

    ecb(status);
}

void BenchmarkClient::AbortTimeout()
{
    Warning("Abort timed out :(");
}

void BenchmarkClient::ExecuteCallback(uint64_t session_id,
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
                case BenchmarkClientMode::OPEN:
                    send_next_in_session = stay_dist_(rand_);
                    next_arrival_us = static_cast<uint64_t>(think_time_dist_(rand_));
                    break;

                case BenchmarkClientMode::CLOSED:
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

                    transport_.TimerMicro(next_arrival_us, std::bind(&BenchmarkClient::SendNextInSession, this, session_id));
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
        BenchmarkClient::BenchState state = GetBenchState();
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

                auto bcb = std::bind(&BenchmarkClient::ExecuteNextOperation, this, session_id);
                auto btcb = []() {};

                auto &client = *clients_[ss.current_client_index()];
                client.Retry(ss.session(), bcb, btcb, timeout_); });
        }
    }
}

void BenchmarkClient::WarmupDone()
{
    started = true;
    Notice("Completed warmup period of %d seconds with %d requests", warmupSec, n);
    n = 0;
}

void BenchmarkClient::CleanupContinue()
{
    auto n = session_states_.size();
    Notice("Waiting for %lu outstanding transactions.", n);

    if (n > 0)
    {
        transport_.TimerMicro(1e6, std::bind(&BenchmarkClient::CleanupContinue, this));
    }
    else
    {
        CooldownDone();
    }
}

void BenchmarkClient::Cleanup()
{
    auto n = session_states_.size();
    Notice("Aborting %lu outstanding transactions.", n);

    if (n > 0)
    {
        for (auto &kv : session_states_)
        {
            auto transaction_id = kv.first;
            auto &ss = kv.second;

            auto op_index = ss.op_index();

            auto client_index = ss.current_client_index();
            auto &client = *clients_[client_index];

            client.ForceAbort(transaction_id);
        }

        transport_.TimerMicro(1e6, std::bind(&BenchmarkClient::CleanupContinue, this));
    }
    else
    {
        CooldownDone();
    }
}

void BenchmarkClient::CooldownDone()
{
    done = true;

    char buf[1024];
    Notice("Finished cooldown period.");
    std::sort(latencies.begin(), latencies.end());

    if (latencies.size() > 0)
    {
        uint64_t ns = latencies[latencies.size() / 2];
        LatencyFmtNS(ns, buf);
        Notice("Median latency is %ld ns (%s)", ns, buf);

        ns = 0;
        for (auto latency : latencies)
        {
            ns += latency;
        }
        ns = ns / latencies.size();
        LatencyFmtNS(ns, buf);
        Notice("Average latency is %ld ns (%s)", ns, buf);

        ns = latencies[latencies.size() * 90 / 100];
        LatencyFmtNS(ns, buf);
        Notice("90th percentile latency is %ld ns (%s)", ns, buf);

        ns = latencies[latencies.size() * 95 / 100];
        LatencyFmtNS(ns, buf);
        Notice("95th percentile latency is %ld ns (%s)", ns, buf);

        ns = latencies[latencies.size() * 99 / 100];
        LatencyFmtNS(ns, buf);
        Notice("99th percentile latency is %ld ns (%s)", ns, buf);
    }
    curr_bdcb_();
}

void BenchmarkClient::OnReply(uint64_t transaction_id, int result, bool erase_session)
{
    Debug("[%lu] OnReply with result %d.", transaction_id, result);
    auto search = session_states_.find(transaction_id);
    ASSERT(search != session_states_.end());

    auto &ss = search->second;
    auto transaction = ss.transaction();
    auto appreq = ss.apprequest();
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
                if (transaction != NULL) {
                    std::cout << transaction->GetTransactionType() << ',' << ns << ',' << currNanos << ','
                          << client_id_ << std::endl;
                } else {
                    std::cout << appreq->GetTransactionType() << ',' << ns << ',' << currNanos << ','
                              << client_id_ << std::endl;
                }
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
    delete appreq;

    if (erase_session)
    {
        auto &client = *clients_[ss.current_client_index()];
        client.EndSession(ss.session());
        session_states_.erase(search);
    }

    n++;
}

BenchmarkClient::BenchState BenchmarkClient::GetBenchState(struct timeval &diff) const
{
    struct timeval currTime;
    gettimeofday(&currTime, NULL);

    diff = timeval_sub(currTime, startTime);
    if (diff.tv_sec > exp_duration_)
    {
        return DONE;
    }
    else if (diff.tv_sec > exp_duration_ - warmupSec)
    {
        return COOL_DOWN;
    }
    else if (started)
    {
        return MEASURE;
    }
    else
    {
        return WARM_UP;
    }
}

BenchmarkClient::BenchState BenchmarkClient::GetBenchState() const
{
    struct timeval diff;
    return GetBenchState(diff);
}

void BenchmarkClient::Finish()
{
    gettimeofday(&endTime, NULL);
    struct timeval diff = timeval_sub(endTime, startMeasureTime);

    std::cout << "#end," << diff.tv_sec << "," << diff.tv_usec << "," << client_id_
              << std::endl;

    Notice("Completed %d requests in " FMT_TIMEVAL_DIFF " seconds", n,
           VA_TIMEVAL_DIFF(diff));
    Notice("%lu outstanding transactions.", session_states_.size());

    if (latencyFilename.size() > 0)
    {
        Latency_FlushTo(latencyFilename.c_str());
    }

    cooldownStarted = true;

    uint64_t cooldown_us = cooldownSec * 1e6;
    transport_.TimerMicro(cooldown_us, std::bind(&BenchmarkClient::Cleanup, this));
}

// Transformed IOCL Apps!!
std::tuple<bool, Value> BenchmarkClient::SendAsynchOperation(const uint64_t session_id, request_utils::Operation opType, int64_t key, Value newValue, Value oldValue, bool singleton)
{
    // //std::cout << "[SendAsynchRequest] Called with session_id=" << session_id
    //           << ", opType=" << static_cast<int>(opType)
    //           << ", key=" << key << std::endl;

    Debug("SendAsynchOperation");
    auto search = session_states_.find(session_id);
    if (search == session_states_.end()) {
        std::cout << "@!@!@!@!@!!@!@!@!@!@!@!@[SendAsynchOperation] ERROR: session_id " << session_id << " not found in session_states_!" << std::endl;
    }
    ASSERT(search != session_states_.end());

    auto &ss = search->second;
    auto &session = ss.session();

    auto rcb = std::bind(&BenchmarkClient::AsynchOperationCallback, this, session_id, std::placeholders::_1, std::placeholders::_2, std::placeholders::_3);

    auto client_index = ss.current_client_index();
    auto &client = *clients_[client_index];

    // //std::cout << "[SendAsynchRequest] About to dispatch operation..." << std::endl;

    switch (opType)
    {
    case request_utils::Operation::GET:
        // //std::cout << "[SendAsynchRequest] Operation: GET" << std::endl;
        break;

    case request_utils::Operation::PUT:
        // //std::cout << "[SendAsynchRequest] Operation: PUT" << std::endl;
        break;

    case request_utils::Operation::INCR:
        // //std::cout << "[SendAsynchRequest] Operation: INCR" << std::endl;
        break;

    case request_utils::Operation::SET:
        // //std::cout << "[SendAsynchRequest] Operation: SET" << std::endl;
        break;

    case request_utils::Operation::SADD:
        // //std::cout << "[SendAsynchRequest] Operation: SADD" << std::endl;
        break;

    case request_utils::Operation::EXISTS:
        // //std::cout << "[SendAsynchRequest] Operation: EXISTS" << std::endl;
        break;

    case request_utils::Operation::HMGET:
        // //std::cout << "[SendAsynchRequest] Operation: HMGET" << std::endl;
        break;

    case request_utils::Operation::HSET:
        // //std::cout << "[SendAsynchRequest] Operation: HSET" << std::endl;
        break;

    case request_utils::Operation::HMSET:
        // //std::cout << "[SendAsynchRequest] Operation: HMSET" << std::endl;
        break;

    case request_utils::Operation::HGETALL:
        // //std::cout << "[SendAsynchRequest] Operation: HGETALL" << std::endl;
        break;

    case request_utils::Operation::ZADD:
        // //std::cout << "[SendAsynchRequest] Operation: ZADD" << std::endl;
        break;

    case request_utils::Operation::ZINCRBY:
        // //std::cout << "[SendAsynchRequest] Operation: ZINCRBY" << std::endl;
        break;

    case request_utils::Operation::ZSCORE:
        // //std::cout << "[SendAsynchRequest] Operation: ZSCORE" << std::endl;
        break;

    case request_utils::Operation::ZRANGE:
        // //std::cout << "[SendAsynchRequest] Operation: ZRANGE" << std::endl;
        break;

    case request_utils::Operation::ZREVRANGE:
        // //std::cout << "[SendAsynchRequest] Operation: ZREVRANGE" << std::endl;
        break;

    case request_utils::Operation::ZAPPEND:
        // //std::cout << "[SendAsynchRequest] Operation: ZAPPEND" << std::endl;
        break;

    default:
        //std::cout << "[SendAsynchRequest] ERROR: Unsupported operation type " << static_cast<int>(opType) << std::endl;
        Panic("NOT YET SUPPORTEDunsupported operation type");
    }
    auto commandId = client.SendAsynchOperation(session, opType, key, newValue, oldValue, rcb, singleton);
    
    return std::make_tuple(true, Value(std::to_string(commandId)));
}

void BenchmarkClient::AsynchOperationCallback(const uint64_t session_id, int status, const request_utils::Value retval, uint64_t commandId)
{
   // std::cerr << "[AsynchRequestCallback] Called with commandId=" << commandId << std::endl;

    int efd_to_signal = -1;

    {
        std::lock_guard<std::mutex> lock(replies_mutex_);

        if (replies_map_.find(commandId) != replies_map_.end())
        {
            std::cerr << "[AsynchRequestCallback] WARNING: Duplicate response for commandId=" << commandId << std::endl;
        }
        replies_map_[commandId] = retval;
        Debug("And the replies map size is %lu", replies_map_.size());
        Debug("It looks like ");
        for (auto const& pair : replies_map_) {
            Debug("commandId %d is in the replies map", pair.first);
        }

        auto efd_it = efd_map_.find(commandId);
        if (efd_it != efd_map_.end()) {
            efd_to_signal = efd_it->second;
            // std::cerr << "[AsynchRequestCallback] Found efd=" << efd_to_signal << " for commandId=" << commandId << std::endl;
            efd_map_.erase(efd_it);
        } else {
           // std::cerr << "[AsynchRequestCallback] No efd found for commandId=, but we have added the reply to the replies map!!" << commandId << std::endl;
        }
    }  // Lock released here

    // Signal the efd outside the lock to avoid holding lock during I/O
    if (efd_to_signal != -1) {
        // Verify the efd is still valid
        int flags = fcntl(efd_to_signal, F_GETFD);
        if (flags == -1) {
            std::cerr << "[AsynchRequestCallback] WARNING: efd " << efd_to_signal << " is no longer valid!" << std::endl;
            return;
        }

        uint64_t val = 1;
        ssize_t written = write(efd_to_signal, &val, sizeof(val));
        if (written != sizeof(val)) {
            std::cerr << "[AsynchRequestCallback] WARNING: Failed to write to efd " << efd_to_signal << std::endl;
        }
    }
}

std::tuple<Value, uint64_t> BenchmarkClient::AwaitAsynchResponse(const uint64_t session_id, uint64_t commandId)
{
    // Debug("Called AwaitAsynchResponse!");

    // std::cout << "[AwaitAsynchResponse] Called with session_id=" << session_id
    //           << ", commandId=" << commandId << std::endl;

    std::lock_guard<std::mutex> lock(replies_mutex_);

    // TODO need to increment the request id!!
    if (replies_map_.find(commandId) != replies_map_.end())
    {
        Debug("Got a response!");
        // std::cout << "[AwaitAsynchResponse] Got a response for commandId=" << commandId << std::endl;

        auto search = session_states_.find(session_id);
        if (search == session_states_.end()) {
            std::cout << "[AwaitAsynchResponse] ERROR: session_id " << session_id << " not found in session_states_!" << std::endl;
        }
        ASSERT(search != session_states_.end());

        auto &ss = search->second;
        // TODO ANJA somehwere in here we need to increment the transaction id!!
        // std::cout << "[AwaitAsynchResponse] Returning value for commandId=" << commandId << std::endl;

        // right now we don't delete the value... for the purposes of double await? TODO ANJA see with austin
        return std::make_tuple(replies_map_[commandId], -1);
    }
    // Debug("response not available!");
    // //std::cout << "[AwaitAsynchResponse] No response yet for commandId=" << commandId << ", creating efd..." << std::endl;

    // Create the event file descriptor WHILE HOLDING THE LOCK
    // This prevents the callback from running between the check above and efd creation
    int efd = eventfd(0, EFD_CLOEXEC);
    if (efd == -1) {
        std::cout << "[AwaitAsynchResponse] Event EFD creation failed" << std::endl;
        Panic("eventfd creation failed");
    }
    // Debug("making an efd! it has value %d", efd);

    // Log the created efd and the commandId it maps to
    // std::cout << "[AwaitAsynchResponse] Created efd=" << efd << " for commandId=" << commandId << std::endl;

    // Map the efd to the commandId WHILE HOLDING THE LOCK
    efd_map_[commandId] = efd;
    Debug("making an efd! it has value %d and is mapped to commandId %d", efd, commandId);
    Debug("the side of the efd_map_ is %lu", efd_map_.size());
    // std::cout << "[AwaitAsynchResponse] Mapped efd=" << efd << " to commandId=" << commandId << std::endl;
    Debug("And the replies map size is %lu", replies_map_.size());
    Debug("It looks like ");
    for (auto const& pair : replies_map_) {
        Debug("commandId %d is in the replies map", pair.first);
    }

    // Return the Value object and the efd
    // Lock is released here - now callback can safely find and signal the efd
    return std::make_tuple(Value{}, efd);
}
