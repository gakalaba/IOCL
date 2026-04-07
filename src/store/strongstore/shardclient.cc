/***********************************************************************
 *
 * store/strongstore/shardclient.cc:
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
#include "store/strongstore/shardclient.h"
#include "store/common/iocl_utils.h"

#include "lib/configuration.h"

namespace strongstore
{

    using namespace std;
    using namespace proto;

    ShardClient::ShardClient(const transport::Configuration &config,
                             Transport *transport, uint64_t client_id, int shard, uint64_t fanout,
                             wound_callback wcb)
        : last_req_id_{0},
          config_{config},
          transport_{transport},
          client_id_{client_id},
          shard_idx_{shard},
          wcb_{wcb},
          fanout_{fanout}
    {
        transport_->Register(this, config_, -1, -1);

        // TODO: Remove hardcoding
        replica_ = 0;
        seqno = 0;
        dummyTimestamp = Timestamp(0, 0);
        slots_.resize(fanout);
        // set all the objects in slots pred_list to size fanout
        for (auto &slot : slots_) {
            slot.pred_list.reserve(fanout);
        }
        get_slots_.resize(fanout);
        pending_prepare_ok_slot_.resize(fanout);
    }

    ShardClient::~ShardClient() {}
    void ShardClient::Close()
    {
    }

    void ShardClient::ReceiveMessage(const TransportAddress &remote,
                                     const std::string &type,
                                     const std::string &data, void *meta_data)
    {
        Panic("Shouldn't be calling this ReceiveMessage");
    }
    void ShardClient::ReceiveMessage(const TransportAddress &remote,
                                     MsgType type,
                                     const std::string &data, void *meta_data)
    {
        Debug("Got message wahoo");
        switch (type) {
        case MsgType::GET_REPLY_TYPE: {
            get_reply_.ParseFromString(data);
            HandleGetReply(get_reply_);
            break;
        }
        case MsgType::LIN_REPLY_TYPE: {
            op_reply_.ParseFromString(data);
            HandleSendOperationReply(op_reply_);
            break;
        }
        case MsgType::TXN_COMMIT_REPLY_TYPE: {
            rw_commit_c_reply_.ParseFromString(data);
            HandleRWCommitCoordinatorReply(rw_commit_c_reply_);
            break;
        }
        case MsgType::TXN_COMMIT_PART_REPLY_TYPE: {
            rw_commit_p_reply_.ParseFromString(data);
            HandleRWCommitParticipantReply(rw_commit_p_reply_);
            break;
        }
        case MsgType::TXN_PREPARE_OK_REPLY_TYPE: {
            prepare_ok_reply_.ParseFromString(data);
            HandlePrepareOKReply(prepare_ok_reply_);
            break;
        }
        case MsgType::TXN_ABORT_REPLY_TYPE: {
            abort_reply_.ParseFromString(data);
            HandleAbortReply(abort_reply_);
            break;
        }
        case MsgType::TXN_WOUND_TYPE: {
            wound_.ParseFromString(data);
            HandleWound(wound_);
            break;
        }
        /*
        else if (type == prepare_abort_reply_.GetTypeName())
        {
            prepare_abort_reply_.ParseFromString(data);
            HandlePrepareAbortReply(prepare_abort_reply_);
        }
        else if (type == ro_commit_reply_.GetTypeName())
        {
            ro_commit_reply_.ParseFromString(data);
            HandleROCommitReply(ro_commit_reply_);
        }
        else if (type == ro_commit_slow_reply_.GetTypeName())
        {
            ro_commit_slow_reply_.ParseFromString(data);
            HandleROCommitSlowReply(ro_commit_slow_reply_);
        }
        */
        default:
            Panic("Received unexpected message type: %u", (uint32_t)type);
        }
    }

    void ShardClient::HandleWound(const proto::Wound &msg)
    {
        uint64_t transaction_id = msg.transaction_id();
        Debug("Received wound for tid: %lu", transaction_id);
        wcb_(transaction_id);
    };

    /* Sends BEGIN to a single shard indexed by i. */
    void ShardClient::Begin(uint64_t transaction_id, const Timestamp &start_time)
    {
        ASSERT((transaction_id != the_transaction_.transaction_id()) || transaction_id == 0);
        the_transaction_.set_start_time(start_time);
        the_transaction_.set_transaction_id(transaction_id);
    }

    bool ShardClient::CheckPriorReadsAndWrites(uint64_t transaction_id, const std::string &key, get_callback gcb)
    {
        if (transaction_id != the_transaction_.transaction_id())
        {
            return false;
        }

        // Read your own writes, check the write set first.
        // auto wsearch = the_transaction_.getWriteSet().find(key);
        // if (wsearch != the_transaction_.getWriteSet().end())
        for (auto &write : the_transaction_.getWriteSet())
        {
            if (write.first == key) {
                gcb(REPLY_OK, key, write.second, Timestamp());
                return true;
            }
        }

        // Consistent reads, check the read set.
        auto rsearch = the_read_set_.find(key);
        if (rsearch != the_read_set_.end())
        {
            gcb(REPLY_OK, key, rsearch->second, Timestamp());
            return true;
        }

        return false;
    }

    void ShardClient::Get(uint64_t transaction_id, const std::string &key,
                          get_callback gcb, get_timeout_callback gtcb,
                          uint32_t timeout)
    {
        Get(transaction_id, key, gcb, gtcb, timeout, false);
    }

    void ShardClient::GetForUpdate(uint64_t transaction_id, const std::string &key,
                                   get_callback gcb, get_timeout_callback gtcb,
                                   uint32_t timeout)
    {
        Get(transaction_id, key, gcb, gtcb, timeout, true);
    }

    void ShardClient::Get(uint64_t transaction_id, const std::string &key,
                          get_callback gcb, get_timeout_callback gtcb,
                          uint32_t timeout, bool for_update)
    {
        // Send the GET operation to appropriate shard.
        Debug("[shard %i] Sending GET [%s]", shard_idx_, key.c_str());

        uint64_t req_id = last_req_id_++;
        uint32_t idx = req_id % fanout_;
        auto &pendingGet = get_slots_[idx];
        ASSERT(!pendingGet.in_use);
        pendingGet.in_use = true;
        pendingGet.gcb = gcb;
        pendingGet.key = key;
        pendingGet.transaction_id = transaction_id;

        ASSERT(transaction_id == the_transaction_.transaction_id());
        auto &start_ts = the_transaction_.start_time();

        // TODO: Setup timeout
        get_.Clear();
        get_.mutable_rid()->set_client_id(client_id_);
        get_.mutable_rid()->set_client_req_id(req_id);
        get_.set_transaction_id(transaction_id);
        start_ts.serialize(get_.mutable_timestamp());
        get_.set_key(key);
        get_.set_for_update(for_update);

        transport_->SendMessageToReplica(this, shard_idx_, replica_, MsgType::GET_TYPE, get_);
    }

    void ShardClient::HandleGetReply(const proto::GetReply &reply)
    {
        uint64_t req_id = reply.rid().client_req_id();
        int status = reply.status();

        uint32_t idx = req_id % fanout_;
        auto &pendingGet = get_slots_[idx];
        ASSERT(pendingGet.in_use);
        get_callback &gcb = pendingGet.gcb;
        std::string &key = pendingGet.key;
        uint64_t transaction_id = pendingGet.transaction_id;

        Debug("[%lu] [shard %i] Received GET reply: %s %d",
              transaction_id, shard_idx_, key.c_str(), status);

        const std::string &val = reply.val();
        Timestamp ts;
        if (status == REPLY_OK)
        {
            ts = Timestamp(reply.timestamp());
        }

        Debug("[%lu] Added %lu.%lu to read set.", transaction_id, ts.getTimestamp(), ts.getID());
        ASSERT(the_transaction_.transaction_id() == transaction_id);
        the_transaction_.addReadSet(key, ts);

        pendingGet.in_use = false;
        gcb(status, key, val, ts);
    }

    void ShardClient::Put(uint64_t transaction_id, const std::string &key, const std::string &value,
                          put_callback pcb, put_timeout_callback ptcb,
                          uint32_t timeout)
    {
        ASSERT(transaction_id == the_transaction_.transaction_id());

        the_transaction_.addWriteSet(key, value);

        pcb(REPLY_OK, key, value);
    }

    // IOCL issue a request
    void ShardClient::SendOperation(uint64_t app_request_id, const std::string op,
                                  const std::string &key, const std::string &value,
                                  op_callback ocb, op_timeout_callback otcb,
                                  uint32_t timeout,
                                  std::vector<OutstandingPred> &outstandingOperationVec,
                                  bool isIOCL)
    {
        // Send the operation to appropriate shard.
        Debug("[shard %i] AppReqiest Sending Operation %s(%s, %s)", shard_idx_, op.c_str(), key.c_str(), value.c_str());

        uint64_t req_id = last_req_id_++;
        uint32_t idx = req_id % fanout_;
        auto &pendingOp = slots_[idx];
        ASSERT((!pendingOp.in_use) && pendingOp.pred_list.empty());
        pendingOp.in_use = true;
        pendingOp.ocb = ocb;

        op_.Clear();
        op_.set_request_type(replication::LinearizeableOperation::KV_OP);
        op_.mutable_rid()->set_client_id(client_id_);
        op_.mutable_rid()->set_client_req_id(req_id);
        op_.set_transaction_id(app_request_id);
        op_.mutable_kv()->set_key(key);
        op_.mutable_kv()->set_value(value);
        if (op == "get")
        {
            op_.mutable_kv()->set_op(replication::KVOpMessage::GET);
        }
        else if (op == "put")
        {
            op_.mutable_kv()->set_op(replication::KVOpMessage::PUT);
        }
        else
        {
            Panic("Unrecognized operation.");
        }
        op_.mutable_kv()->set_idx(0); // slot index is decided at the server

        // Set the optional fields (myshardtag and pred_list) if IOCL
        if (isIOCL)
        {
            uint64_t myshardtag = CreateTag(client_id_, seqno);
            seqno++;
            op_.set_shardtag(myshardtag);
            op_.set_intkey(std::stoull(key)); // for iocl optimization

            // Construct predecessor list and Issue coordination requests
            replication::SuccessorRequestMessage coordReqMsg;
            coordReqMsg.set_s(myshardtag); // my shard tag
            coordReqMsg.set_shardidx(shard_idx_); // who pred should return to??
            uint32_t i = 0;
            for (auto &entry : outstandingOperationVec) {
                // increment refcount entry
                entry.refcount++;
                // Add this entry to predecessor list and the RPC message
                op_.add_predlist(entry.tag);
                pendingOp.pred_list.push_back(std::make_pair(entry.tag, entry.shardid));
                // Send coordination message to predecessor shard
                coordReqMsg.set_p(entry.tag);
                coordReqMsg.set_predidx(i);
                if (!transport_->SendMessageToReplica(this, entry.shardid, 0, MsgType::CLIENT_COORD_TYPE, coordReqMsg))
                {
                    Warning("Could not send request to replicas.");
                }
                i++;
            }
            // Add self to outstanding operations and refcount lists
            outstandingOperationVec.push_back(OutstandingPred{myshardtag, (uint32_t)shard_idx_, 1});
        }

        transport_->SendMessageToReplica(this, shard_idx_, replica_, MsgType::LIN_OP_TYPE, op_);
    }

    // IOCL receive the response
    void ShardClient::HandleSendOperationReply(const proto::LinearizeableReply &reply)
    {
        uint64_t req_id = reply.rid().client_req_id();
        int status = reply.status();
        string retval = reply.return_value();

        uint32_t idx = req_id % fanout_;
        auto &pendingOp = slots_[idx];
        ASSERT(pendingOp.in_use);

        op_callback ocb = std::move(pendingOp.ocb); // wrapped in move to make efficient

        ocb(status, retval, pendingOp.pred_list);
        pendingOp.in_use = false;
        pendingOp.pred_list.clear();
    }

    void ShardClient::ROCommit(uint64_t transaction_id,
                               const std::vector<std::string> &keys,
                               const Timestamp &commit_timestamp,
                               const Timestamp &min_read_timestamp,
                               ro_commit_callback ccb,
                               ro_commit_slow_callback cscb,
                               ro_commit_timeout_callback ctcb, uint32_t timeout)
    {
        Debug("[%lu] [shard %i] Sending ROCommit", transaction_id, shard_idx_);

        uint64_t req_id = last_req_id_++;
        PendingROCommit *pendingROCommit = new PendingROCommit(transaction_id, req_id);
        pendingROCommits[req_id] = pendingROCommit;
        pendingROCommit->ccb = ccb;
        pendingROCommit->cscb = cscb;
        pendingROCommit->ctcb = ctcb;
        pendingROCommit->n_slow_replies = 0;

        // TODO: Setup timeout
        ro_commit_.mutable_rid()->set_client_id(client_id_);
        ro_commit_.mutable_rid()->set_client_req_id(req_id);
        ro_commit_.set_transaction_id(transaction_id);
        commit_timestamp.serialize(ro_commit_.mutable_commit_timestamp());
        min_read_timestamp.serialize(ro_commit_.mutable_min_timestamp());

        ro_commit_.clear_keys();
        for (auto &k : keys)
        {
            ro_commit_.add_keys(k.c_str());
        }

        transport_->SendMessageToReplica(this, shard_idx_, replica_, ro_commit_);
    }

    void ShardClient::HandleROCommitSlowReply(const proto::ROCommitSlowReply &reply)
    {
        uint64_t req_id = reply.rid().client_req_id();

        auto itr = pendingROCommits.find(req_id);
        if (itr == pendingROCommits.end())
        {
            Debug("[%d][%lu] ROCommitReply for stale request.", shard_idx_, req_id);
            return; // stale request
        }

        PendingROCommit *req = itr->second;
        ASSERT(req->n_slow_replies > 0);

        ro_commit_slow_callback cscb = req->cscb;

        uint64_t transaction_id = reply.transaction_id();
        const Timestamp commit_ts{reply.commit_timestamp()};
        bool is_commit = reply.is_commit();

        req->n_slow_replies -= 1;
        if (req->n_slow_replies == 0)
        {
            pendingROCommits.erase(itr);
            delete req;
        }

        cscb(shard_idx_, transaction_id, commit_ts, is_commit);
    }

    void ShardClient::HandleROCommitReply(const proto::ROCommitReply &reply)
    {
        uint64_t req_id = reply.rid().client_req_id();

        auto itr = pendingROCommits.find(req_id);
        if (itr == pendingROCommits.end())
        {
            Debug("[%d][%lu] ROCommitReply for stale request.", shard_idx_, req_id);
            return; // stale request
        }

        PendingROCommit *req = itr->second;
        ro_commit_callback ccb = req->ccb;
        ASSERT(req->n_slow_replies == 0);

        std::vector<Value> values;
        for (auto &v : reply.values())
        {
            values.emplace_back(v);
        }

        std::vector<PreparedTransaction> prepares;
        for (auto &p : reply.prepares())
        {
            prepares.emplace_back(p);
        }

        uint64_t n_prepares = prepares.size();
        if (n_prepares == 0)
        {
            pendingROCommits.erase(itr);
            delete req;
        }
        else
        {
            req->n_slow_replies = n_prepares;
        }

        ccb(shard_idx_, values, prepares);
    }

    void ShardClient::RWCommitCoordinator(
        uint64_t transaction_id,
        const std::set<int> participants, Timestamp &nonblock_timestamp,
        rw_coord_commit_callback ccb, rw_coord_commit_timeout_callback ctcb, uint32_t timeout)
    {
        Debug("[%lu] [shard %i] Sending RWCommitCoordinator", transaction_id, shard_idx_);

        ASSERT(transaction_id == the_transaction_.transaction_id());

        uint64_t req_id = last_req_id_++;
        ASSERT(!pending_rw_coord_commit_slot_.in_use);
        pending_rw_coord_commit_slot_.ccb = ccb;
        pending_rw_coord_commit_slot_.in_use = true;
        pending_rw_coord_commit_slot_.transaction_id = transaction_id;

        // TODO: Setup timeout
        rw_commit_c_.Clear();
        rw_commit_c_.mutable_rid()->set_client_id(client_id_);
        rw_commit_c_.mutable_rid()->set_client_req_id(req_id);
        rw_commit_c_.set_transaction_id(transaction_id);
        the_transaction_.serialize(rw_commit_c_.mutable_transaction());
        nonblock_timestamp.serialize((rw_commit_c_.mutable_nonblock_timestamp()));

        for (int p : participants)
        {
            rw_commit_c_.add_participants(p);
        }

        transport_->SendMessageToReplica(this, shard_idx_, replica_, MsgType::TXN_COMMIT_TYPE, rw_commit_c_);
    }

    void ShardClient::HandleRWCommitCoordinatorReply(const proto::RWCommitCoordinatorReply &reply)
    {
        uint64_t req_id = reply.rid().client_req_id();

        ASSERT(pending_rw_coord_commit_slot_.in_use); // hoping this isn't too conservative when we start having aborts?
        rw_coord_commit_callback &ccb = pending_rw_coord_commit_slot_.ccb;
        uint64_t transaction_id = pending_rw_coord_commit_slot_.transaction_id;
        pending_rw_coord_commit_slot_.in_use = false;

        ASSERT(transaction_id == the_transaction_.transaction_id());
        the_transaction_.clear();

        Debug("[shard %i] COMMIT timestamp %lu.%lu", shard_idx_,
              reply.commit_timestamp().timestamp(), reply.commit_timestamp().id());
        ccb(reply.status(), Timestamp(reply.commit_timestamp()), Timestamp(reply.nonblock_timestamp()));
    }

    void ShardClient::RWCommitParticipant(uint64_t transaction_id,
                                          int coordinator_shard, Timestamp &nonblock_timestamp,
                                          rw_part_commit_callback ccb, rw_part_commit_timeout_callback ctcb,
                                          uint32_t timeout)
    {
        Debug("[%lu] [shard %i] Sending RWCommitParticipant", transaction_id, shard_idx_);

        ASSERT(transaction_id == the_transaction_.transaction_id());

        uint64_t req_id = last_req_id_++;
        ASSERT(!pending_rw_part_commit_slot_.in_use);
        pending_rw_part_commit_slot_.ccb = ccb;
        pending_rw_part_commit_slot_.in_use = true;
        pending_rw_part_commit_slot_.transaction_id = transaction_id;

        // TODO: Setup timeout
        rw_commit_p_.Clear();
        rw_commit_p_.mutable_rid()->set_client_id(client_id_);
        rw_commit_p_.mutable_rid()->set_client_req_id(req_id);
        rw_commit_p_.set_transaction_id(transaction_id);
        the_transaction_.serialize(rw_commit_p_.mutable_transaction());
        rw_commit_p_.set_coordinator_shard(coordinator_shard);
        nonblock_timestamp.serialize((rw_commit_p_.mutable_nonblock_timestamp()));

        transport_->SendMessageToReplica(this, shard_idx_, replica_, MsgType::TXN_COMMIT_PART_TYPE, rw_commit_p_);
    }

    void ShardClient::HandleRWCommitParticipantReply(const proto::RWCommitParticipantReply &reply)
    {
        Debug("[shard %i] Received RWCommitParticipant", shard_idx_);
        uint64_t req_id = reply.rid().client_req_id();

        ASSERT(pending_rw_part_commit_slot_.in_use); // hoping this isn't too conservative when we start having aborts?
        rw_part_commit_callback ccb = pending_rw_part_commit_slot_.ccb;
        uint64_t transaction_id = pending_rw_part_commit_slot_.transaction_id;
        pending_rw_part_commit_slot_.in_use = false;

        ASSERT(transaction_id == the_transaction_.transaction_id());
        the_transaction_.clear();

        ccb(reply.status());
    }

    void ShardClient::PrepareOK(uint64_t transaction_id, int participant_shard,
                                const Timestamp &prepare_timestamp, const Timestamp &nonblock_ts,
                                prepare_callback pcb,
                                prepare_timeout_callback ptcb, uint32_t timeout)
    {
        Debug("[shard %i] Sending PrepareOK [%lu]", shard_idx_, transaction_id);

        uint64_t req_id = last_req_id_++;
        uint64_t idx = req_id % fanout_;
        auto &pendingPrepareOKSlot = pending_prepare_ok_slot_[idx];
        ASSERT(!pendingPrepareOKSlot.in_use);
        pendingPrepareOKSlot.in_use = true;
        pendingPrepareOKSlot.pcb = pcb;

        // TODO: Setup timeout
        prepare_ok_.mutable_rid()->set_client_id(client_id_);
        prepare_ok_.mutable_rid()->set_client_req_id(req_id);
        prepare_ok_.set_transaction_id(transaction_id);
        prepare_ok_.set_participant_shard(participant_shard);
        prepare_timestamp.serialize(prepare_ok_.mutable_prepare_timestamp());
        nonblock_ts.serialize(prepare_ok_.mutable_nonblock_timestamp());

        transport_->SendMessageToReplica(this, shard_idx_, replica_, MsgType::TXN_PREPARE_OK_TYPE, prepare_ok_);
    }

    void ShardClient::HandlePrepareOKReply(const proto::PrepareOKReply &reply)
    {
        Debug("[shard %i] Received PrepareOKReply", shard_idx_);
        uint64_t req_id = reply.rid().client_req_id();

        uint32_t idx = req_id % fanout_;
        auto &pendingPrepareOKSlot = pending_prepare_ok_slot_[idx];
        ASSERT(pendingPrepareOKSlot.in_use);
        prepare_callback pcb = pendingPrepareOKSlot.pcb;

        Debug("[shard %i] COMMIT timestamp [%lu.%lu]", shard_idx_,
              reply.commit_timestamp().timestamp(), reply.commit_timestamp().id());
        pcb(reply.status(), Timestamp(reply.commit_timestamp()));
    }

    void ShardClient::PrepareAbort(uint64_t transaction_id, int participant_shard,
                                   prepare_callback pcb,
                                   prepare_timeout_callback ptcb,
                                   uint32_t timeout)
    {
        Debug("[shard %i] Sending PrepareAbort [%lu]", shard_idx_, transaction_id);

        uint64_t req_id = last_req_id_++;
        PendingPrepareAbort *pendingPrepareAbort = new PendingPrepareAbort(transaction_id, req_id);
        pendingPrepareAborts[req_id] = pendingPrepareAbort;
        pendingPrepareAbort->pcb = pcb;
        pendingPrepareAbort->ptcb = ptcb;

        // TODO: Setup timeout
        prepare_abort_.mutable_rid()->set_client_id(client_id_);
        prepare_abort_.mutable_rid()->set_client_req_id(req_id);
        prepare_abort_.set_transaction_id(transaction_id);
        prepare_abort_.set_participant_shard(participant_shard);

        transport_->SendMessageToReplica(this, shard_idx_, replica_,
                                         prepare_abort_);
    }

    void ShardClient::HandlePrepareAbortReply(
        const proto::PrepareAbortReply &reply)
    {
        Debug("[shard %i] Received PrepareAbortReply", shard_idx_);
        uint64_t req_id = reply.rid().client_req_id();

        auto itr = pendingPrepareAborts.find(req_id);
        if (itr == pendingPrepareAborts.end())
        {
            Debug("[%d][%lu] PrepareAbortReply for stale request.", shard_idx_,
                  req_id);
            return; // stale request
        }

        PendingPrepareAbort *req = itr->second;
        prepare_callback pcb = req->pcb;
        pendingPrepareAborts.erase(itr);
        delete req;

        pcb(reply.status(), Timestamp());
    }

    void ShardClient::Abort(uint64_t transaction_id, abort_callback acb,
                            abort_timeout_callback atcb, uint32_t timeout)
    {
        Debug("[%lu] [shard %i] Sending Abort", transaction_id, shard_idx_);

        uint64_t req_id = last_req_id_++;
        ASSERT(!pending_abort_slot_.in_use);
        pending_abort_slot_.in_use = true;
        pending_abort_slot_.acb = acb;
        pending_abort_slot_.transaction_id = transaction_id;

        // TODO: Setup timeout
        abort_.Clear();
        abort_.mutable_rid()->set_client_id(client_id_);
        abort_.mutable_rid()->set_client_req_id(req_id);
        abort_.set_transaction_id(transaction_id);

        transport_->SendMessageToReplica(this, shard_idx_, replica_, MsgType::TXN_ABORT_TYPE, abort_);
    }

    void ShardClient::Wound(uint64_t transaction_id)
    {
        Debug("[%lu] [shard %i] Sending wound", transaction_id, shard_idx_);

        wound_.set_transaction_id(transaction_id);

        transport_->SendMessageToReplica(this, shard_idx_, replica_, MsgType::TXN_WOUND_TYPE, wound_);
    }

    void ShardClient::AbortGet(uint64_t transaction_id)
    {
        Debug("[%lu] [shard %i] Aborting GET", transaction_id, shard_idx_);

        // Loop through pending get slots
        for (uint32_t idx = 0; idx < fanout_; idx++)
        {
            auto &pendingGet = get_slots_[idx];
            if (pendingGet.in_use && pendingGet.transaction_id == transaction_id)
            {
                get_callback &gcb = pendingGet.gcb;
                std::string &key = pendingGet.key;

                pendingGet.in_use = false;

                gcb(REPLY_FAIL, key, "", {});
            }
        }
    }

    void ShardClient::AbortPut(uint64_t transaction_id)
    {
        Debug("[%lu] [shard %i] Aborting PUT", transaction_id, shard_idx_);

        Panic("No PUT in progress!");
    }

    void ShardClient::HandleAbortReply(const proto::AbortReply &reply)
    {
        Debug("[shard %i] Received HandleAbortReply for req_id %lu", shard_idx_, reply.rid().client_req_id());
        uint64_t req_id = reply.rid().client_req_id();

        ASSERT(pending_abort_slot_.in_use);
        uint64_t transaction_id = pending_abort_slot_.transaction_id;
        abort_callback acb = pending_abort_slot_.acb;
        pending_abort_slot_.in_use = false;

        if (reply.status() == REPLY_OK)
        {
            ASSERT(transaction_id == the_transaction_.transaction_id());
            the_transaction_.clear();
        }

        acb();
    }

} // namespace strongstore
