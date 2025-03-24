/***********************************************************************
 *
 * store/benchmark/async/bench_client.h:
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
#ifndef OPEN_BENCHMARK_IOCL_CLIENT_H
#define OPEN_BENCHMARK_IOCL_CLIENT_H

#include <functional>
#include <memory>
#include <random>
#include <unordered_map>
#include <vector>

#include "lib/latency.h"
#include "lib/message.h"
#include "lib/transport.h"
#include "store/common/frontend/async_apprequest.h"
#include "store/benchmark/async/bench_client.h"
#include "store/common/frontend/client.h"
#include "store/common/stats.h"
#include "store/common/transaction.h"

typedef std::function<void(transaction_status_t)> execute_callback;

typedef std::function<void()> bench_done_callback;

class BenchmarkClientIOCL : public BenchmarkClient
{
public:
    BenchmarkClientIOCL(const std::vector<Client *> &clients, uint32_t timeout,
                        Transport &transport, uint64_t id,
                        double switch_probability,
                        double arrival_rate, double think_time, double stay_probability,
                        int mpl,
                        int expDuration, int warmupSec, int cooldownSec,
                        uint32_t abortBackoff, bool retryAborted,
                        uint32_t maxBackoff, uint32_t maxAttempts,
                        uint64_t fanout,
                        const std::string &latencyFilename = "");
    virtual ~BenchmarkClientIOCL();

    // void Start(bench_done_callback bdcb);
    void OnReply(uint64_t transaction_id, int result, bool erase_session);

    void SendNext();
    void ExecuteCallback(uint64_t transaction_id, transaction_status_t result);

    inline bool IsFullyDone() { return done; }

    struct Latency_t latency;
    std::vector<uint64_t> latencies;

    inline const Stats &GetStats() const { return stats; }

protected:
    virtual AsyncAppRequest *GetNextAppRequest() = 0;

    inline std::mt19937 &GetRand() { return rand_; }

    enum BenchState
    {
        WARM_UP = 0,
        MEASURE = 1,
        COOL_DOWN = 2,
        DONE = 3
    };
    BenchState GetBenchState(struct timeval &diff) const;
    BenchState GetBenchState() const;

    Stats stats;

private:
    class SessionState
    {
    public:
        SessionState(Session &session, AsyncAppRequest *apprequest, execute_callback ecb, std::size_t client_index, uint64_t fanout)
            : lat_{}, session_{session}, apprequest_{apprequest}, ecb_{ecb}, n_attempts_{1}, op_index_{0}, current_client_index_{client_index}, current_client_txn_count_{0}, fanout_{fanout} {}

        Session &session() { return session_; }
        AsyncAppRequest *apprequest() const { return apprequest_; }
        execute_callback ecb() const { return ecb_; }

        Latency_Frame_t *lat() { return &lat_; }

        uint64_t n_attempts() const { return n_attempts_; }

        uint64_t op_index() const { return op_index_; }
        void incr_op_index() { op_index_++; }

        std::size_t current_client_index() const { return current_client_index_; }

        uint64_t get_fanout() const { return fanout_; };

    private:
        Latency_Frame_t lat_;
        std::reference_wrapper<Session> session_;
        AsyncAppRequest *apprequest_;
        execute_callback ecb_;
        uint64_t n_attempts_;
        std::size_t op_index_;
        std::size_t current_client_index_;
        std::size_t current_client_txn_count_;
        uint64_t fanout_;
    };

    void SendNextInSession(const uint64_t session_id);

    void ExecuteNextOperation(const uint64_t session_id);

    void GetCallback(const uint64_t session_id,
                     int status, const std::string &key, const std::string &val, Timestamp ts);
    void GetTimeout(const uint64_t session_id,
                    int status, const std::string &key);

    void PutCallback(const uint64_t session_id,
                     int status, const std::string &key, const std::string &val);
    void PutTimeout(const uint64_t session_id,
                    int status, const std::string &key, const std::string &val);

    void Finish();
    void WarmupDone();
    void CooldownDone();
    void Cleanup();
    void CleanupContinue();

    std::unordered_map<uint64_t, SessionState> session_states_;

    uint32_t timeout_;
    std::mt19937 rand_;
    std::exponential_distribution<> next_arrival_dist_;
    std::exponential_distribution<> think_time_dist_;
    std::bernoulli_distribution stay_dist_;
    std::bernoulli_distribution switch_dist_;
    int n;
    int n_sessions_started_;
    int mpl_;
    int exp_duration_;
    int warmupSec;
    int cooldownSec;
    struct timeval startTime;
    struct timeval endTime;
    struct timeval startMeasureTime;
    string latencyFilename;
    int msSinceStart;
    bench_done_callback curr_bdcb_;

    uint64_t maxBackoff;
    uint64_t abortBackoff;
    bool retryAborted;
    int64_t maxAttempts;

    bool started;
    bool done;
    bool cooldownStarted;
    uint64_t fanout_;
};

#endif /* OPEN_BENCHMARK_IOCL_CLIENT_H */
