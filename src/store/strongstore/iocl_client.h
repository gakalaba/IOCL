/***********************************************************************
 *
 * store/strongstore/iocl_client.h:
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
#ifndef _IOCL_CLIENT_H_
#define _IOCL_CLIENT_H_

#include <bitset>
#include <memory>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>

#include "lib/assert.h"
#include "lib/configuration.h"
#include "lib/latency.h"
#include "lib/message.h"
#include "lib/udptransport.h"
#include "replication/vr/client.h"
#include "store/common/frontend/client.h"
#include "store/common/partitioner.h"
#include "store/common/truetime.h"
#include "store/strongstore/common.h"
#include "store/strongstore/networkconfig.h"
#include "store/strongstore/preparedtransaction.h"
#include "store/strongstore/replicaclient.h"
#include "store/strongstore/strong-proto.pb.h"

namespace strongstore
{

    class StrongSession : public ::Session
    {
    public:
        StrongSession()
            : ::Session(), apprequest_id_{static_cast<uint64_t>(-1)}, start_ts_{0, 0}, values_{}, state_{EXECUTING} {}

        uint64_t apprequest_id() const { return apprequest_id_; }
        const Timestamp &start_ts() const { return start_ts_; }

    protected:
        // friend class Client;
        // friend class IOCLClient;

        void start_transaction(uint64_t apprequest_id, const Timestamp &start_ts)
        {
            apprequest_id_ = apprequest_id;
            start_ts_ = start_ts;
            values_.clear();
            state_ = EXECUTING;
        }

        void retry_transaction(uint64_t apprequest_id)
        {
            apprequest_id_ = apprequest_id;
            values_.clear();
            state_ = EXECUTING;
        }

        enum State
        {
            EXECUTING = 0,
            GETTING,
            PUTTING,
            COMMITTING,
            NEEDS_ABORT,
            ABORTING
        };

        State state() const { return state_; }

        bool executing() const { return (state_ == EXECUTING); }
        bool needs_aborts() const { return (state_ == NEEDS_ABORT); }

        void set_committing() { state_ = COMMITTING; }
        void set_needs_abort() { state_ = NEEDS_ABORT; }
        void set_aborting() { state_ = ABORTING; }

        std::unordered_map<std::string, std::list<Value>> &mutable_values() { return values_; }

    private:
        uint64_t apprequest_id_;
        Timestamp start_ts_;
        std::unordered_map<std::string, std::list<Value>> values_;
        State state_;
    };

    class IOCLClient : public ::Client
    {
    public:
        IOCLClient(Consistency consistency, const NetworkConfiguration &net_config,
                   const std::string &client_region, transport::Configuration &config,
                   uint64_t id, int nshards, int closestReplic, Transport *transport,
                   Partitioner *part, TrueTime &tt, bool debug_stats,
                   double nb_time_alpha);
        virtual ~IOCLClient();

        virtual Session &BeginSession() override;
        virtual Session &ContinueSession(rss::Session &session) override;
        virtual rss::Session EndSession(Session &session) override;

        // Overriding functions from ::Client
        // Begin a request
        virtual void Begin(Session &session, begin_callback bcb);

        // Begin a retried transaction.
        virtual void Retry(Session &session, begin_callback bcb,
                           begin_timeout_callback btcb, uint32_t timeout) override;

        // Get the value corresponding to key.
        virtual void Get(Session &session, const std::string &key,
                         get_callback gcb, get_timeout_callback gtcb,
                         uint32_t timeout = GET_TIMEOUT) override;

        // Set the value for the given key.
        virtual void Put(Session &session, const std::string &key, const std::string &value,
                         put_callback pcb, put_timeout_callback ptcb,
                         uint32_t timeout = PUT_TIMEOUT) override;

    private:
        const static std::size_t MAX_SHARDS = 16;

        struct PendingRequest
        {
            PendingRequest(uint64_t id)
                : id(id), outstandingPrepares(0) {}

            ~PendingRequest() {}

            commit_callback ccb;
            commit_timeout_callback ctcb;
            abort_callback acb;
            abort_timeout_callback atcb;
            uint64_t id;
            int outstandingPrepares;
        };

        void ContinueRetry(Session &session, begin_callback bcb);

        // choose coordinator from participants
        void CalculateCoordinatorChoices();
        int ChooseCoordinator(StrongSession &session);

        std::unordered_map<std::bitset<MAX_SHARDS>, int> coord_choices_;
        std::unordered_map<std::bitset<MAX_SHARDS>, uint16_t> min_lats_;

        std::unordered_map<uint64_t, StrongSession> sessions_;
        std::unordered_map<uint64_t, StrongSession &> sessions_by_apprequest_id_;
        std::unordered_map<uint64_t, Timestamp> tmins_;

        const strongstore::NetworkConfiguration &net_config_;
        const std::string client_region_;

        const std::string service_name_;

        transport::Configuration &config_;

        // Unique ID for this client.
        uint64_t client_id_;

        // Number of shards in SpanStore.
        uint64_t nshards_;

        // Transport used by paxos client proxies.
        Transport *transport_;

        // Client for each shard.
        std::vector<ReplicaClient *> rclients_;

        // Partitioner
        Partitioner *part_;

        // TrueTime server.
        TrueTime &tt_;

        uint64_t next_apprequest_id_;

        uint64_t last_req_id_;
        std::unordered_map<uint64_t, PendingRequest *> pending_reqs_;

        Latency_t op_lat_;
        Latency_t commit_lat_;

        Consistency consistency_;

        double nb_time_alpha_;

        bool debug_stats_;
    };

} // namespace strongstore

#endif /* _IOCL_CLIENT_H_ */
