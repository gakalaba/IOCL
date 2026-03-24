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
        get_slots_.resize(fanout);
    }

    ShardClient::~ShardClient() {}
    void ShardClient::Close()
    {
    }

    void ShardClient::ReceiveMessage(const TransportAddress &remote,
                                     const std::string &type,
                                     const std::string &data, void *meta_data)
    {
        Debug("Got message wahoo");
        if (type == dummy_get_reply_.GetTypeName())
        {
            dummy_get_reply_.ParseFromString(data);
            HandleGetReply(dummy_get_reply_);
        }
        else if (type == dummy_reply_.GetTypeName())
        {
            dummy_reply_.ParseFromString(data);
            HandleSendOperationReply(dummy_reply_);
        }
        else if (type == dummy_commit_reply_.GetTypeName())
        {
            dummy_commit_reply_.ParseFromString(data);
            HandleRWCommitCoordinatorReply(dummy_commit_reply_);
        }
        else if (type == rw_commit_p_reply_.GetTypeName())
        {
            rw_commit_p_reply_.ParseFromString(data);
            HandleRWCommitParticipantReply(rw_commit_p_reply_);
        }
        else if (type == prepare_ok_reply_.GetTypeName())
        {
            prepare_ok_reply_.ParseFromString(data);
            HandlePrepareOKReply(prepare_ok_reply_);
        }
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
        else if (type == abort_reply_.GetTypeName())
        {
            abort_reply_.ParseFromString(data);
            HandleAbortReply(abort_reply_);
        }
        else if (type == wound_.GetTypeName())
        {
            wound_.ParseFromString(data);
            HandleWound(wound_);
        }
        else
        {
            Panic("Received unexpected message type: %s", type.c_str());
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
        Debug("[%lu] [shard %i] BEGIN", transaction_id, shard_idx_);

        auto search = transactions_.find(transaction_id);
        ASSERT(search == transactions_.end());

        auto &t = transactions_[transaction_id];

        t.set_start_time(start_time);
    }

    bool ShardClient::CheckPriorReadsAndWrites(uint64_t transaction_id, const std::string &key, get_callback gcb)
    {
        auto search = transactions_.find(transaction_id);
        if (search == transactions_.end())
        {
            return false;
        }

        auto &txn = search->second;

        // Read your own writes, check the write set first.
        auto wsearch = txn.getWriteSet().find(key);
        if (wsearch != txn.getWriteSet().end())
        {
            gcb(REPLY_OK, key, wsearch->second, Timestamp());
            return true;
        }

        // Consistent reads, check the read set.
        auto rssearch = read_sets_.find(transaction_id);
        if (rssearch != read_sets_.end())
        {
            auto &read_set = rssearch->second;
            auto rsearch = read_set.find(key);
            if (rsearch != read_set.end())
            {
                gcb(REPLY_OK, key, rsearch->second, Timestamp());
                return true;
            }
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
        uint64_t shardtag = CreateTag(client_id_, req_id);
        uint32_t idx = req_id % fanout_;
        auto &pendingGet = get_slots_[idx];
        ASSERT(!pendingGet.in_use);
        pendingGet.in_use = true;

        // PendingGet *pendingGet = new PendingGet(transaction_id, req_id);
        // pendingGets[shardtag] = pendingGet;
        // pendingGet->key = key;
        pendingGet.gcb = gcb;
        // pendingGet->gtcb = gtcb;

        // auto search = transactions_.find(transaction_id);
        // ASSERT(search != transactions_.end());
        // auto &t = search->second;
        // auto &start_ts = t.start_time();

        // TODO: Setup timeout
        dummy_get_.Clear();
        dummy_get_.set_req_id(shardtag);
        // get_.Clear();
        // get_.mutable_rid()->set_client_id(client_id_);
        // get_.mutable_rid()->set_client_req_id(req_id);
        // get_.set_transaction_id(transaction_id);
        // start_ts.serialize(get_.mutable_timestamp());
        // get_.set_key(key);
        // get_.set_for_update(for_update);

        transport_->SendMessageToReplica(this, shard_idx_, replica_, dummy_get_);
        // transport_->SendMessageToReplica(this, shard_idx_, replica_, get_);
    }

    void ShardClient::HandleGetReply(const proto::DummyGetReply &reply)
    {
        // uint64_t req_id = reply.rid().client_req_id();
        // int status = reply.status();

        // auto itr = pendingGets.find(req_id);
        // auto itr = pendingGets.find(reply.req_id());
        // if (itr == pendingGets.end())
        // {
        //     Panic("Didn't find pending GET request for req_id %lu!", reply.req_id());
        //     // Debug("[%d][%lu] GetReply for stale request for req_id %lu.", shard_idx_, req_id, req_id);
        //     return; // stale request
        // }

        uint64_t req_id = reply.req_id();
        uint32_t idx = (req_id & 0xFFFFFFFF) % fanout_;
        auto &pendingGet = get_slots_[idx];
        ASSERT(pendingGet.in_use);
        // PendingGet *req = itr->second;
        Debug("Handling GET reply with req_id = %lu", reply.req_id());
        get_callback gcb = pendingGet.gcb;
        // std::string key = req->key;
        // pendingGets.erase(itr);
        // delete req;

        // Debug("[%lu] [shard %i] Received GET reply: %s %d",
        //       transaction_id, shard_idx_, key.c_str(), status);

        // std::string val;
        // Timestamp ts;
        // if (status == REPLY_OK)
        // {
        //     val = reply.val();
        //     ts = Timestamp(reply.timestamp());
        // }

        // Debug("[%lu] Added %lu.%lu to read set.", transaction_id, ts.getTimestamp(), ts.getID());
        // transactions_[transaction_id].addReadSet(key, ts);
        // read_sets_[transaction_id][key] = val;

        pendingGet.in_use = false;
        gcb(0, "", "", dummyTimestamp);
    }

    void ShardClient::Put(uint64_t transaction_id, const std::string &key, const std::string &value,
                          put_callback pcb, put_timeout_callback ptcb,
                          uint32_t timeout)
    {
        auto search = transactions_.find(transaction_id);
        ASSERT(search != transactions_.end());

        auto &t = search->second;
        t.addWriteSet(key, value);

        pcb(REPLY_OK, key, value);
    }

    // IOCL issue a request
    void ShardClient::SendOperation(uint64_t app_request_id, const std::string op,
                                  const std::string &key, const std::string &value,
                                  op_callback ocb, op_timeout_callback otcb,
                                  uint32_t timeout,
                                  std::list<std::pair<uint64_t, uint32_t>> &outstandingOperationList,
                                  std::list<uint16_t> &outstandingOperationRefCount,
                                  bool isIOCL)
    {
        // Send the operation to appropriate shard.
        Debug("[shard %i] AppReqiest Sending Operation %s(%s, %s)", shard_idx_, op.c_str(), key.c_str(), value.c_str());

        uint64_t req_id = last_req_id_++;
        uint64_t myshardtag = CreateTag(client_id_, req_id);
        Debug("Storing the request in pendingReqs with app_request_id = %lu and its reqid = %lu", app_request_id, req_id);
        // PendingOperation *pendingOp = new PendingOperation(app_request_id, req_id);
        uint32_t idx = req_id % fanout_;
        auto &pendingOp = slots_[idx];
        ASSERT(!pendingOp.in_use);
        pendingOp.in_use = true;
        pendingOp.ocb = ocb;
        // pendingOp->op = op;
        // pendingOp->key = key;
        // pendingOp->val = value;
        // pendingOp->ocb = ocb;
        // pendingOp->otcb = otcb;

        // TODO: Setup timeout
        dummy_op_.Clear();
        dummy_op_.set_req_id(myshardtag);
        dummy_op_.set_idx(0);
        // op_.Clear();
        // op_.mutable_rid()->set_client_id(client_id_);
        // op_.mutable_rid()->set_client_req_id(req_id);
        // op_.set_transaction_id(app_request_id);
        // op_.set_key(key);
        // op_.set_value(value);
        // op_.set_op(op);

        // Set the optional fields (myshardtag and pred_list) if IOCL
        // if (isIOCL)
        // {
        //     Debug("IT IS IOCL!!! Setting myshardtag and pred_list");
        //     uint64_t myshardtag = CreateTag(client_id_, seqno);
        //     Debug("this client_id_ = %lu, this seqno at this shard is %lu, and myshardtag = %lu", client_id_, seqno, myshardtag);
        //     seqno++;
        //     op_.set_shardtag(myshardtag);
        //     op_.set_intkey(std::stoull(key)); // for iocl optimization

        //     // Construct predecessor list
        //     auto it1 = outstandingOperationList.begin();
        //     auto it2 = outstandingOperationRefCount.begin();
        //     pendingOp->pred_list.reserve(outstandingOperationList.size());
        //     while (it1 != outstandingOperationList.end() && it2 != outstandingOperationRefCount.end()) {
        //         // increment refcount entry
        //         (*it2)++;
        //         // Add this entry to predecessor list and the RPC message
        //         op_.add_predlist((*it1).first);
        //         op_.add_shardlist((*it1).second);
        //         pendingOp->pred_list.push_back(*it1);
        //         Debug("Added predecessor tag = %lu with shard idx %u", (*it1).first, (*it1).second);
        //         ++it1;
        //         ++it2;
        //     }
        //     // Add self to outstanding operations and refcount lists
        //     outstandingOperationList.push_back(std::make_pair(myshardtag, shard_idx_));
        //     outstandingOperationRefCount.push_back(1);
        //     // Print the outstnadingOperationsList and the outstnaidngOperationRefCount in a single loop
        //     auto itl = outstandingOperationList.begin();
        //     auto itr = outstandingOperationRefCount.begin();
        //     for (;
        //          itl != outstandingOperationList.end() && itr != outstandingOperationRefCount.end();
        //          ++itl, ++itr) {
        //         Debug("(tag %lu at shard %u) has refcount %u", itl->first, itl->second, *itr);
        //     }
        //     Debug("the size of the op is %lu", op_.ByteSizeLong());
        // } else {
        //     Debug("Not IOCL, so not setting myshardtag and pred_list");
        //     Debug("the size of the op is %lu", op_.ByteSizeLong());
        // }

        Debug("The shard client is sending the message to replica where shard_idx = %d and replica_ = %d", shard_idx_, replica_);
        // transport_->SendMessageToReplica(this, shard_idx_, replica_, op_);
        transport_->SendMessageToReplica(this, shard_idx_, replica_, dummy_op_);
    }

    // IOCL receive the response
    // void ShardClient::HandleSendOperationReply(const proto::LinearizeableReply &reply)
    void ShardClient::HandleSendOperationReply(const proto::DummyReply &reply)
    {
        // Debug("shard client got LinearizeableReply!");
        // uint64_t req_id = reply.rid().client_req_id();
        uint64_t req_id = reply.req_id();
        Debug("Shard client got reply for client_id = %d and req_id = %lu", client_id_, req_id);
        // Debug("the app_request_id = %lu", req_id);
        // int status = reply.status();
        // string retval = reply.return_value();

        uint32_t idx = (req_id & 0xFFFFFFFF) % fanout_;
        auto &pendingOp = slots_[idx];
        ASSERT(pendingOp.in_use);

        // uint64_t app_request_id = op->transaction_id;
        op_callback ocb = std::move(pendingOp.ocb); // wrapped in move to make efficient
        std::vector<std::pair<uint64_t, uint32_t>> pred_list = std::move(pendingOp.pred_list);
        // Debug("moving the pred_list of size %lu", pred_list.size());
        // delete op;
        pendingOp.in_use = false;

        // Debug("[shard %i] Received SendOperation (part of app request %lu) reply with status %d and return value %s",
        //       shard_idx_, app_request_id, status, retval.c_str());

        // maybe we could compare the vals from reply.val and req.val to make sure it's all marshalled right?

        ocb(0, "", pred_list);
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

        // auto search = transactions_.find(transaction_id);
        // ASSERT(search != transactions_.end());

        // const auto &t = search->second;

        uint64_t req_id = last_req_id_++;
        // PendingRWCoordCommit *pendingCommit = new PendingRWCoordCommit(transaction_id, req_id);
        // pendingRWCoordCommits[transaction_id] = pendingCommit;
        ASSERT(pending_commit_slot_.in_use == false);
        pending_commit_slot_.ccb = ccb;
        pending_commit_slot_.in_use = true;
        // pendingCommit->ccb = ccb;
        // pendingCommit->ctcb = ctcb;
        Debug("and added to pendingRWCoordCommits with req_id = %d and (key) transaction_id = %lu", req_id, transaction_id);

        // TODO: Setup timeout
        dummy_commit_.Clear();
        dummy_commit_.set_req_id(transaction_id);
        dummy_commit_.set_idx(0);
        // rw_commit_c_.Clear();
        // rw_commit_c_.mutable_rid()->set_client_id(client_id_);
        // rw_commit_c_.mutable_rid()->set_client_req_id(req_id);
        // rw_commit_c_.set_transaction_id(transaction_id);
        // t.serialize(rw_commit_c_.mutable_transaction());
        // nonblock_timestamp.serialize((rw_commit_c_.mutable_nonblock_timestamp()));

        // for (int p : participants)
        // {
        //     rw_commit_c_.add_participants(p);
        // }

        // transport_->SendMessageToReplica(this, shard_idx_, replica_, rw_commit_c_);
        transport_->SendMessageToReplica(this, shard_idx_, replica_, dummy_commit_);
    }

    void ShardClient::HandleRWCommitCoordinatorReply(const proto::DummyCommitReply &reply)
    {
        // uint64_t req_id = reply.rid().client_req_id();
        uint64_t req_id = reply.req_id();
        Debug("Got RWCommitCoordinatorReply for req_id = %lu", req_id);

        // auto itr = pendingRWCoordCommits.find(req_id);
        // if (itr == pendingRWCoordCommits.end())
        // {
        //     Debug("[%d][%lu] RWCommitCoordinatorReply for stale request.", shard_idx_, req_id);
        //     return; // stale request
        // }

        // PendingRWCoordCommit *req = itr->second;
        // uint64_t transaction_id = req->transaction_id;
        // rw_coord_commit_callback ccb = req->ccb;
        ASSERT(pending_commit_slot_.in_use);
        rw_coord_commit_callback ccb = pending_commit_slot_.ccb;
        // pendingRWCoordCommits.erase(itr);
        // delete req;
        pending_commit_slot_.in_use = false;

        // transactions_.erase(transaction_id);
        // read_sets_.erase(transaction_id);

        // Debug("[shard %i] COMMIT timestamp %lu.%lu", shard_idx_,
        //       reply.commit_timestamp().timestamp(), reply.commit_timestamp().id());
        // ccb(reply.status(), Timestamp(reply.commit_timestamp()), Timestamp(reply.nonblock_timestamp()));
        ccb(0, dummyTimestamp, dummyTimestamp);
    }

    void ShardClient::RWCommitParticipant(uint64_t transaction_id,
                                          int coordinator_shard, Timestamp &nonblock_timestamp,
                                          rw_part_commit_callback ccb, rw_part_commit_timeout_callback ctcb,
                                          uint32_t timeout)
    {
        Debug("[%lu] [shard %i] Sending RWCommitParticipant", transaction_id, shard_idx_);

        auto search = transactions_.find(transaction_id);
        ASSERT(search != transactions_.end());

        const auto &t = search->second;

        uint64_t req_id = last_req_id_++;
        PendingRWParticipantCommit *pendingCommit = new PendingRWParticipantCommit(transaction_id, req_id);
        pendingRWParticipantCommits[req_id] = pendingCommit;
        pendingCommit->ccb = ccb;
        pendingCommit->ctcb = ctcb;

        // TODO: Setup timeout
        rw_commit_p_.Clear();
        rw_commit_p_.mutable_rid()->set_client_id(client_id_);
        rw_commit_p_.mutable_rid()->set_client_req_id(req_id);
        rw_commit_p_.set_transaction_id(transaction_id);
        t.serialize(rw_commit_p_.mutable_transaction());
        rw_commit_p_.set_coordinator_shard(coordinator_shard);
        nonblock_timestamp.serialize((rw_commit_p_.mutable_nonblock_timestamp()));

        transport_->SendMessageToReplica(this, shard_idx_, replica_, rw_commit_p_);
    }

    void ShardClient::HandleRWCommitParticipantReply(const proto::RWCommitParticipantReply &reply)
    {
        Debug("[shard %i] Received RWCommitParticipant", shard_idx_);
        uint64_t req_id = reply.rid().client_req_id();

        auto itr = pendingRWParticipantCommits.find(req_id);
        if (itr == pendingRWParticipantCommits.end())
        {
            Debug("[%d][%lu] RWCommitParticipantReply for stale request.", shard_idx_, req_id);
            return; // stale request
        }

        PendingRWParticipantCommit *req = itr->second;
        uint64_t transaction_id = req->transaction_id;
        rw_part_commit_callback ccb = req->ccb;
        pendingRWParticipantCommits.erase(itr);
        delete req;

        transactions_.erase(transaction_id);
        read_sets_.erase(transaction_id);

        ccb(reply.status());
    }

    void ShardClient::PrepareOK(uint64_t transaction_id, int participant_shard,
                                const Timestamp &prepare_timestamp, const Timestamp &nonblock_ts,
                                prepare_callback pcb,
                                prepare_timeout_callback ptcb, uint32_t timeout)
    {
        Debug("[shard %i] Sending PrepareOK [%lu]", shard_idx_, transaction_id);

        uint64_t req_id = last_req_id_++;
        PendingPrepareOK *pendingPrepareOK = new PendingPrepareOK(transaction_id, req_id);
        pendingPrepareOKs[req_id] = pendingPrepareOK;
        pendingPrepareOK->pcb = pcb;
        pendingPrepareOK->ptcb = ptcb;

        // TODO: Setup timeout
        prepare_ok_.mutable_rid()->set_client_id(client_id_);
        prepare_ok_.mutable_rid()->set_client_req_id(req_id);
        prepare_ok_.set_transaction_id(transaction_id);
        prepare_ok_.set_participant_shard(participant_shard);
        prepare_timestamp.serialize(prepare_ok_.mutable_prepare_timestamp());
        nonblock_ts.serialize(prepare_ok_.mutable_nonblock_timestamp());

        transport_->SendMessageToReplica(this, shard_idx_, replica_, prepare_ok_);
    }

    void ShardClient::HandlePrepareOKReply(const proto::PrepareOKReply &reply)
    {
        Debug("[shard %i] Received PrepareOKReply", shard_idx_);
        uint64_t req_id = reply.rid().client_req_id();

        auto itr = pendingPrepareOKs.find(req_id);
        if (itr == pendingPrepareOKs.end())
        {
            Debug("[%d][%lu] PrepareOKReply for stale request.", shard_idx_,
                  req_id);
            return; // stale request
        }

        PendingPrepareOK *req = itr->second;
        prepare_callback pcb = req->pcb;
        pendingPrepareOKs.erase(itr);
        delete req;

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
        PendingAbort *pendingAbort = new PendingAbort(transaction_id, req_id);
        pendingAborts[req_id] = pendingAbort;
        pendingAbort->acb = acb;
        pendingAbort->atcb = atcb;

        // TODO: Setup timeout
        abort_.Clear();
        abort_.mutable_rid()->set_client_id(client_id_);
        abort_.mutable_rid()->set_client_req_id(req_id);
        abort_.set_transaction_id(transaction_id);

        transport_->SendMessageToReplica(this, shard_idx_, replica_, abort_);
    }

    void ShardClient::Wound(uint64_t transaction_id)
    {
        Debug("[%lu] [shard %i] Sending wound", transaction_id, shard_idx_);

        wound_.set_transaction_id(transaction_id);

        transport_->SendMessageToReplica(this, shard_idx_, replica_, wound_);
    }

    void ShardClient::AbortGet(uint64_t transaction_id)
    {
        Debug("[%lu] [shard %i] Aborting GET", transaction_id, shard_idx_);

        for (auto it = pendingGets.begin(); it != pendingGets.end(); )
        {
            if (it->second->transaction_id == transaction_id)
            {
                PendingGet *req = it->second;
                uint64_t transaction_id = req->transaction_id;
                get_callback gcb = req->gcb;
                std::string key = req->key;

                it = pendingGets.erase(it);
                delete req;

                gcb(REPLY_FAIL, key, "", {});
            } else {
                ++it;
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

        auto itr = pendingAborts.find(req_id);
        if (itr == pendingAborts.end())
        {
            Debug("[%d][%lu] HandleAbortReply for stale request.", shard_idx_,
                  req_id);
            return; // stale request
        }

        PendingAbort *req = itr->second;
        uint64_t transaction_id = req->transaction_id;
        abort_callback acb = req->acb;
        pendingAborts.erase(itr);
        delete req;

        if (reply.status() == REPLY_OK)
        {
            transactions_.erase(transaction_id);
            read_sets_.erase(transaction_id);
        }

        acb();
    }

} // namespace strongstore
