// -*- mode: c++; c-file-style: "k&r"; c-basic-offset: 4 -*-
/***********************************************************************
 *
 * store/strongstore/server.cc:
 *   A single transactional server replica.
 *
 * Copyright 2022 Jeffrey Helt, Matthew Burke, Amit Levy, Wyatt Lloyd
 * Copyright 2015 Irene Zhang <iyzhang@cs.washington.edu>
 *                Naveen Kr. Sharma <naveenks@cs.washington.edu>
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
#include "store/strongstore/server.h"

#include <algorithm>
#include <functional>
#include <memory>
#include <unordered_set>

namespace strongstore
{

    using namespace std;
    using namespace proto;
    using namespace replication;
    TrueTime Server::dummyTT{};

    Server::Server(Consistency consistency,
                   const transport::Configuration &shard_config,
                   const transport::Configuration &replica_config,
                   uint64_t server_id, int shard_idx, int replica_idx,
                   Transport *transport, const TrueTime &tt, bool debug_stats)
        : PingServer(transport),
          tt_{tt},
          transactions_{shard_idx, consistency, tt_},
          shard_config_{shard_config},
          replica_config_{replica_config},
          transport_{transport},
          server_id_{server_id},
          min_prepare_timestamp_{},
          shard_idx_{shard_idx},
          replica_idx_{replica_idx},
          consistency_{consistency},
          debug_stats_{debug_stats}
    {
        transport_->Register(this, shard_config_, shard_idx_, replica_idx_);

        auto pokcb = std::bind(&Server::PrepareOKCallback, this, std::placeholders::_1, std::placeholders::_2, std::placeholders::_3);
        for (int i = 0; i < shard_config_.g; i++)
        {
            // passing in dummy fanout for now, since not used by these shard clients
            shard_clients_.push_back(new ShardClient(shard_config_, transport, server_id_, i, 0, [](uint64_t) {}, pokcb));
        }

        if (debug_stats_)
        {
            Panic("Debug stats disabled!");
            _Latency_Init(&ro_wait_lat_, "ro_wait_lat");
        }
        int N = 30000;
        get_slots_.resize(N);
        free_get_slots_.reserve(N);
        for (uint32_t i = 0; i < N; i++) {
            free_get_slots_.push_back(N - 1 - i);
        }
        rw_commit_c_slots_ = new SlotPool<PendingRWCommitCoordinatorReplySlot>(N);
        rw_commit_p_slots_ = new SlotPool<PendingRWCommitParticipantReplySlot>(N);
        prepare_ok_slots_ = new SlotPool<PendingPrepareOKReplySlot>(N);
        // Debug event loop delay
        // expected_fire_us = 0;
        // transport_->TimerMicro(1000, std::bind(&Server::DelayOnEventLoop, this));
    }

    Server::Server(Consistency consistency, const transport::Configuration &shard_config,
                   const transport::Configuration &replica_config,
                   uint64_t server_id, int shard_idx, int replica_idx,
                   Transport *transport, LinearizableProtocol linproto, bool debug_stats)
        : PingServer(transport),
          tt_{dummyTT},                 // filler, will not use
          transactions_{0, SS, tt_}, // filler, will not use
          shard_config_{shard_config},
          replica_config_{replica_config},
          transport_{transport},
          server_id_{server_id},
          min_prepare_timestamp_{},
          shard_idx_{shard_idx},
          replica_idx_{replica_idx},
          debug_stats_{debug_stats},
          consistency_{consistency}
    {
        transport_->Register(this, shard_config_, shard_idx_, replica_idx_);

        /*for (int i = 0; i < shard_config_.g; i++)
        {
            shard_clients_.push_back(new ShardClient(shard_config_, transport, server_id_, i));
        }*/
        Debug("okayyyy starting up!");

        if (debug_stats_)
        {
            Panic("Debug stats disabled!");
            _Latency_Init(&ro_wait_lat_, "ro_wait_lat");
        }
        op_slots_ = new SlotPool<PendingOpReplySlot>(30000);
        // Debug event loop delay
        // expected_fire_us = 0;
        // transport_->TimerMicro(1000, std::bind(&Server::DelayOnEventLoop, this));
    }

    Server::~Server()
    {
        for (auto s : shard_clients_)
        {
            delete s;
        }

        if (debug_stats_)
        {
            Latency_Dump(&ro_wait_lat_);
        }

        delete op_slots_;
        delete rw_commit_c_slots_;
        delete rw_commit_p_slots_;
        delete prepare_ok_slots_;
    }

    void Server::SeeAllTxns()
    {
        transactions_.ShowAllTxns();
    }

    void Server::PrintAFewThings() {
        Notice("Printing a few things...");
        if (op_slots_) {
            Notice("size of _op_slots is %lu", op_slots_->Size());
        }
        if (rw_commit_c_slots_) {
            Notice("rw_commit_c_slots_ is %lu", rw_commit_c_slots_->Size());
        }
        if (rw_commit_p_slots_) {
            Notice("rw_commit_p_slots_ is %lu", rw_commit_p_slots_->Size());
        }
        if (prepare_ok_slots_) {
            Notice("prepare_ok_slots_ is %lu", prepare_ok_slots_->Size());
        }
        Notice("get_slots_ size is %lu", transaction_id_to_get_slots_.size());
    }


    void Server::DelayOnEventLoop()
    {
        uint64_t now = now_us();
        if (expected_fire_us != 0) {
            int64_t delay = (int64_t)now - (int64_t)expected_fire_us;
            if (delay > 0) {
                Notice("Server %d/%d event loop delay detected! HEARTBEAT delay_us=%ld", shard_idx_, replica_idx_, delay);
            } else {
                Notice("Negative delay?? Server %d/%d event loop ahead by %ld us", shard_idx_, replica_idx_, -delay);
            }
        }
        expected_fire_us = now + 1000;
        transport_->TimerMicro(1000, std::bind(&Server::DelayOnEventLoop, this));
    }

    void Server::Close()
    {
    }

    void Server::SetReplica(replication::Replica *replica)
    {
        this->replica_ = replica;
    }

    void Server::ReceiveMessage(const TransportAddress &remote,
                                const std::string &type, const std::string &data,
                                void *meta_data)
    {
        Panic("Don't use this version of ReceiveMessage");
    }
    void Server::ReceiveMessage(const TransportAddress &remote,
                                MsgType type, const std::string &data,
                                void *meta_data)
    {
        Debug("hi! we're in Server::ReceiveMessage, and we got a message of type %u", (uint32_t)type);
        switch (type) {
        case MsgType::GET_TYPE: {
            get_.ParseFromString(data);
            HandleGet(remote, get_);
            break;
        }
        case MsgType::LIN_OP_TYPE: {
            op_.ParseFromString(data);
            HandleSendOperation(remote, op_);
            break;
        }
        case MsgType::CLIENT_COORD_TYPE: {
            SuccessorRequestMessage succ;
            succ.ParseFromString(data);
            HandleClientCoordination(succ);
            break;
        }
        case MsgType::TXN_COMMIT_TYPE: {
            rw_commit_c_.ParseFromString(data);
            HandleRWCommitCoordinator(remote, rw_commit_c_);
            break;
        }
        case MsgType::TXN_COMMIT_PART_TYPE: {
            rw_commit_p_.ParseFromString(data);
            HandleRWCommitParticipant(remote, rw_commit_p_);
            break;
        }
        case MsgType::TXN_PREPARE_OK_TYPE: {
            prepare_ok_.ParseFromString(data);
            HandlePrepareOK(remote, prepare_ok_);
            break;
        }
        case MsgType::TXN_ABORT_TYPE: {
            abort_.ParseFromString(data);
            HandleAbort(remote, abort_);
            break;
        }
        case MsgType::TXN_WOUND_TYPE: {
            wound_.ParseFromString(data);
            HandleWound(remote, wound_);
            break;
        }
        /*
        else if (type == prepare_abort_.GetTypeName())
        {
            prepare_abort_.ParseFromString(data);
            HandlePrepareAbort(remote, prepare_abort_);
        }
        else if (type == ro_commit_.GetTypeName())
        {
            ro_commit_.ParseFromString(data);
            HandleROCommit(remote, ro_commit_);
        }
        else if (type == ping_.GetTypeName())
        {
            ping_.ParseFromString(data);
            HandlePingMessage(this, remote, ping_);
        }
        */
        default:
            Panic("Received unexpected message type: %u", (uint32_t)type);
        }
    }

    void Server::HandleGet(const TransportAddress &remote, proto::Get &msg)
    {
        Debug("getting Get with req_id = %lu", msg.rid().client_req_id());
        uint64_t transaction_id = msg.transaction_id();
        const std::string &key = msg.key();
        const Timestamp timestamp{msg.timestamp()};
        bool for_update = msg.has_for_update() && msg.for_update();

        Debug("[%lu] Received GET request: %s %d and from clientid %lu", transaction_id, key.c_str(), for_update, msg.rid().client_id());

        size_t get_idx = transactions_.StartGet(transaction_id, remote, key, for_update);

        LockAcquireResult r;
        if (for_update)
        {
            r = locks_.AcquireReadWriteLock(transaction_id, timestamp, key);
        }
        else
        {
            r = locks_.AcquireReadLock(transaction_id, timestamp, key);
        }

        if (r.status == LockStatus::ACQUIRED)
        {
            ASSERT(r.wound_rws.size() == 0);

            std::pair<TimestampID, std::string> value;
            // read the value from the store! this doesn't need to be replicated
            ASSERT(store_.get(key, value));

            get_reply_.Clear();
            get_reply_.mutable_rid()->CopyFrom(msg.rid());
            get_reply_.set_status(REPLY_OK);

            get_reply_.set_val(value.second);
            value.first.timestamp.serialize(get_reply_.mutable_timestamp());

            // respond back to the client (shard client)
            transport_->SendMessage(this, remote, MsgType::GET_REPLY_TYPE, get_reply_);

            transactions_.FinishGet(transaction_id, get_idx);
        }
        else if (r.status == LockStatus::FAIL)
        {
            Panic("Don't think we should be able to enter this case without WaitDie implemented??");
            ASSERT(r.wound_rws.size() == 0);

            get_reply_.Clear();
            get_reply_.mutable_rid()->CopyFrom(msg.rid());
            get_reply_.set_status(REPLY_FAIL);

            transport_->SendMessage(this, remote, MsgType::GET_REPLY_TYPE, get_reply_);

            const Transaction &transaction = transactions_.GetTransaction(transaction_id);

            LockReleaseResult rr = locks_.ReleaseLocks(transaction_id, transaction);
            transactions_.AbortGet(transaction_id, key);

            NotifyPendingRWs(transaction_id, rr.notify_rws);
        }
        else if (r.status == LockStatus::WAITING)
        {
            Debug("Going to wait");
            // Grab an idx
            ASSERT(!free_get_slots_.empty());
            uint32_t idx = free_get_slots_.back();
            free_get_slots_.pop_back();
            // Set fields
            PendingGetReplySlot &reply = get_slots_[idx];
            ASSERT(!reply.in_use);
            reply.in_use = true;
            reply.remote = &remote;
            reply.client_id = msg.rid().client_id();
            reply.client_req_id = msg.rid().client_req_id();
            reply.key = key;
            reply.get_idx = get_idx;
            // Add to map from transaction_id to pending get slots
            transaction_id_to_get_slots_[transaction_id].push_back(idx);

            transactions_.PauseGet(transaction_id, get_idx);

            WoundPendingRWs(transaction_id, r.wound_rws);
        }
        else
        {
            NOT_REACHABLE();
        }
    }

    void Server::HandleSendOperation(const TransportAddress &remote, replication::LinearizeableOperation &msg)
    {
        // Debug("Calling HandleSendOperation! with msg.op = %s, msg.key = %s, msg.value = %s", msg.op().c_str(), msg.key().c_str(), msg.value().c_str());
        Debug("Calling HandleSendOperation! with msg.req_id = %lu", msg.rid().client_req_id());

        // Grab an idx
        uint32_t idx = op_slots_->Alloc();
        PendingOpReplySlot &reply = op_slots_->GetByIdx(idx);
        reply.remote = &remote;
        msg.mutable_kv()->set_idx(idx);

        transport_->TimerMicro(0, [this, m = std::move(msg)]() mutable {
            this->replica_->HandleRequest(m);
        });
    }

    void Server::HandleClientCoordination(replication::SuccessorRequestMessage &msg)
    {
        transport_->TimerMicro(0, [this, m = std::move(msg)]() mutable {
            this->replica_->HandleCoordination(m);
        });
    }

    void Server::ContinueGetAbort(uint64_t transaction_id)
    {
        Debug("[%lu]in continueGetAbort!", transaction_id);
        auto search = transaction_id_to_get_slots_.find(transaction_id);
        if (search == transaction_id_to_get_slots_.end())
        {
            Debug("[%lu] No pending GET requests to abort", transaction_id);
            return;
        }

        auto &idxs_vector = search->second;

        for (uint32_t idx : idxs_vector) {
            auto &reply = get_slots_[idx];
            ASSERT(reply.in_use);

            // Debug("[%lu] Aborting GET request", transaction_id);

            get_reply_.Clear();
            get_reply_.mutable_rid()->set_client_id(reply.client_id);
            get_reply_.mutable_rid()->set_client_req_id(reply.client_req_id);
            get_reply_.set_status(REPLY_FAIL);
            // ANJATODO any other logic that is for locks!?!?!?

            TransactionState s = transactions_.ContinueGet(transaction_id, reply.get_idx);
            ASSERT(s == ABORTED);

            transport_->SendMessage(this, *reply.remote, MsgType::GET_REPLY_TYPE, get_reply_);

            reply.in_use = false;
            reply.remote = nullptr;
            reply.key.clear();
            free_get_slots_.push_back(idx);
        }
        transaction_id_to_get_slots_.erase(search);
    }

    void Server::ContinueGet(uint64_t transaction_id, const std::vector<std::pair<std::string, std::string>> & prevHolderWriteSet)
    {
        Debug("[%lu]in continueGet!", transaction_id);
        // Have a set of keys from the holder Transaction, need to see which ones 
        // overlap with the set of keys from the transaction_id we want to carry on with
        auto search = transaction_id_to_get_slots_.find(transaction_id);
        if (search == transaction_id_to_get_slots_.end())
        {
            Debug("[%lu] No pending GET requests to continue", transaction_id);
            return;
        }

        auto &idxs_vector = search->second;
        for (size_t i = 0; i < idxs_vector.size(); ) {
            uint32_t idx = idxs_vector[i];
            auto &reply = get_slots_[idx];
            ASSERT(reply.in_use);
            if (locks_.HasReadLock(transaction_id, reply.key)) {
                // ASSERT(prevHolderWriteSet.find(reply.key) != prevHolderWriteSet.end());
                get_reply_.Clear();
                get_reply_.mutable_rid()->set_client_id(reply.client_id);
                get_reply_.mutable_rid()->set_client_req_id(reply.client_req_id);

                TransactionState s = transactions_.ContinueGet(transaction_id, reply.get_idx);
                if (s == READING)
                {
                    // ASSERT(locks_.HasReadLock(transaction_id, reply.key));

                    std::pair<TimestampID, std::string> value;
                    ASSERT(store_.get(reply.key, value));

                    get_reply_.set_status(REPLY_OK);
                    get_reply_.set_val(value.second);

                    value.first.timestamp.serialize(get_reply_.mutable_timestamp());

                    transport_->SendMessage(this, *reply.remote, MsgType::GET_REPLY_TYPE, get_reply_);

                    transactions_.FinishGet(transaction_id, reply.get_idx);
                }
                else if (s == ABORTED)
                {
                    Panic("Shouldn't we be handling this only from ContinueGetAbort?????");
                    // Already aborted
                    get_reply_.set_status(REPLY_FAIL);
                    transport_->SendMessage(this, *reply.remote, MsgType::GET_REPLY_TYPE, get_reply_);
                }
                else
                {
                    NOT_REACHABLE();
                }

                reply.in_use = false;
                reply.remote = nullptr;
                reply.key.clear();
                free_get_slots_.push_back(idx);
                idxs_vector[i] = idxs_vector.back();
                idxs_vector.pop_back();
            } else {
                ++i;
            }
        }

        if (idxs_vector.empty()) {
            transaction_id_to_get_slots_.erase(search);
        }
    }

    const Timestamp Server::GetPrepareTimestamp(uint64_t client_id)
    {
        uint64_t ts = std::max(tt_.Now().earliest(), min_prepare_timestamp_.getTimestamp() + 1);
        const Timestamp prepare_timestamp{ts, client_id};
        min_prepare_timestamp_ = prepare_timestamp;

        return prepare_timestamp;
    }

    void Server::WoundPendingRWs(uint64_t transaction_id, const std::unordered_set<uint64_t> &rws)
    {
        for (uint64_t rw : rws)
        {
            ASSERT(transaction_id != rw);
            // Debug("[%lu] Wounding %lu", transaction_id, rw);
            TransactionState s = transactions_.GetRWTransactionState(rw);
            ASSERT(s != NOT_FOUND);

            if (s == PARALLEL_READING || s == READ_WAIT)
            {
                // Send wound to client
                std::shared_ptr<TransportAddress> remote = transactions_.GetClientAddr(rw);
                wound_.set_transaction_id(rw);
                transport_->SendMessage(this, *remote, MsgType::TXN_WOUND_TYPE, wound_);
            }
            else if (s == PREPARING || s == WAIT_PARTICIPANTS || s == PREPARE_WAIT || s == PREPARED)
            {
                // Send wound to coordinator
                int coordinator = transactions_.GetCoordinator(rw);
                ASSERT(coordinator >= 0);
                shard_clients_[coordinator]->Wound(rw);
            }
            else if (s == COMMITTING || s == COMMITTED || s == ABORTED)
            {
                // Debug("[%lu] Not wounding. Will complete soon", rw);
            }
            else
            {
                NOT_REACHABLE();
            }
        }
    }

    void Server::NotifyPendingRWs(uint64_t transaction_id, const std::unordered_set<uint64_t> &rws, const std::vector<std::pair<std::string, std::string>> & prevHolderWriteSet)
    {
        for (uint64_t waiting_rw : rws)
        {
            if (transaction_id != waiting_rw)
            {
                ContinueGet(waiting_rw, prevHolderWriteSet);
                ContinueCoordinatorPrepare(waiting_rw);
                ContinueParticipantPrepare(waiting_rw);
            }
        }
    }

    void Server::NotifyPendingRWs(uint64_t transaction_id, const std::unordered_set<uint64_t> &rws)
    {
        for (uint64_t waiting_rw : rws)
        {
            if (transaction_id != waiting_rw)
            {
                Debug("[%lu] continuing %lu", transaction_id, waiting_rw);
                ASSERT(transaction_id_to_get_slots_.find(waiting_rw) == transaction_id_to_get_slots_.end());
                // ContinueGet(waiting_rw);
                ContinueCoordinatorPrepare(waiting_rw);
                ContinueParticipantPrepare(waiting_rw);
            }
        }
    }

    /*
    void Server::NotifyPendingROs(const std::unordered_set<uint64_t> &ros)
    {
        for (uint64_t waiting_ro : ros)
        {
            ContinueROCommit(waiting_ro);
        }
    }

    void Server::NotifySlowPathROs(const std::unordered_set<uint64_t> &ros, uint64_t rw_transaction_id,
                                   bool is_commit, const Timestamp &commit_ts)
    {
        for (uint64_t ro : ros)
        {
            SendROSlowPath(ro, rw_transaction_id, is_commit, commit_ts);
        }
    }

    void Server::SendROSlowPath(uint64_t ro_transaction_id, uint64_t rw_transaction_id,
                                bool is_commit, const Timestamp &commit_ts)
    {
        ASSERT(consistency_ == RSS);
        auto search = pending_ro_commit_replies_.find(ro_transaction_id);
        ASSERT(search != pending_ro_commit_replies_.end());

        // Debug("[%lu] Sending slow path reply for %lu", ro_transaction_id, rw_transaction_id);

        PendingROCommitReply *reply = search->second;
        ASSERT(reply->n_slow_path_replies > 0);

        if (transactions_.GetROTransactionState(ro_transaction_id) != SLOW_PATH)
        {
            // Debug("[%lu] Fast path reply not yet sent", ro_transaction_id);
            // Debug("s: %d", static_cast<int>(transactions_.GetROTransactionState(ro_transaction_id)));
            reply->n_slow_path_replies -= 1;
            return;
        }

        uint64_t client_id = reply->rid.client_id();
        uint64_t client_req_id = reply->rid.client_req_id();
        const TransportAddress *remote = reply->rid.addr();

        ro_commit_slow_reply_.mutable_rid()->set_client_id(client_id);
        ro_commit_slow_reply_.mutable_rid()->set_client_req_id(client_req_id);
        ro_commit_slow_reply_.set_transaction_id(rw_transaction_id);
        ro_commit_slow_reply_.set_is_commit(is_commit);
        commit_ts.serialize(ro_commit_slow_reply_.mutable_commit_timestamp());

        transport_->SendMessage(this, *remote, ro_commit_slow_reply_);

        uint64_t n_slow_path_replies = reply->n_slow_path_replies - 1;
        if (n_slow_path_replies == 0)
        {
            delete remote;
            delete reply;
            pending_ro_commit_replies_.erase(search);

            transactions_.FinishROSlowPath(ro_transaction_id);
        }
        else
        {
            reply->n_slow_path_replies = n_slow_path_replies;
        }
    }

    void Server::HandleROCommit(const TransportAddress &remote, proto::ROCommit &msg)
    {
        uint64_t client_id = msg.rid().client_id();
        uint64_t client_req_id = msg.rid().client_req_id();
        uint64_t transaction_id = msg.transaction_id();

        // Debug("[%lu] Received ROCommit request", transaction_id);

        std::unordered_set<std::string> keys{msg.keys().begin(), msg.keys().end()};

        const Timestamp commit_ts{msg.commit_timestamp()};
        const Timestamp min_ts{msg.min_timestamp()};

        min_prepare_timestamp_ = std::max(min_prepare_timestamp_, commit_ts); // TODO: is this correct?

        TransactionState s = transactions_.StartRO(transaction_id, keys, min_ts, commit_ts);
        if (s == PREPARE_WAIT)
        {
            // Debug("[%lu] Waiting for prepared transactions", transaction_id);
            auto reply = new PendingROCommitReply(client_id, client_req_id, remote.clone());
            reply->n_slow_path_replies = transactions_.GetRONumberSkipped(transaction_id);
            auto inserted = pending_ro_commit_replies_.insert({transaction_id, reply});
            if (!inserted.second) {
                Panic("Duplicate ROCommit request for transaction_id = %lu", transaction_id);
            }
            // pending_ro_commit_replies_[transaction_id] = reply;

            if (debug_stats_)
            {
                _Latency_StartRec(&reply->wait_lat);
            }

            return;
        }

        ro_commit_reply_.Clear();
        ro_commit_reply_.mutable_rid()->set_client_id(client_id);
        ro_commit_reply_.mutable_rid()->set_client_req_id(client_req_id);
        ro_commit_reply_.set_transaction_id(transaction_id);

        std::pair<TimestampID, std::string> value;
        for (auto &k : keys)
        {
            ASSERT(store_.get(k, {commit_ts, transaction_id}, value));
            proto::ReadReply *rreply = ro_commit_reply_.add_values();
            rreply->set_transaction_id(value.first.transaction_id);
            value.first.timestamp.serialize(rreply->mutable_timestamp());
            rreply->set_key(k.c_str());
            rreply->set_val(value.second.c_str());
        }

        if (consistency_ == RSS && transactions_.GetRONumberSkipped(transaction_id) > 0)
        {
            const std::vector<PreparedTransaction> skipped_prepares = transactions_.GetROSkippedRWTransactions(transaction_id);

            // Add for slow replies
            auto reply = new PendingROCommitReply(client_id, client_req_id, remote.clone());
            reply->n_slow_path_replies = skipped_prepares.size();
            auto inserted = pending_ro_commit_replies_.insert({transaction_id, reply});
            if (!inserted.second) {
                Panic("Duplicate ROCommit request for transaction_id = %lu", transaction_id);
            }
            // pending_ro_commit_replies_[transaction_id] = reply;

            for (auto &pt : skipped_prepares)
            {
                // Debug("[%lu] replying with skipped prepare: %lu", transaction_id, pt.transaction_id());
                proto::PreparedTransactionMessage *ptm = ro_commit_reply_.add_prepares();
                pt.serialize(ptm);
            }

            transport_->SendMessage(this, remote, ro_commit_reply_);

            transactions_.StartROSlowPath(transaction_id);
        }
        else
        {
            transport_->SendMessage(this, remote, ro_commit_reply_);

            transactions_.CommitRO(transaction_id);
        }
    }

    void Server::ContinueROCommit(uint64_t transaction_id)
    {
        auto search = pending_ro_commit_replies_.find(transaction_id);
        ASSERT(search != pending_ro_commit_replies_.end());

        PendingROCommitReply *reply = search->second;

        uint64_t client_id = reply->rid.client_id();
        uint64_t client_req_id = reply->rid.client_req_id();
        const TransportAddress *remote = reply->rid.addr();

        // Debug("[%lu] Continuing RO commit", transaction_id);

        transactions_.ContinueRO(transaction_id);

        const Timestamp &commit_ts = transactions_.GetROCommitTimestamp(transaction_id);
        const std::unordered_set<std::string> &keys = transactions_.GetROKeys(transaction_id);

        ro_commit_reply_.Clear();
        ro_commit_reply_.mutable_rid()->set_client_id(client_id);
        ro_commit_reply_.mutable_rid()->set_client_req_id(client_req_id);
        ro_commit_reply_.set_transaction_id(transaction_id);

        std::pair<TimestampID, std::string> value;
        for (auto &k : keys)
        {
            ASSERT(store_.get(k, {commit_ts, transaction_id}, value));
            proto::ReadReply *rreply = ro_commit_reply_.add_values();
            rreply->set_transaction_id(value.first.transaction_id);
            value.first.timestamp.serialize(rreply->mutable_timestamp());
            rreply->set_key(k.c_str());
            rreply->set_val(value.second.c_str());
        }

        if (consistency_ == RSS && transactions_.GetRONumberSkipped(transaction_id) > 0)
        {
            const std::vector<PreparedTransaction> skipped_prepares = transactions_.GetROSkippedRWTransactions(transaction_id);
            // Add for slow replies
            reply->n_slow_path_replies = skipped_prepares.size();

            for (auto &pt : skipped_prepares)
            {
                // Debug("[%lu] replying with skipped prepare: %lu", transaction_id, pt.transaction_id());
                proto::PreparedTransactionMessage *ptm = ro_commit_reply_.add_prepares();
                pt.serialize(ptm);
            }

            transport_->SendMessage(this, *remote, ro_commit_reply_);

            transactions_.StartROSlowPath(transaction_id);
        }
        else
        {
            transport_->SendMessage(this, *remote, ro_commit_reply_);

            delete remote;
            delete reply;
            pending_ro_commit_replies_.erase(search);

            transactions_.CommitRO(transaction_id);
        }
    }
    */

    void Server::ReplicateAbort(uint64_t client_id, uint64_t client_req_id, uint64_t transaction_id)
    {
        // Debug("replicating an ABORT message! for TID %lu", transaction_id);
        replication::LinearizeableOperation abort_op;
        abort_op.Clear();
        abort_op.set_request_type(replication::LinearizeableOperation::ABORT);
        abort_op.set_transaction_id(transaction_id);
        abort_op.mutable_rid()->set_client_id(client_id);
        abort_op.mutable_rid()->set_client_req_id(client_req_id);

        transport_->TimerMicro(0, [this, m = std::move(abort_op)]() mutable {
            this->replica_->HandleRequest(m);
        });
    }

    void Server::ReplicateCommit(uint64_t client_id, uint64_t client_req_id, uint64_t transaction_id, const Timestamp &commit_ts)
    {
        // Debug("replicating a commit message! for TID %lu", transaction_id);
        replication::LinearizeableOperation commit_op;
        commit_op.Clear();
        commit_op.set_request_type(replication::LinearizeableOperation::COMMIT);
        commit_op.set_transaction_id(transaction_id);
        commit_ts.serialize(commit_op.mutable_commit()->mutable_commit_timestamp());
        commit_op.mutable_rid()->set_client_id(client_id);
        commit_op.mutable_rid()->set_client_req_id(client_req_id);

        transport_->TimerMicro(0, [this, m = std::move(commit_op)]() mutable {
            this->replica_->HandleRequest(m);
        });
    }

    void Server::ReplicateCoordinatorCommit(uint64_t client_id,
                uint64_t client_req_id, uint64_t transaction_id, const Transaction &transaction,
                const Timestamp &start_ts, const Timestamp &nonblock_ts, const Timestamp &commit_ts,
                const std::vector<int> &participants)
    {
        // Debug("replicating a coordinator commit message! for TID %lu", transaction_id);
        LinearizeableOperation commit_op;
        commit_op.Clear();
        commit_op.set_request_type(replication::LinearizeableOperation::COMMIT);
        commit_op.mutable_rid()->set_client_id(client_id);
        commit_op.mutable_rid()->set_client_req_id(client_req_id);
        commit_op.set_transaction_id(transaction_id);

        auto prepare = commit_op.mutable_prepare();

        transaction.serialize(prepare->mutable_txn());
        start_ts.serialize(prepare->mutable_timestamp());
        prepare->set_coordinator(shard_idx_);
        nonblock_ts.serialize(prepare->mutable_nonblock_ts());
        for (int p : participants)
        {
            prepare->add_participants(p);
        }

        commit_ts.serialize(commit_op.mutable_commit()->mutable_commit_timestamp());

        transport_->TimerMicro(0, [this, m = std::move(commit_op)]() mutable {
            this->replica_->HandleRequest(m);
        });
    }

    void Server::ReplicatePrepare(uint64_t client_id, uint64_t client_req_id, uint64_t transaction_id,
                const Transaction &transaction, const Timestamp &prepare_ts, const Timestamp &nonblock_ts)
    {
        // Debug("Replicating a prepare message! for TID", transaction_id);
        LinearizeableOperation prepare_op;
        prepare_op.Clear();
        prepare_op.set_request_type(replication::LinearizeableOperation::PREPARE);
        prepare_op.mutable_rid()->set_client_id(client_id);
        prepare_op.mutable_rid()->set_client_req_id(client_req_id);
        prepare_op.set_transaction_id(transaction_id);

        auto prepare = prepare_op.mutable_prepare();

        transaction.serialize(prepare->mutable_txn());
        prepare_ts.serialize(prepare->mutable_timestamp());
        prepare->set_coordinator(shard_idx_);
        nonblock_ts.serialize(prepare->mutable_nonblock_ts());

        transport_->TimerMicro(0, [this, m = std::move(prepare_op)]() mutable {
            this->replica_->HandleRequest(m);
        });
    }

    void Server::HandleRWCommitCoordinator(const TransportAddress &remote, proto::RWCommitCoordinator &msg)
    {
        uint64_t client_id = msg.rid().client_id();
        uint64_t client_req_id = msg.rid().client_req_id();

        uint64_t transaction_id = msg.transaction_id();

        std::vector<int> participants{msg.participants().begin(),
                                             msg.participants().end()};
        const Transaction transaction{msg.transaction()};
        const Timestamp nonblock_ts{msg.nonblock_timestamp()};

        Debug("[%lu] Coordinator for transaction with %lu participants", transaction_id, participants.size());

        const TrueTimeInterval now = tt_.Now();
        const Timestamp start_ts{now.latest(), client_id};
        TransactionState s = transactions_.StartCoordinatorPrepare(transaction_id, start_ts, shard_idx_,
                                                                   participants, transaction, nonblock_ts);

        if (s == PREPARING)
        {
            // Debug("[%lu] Coordinator preparing", transaction_id);

            LockAcquireResult ar = locks_.AcquireLocks(transaction_id, transaction);
            if (ar.status == LockStatus::ACQUIRED)
            {
                ASSERT(ar.wound_rws.size() == 0);
                const Timestamp prepare_ts = GetPrepareTimestamp(client_id);
                transactions_.FinishCoordinatorPrepare(transaction_id, prepare_ts);
                const Timestamp &commit_ts = transactions_.GetRWCommitTimestamp(transaction_id);

                // Grab an idx
                if (rw_commit_c_slots_->ContainsKey(transaction_id)) {
                    Panic("Duplicate RWCommitCoordinator request for transaction_id = %lu", transaction_id);
                }
                PendingRWCommitCoordinatorReplySlot &reply = rw_commit_c_slots_->Alloc(transaction_id);
                reply.remote = &remote;
                reply.client_id = client_id;
                reply.client_req_id = client_req_id;
                ReplicateCoordinatorCommit(client_id, client_req_id,
                        transaction_id, transaction, start_ts, nonblock_ts, commit_ts, participants);
            }
            else if (ar.status == LockStatus::FAIL)
            {
                Panic("uhhh i don't think this can happen without WaitDie implemented??");
                ASSERT(ar.wound_rws.size() == 0);
                // Debug("[%lu] Coordinator prepare failed", transaction_id);
                LockReleaseResult rr = locks_.ReleaseLocks(transaction_id, transaction);

                SendRWCommmitCoordinatorReplyFail(remote, client_id, client_req_id);

                NotifyPendingRWs(transaction_id, rr.notify_rws);

                transactions_.AbortPrepare(transaction_id);
            }
            else if (ar.status == LockStatus::WAITING)
            {
                Debug("[%lu] Waiting", transaction_id);
                // Grab an idx
                if (rw_commit_c_slots_->ContainsKey(transaction_id)) {
                    Panic("Duplicate RWCommitCoordinator request for transaction_id = %lu", transaction_id);
                }
                PendingRWCommitCoordinatorReplySlot &reply = rw_commit_c_slots_->Alloc(transaction_id);
                reply.remote = &remote;
                reply.client_id = client_id;
                reply.client_req_id = client_req_id;

                transactions_.PausePrepare(transaction_id);

                WoundPendingRWs(transaction_id, ar.wound_rws);
            }
            else
            {
                NOT_REACHABLE();
            }
        }
        else if (s == ABORTED)
        {
            // Debug("[%lu] Already aborted", transaction_id);

            SendRWCommmitCoordinatorReplyFail(remote, client_id, client_req_id);

            SendAbortParticipants(transaction_id, participants);
        }
        else if (s == WAIT_PARTICIPANTS)
        {
            // Debug("[%lu] Waiting for other participants", transaction_id);
            // Grab an idx
            if (rw_commit_c_slots_->ContainsKey(transaction_id)) {
                Panic("Duplicate RWCommitCoordinator request for transaction_id = %lu", transaction_id);
            }
            PendingRWCommitCoordinatorReplySlot &reply = rw_commit_c_slots_->Alloc(transaction_id);
            reply.remote = &remote;
            reply.client_id = client_id;
            reply.client_req_id = client_req_id;
        }
        else
        {
            NOT_REACHABLE();
        }
    }

    void Server::ContinueCoordinatorPrepare(uint64_t transaction_id)
    {
        PendingRWCommitCoordinatorReplySlot *pending_reply = rw_commit_c_slots_->GetByKeyIfPresent(transaction_id);
        if (pending_reply == nullptr)
        {
            return;
        }

        TransactionState s = transactions_.ContinuePrepare(transaction_id);
        if (s == PREPARING)
        {
            const Transaction &transaction = transactions_.GetTransaction(transaction_id);
            LockAcquireResult ar = locks_.AcquireLocks(transaction_id, transaction);
            if (ar.status == LockStatus::ACQUIRED)
            {
                ASSERT(ar.wound_rws.size() == 0);
                const Timestamp prepare_ts = GetPrepareTimestamp(pending_reply->client_id);
                transactions_.FinishCoordinatorPrepare(transaction_id, prepare_ts);
                const Timestamp &commit_ts = transactions_.GetRWCommitTimestamp(transaction_id);

                const Timestamp &start_ts = transactions_.GetStartTimestamp(transaction_id);
                const std::vector<int> &participants = transactions_.GetParticipants(transaction_id);
                const Timestamp &nonblock_ts = transactions_.GetNonBlockTimestamp(transaction_id);

                ReplicateCoordinatorCommit(pending_reply->client_id, pending_reply->client_req_id,
                        transaction_id, transaction, start_ts, nonblock_ts, commit_ts, participants);
            }
            else if (ar.status == LockStatus::FAIL)
            {
                Panic("Don't think we should be able to enter this case without WaitDie implemented??");
                ASSERT(ar.wound_rws.size() == 0);
                // Debug("[%lu] Coordinator prepare failed", transaction_id);
                LockReleaseResult rr = locks_.ReleaseLocks(transaction_id, transaction);

                SendRWCommmitCoordinatorReplyFail(*pending_reply->remote, pending_reply->client_id, pending_reply->client_req_id);
                rw_commit_c_slots_->FreeByKey(transaction_id);
                pending_reply->remote = nullptr;

                NotifyPendingRWs(transaction_id, rr.notify_rws);

                transactions_.AbortPrepare(transaction_id);
            }
            else if (ar.status == LockStatus::WAITING)
            {
                Debug("[%lu] Waiting", transaction_id);

                transactions_.PausePrepare(transaction_id);

                WoundPendingRWs(transaction_id, ar.wound_rws);
            }
            else
            {
                NOT_REACHABLE();
            }
        }
        else if (s == PREPARED || s == COMMITTING || s == COMMITTED)
        {
            Debug("[%lu] Already prepared", transaction_id);
        }
        else if (s == ABORTED)
        { // Already aborted
            Debug("[%lu] Already aborted", transaction_id);
        }
        else
        {
            NOT_REACHABLE();
        }
    }

    void Server::SendRWCommmitCoordinatorReplyOK(uint64_t transaction_id,
                                                 const Timestamp &commit_ts,
                                                 const Timestamp &nonblock_ts)
    {
        PendingRWCommitCoordinatorReplySlot *pending_reply = rw_commit_c_slots_->GetByKeyIfPresent(transaction_id);
        if (pending_reply == nullptr)
        {
            return;
        }

        rw_commit_c_reply_.mutable_rid()->set_client_id(pending_reply->client_id);
        rw_commit_c_reply_.mutable_rid()->set_client_req_id(pending_reply->client_req_id);
        rw_commit_c_reply_.set_status(REPLY_OK);

        Debug("Sending commit reply to client with req_id = %lu", transaction_id);
        transport_->SendMessage(this, *pending_reply->remote, MsgType::TXN_COMMIT_REPLY_TYPE, rw_commit_c_reply_);
        rw_commit_c_slots_->FreeByKey(transaction_id);
        pending_reply->remote = nullptr;
    }

    void Server::SendRWCommmitCoordinatorReplyFail(const TransportAddress &remote,
                                                   uint64_t client_id,
                                                   uint64_t client_req_id)
    {
        rw_commit_c_reply_.mutable_rid()->set_client_id(client_id);
        rw_commit_c_reply_.mutable_rid()->set_client_req_id(client_req_id);
        rw_commit_c_reply_.set_status(REPLY_FAIL);

        transport_->SendMessage(this, remote, MsgType::TXN_COMMIT_REPLY_TYPE, rw_commit_c_reply_);
    }

    void Server::SendPrepareOKRepliesOK(uint64_t transaction_id, const Timestamp &commit_ts)
    {
        PendingPrepareOKReplySlot *reply = prepare_ok_slots_->GetByKeyIfPresent(transaction_id);
        if (reply == nullptr)
        {
            // Debug("[%lu] No pending prepare ok reply found", transaction_id);
            return;
        }

        prepare_ok_reply_.set_status(REPLY_OK);
        commit_ts.serialize(prepare_ok_reply_.mutable_commit_timestamp());


        // now that we removed callback with binding, put TID here (we weren't using req_id_)
        prepare_ok_reply_.mutable_rid()->set_client_req_id(transaction_id);
        for (auto &prid : reply->participant_rids)
        {
            prepare_ok_reply_.mutable_rid()->set_client_id(prid.client_id);

            transport_->SendMessage(this, *prid.remote, MsgType::TXN_PREPARE_OK_REPLY_TYPE, prepare_ok_reply_);
            prid.remote = nullptr;
        }

        reply->participant_rids.clear();
        prepare_ok_slots_->FreeByKey(transaction_id);
    }

    void Server::SendPrepareOKRepliesFail(uint64_t transaction_id, PendingPrepareOKReplySlot &reply)
    {
        prepare_ok_reply_.set_status(REPLY_FAIL);
        prepare_ok_reply_.clear_commit_timestamp();

        prepare_ok_reply_.mutable_rid()->set_client_req_id(transaction_id);
        for (auto &prid : reply.participant_rids)
        {
            prepare_ok_reply_.mutable_rid()->set_client_id(prid.client_id);

            transport_->SendMessage(this, *prid.remote, MsgType::TXN_PREPARE_OK_REPLY_TYPE, prepare_ok_reply_);
            prid.remote = nullptr;
        }
    }

    void Server::SendRWCommmitParticipantReplyOK(uint64_t transaction_id)
    {
        // PendingRWCommitParticipantReplySlot &pending_reply = rw_commit_p_slots_->GetByKey(transaction_id);

        // NO LONGER SENDING
        // rw_commit_p_reply_.mutable_rid()->set_client_id(pending_reply.client_id);
        // rw_commit_p_reply_.mutable_rid()->set_client_req_id(pending_reply.client_req_id);
        // rw_commit_p_reply_.set_status(REPLY_OK);

        // transport_->SendMessage(this, *pending_reply.remote, MsgType::TXN_COMMIT_PART_REPLY_TYPE, rw_commit_p_reply_);
        rw_commit_p_slots_->FreeByKey(transaction_id);
    }

    void Server::SendRWCommmitParticipantReplyFail(uint64_t transaction_id)
    {
        // PendingRWCommitParticipantReplySlot &pending_reply = rw_commit_p_slots_->GetByKey(transaction_id);

        // NO LONGER SENDING
        // rw_commit_p_reply_.mutable_rid()->set_client_id(pending_reply.client_id);
        // rw_commit_p_reply_.mutable_rid()->set_client_req_id(pending_reply.client_req_id);
        // rw_commit_p_reply_.set_status(REPLY_FAIL);

        // transport_->SendMessage(this, *pending_reply.remote, MsgType::TXN_COMMIT_PART_REPLY_TYPE, rw_commit_p_reply_);
        rw_commit_p_slots_->FreeByKey(transaction_id);
    }

    // void Server::SendRWCommmitParticipantReplyFail(const TransportAddress &remote,
    //                                                uint64_t client_id,
    //                                                uint64_t client_req_id)
    // {
    //     rw_commit_p_reply_.mutable_rid()->set_client_id(client_id);
    //     rw_commit_p_reply_.mutable_rid()->set_client_req_id(client_req_id);
    //     rw_commit_p_reply_.set_status(REPLY_FAIL);

    //     transport_->SendMessage(this, remote, MsgType::TXN_COMMIT_PART_REPLY_TYPE, rw_commit_p_reply_);
    // }

    void Server::HandleRWCommitParticipant(const TransportAddress &remote, proto::RWCommitParticipant &msg)
    {
        uint64_t client_id = msg.rid().client_id();
        uint64_t client_req_id = msg.rid().client_req_id();

        uint64_t transaction_id = msg.transaction_id();
        int coordinator = msg.coordinator_shard();

        const Transaction transaction{msg.transaction()};
        const Timestamp nonblock_ts{msg.nonblock_timestamp()};

        Debug("[%lu] Participant for transaction", transaction_id);

        TransactionState s = transactions_.StartParticipantPrepare(transaction_id, coordinator, transaction, nonblock_ts);
        if (s == PREPARING)
        {
            LockAcquireResult ar = locks_.AcquireLocks(transaction_id, transaction);
            if (ar.status == LockStatus::ACQUIRED)
            {
                ASSERT(ar.wound_rws.size() == 0);
                const Timestamp prepare_ts = GetPrepareTimestamp(client_id);

                transactions_.SetParticipantPrepareTimestamp(transaction_id, prepare_ts);

                if (rw_commit_p_slots_->ContainsKey(transaction_id)) {
                    Panic("Duplicate RWCommitParticipant request for transaction_id = %lu", transaction_id);
                }
                PendingRWCommitParticipantReplySlot &reply = rw_commit_p_slots_->Alloc(transaction_id);
                reply.client_id = client_id;
                reply.client_req_id = client_req_id;
                ReplicatePrepare(client_id, client_req_id, transaction_id, transaction, prepare_ts, nonblock_ts);
            }
            else if (ar.status == LockStatus::FAIL)
            {
                Panic("Don't think we should be able to enter this case without WaitDie implemented??");
                ASSERT(ar.wound_rws.size() == 0);
                // Debug("[%lu] Participant prepare failed", transaction_id);
                LockReleaseResult rr = locks_.ReleaseLocks(transaction_id, transaction);

                // TODO: Handle timeout
                shard_clients_[coordinator]->PrepareAbort(
                    transaction_id, shard_idx_,
                    std::bind(&Server::PrepareAbortCallback, this, transaction_id,
                              placeholders::_1, placeholders::_2),
                    [](int, Timestamp) {}, PREPARE_TIMEOUT);

                // Reply to client
                // SendRWCommmitParticipantReplyFail(remote, client_id, client_req_id);

                NotifyPendingRWs(transaction_id, rr.notify_rws);

                transactions_.AbortPrepare(transaction_id);
            }
            else if (ar.status == LockStatus::WAITING)
            {
                Debug("[%lu] Waiting", transaction_id);

                if (rw_commit_p_slots_->ContainsKey(transaction_id)) {
                    Panic("Duplicate RWCommitParticipant request for transaction_id = %lu", transaction_id);
                }
                PendingRWCommitParticipantReplySlot &reply = rw_commit_p_slots_->Alloc(transaction_id);
                reply.client_id = client_id;
                reply.client_req_id = client_req_id;

                transactions_.PausePrepare(transaction_id);

                WoundPendingRWs(transaction_id, ar.wound_rws);
            }
            else
            {
                NOT_REACHABLE();
            }
        }
        else if (s == ABORTED)
        {
            // Debug("[%lu] Already aborted", transaction_id);

            // Reply to client
            // SendRWCommmitParticipantReplyFail(remote, client_id, client_req_id);
        }
        else
        {
            NOT_REACHABLE();
        }
    }

    void Server::ContinueParticipantPrepare(uint64_t transaction_id)
    {
        PendingRWCommitParticipantReplySlot *pending_reply = rw_commit_p_slots_->GetByKeyIfPresent(transaction_id);
        if (pending_reply == nullptr)
        {
            return;
        }

        TransactionState s = transactions_.ContinuePrepare(transaction_id);
        if (s == PREPARING)
        {
            const int coordinator = transactions_.GetCoordinator(transaction_id);
            const Transaction &transaction = transactions_.GetTransaction(transaction_id);

            LockAcquireResult ar = locks_.AcquireLocks(transaction_id, transaction);
            if (ar.status == LockStatus::ACQUIRED)
            {
                ASSERT(ar.wound_rws.size() == 0);
                const Timestamp prepare_ts = GetPrepareTimestamp(pending_reply->client_id);

                transactions_.SetParticipantPrepareTimestamp(transaction_id, prepare_ts);

                const Timestamp &nonblock_ts = transactions_.GetNonBlockTimestamp(transaction_id);

                ReplicatePrepare(pending_reply->client_id, pending_reply->client_req_id, transaction_id, transaction, prepare_ts, nonblock_ts);
            }
            else if (ar.status == LockStatus::FAIL)
            {
                Panic("Don't think we should be able to enter this case without WaitDie implemented??");
                ASSERT(ar.wound_rws.size() == 0);
                // Debug("[%lu] Participant prepare failed", transaction_id);
                LockReleaseResult rr = locks_.ReleaseLocks(transaction_id, transaction);

                // TODO: Handle timeout
                shard_clients_[coordinator]->PrepareAbort(
                    transaction_id, shard_idx_,
                    std::bind(&Server::PrepareAbortCallback, this, transaction_id,
                              placeholders::_1, placeholders::_2),
                    [](int, Timestamp) {}, PREPARE_TIMEOUT);

                // Reply to client
                // SendRWCommmitParticipantReplyFail(*pending_reply.remote, pending_reply.client_id, pending_reply.client_req_id);
                rw_commit_p_slots_->FreeByKey(transaction_id);

                NotifyPendingRWs(transaction_id, rr.notify_rws);

                transactions_.AbortPrepare(transaction_id);
            }
            else if (ar.status == LockStatus::WAITING)
            {
                Debug("[%lu] Waiting", transaction_id);

                transactions_.PausePrepare(transaction_id);

                WoundPendingRWs(transaction_id, ar.wound_rws);
            }
            else
            {
                NOT_REACHABLE();
            }
        }
        else if (s == PREPARED || s == COMMITTING || s == COMMITTED)
        {
            // Debug("[%lu] Already prepared", transaction_id);
        }
        else if (s == ABORTED)
        { // Already aborted
            // Debug("[%lu] Already aborted", transaction_id);
        }
        else
        {
            NOT_REACHABLE();
        }
    }

    // What a Participant runs when the coordinator responds to it after it has prepared (heard from all participants)
    void Server::PrepareOKCallback(uint64_t transaction_id, int status, Timestamp commit_ts)
    {
        // Debug("[%lu] Received PREPARE_OK callback: %d %d", transaction_id, shard_idx_, status);

        if (status == REPLY_OK)
        {
            TransactionState s = transactions_.ParticipantReceivePrepareOK(transaction_id);
            ASSERT(s == COMMITTING);

            // TODO: Handle timeout
            ReplicateCommit(0, 0, transaction_id, commit_ts);
        }
        else if (status == REPLY_FAIL)
        {
            TransactionState s = transactions_.GetRWTransactionState(transaction_id);
            if (s == ABORTED)
            {
                // Debug("[%lu] Already aborted", transaction_id);
                return;
            }

            ASSERT(s == PREPARED);

            const Transaction &transaction = transactions_.GetTransaction(transaction_id);

            LockReleaseResult rr = locks_.ReleaseLocks(transaction_id, transaction);
            auto prevHolderWriteSet = std::move(transaction.getWriteSet());
            transactions_.Abort(transaction_id);
            // We are going to abort this for suresies

            // TODO: Handle timeout
            ReplicateAbort(0, 0, transaction_id); // don't really care about clientid, etc.
            ContinueGetAbort(transaction_id); // which will remove it before the next NotifyPendingRWs call

            NotifyPendingRWs(transaction_id, rr.notify_rws, prevHolderWriteSet);
            // NotifyPendingROs(fr.notify_ros);
            // NotifySlowPathROs(fr.notify_slow_path_ros, transaction_id, false);
        }
        else
        {
            NOT_REACHABLE();
        }
    }

    void Server::PrepareAbortCallback(uint64_t transaction_id, int status,
                                      Timestamp timestamp)
    {
        ASSERT(status == REPLY_OK);

        // Debug("[%lu] Received PREPARE_ABORT callback: %d %d", transaction_id, shard_idx_, status);
    }

    // Coordinator processes PrepareOK from a participant shard leader
    void Server::HandlePrepareOK(const TransportAddress &remote, proto::PrepareOK &msg)
    {
        uint64_t client_id = msg.rid().client_id();
        uint64_t client_req_id = msg.rid().client_req_id();

        uint64_t transaction_id = msg.transaction_id();

        int participant_shard = msg.participant_shard();
        const Timestamp prepare_ts{msg.prepare_timestamp()};
        const Timestamp nonblock_ts{msg.nonblock_timestamp()};

        Debug("[%lu] Received Prepare OK from participant shard %d", transaction_id, participant_shard);

        PendingPrepareOKReplySlot &reply = prepare_ok_slots_->AllocIfNotPresent(transaction_id);

        // Check for duplicate Prepare OKs from the same participant (shouldn't happen)
        bool is_dup = false;
        for (auto &prid : reply.participant_rids)
        {
            if (prid.client_id == client_id && prid.client_req_id == client_req_id)
            {
                is_dup = true;
                Panic("WE HAVE A DUP???");
                break;
            }
        }
        if (!is_dup)
        {
            reply.participant_rids.push_back(PendingPrepareOKReplySlot::Rid{&remote, client_id, client_req_id});
        }
        // reply.participant_rids.push_back(PendingPrepareOKReplySlot::Rid{&remote, client_id, client_req_id});

        TransactionState s = transactions_.CoordinatorReceivePrepareOK(transaction_id, participant_shard, prepare_ts, nonblock_ts);
        if (s == PREPARING)
        {
            // Debug("[%lu] Coordinator preparing", transaction_id);

            const std::vector<int> &participants = transactions_.GetParticipants(transaction_id);
            const Transaction &transaction = transactions_.GetTransaction(transaction_id);

            LockAcquireResult ar = locks_.AcquireLocks(transaction_id, transaction);
            if (ar.status == LockStatus::ACQUIRED)
            {
                ASSERT(ar.wound_rws.size() == 0);
                const Timestamp prepare_ts = GetPrepareTimestamp(client_id);
                transactions_.FinishCoordinatorPrepare(transaction_id, prepare_ts);

                const Timestamp &commit_ts = transactions_.GetRWCommitTimestamp(transaction_id);
                const Timestamp &start_ts = transactions_.GetStartTimestamp(transaction_id);
                const Timestamp &nonblock_ts = transactions_.GetNonBlockTimestamp(transaction_id);

                // TODO: Handle timeout
                ReplicateCoordinatorCommit(client_id, client_req_id,
                        transaction_id, transaction, start_ts, nonblock_ts, commit_ts, participants);
            }
            else if (ar.status == FAIL)
            {
                Panic("Don't think we should be able to enter this case without WaitDie implemented??");
                ASSERT(ar.wound_rws.size() == 0);
                // Debug("[%lu] Coordinator prepare failed", transaction_id);
                LockReleaseResult rr = locks_.ReleaseLocks(transaction_id, transaction);

                // Reply to participants
                SendPrepareOKRepliesFail(transaction_id, reply);
                reply.participant_rids.clear();
                prepare_ok_slots_->FreeByKey(transaction_id);

                // Notify other participants
                SendAbortParticipants(transaction_id, participants);

                // Reply to client
                PendingRWCommitCoordinatorReplySlot &pending_reply = rw_commit_c_slots_->GetByKey(transaction_id);
                SendRWCommmitCoordinatorReplyFail(*pending_reply.remote, pending_reply.client_id, pending_reply.client_req_id);
                rw_commit_c_slots_->FreeByKey(transaction_id);
                pending_reply.remote = nullptr;

                // Notify waiting RW transactions
                NotifyPendingRWs(transaction_id, rr.notify_rws);

                transactions_.AbortPrepare(transaction_id);
            }
            else if (ar.status == WAITING)
            {
                Debug("[%lu] Waiting", transaction_id);

                transactions_.PausePrepare(transaction_id);

                WoundPendingRWs(transaction_id, ar.wound_rws);
            }
            else
            {
                NOT_REACHABLE();
            }
        }
        else if (s == ABORTED)
        { // Already aborted
            // Debug("[%lu] Already aborted", transaction_id);

            // Reply to participants
            SendPrepareOKRepliesFail(transaction_id, reply);
            reply.participant_rids.clear();
            prepare_ok_slots_->FreeByKey(transaction_id);
        }
        else if (s == WAIT_PARTICIPANTS)
        {
            // Debug("[%lu] Waiting for other participants", transaction_id);
        }
        else
        {
            NOT_REACHABLE();
        }
    }

    void Server::HandlePrepareAbort(const TransportAddress &remote, proto::PrepareAbort &msg)
    {
        Panic("Don't think this can be reached without WaitDie implemented??");
        uint64_t transaction_id = msg.transaction_id();

        // Debug("[%lu] Received Prepare ABORT", transaction_id);

        prepare_abort_reply_.mutable_rid()->CopyFrom(msg.rid());

        TransactionState state = transactions_.GetRWTransactionState(transaction_id);
        if (state == NOT_FOUND)
        {
            // Debug("[%lu] Transaction not in progress", transaction_id);

            prepare_abort_reply_.set_status(REPLY_OK);
            transport_->SendMessage(this, remote, prepare_abort_reply_);

            transactions_.Abort(transaction_id);
            return;
        }

        if (state == ABORTED)
        { // Already aborted
            // Debug("[%lu] Transaction already aborted", transaction_id);

            prepare_abort_reply_.set_status(REPLY_OK);
            transport_->SendMessage(this, remote, prepare_abort_reply_);
            return;
        }

        ASSERT(state == PARALLEL_READING || state == READ_WAIT || state == WAIT_PARTICIPANTS);

        // Release locks acquired during GETs
        const Transaction &transaction = transactions_.GetTransaction(transaction_id);
        LockReleaseResult rr = locks_.ReleaseLocks(transaction_id, transaction);

        // Reply to client
        PendingRWCommitCoordinatorReplySlot *pending_reply = rw_commit_c_slots_->GetByKeyIfPresent(transaction_id);
        if (pending_reply != nullptr) {
            SendRWCommmitCoordinatorReplyFail(*(pending_reply->remote), pending_reply->client_id, pending_reply->client_req_id);
            // Notify participants
            const std::vector<int> &participants = transactions_.GetParticipants(transaction_id);
            SendAbortParticipants(transaction_id, participants);

            rw_commit_c_slots_->FreeByKey(transaction_id);
            pending_reply->remote = nullptr;
        }

        // Reply to OK participants
        PendingPrepareOKReplySlot *reply = prepare_ok_slots_->GetByKeyIfPresent(transaction_id);
        if (reply != nullptr)
        {
            SendPrepareOKRepliesFail(transaction_id, *reply);
            reply->participant_rids.clear();
            prepare_ok_slots_->FreeByKey(transaction_id);
        }

        prepare_abort_reply_.set_status(REPLY_OK);
        transport_->SendMessage(this, remote, prepare_abort_reply_);

        NotifyPendingRWs(transaction_id, rr.notify_rws);

        transactions_.Abort(transaction_id);
    }

    void Server::HandleWound(const TransportAddress &remote, proto::Wound &msg)
    {
        uint64_t transaction_id = msg.transaction_id();

        // Debug("[%lu] Received Wound request", transaction_id);

        TransactionState state = transactions_.GetRWTransactionState(transaction_id);

        if (state == ABORTED)
        {
            // Debug("[%lu] Transaction already aborted", transaction_id);
            return;
        }

        if (state == COMMITTING || state == COMMITTED)
        {
            // Debug("[%lu] Transaction already committing", transaction_id);
            return;
        }

        ASSERT(state != PREPARED);

        if (state == PREPARING || state == WAIT_PARTICIPANTS || state == PREPARE_WAIT)
        {
            // Only coordinator should handle wounds
            ASSERT(transactions_.GetCoordinator(transaction_id) == shard_idx_);

            // Reply to client
            PendingRWCommitCoordinatorReplySlot *pending_reply = rw_commit_c_slots_->GetByKeyIfPresent(transaction_id);
            if (pending_reply != nullptr) {
                SendRWCommmitCoordinatorReplyFail(*(pending_reply->remote), pending_reply->client_id, pending_reply->client_req_id);
                rw_commit_c_slots_->FreeByKey(transaction_id);
                pending_reply->remote = nullptr;
            }

            // Reply to OK participants
            PendingPrepareOKReplySlot *reply = prepare_ok_slots_->GetByKeyIfPresent(transaction_id);
            if (reply != nullptr)
            {
                SendPrepareOKRepliesFail(transaction_id, *reply);
                reply->participant_rids.clear();
                prepare_ok_slots_->FreeByKey(transaction_id);
            }

            const std::vector<int> &participants = transactions_.GetParticipants(transaction_id);
            SendAbortParticipants(transaction_id, participants);
        }

        LockReleaseResult rr;

        // Coordinator may not yet know about this transaction
        // If so, no locks to release.
        std::vector<std::pair<std::string, std::string>> prevHolderWriteSet;
        if (state != NOT_FOUND)
        {
            const Transaction &transaction = transactions_.GetTransaction(transaction_id);
            rr = locks_.ReleaseLocks(transaction_id, transaction);
            prevHolderWriteSet = std::move(transaction.getWriteSet());
        }

        transactions_.Abort(transaction_id);
        ContinueGetAbort(transaction_id); // which will remove it before the next NotifyPendingRWs call

        NotifyPendingRWs(transaction_id, rr.notify_rws, prevHolderWriteSet);
        // NotifyPendingROs(fr.notify_ros);
        // NotifySlowPathROs(fr.notify_slow_path_ros, transaction_id, false);
    }

    void Server::HandleAbort(const TransportAddress &remote, proto::Abort &msg)
    {
        uint64_t transaction_id = msg.transaction_id();

        Debug("[%lu] Received Abort request on shard %d", transaction_id, shard_idx_);

        abort_reply_.mutable_rid()->CopyFrom(msg.rid());

        TransactionState state = transactions_.GetRWTransactionState(transaction_id);

        if (state == ABORTED)
        {
            // Debug("[%lu] Transaction already aborted", transaction_id);
            abort_reply_.set_status(REPLY_OK);
            transport_->SendMessage(this, remote, MsgType::TXN_ABORT_REPLY_TYPE, abort_reply_);
            return;
        }

        if (state == COMMITTING || state == COMMITTED)
        {
            // Debug("[%lu] Transaction already committing", transaction_id);
            abort_reply_.set_status(REPLY_FAIL);
            transport_->SendMessage(this, remote, MsgType::TXN_ABORT_REPLY_TYPE, abort_reply_);
            return;
        }

        LockReleaseResult rr;
        // Participant may not yet know about this transaction
        // If so, no locks to release.
        std::vector<std::pair<std::string, std::string>> prevHolderWriteSet;
        if (state != NOT_FOUND)
        {
            const Transaction &transaction = transactions_.GetTransaction(transaction_id);
            rr = locks_.ReleaseLocks(transaction_id, transaction);
            prevHolderWriteSet = std::move(transaction.getWriteSet());
        } else {
            Warning("Transaction %lu not found during abort!!!!!!!!!!", transaction_id);
        }

        transactions_.Abort(transaction_id);

        if (state == PREPARING || state == PREPARED)
        {
            // TODO: Handle timeout
            ReplicateAbort(0, 0, transaction_id); // don't really care about clientid, etc.
        }

        abort_reply_.set_status(REPLY_OK);
        transport_->SendMessage(this, remote, MsgType::TXN_ABORT_REPLY_TYPE, abort_reply_);

        // Reply to client for any ongoing GETs
        ContinueGetAbort(transaction_id);

        NotifyPendingRWs(transaction_id, rr.notify_rws, prevHolderWriteSet);
        // NotifyPendingROs(fr.notify_ros);
        // NotifySlowPathROs(fr.notify_slow_path_ros, transaction_id, false);
    }

    void Server::SendAbortParticipants(uint64_t transaction_id, const std::vector<int> &participants)
    {
        for (int p : participants)
        {
            if (p != shard_idx_)
            { // Don't send abort to self (coordinator)
                // TODO: Handle timeout
                shard_clients_[p]->Abort(
                    transaction_id,
                    [transaction_id]() { /*Debug("[%lu] Received ABORT participant callback", transaction_id);*/ },
                    []() {}, ABORT_TIMEOUT);
            }
        }
    }

    void Server::RespondToClientOperation(const TransportAddress *remote, uint32_t idx,
                        uint64_t clientid, uint64_t client_req_id, int status, string retval)
    {
        Debug("got this status %d and this retval %s for transaction_id = %lu", status, retval.c_str(), client_req_id);

        op_reply_.Clear();
        op_reply_.mutable_rid()->set_client_id(clientid);
        op_reply_.mutable_rid()->set_client_req_id(client_req_id);
        op_reply_.set_status(status);
        op_reply_.set_return_value(retval);
        op_reply_.set_transaction_id(client_req_id); //op_reply_.set_transaction_id(transaction_id); ???

        transport_->SendMessage(this, *remote, MsgType::LIN_REPLY_TYPE, op_reply_);
    }

    void Server::CoordinatorCommitTransaction(uint64_t transaction_id, const Timestamp commit_ts)
    {
        Debug("[%lu] Commiting", transaction_id);
        // if (replica_idx_ != 0) {
        //     Notice("[%lu] Commiting", transaction_id);
        // }

        const Timestamp nonblock_ts = transactions_.GetNonBlockTimestamp(transaction_id);
        size_t n_participants = transactions_.GetNumParticipants(transaction_id);

        // Commit writes
        const Transaction &transaction = transactions_.GetTransaction(transaction_id);
        for (auto &write : transaction.getWriteSet())
        {
            // apply all the buffered writes to the store!
            store_.put(write.first, write.second, {commit_ts, transaction_id});
        }

        if (transaction.getWriteSet().size() > 0)
        {
            min_prepare_timestamp_ = std::max(min_prepare_timestamp_, commit_ts);
        }

        LockReleaseResult rr = locks_.ReleaseLocks(transaction_id, transaction);
        auto prevHolderWriteSet = std::move(transaction.getWriteSet());
        transactions_.Commit(transaction_id); // transaction object doesn't exist after this point!!

        if (replica_idx_ != 0) return;
        // Reply to client
        SendRWCommmitCoordinatorReplyOK(transaction_id, commit_ts, nonblock_ts);

        // Reply to participants
        if (n_participants > 1) {
            SendPrepareOKRepliesOK(transaction_id, commit_ts);
        }

        // Continue waiting RW transactions
        NotifyPendingRWs(transaction_id, rr.notify_rws, prevHolderWriteSet);

        // Continue waiting RO transactions <-- REMOVED FOR NOW
        // NotifyPendingROs(fr.notify_ros);
        // NotifySlowPathROs(fr.notify_slow_path_ros, transaction_id, true, commit_ts);
    }

    void Server::ParticipantCommitTransaction(uint64_t transaction_id, const Timestamp commit_ts)
    {
        // Debug("[%lu] Participant Commiting", transaction_id);

        // Commit writes
        const Transaction &transaction = transactions_.GetTransaction(transaction_id);
        for (auto &write : transaction.getWriteSet())
        {
            // apply all the buffered writes to the store!
            store_.put(write.first, write.second, {commit_ts, transaction_id});
        }

        if (transaction.getWriteSet().size() > 0)
        {
            min_prepare_timestamp_ = std::max(min_prepare_timestamp_, commit_ts);
        }

        LockReleaseResult rr = locks_.ReleaseLocks(transaction_id, transaction);
        auto prevHolderWriteSet = std::move(transaction.getWriteSet());
        transactions_.Commit(transaction_id);

        // Continue waiting RW transactions
        NotifyPendingRWs(transaction_id, rr.notify_rws, prevHolderWriteSet);

        // Continue waiting RO transactions <-- REMOVED FOR NOW
        // NotifyPendingROs(fr.notify_ros);
        // NotifySlowPathROs(fr.notify_slow_path_ros, transaction_id, true, commit_ts);
    }

    void Server::LeaderUpcall(opnum_t opnum, const string &op, bool &replicate,
                              string &response)
    {
        Panic("Shouldnt be caling this!");

        // Request request;
        // LinearizeableOperation linreq;
        // if (consistency_ != LIN)
        // {
        //     request.ParseFromString(op);
        //     switch (request.op())
        //     {
        //     case strongstore::proto::Request::PREPARE:
        //     case strongstore::proto::Request::COMMIT:
        //     case strongstore::proto::Request::ABORT:
        //         replicate = true;
        //         response = op;
        //         break;
        //     default:
        //         Panic("Unrecognized operation.");
        //     }
        // } else {
        //     linreq.ParseFromString(op);
        //     replicate = true;
        //     response = op;
        //     Debug("was able to parse LinearizeableOperation!");
        // }
    }

    /* Gets called when a command is issued using client.Invoke(...) to this
     * replica group.
     * opnum is the operation number.
     * op is the request string passed by the client.
     * response is the reply which will be sent back to the client.
     */
    void Server::ReplicaUpcall(const LinearizeableOperation &msg)
    {
        // Debug("Received Replica Upcall in strongstore server: %lu %s", opnum, op.c_str());
        if (consistency_ == LIN)
        {
            ReplicaUpcallAppRequest(msg);
            return;
        }

        uint64_t transaction_id = msg.transaction_id();

        if (msg.request_type() == replication::LinearizeableOperation::PREPARE)
        {
            Notice("[%lu] Received Participant PREPARE", transaction_id);
            // Participant Shard Replica Upcall for Prepare

            TransactionState s = transactions_.GetRWTransactionState(transaction_id);
            if (s == ABORTED)
            {
                Notice("[%lu] Already aborted", transaction_id);
                if (replica_idx_ == 0)
                {
                    // Have only the leader issue these messages
                    SendRWCommmitParticipantReplyFail(transaction_id);
                }
            }
            else if (s == NOT_FOUND)
            {
                ASSERT(replica_idx_ != 0);
                // Participant Replica prepare
                const Timestamp prepare_ts{msg.prepare().timestamp()};
                int coordinator = msg.prepare().coordinator();
                const Transaction transaction{msg.prepare().txn()};
                const Timestamp nonblock_ts{msg.prepare().nonblock_ts()};

                s = transactions_.StartParticipantPrepare(transaction_id, coordinator, transaction, nonblock_ts);
                ASSERT(s == PREPARING);

                LockAcquireResult ar = locks_.AcquireLocks(transaction_id, transaction);
                //WHY DOES THIS FAIL!?!?
                if (ar.status != LockStatus::ACQUIRED) {
                    std::vector<uint64_t> holders = locks_.WhoHolds(transaction);
                    Warning("I'm replica %d on shard %d and lock acquire failed during Participant Shard Replica replicating Prepare for transaction %lu | I got status %d instead", replica_idx_, shard_idx_, transaction_id, ar.status);
                    Notice("Holders are....");
                    for (uint64_t h : holders) {
                        Notice("%lu", h);
                    }
                }
                ASSERT(ar.status == LockStatus::ACQUIRED);

                transactions_.SetParticipantPrepareTimestamp(transaction_id, prepare_ts);

                transactions_.FinishParticipantPrepare(transaction_id);
            }
            else if (s == PREPARING)
            {
                ASSERT(replica_idx_ == 0);
                // Participant Leader prepare
                TransactionState s = transactions_.FinishParticipantPrepare(transaction_id);
                ASSERT(s == PREPARED);
                int coordinator = transactions_.GetCoordinator(transaction_id);
                const Timestamp &prepare_ts = transactions_.GetPrepareTimestamp(transaction_id);
                const Timestamp &nonblock_ts = transactions_.GetNonBlockTimestamp(transaction_id);
                // TODO: Handle timeout
                // Debug("Particiant leader on shard %d is sending PrepareOK to coordinator shard %d", shard_idx_, coordinator);
                shard_clients_[coordinator]->PrepareOK(
                    transaction_id, shard_idx_, prepare_ts, nonblock_ts);

                // Reply to client
                SendRWCommmitParticipantReplyOK(transaction_id);
            }
            else if (s == PREPARED) {
                Panic("How did this happen?");
            }
            else
            {
                NOT_REACHABLE();
            }

        }
        else if (msg.request_type() == replication::LinearizeableOperation::COMMIT)
        {
            // Debug("[%lu] Received COMMIT", transaction_id);

            const Timestamp commit_ts{msg.commit().commit_timestamp()};

            if (msg.has_prepare())
            { // Coordinator commit
                Notice("[%lu] Coordinator commit", transaction_id);

                if (transactions_.GetRWTransactionState(transaction_id) != COMMITTING)
                {
                    // Coordinator Replica Commit
                    ASSERT(replica_idx_ != 0);
                    const Timestamp start_ts{msg.prepare().timestamp()};
                    int coordinator = msg.prepare().coordinator();
                    const std::vector<int> participants{msg.prepare().participants().begin(),
                                                               msg.prepare().participants().end()};
                    const Transaction transaction{msg.prepare().txn()};
                    const Timestamp nonblock_ts{msg.prepare().nonblock_ts()};

                    ASSERT(coordinator == shard_idx_);

                    TransactionState s = transactions_.StartCoordinatorPrepare(transaction_id, start_ts, coordinator,
                                                                               participants, transaction, nonblock_ts);
                    for (int p : participants)
                    {
                        if (p != coordinator)
                        {
                            s = transactions_.CoordinatorReceivePrepareOK(transaction_id, p, commit_ts, nonblock_ts);
                        }
                    }
                    ASSERT(s == PREPARING);

                    LockAcquireResult ar = locks_.AcquireLocks(transaction_id, transaction);
                    //WHY DOES THIS FAIL!?!?
                    if (ar.status != LockStatus::ACQUIRED) {
                        std::vector<uint64_t> holders = locks_.WhoHolds(transaction);
                        Warning("I'm replica %d on shard %d and lock acquire failed during Coordinator Shard Replica replicating Commit for transaction %lu | I got status %d instead", replica_idx_, shard_idx_, transaction_id, ar.status);
                        Notice("Holders are....");
                        for (uint64_t h : holders) {
                            Notice("%lu", h);
                        }
                    }
                    ASSERT(ar.status == LockStatus::ACQUIRED);

                    transactions_.FinishCoordinatorPrepare(transaction_id, commit_ts);
                }
                else
                {
                    // Coordinator Leader Commit
                    ASSERT(replica_idx_ == 0);
                    Debug("[%lu] Already prepared", transaction_id);
                }

                uint64_t commit_wait_us = tt_.TimeToWaitUntilMicros(commit_ts.getTimestamp());
                if (commit_wait_us > 0) {
                    Debug("[%lu] delaying commit by %lu us", transaction_id, commit_wait_us);
                    if (replica_idx_ != 0) {
                        Warning("[%lu] delaying commit by %lu us", transaction_id, commit_wait_us);
                    }
                    transport_->TimerMicro(commit_wait_us, std::bind(&Server::CoordinatorCommitTransaction, this, transaction_id, commit_ts));
                } else {
                    CoordinatorCommitTransaction(transaction_id, commit_ts);
                }
            }
            else
            { // Participant commit
                Notice("[%lu] Participant commit", transaction_id);
                if (transactions_.GetRWTransactionState(transaction_id) != COMMITTING)
                {
                    ASSERT(replica_idx_ != 0);
                    transactions_.ParticipantReceivePrepareOK(transaction_id);
                }

                ParticipantCommitTransaction(transaction_id, commit_ts);
            }
        }
        else if (msg.request_type() == replication::LinearizeableOperation::ABORT)
        {
            Notice("[%lu] Received ABORT", transaction_id);
            TransactionState s = transactions_.GetRWTransactionState(transaction_id);

            if (s != ABORTED)
            { // replica abort
                ASSERT(replica_idx_ != 0);
                if (s == NOT_FOUND) {
                    Warning("[%lu] Replica received ABORT for unknown txn on shard %d replica %d",
                        transaction_id, shard_idx_, replica_idx_);
                    return;
                }
                const Transaction &transaction = transactions_.GetTransaction(transaction_id);

                LockReleaseResult rr = locks_.ReleaseLocks(transaction_id, transaction);
                auto prevHolderWriteSet = std::move(transaction.getWriteSet());
                transactions_.Abort(transaction_id);
                ContinueGetAbort(transaction_id); // which will remove it before the next NotifyPendingRWs call

                NotifyPendingRWs(transaction_id, rr.notify_rws, prevHolderWriteSet);
                // NotifyPendingROs(fr.notify_ros);
                // NotifySlowPathROs(fr.notify_slow_path_ros, transaction_id, false);
            }
        }
        else
        {
            NOT_REACHABLE();
        }
    }

    // TODO figure out interface for stuff to work with transformed apps
    void Server::ReplicaUpcallAppRequest(const LinearizeableOperation &msg)
    {
        Debug("inside ReplicaUpcall with req_id = %lu", msg.rid().client_req_id());

        string retval;
        int status = REPLY_OK;
        if (msg.kv().op() == replication::KVOpMessage::GET)
        {
            if (!linearizeable_kv_store_.get(msg.kv().key(), retval))
            {
                status = REPLY_FAIL;
            };
        }
        else if (msg.kv().op() == replication::KVOpMessage::PUT)
        {
            Debug("the request is put");
            if (!linearizeable_kv_store_.put(msg.kv().key(), msg.kv().value()))
            {
                status = REPLY_FAIL;
            };
        }
        else
        {
            Panic("Unrecognized operation.");
        }
        if (replica_idx_ != 0) return;

        uint32_t idx = msg.kv().idx();
        PendingOpReplySlot &pending_reply = op_slots_->GetByIdx(idx);
        ASSERT(pending_reply.in_use);
        RespondToClientOperation(pending_reply.remote, idx, msg.rid().client_id(), msg.rid().client_req_id(), status, retval);
        op_slots_->FreeByIdx(idx);
        pending_reply.remote = nullptr;
    }

    void Server::UnloggedUpcall(const string &op, string &response)
    {
        NOT_IMPLEMENTED();
    }

    void Server::Load(const string &key, const string &value,
                      const Timestamp timestamp)
    {
        if (consistency_ == LIN)
        {
            linearizeable_kv_store_.put(key, value);
        }
        else {
            store_.put(key, value, {timestamp, 0});
        }
    }

} // namespace strongstore
