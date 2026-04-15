// -*- mode: c++; c-file-style: "k&r"; c-basic-offset: 4 -*-
/***********************************************************************
 *
 * iocl_ct/replica.cc:
 *   IOCL Constant Time protocol
 *
 * Copyright 2025 Anja Kalaba <akalaba@princeton.edu>
 * Copyright 2022 Jeffrey Helt, Matthew Burke, Amit Levy, Wyatt Lloyd
 * Copyright 2013 Dan R. K. Ports  <drkp@cs.washington.edu>
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

#include "replication/common/replica.h"

#include <algorithm>
#include <unordered_set>

#include "lib/assert.h"
#include "lib/configuration.h"
#include "lib/message.h"
#include "lib/transport.h"
#include "replication/iocl_ct/replica.h"
#include "replication/iocl_ct/iocl_ct-proto.pb.h"

#define RDebug(fmt, ...) Debug("[%d] " fmt, myIdx, ##__VA_ARGS__)
#define RNotice(fmt, ...) Notice("[%d] " fmt, myIdx, ##__VA_ARGS__)
#define RWarning(fmt, ...) Warning("[%d] " fmt, myIdx, ##__VA_ARGS__)
#define RPanic(fmt, ...) Panic("[%d] " fmt, myIdx, ##__VA_ARGS__)

namespace replication
{
    namespace iocl_ct
    {

        using namespace proto;

        IOCL_CTReplica::IOCL_CTReplica(transport::Configuration config, int groupIdx, int myIdx,
                             Transport *transport, unsigned int batchSize,
                             AppReplica *app, bool debug_stats)
            : Replica(config, groupIdx, myIdx, transport, app),
              batchSize(batchSize),
              log(false),
              startViewChangeQuorum(config.QuorumSize() - 1, config.n),
              doViewChangeQuorum(config.QuorumSize() - 1, config.n),
              debug_stats_{debug_stats}
        {
            this->status = STATUS_NORMAL;
            this->view = 0;
            this->lastOp = 0;
            this->lastUnorderedOp = 0;
            this->lastCommitted = 0;
            this->lastRequestStateTransferView = 0;
            this->lastRequestStateTransferOpnum = 0;
            lastBatchEnd = 0;
            lastUnorderedBatchEnd = 0;
            Q = config.QuorumSize() - 1;

            if (batchSize > 1)
            {
                Notice("Batching enabled; batch size %d", batchSize);
            }

            this->viewChangeTimeout =
                new Timeout(transport, 5000, [this]()
                            { StartViewChange(view + 1); });
            this->nullCommitTimeout =
                new Timeout(transport, 1000, [this]()
                            { SendNullCommit(); });
            this->stateTransferTimeout = new Timeout(transport, 1000, [this]()
                                                     {
                this->lastRequestStateTransferView = 0;
                this->lastRequestStateTransferOpnum = 0; });
            this->stateTransferTimeout->Start();
            this->resendPrepareTimeout =
                new Timeout(transport, 500, [this]()
                            { ResendPrepare(); });
            this->closeBatchTimeout =
                new Timeout(transport, 300, [this]()
                            { CloseBatch(); });
            this->resendUnorderedPrepareTimeout =
                new Timeout(transport, 30000, [this]()
                            { ResendUnorderedPrepare(); });
            this->closeUnorderedBatchTimeout =
                new Timeout(transport, 300, [this]()
                            { CloseUnorderedBatch(); });

            if (AmLeader())
            {
                nullCommitTimeout->Start();
            }
            else
            {
                viewChangeTimeout->Start();
            }

            if (debug_stats_)
            {
                _Latency_Init(&rec_to_upcall_lat_, "rec_to_upcall");
                _Latency_Init(&upcall_to_exec_lat_, "upcall_to_exec");
                _Latency_Init(&exec_to_sent_lat_, "exec_to_sent");
            }

            // Avoid slow down from rehashing of maps
            entryStore.reserve(200000);
            log.reserve(200000);
            // shardtagToEntryIdx.reserve(200000);
            shardTS = 0;
        }

        IOCL_CTReplica::~IOCL_CTReplica()
        {
            delete viewChangeTimeout;
            delete nullCommitTimeout;
            delete stateTransferTimeout;
            delete resendPrepareTimeout;
            delete closeBatchTimeout;

            if (debug_stats_)
            {
                Latency_Dump(&rec_to_upcall_lat_);
                Latency_Dump(&upcall_to_exec_lat_);
                Latency_Dump(&exec_to_sent_lat_);
            }

            for (auto &kv : pendingPrepares)
            {
                delete kv.first;
            }
        }

        void IOCL_CTReplica::AppendToLog(opnum_t new_entry_opnum, uint32_t idx)
        {
            opnum_t start = 1;
            if (log.empty()) {
                ASSERT(new_entry_opnum == start);
            } else {
                uint32_t last_idx = log.back();
                const IoclEntry &lastEntry = Entry(last_idx);
                ASSERT(new_entry_opnum == lastEntry.viewstamp.opnum+1);
            }

            log.push_back(idx);
        }

        // This really ought to be const
        IoclEntry *IOCL_CTReplica::FindInLog(opnum_t opnum)
        {
            opnum_t start = 1;
            if (log.empty()) {
                return NULL;
            }

            if (opnum < start) {
                return NULL;
            }

            if (opnum-start > log.size()-1) {
                return NULL;
            }

            uint32_t idx = log[opnum-start];
            ASSERT(idx >= 0 && idx < entryStore.size());
            IoclEntry &entry = Entry(idx);
            ASSERT(entry.viewstamp.opnum == opnum);
            return &entry;
        }

        viewstamp_t IOCL_CTReplica::LastViewstampOfLog() const
        {
            if (log.empty()) {
                return viewstamp_t(0, 0);
            } else {
                uint32_t last_idx = log.back();
                const IoclEntry &lastEntry = Entry(last_idx);
                return lastEntry.viewstamp;
            }
        }

        bool IOCL_CTReplica::AmLeader() const
        {
            return (configuration.GetLeaderIndex(view) == myIdx);
        }

        void IOCL_CTReplica::CommitUpTo(opnum_t upto)
        {
            Debug("CommitUpTo for opnum %lu!!", upto);
            while (lastCommitted < upto)
            {
                lastCommitted++;

                /* Find operation in log */
                IoclEntry *entry = FindInLog(lastCommitted);
                if (entry == nullptr)
                {
                    RPanic("Did not find operation " FMT_OPNUM " in log",
                           lastCommitted);
                }

                /* Mark it as committed */
                entry->state = IOCL_STATE_COMMITTED;
                if (!AmLeader()) {
                    /* Replica path: */
                    /* Execute it */
                    ReplicaUpcall(entry->request);
                } else {
                    /* If there are other ops that haven't received their
                    N final ACKs, then this op must block behind them.
                    Similarly, if there is no sublog, but this op hasn't
                    received all N final ACKs, insert it to the log and
                    do NOT execute it (for now replicas do, since we don't
                    track acks there, and i don't want to implement the final
                    ACK probably via piggybacking mechanism)*/
                    if ((perKeySubLogs.find(entry->intkey) != perKeySubLogs.end()) ||
                        (entry->final_ack_count != entry->num_predecessors)) {
                        Debug("I'm in this case where .... entry->final_ack_count = %u and entry->num_predecessors = %u", entry->final_ack_count, entry->num_predecessors);
                        Debug("And existence = %d", perKeySubLogs.find(entry->intkey) != perKeySubLogs.end());
                        // Warning("Not committing operation " FMT_OPNUM " because not all predecessor final ACKs have arrived (%d/%d)",
                        //         lastCommitted, entry->final_ack_count, entry->num_predecessors);
                        ASSERT(entry->final_ack_count <= entry->num_predecessors);
                        // Add ourselves to the perKeySubLog to be executed when all N Acks arrive
                        auto &sublog = perKeySubLogs[entry->intkey];
                        sublog.ops.push_back(lastCommitted);
                    } else {
                        /* Execute it */
                        Debug("ACTUALLY i'm hre in this case going to call ReplicaUpcall for opnum " FMT_OPNUM, lastCommitted);
                        ASSERT(entry->final_ack_count == entry->num_predecessors);
                        ASSERT(perKeySubLogs.find(entry->intkey) == perKeySubLogs.end());
                        /* We can immediately execute this entry */
                        ReplicaUpcall(entry->request);
                    }
                }
            }
        }

        void IOCL_CTReplica::SendPrepareOKs(opnum_t oldLastOp)
        {
            /* Send PREPAREOKs for new uncommitted operations */
            for (opnum_t i = oldLastOp; i <= lastOp; i++)
            {
                /* It has to be new *and* uncommitted */
                if (i <= lastCommitted)
                {
                    continue;
                }

                const IoclEntry *entry = FindInLog(i);
                if (!entry)
                {
                    RPanic("Did not find operation " FMT_OPNUM " in log", i);
                }
                ASSERT(entry->state == IOCL_STATE_PREPARED);
                // UpdateClientTable(entry->request);

                PrepareOKMessage reply;
                reply.set_view(view);
                reply.set_opnum(i);
                reply.set_replicaidx(myIdx);

                RDebug("Sending PREPAREOK " FMT_VIEWSTAMP
                       " for new uncommitted operation",
                       reply.view(), reply.opnum());

                if (!(transport->SendMessageToReplica(
                        this, configuration.GetLeaderIndex(view), reply)))
                {
                    RWarning("Failed to send PrepareOK message to leader");
                }
            }
        }

        void IOCL_CTReplica::RequestStateTransfer()
        {
            Panic("Shouldn't be calling this");
            RequestStateTransferMessage m;
            m.set_view(view);
            m.set_opnum(lastCommitted);

            if ((lastRequestStateTransferOpnum != 0) &&
                (lastRequestStateTransferView == view) &&
                (lastRequestStateTransferOpnum == lastCommitted))
            {
                RDebug("Skipping state transfer request " FMT_VIEWSTAMP
                       " because we already requested it",
                       view, lastCommitted);
                return;
            }

            RNotice("Requesting state transfer: " FMT_VIEWSTAMP, view, lastCommitted);

            this->lastRequestStateTransferView = view;
            this->lastRequestStateTransferOpnum = lastCommitted;

            if (!transport->SendMessageToAll(this, m))
            {
                RWarning("Failed to send RequestStateTransfer message to all replicas");
            }
        }

        void IOCL_CTReplica::EnterView(view_t newview)
        {
            RNotice("Entering new view " FMT_VIEW, newview);

            view = newview;
            status = STATUS_NORMAL;
            lastBatchEnd = lastOp;

            if (AmLeader())
            {
                viewChangeTimeout->Stop();
                nullCommitTimeout->Start();
            }
            else
            {
                viewChangeTimeout->Start();
                nullCommitTimeout->Stop();
                resendPrepareTimeout->Stop();
                closeBatchTimeout->Stop();
            }

            startViewChangeQuorum.Clear();
            doViewChangeQuorum.Clear();
        }

        void IOCL_CTReplica::StartViewChange(view_t newview)
        {
            RNotice("Starting view change for view " FMT_VIEW, newview);
            return;

            view = newview;
            status = STATUS_VIEW_CHANGE;

            viewChangeTimeout->Reset();
            nullCommitTimeout->Stop();
            resendPrepareTimeout->Stop();
            closeBatchTimeout->Stop();

            StartViewChangeMessage m;
            m.set_view(newview);
            m.set_replicaidx(myIdx);
            m.set_lastcommitted(lastCommitted);

            if (!transport->SendMessageToAll(this, m))
            {
                RWarning("Failed to send StartViewChange message to all replicas");
            }
        }

        void IOCL_CTReplica::SendNullCommit()
        {
            Debug("Sending null commit");
            CommitMessage cm;
            cm.set_view(this->view);
            cm.set_opnum(this->lastCommitted);

            ASSERT(AmLeader());

            if (!(transport->SendMessageToAll(this, MsgType::COMMIT_TYPE, cm)))
            {
                RWarning("Failed to send null COMMIT message to all replicas");
            }

            nullCommitTimeout->Reset();
        }

        // void IOCL_CTReplica::UpdateClientTable(const Request &req)
        // {
        //     Panic("Shouldn't be calling this right now");
        //     ClientTableEntry &entry = clientTable[req.clientid()];
        //     Debug("the request has clientid %lu and clientreqid %lu",
        //            req.clientid(), req.clientreqid());
        //     Debug("we are checking entry.lastReqId (= %lu) < req.clientreqid (= %lu)",
        //            entry.lastReqId, req.clientreqid());

        //     if (entry.lastReqId > req.clientreqid()) {

        //         Panic("we are checking entry.lastReqId (= %lu) < req.clientreqid (= %lu)",
        //            entry.lastReqId, req.clientreqid());
        //     }

        //     if (entry.lastReqId == req.clientreqid())
        //     {
        //         return;
        //     }

        //     entry.lastReqId = req.clientreqid();
        //     entry.replied = false;
        //     entry.reply.Clear();
        // }

        void IOCL_CTReplica::ResendPrepare()
        {
            ASSERT(AmLeader());
            if (lastOp == lastCommitted)
            {
                return;
            }
            uint32_t idx = log[lastPrepare.opnum()-1];
            const IoclEntry& entry = Entry(idx);
            RNotice("Resending prepare with opnum = " FMT_OPNUM, lastPrepare.opnum());
            RNotice("The lastPrepare was for shardtag = %lu and clientid %lu ", entry.myShardTag, entry.request.rid().client_id());
            if (!(transport->SendMessageToAll(this, MsgType::PREPARE_TYPE, lastPrepare)))
            {
                RWarning("Failed to ressend prepare message to all replicas");
            }
            // Keep retrying
            resendPrepareTimeout->Reset();
        }

        void IOCL_CTReplica::ResendUnorderedPrepare()
        {
            ASSERT(AmLeader());
            if (entryStore.empty())
            {
                return;
            }
            RNotice("Resending unordered prepare for last message with shardtag = %lu and from clientid %lu", lastUnorderedPrepare.request(0).shardtag(), lastUnorderedPrepare.request(0).rid().client_id());
            if (!(transport->SendMessageToAll(this, MsgType::UNORDERED_PREPARE_TYPE, lastUnorderedPrepare)))
            {
                RWarning("Failed to ressend prepare message to all replicas");
            }
            // Keep retrying
            resendUnorderedPrepareTimeout->Reset();
        }

        void IOCL_CTReplica::CloseUnorderedBatch()
        {
            ASSERT(AmLeader());
            ASSERT(lastUnorderedBatchEnd < lastUnorderedOp);
            /* Send the unordered prepare messages */
            opnum_t unorderedBatchStart = lastUnorderedBatchEnd + 1;
            int batchSize = lastUnorderedOp - unorderedBatchStart + 1;

            UnorderedPrepareMessage &up = lastUnorderedPrepare;
            up.Clear();
            up.set_view(view);
            up.set_opnum(lastUnorderedOp);
            up.set_batchstart(unorderedBatchStart);

            auto *reqs = up.mutable_request();
            reqs->Reserve(batchSize);

            for (opnum_t i = unorderedBatchStart; i <= lastUnorderedOp; i++)
            {
                const IoclEntry& entry = Entry(i-1);
                ASSERT(entry.viewstamp.view == view);
                reqs->Add()->CopyFrom(entry.request);
            }

            if (!(transport->SendMessageToAll(this, MsgType::UNORDERED_PREPARE_TYPE, up)))
            {
                RWarning("Failed to send UNORDERED_PREPARE message to all replicas");
            }
            lastUnorderedBatchEnd = lastUnorderedOp;

            resendUnorderedPrepareTimeout->Reset();
            closeUnorderedBatchTimeout->Stop();

        }

        void IOCL_CTReplica::CloseBatch()
        {
            ASSERT(AmLeader());
            ASSERT(lastBatchEnd < lastOp);

            opnum_t batchStart = lastBatchEnd + 1;
            int batchSize = lastOp - batchStart + 1;

            RDebug("Sending batched prepare from " FMT_OPNUM " to " FMT_OPNUM,
                   batchStart, lastOp);
            /* Send prepare messages */
            PrepareMessage &p = lastPrepare;
            p.Clear();
            p.set_view(view);
            p.set_opnum(lastOp);
            p.set_batchstart(batchStart);

            auto *shardtags = p.mutable_shardtags();
            auto *chains = p.mutable_timestamp_chains();

            shardtags->Reserve(batchSize);

            for (opnum_t i = batchStart; i <= lastOp; i++)
            {
                uint32_t idx = log[i-1];
                IoclEntry &entry = Entry(idx);
                ASSERT(entry.viewstamp.view == view);
                ASSERT(entry.viewstamp.opnum == i);
                ASSERT(entry.predecessorArrivalTs.size() == entry.num_predecessors);

                shardtags->Add(entry.myShardTag);

                chains->Reserve(chains->size() + entry.num_predecessors + 1);
                for (uint64_t ts : entry.predecessorArrivalTs) {
                    chains->Add(ts);
                }
                chains->Add(entry.finalTs);
                Debug("Sending shardtag = %lu and opnum = %lu and finalTs = %lu", entry.myShardTag, entry.viewstamp.opnum, entry.finalTs);
            }

            if (!(transport->SendMessageToAll(this, MsgType::PREPARE_TYPE, p)))
            {
                RWarning("Failed to send prepare message to all replicas");
            }
            lastBatchEnd = lastOp;

            resendPrepareTimeout->Reset();
            closeBatchTimeout->Stop();
        }

        void IOCL_CTReplica::ReceiveMessage(const TransportAddress &remote, const string &type,
                                       const string &data, void *meta_data)
        {
            Panic("Don't call this version of ReceiveMessage");
        }

        void IOCL_CTReplica::ReceiveMessage(const TransportAddress &remote,
                                       MsgType type, const string &data,
                                       void *meta_data)
        {
            switch (type) {
            case MsgType::UNORDERED_PREPARE_TYPE: {
                Debug("Received UNORDERED_PREPARE message");
                unorderedPrepareRecv.Clear();
                unorderedPrepareRecv.ParseFromString(data);
                HandleUnorderedPrepare(remote, unorderedPrepareRecv);
                break;
            }
            case MsgType::UNORDERED_PREPARE_OK_TYPE: {
                Debug("Received UNORDERED_PREPARE_OK message");
                unorderedPrepareOKRecv.Clear();
                unorderedPrepareOKRecv.ParseFromString(data);
                HandleUnorderedPrepareOK(remote, unorderedPrepareOKRecv);
                break;
            }
            case MsgType::PREPARE_TYPE: {
                Debug("Received PREPARE message");
                prepareRecv.Clear();
                prepareRecv.ParseFromString(data);
                HandlePrepare(remote, prepareRecv);
                break;
            }
            case MsgType::PREPARE_OK_TYPE: {
                Debug("Received PREPARE_OK message");
                prepareOKRecv.Clear();
                prepareOKRecv.ParseFromString(data);
                HandlePrepareOK(remote, prepareOKRecv);
                break;
            }
            case MsgType::COMMIT_TYPE: {
                Debug("Received COMMIT message");
                commitRecv.Clear();
                commitRecv.ParseFromString(data);
                HandleCommit(remote, commitRecv);
                break;
            }
            case MsgType::COORD_RESP_TYPE: {
                Debug("Received COORDINATION REPLY message");
                // Predecessor reply arrived
                coordRespRecv.Clear();
                coordRespRecv.ParseFromString(data);
                HandleCoordinationReply(remote, coordRespRecv);
                break;
            }
            case MsgType::COORD_FINAL_TYPE: {
                Debug("Received COORDINATION FINAL ACK message");
                // Predecessor final ACK arrived
                coordFinalRecv.Clear();
                coordFinalRecv.ParseFromString(data);
                HandleCoordinationFinal(remote, coordFinalRecv);
                break;
            }
            /*
            else if (type == unloggedRequest.GetTypeName())
            {
                unloggedRequest.ParseFromString(data);
                HandleUnloggedRequest(remote, unloggedRequest);
            }
            else if (type == requestStateTransfer.GetTypeName())
            {
                requestStateTransfer.ParseFromString(data);
                HandleRequestStateTransfer(remote, requestStateTransfer);
            }
            else if (type == stateTransfer.GetTypeName())
            {
                stateTransfer.ParseFromString(data);
                HandleStateTransfer(remote, stateTransfer);
            }
            else if (type == startViewChange.GetTypeName())
            {
                startViewChange.ParseFromString(data);
                HandleStartViewChange(remote, startViewChange);
            }
            else if (type == doViewChange.GetTypeName())
            {
                doViewChange.ParseFromString(data);
                HandleDoViewChange(remote, doViewChange);
            }
            else if (type == startView.GetTypeName())
            {
                startView.ParseFromString(data);
                HandleStartView(remote, startView);
            }
            */
            default:
                RPanic("Received unexpected message type in iocl_ct proto: %u",
                       (uint32_t)type);
            }
        }

        void IOCL_CTReplica::HandleRequest(LinearizeableOperation &msg)
        {
            Debug("Received request with op %d, key %s, value %s, server slot idx %u, shardtag %lu, inteky %lu, and rid (%lu, %lu)",
                   msg.kv().op(), msg.kv().key().c_str(), msg.kv().value().c_str(), msg.kv().slot_idx(), msg.shardtag(), msg.intkey(),
                   msg.rid().client_id(), msg.rid().client_req_id());
            Debug("And ti has predlist %d", msg.predlist().size());
            Debug("And the predecessor shardtags are:");
            for (int i = 0; i < msg.predlist().size(); i++) {
                Debug("pred %d: %lu", i, msg.predlist(i));
            }
            // Latency_Start(&rec_to_upcall_lat_);
            viewstamp_t v;

            if (status != STATUS_NORMAL)
            {
                RNotice("Ignoring request due to abnormal status");
                return;
            }

            if (!AmLeader())
            {
                RDebug("Ignoring request because I'm not the leader");
                return;
            }

            // Assign it an opnum within this view --> this is
            // strictly to compy with quorum checking which
            // currently is unique per viewstamp_t
            ++this->lastUnorderedOp;
            v.view = this->view;
            v.opnum = this->lastUnorderedOp;

            // Add the request to the unordered bag
            ASSERT((msg.request_type() == replication::LinearizeableOperation::KV_OP) && msg.has_kv());
            uint64_t shardtag = msg.shardtag();
            uint64_t intkey = msg.intkey();
            uint16_t num_predecessors = msg.predlist().size();
            uint32_t idx = entryStore.size();
            entryStore.emplace_back(v, IOCL_STATE_ARRIVED, std::move(msg), shardtag, intkey, num_predecessors);
            IoclEntry &entry = Entry(idx);
            ASSERT(entry.viewstamp.opnum - 1 == idx);
            entry.predecessorArrivalTs.resize(entry.num_predecessors);

            // Add entry to "ordered" unorderedBag (for batching)
            ASSERT(shardtagToEntryIdx.find(shardtag) == shardtagToEntryIdx.end());
            shardtagToEntryIdx[shardtag] = idx;

            // Go through any outstanding predecessor replies and add them in
            auto pit = outstandingCoordinationResps.find(shardtag);
            if (pit != outstandingCoordinationResps.end()) {
                std::vector<outCoordResp> &ocr = pit->second;
                for (const auto& resp : ocr) {
                    ASSERT(resp.predidx < entry.num_predecessors);
                    // ASSERT(entryPtr->predecessorArrivalTs[resp.predidx()] == 0);
                    entry.predecessorArrivalTs[resp.predidx] = resp.arrivalTs;
                    entry.ACKs++;
                }
                outstandingCoordinationResps.erase(pit);
            }
            // Also go through any outstanding predecessor final ACKs and add them in
            auto fit = outstandingCoordinationFinals.find(shardtag);
            if (fit != outstandingCoordinationFinals.end()) {
                uint64_t outstanding_final_mask = fit->second;
                entry.final_ack_mask |= outstanding_final_mask;
                // Count how many bits are set in the outstanding_final_mask and add that to the final_ack_count
                uint8_t count = __builtin_popcountll(outstanding_final_mask);
                entry.final_ack_count += count;
                outstandingCoordinationFinals.erase(fit);
            }


            if (lastUnorderedOp - lastUnorderedBatchEnd + 1 > batchSize)
            {
                CloseUnorderedBatch();
            }
            else
            {
                if (!closeUnorderedBatchTimeout->Active())
                {
                    closeUnorderedBatchTimeout->Start();
                }
            }
            nullCommitTimeout->Reset();
        }

        uint64_t IOCL_CTReplica::FoldL(const std::vector<uint64_t> &pl)
        {
            // check if empty
            if (pl.size() == 0)
            {
                return 0;
            }
            auto v = pl[0];
            for (uint64_t e : pl)
            {
                if (e > v)
                {
                    v = e + 1;
                }
                else
                {
                    v++;
                }
            }
            return v;
        }

        // INVARIANT: the sublog is never empty!
        void IOCL_CTReplica::ReadyFinalRoutine(uint64_t intkey)
        {
            /* Execute as many head entries from the sublog as are ready */
            auto &sublog = perKeySubLogs[intkey];
            while (sublog.head < sublog.ops.size()) {
                opnum_t headOpnum = sublog.ops[sublog.head];
                const IoclEntry *head_entry = FindInLog(headOpnum);
                ASSERT(head_entry->state == IOCL_STATE_COMMITTED);

                if (head_entry->final_ack_count != head_entry->num_predecessors) {
                    /* Still waiting on final ACKs, done with loop */
                    ASSERT(head_entry->final_ack_count <= head_entry->num_predecessors);
                    break;
                }
                /* Remove from sublog */
                sublog.head++;
                /* Execute it */
                ReplicaUpcall(head_entry->request);
            }
            if (sublog.head == sublog.ops.size()) {
                /* If we've executed everything in the sublog, remove it to save space */
                perKeySubLogs.erase(intkey);
            }
        }

        void IOCL_CTReplica::ReadyRoutine(uint64_t intkey)
        {
            Debug("hi");
            auto it = perKeySubqueues.find(intkey);
            ASSERT(it != perKeySubqueues.end());
            auto &sq = it->second;
            Debug("The subqueue length is %lu", sq.size());

            while (!sq.empty()) {
                uint32_t idx = *sq.begin();
                IoclEntry &head = Entry(idx);
                if (head.state != IOCL_STATE_PERSISTED && head.state != IOCL_STATE_READY) {
                    Warning("the head (shardtag = %lu) is currently neither PERSISTED nor READY, instead it is in state %d", head.myShardTag,
                            head.state);
                    ASSERT(head.state == IOCL_STATE_PERSISTED || head.state == IOCL_STATE_READY);
                }
                if (head.state != IOCL_STATE_READY) {
                    Debug("it's not ready! it's state is %d", head.state);
                    break;
                }
                /* Progress to REQUEST ordered */
                sq.erase(sq.begin());
                /* Send out the Final ACK to all successors */
                for (auto& kv : head.successors) {
                    const SuccessorKey &succ = kv.first;
                    SuccessorInfo &succ_info = kv.second;
                    if (!succ_info.final_sent) {
                        predFinalSend.set_s(succ.s_shardtag);
                        predFinalSend.set_predidx(succ_info.predidx);
                        if (!(transport->SendMessageToReplica(this, succ.s_shardidx, 0, MsgType::COORD_FINAL_TYPE, predFinalSend)))
                        {
                            RWarning("Failed to send SuccessorRequest message to client");
                        }
                        // Mark that we've sent to this successor
                        succ_info.final_sent = true;
                    }
                }

                /* Assign it a real opnum for this view in the ordered log */
                ASSERT(head.viewstamp.opnum - 1 == idx);
                viewstamp_t v;
                ++this->lastOp;
                v.view = this->view;
                v.opnum = this->lastOp;
                head.viewstamp = v;
                /* Set it as Prepared (since it isn't quite committed yet ) */
                head.state = IOCL_STATE_PREPARED;

                /* Add the request to my log */
                AppendToLog(head.viewstamp.opnum, idx);

                if (lastOp - lastBatchEnd + 1 > batchSize)
                {
                    Debug("ok, i am ready now! Adding to ordered log and replicating!");
                    CloseBatch();
                }
                else
                {
                    Panic("should always be batching with IOCL protocol");
                    if (!closeBatchTimeout->Active())
                    {
                        closeBatchTimeout->Start();
                    }
                }
            }
            if (sq.empty()) {
                /* If we've executed everything in the sublog, remove it to save space */
                Debug("erasing this subqueue");
                perKeySubqueues.erase(intkey);
            }
        }

        void IOCL_CTReplica::InsertInSubqueue(uint64_t intkey, uint32_t idx) {
            Debug("INSERTING into SUBQUEUE.... for idx = %u and intkey %lu and shardtag %lu and finalTs = %lu", idx, intkey, Entry(idx).myShardTag, Entry(idx).finalTs);
            auto it = perKeySubqueues.find(intkey);
            // Create subqueue (ordered set) for this intkey if it doesn't exist yet,
            // giving it access to entryStore for comparisons
            if (it == perKeySubqueues.end()) {
                it = perKeySubqueues.emplace(
                    intkey,
                    std::set<uint32_t, EntryReadyCompareIdx>(EntryReadyCompareIdx{&entryStore})
                ).first;
            }
            // Make sure we are inserting, NOT reinserting
            ASSERT(std::find(it->second.begin(), it->second.end(), idx) == it->second.end());
            ASSERT(it->second.find(idx) == it->second.end());
            // N*LogN insertion into the subqueue
            it->second.insert(idx);
            Debug("     The length of this subqueue is now %lu", it->second.size());
            // entry = entryStore[idx] and
            // EntryReadyCompareIdx compare by finalTs
        }

        void IOCL_CTReplica::HandleUnorderedPrepareOK(const TransportAddress &remote,
                                      const UnorderedPrepareOKMessage &msg)
        {
            RDebug("Received UNORDERED_PREPAREOK <" FMT_VIEW ", " FMT_OPNUM "> from replica %d",
                   msg.view(), msg.opnum(), msg.replicaidx());
            // Latency_Start(&rec_to_upcall_lat_);
            viewstamp_t v;
            if (status != STATUS_NORMAL)
            {
                RNotice("Ignoring UNORDERED_PREPAREOK due to abnormal status");
                return;
            }

            if (msg.view() < this->view)
            {
                RDebug("Ignoring UNORDERED_PREPAREOK due to stale view");
                return;
            }

            if (msg.view() > this->view)
            {
                RequestStateTransfer();
                return;
            }

            if (!AmLeader())
            {
                RDebug("Ignoring UNORDERED_PREPAREOK because I'm not the leader");
                return;
            }

            IoclEntry &entry = Entry(msg.batchstart()-1);
            uint64_t bit = 1ULL << msg.replicaidx();
            if ((entry.u_prepare_ok_mask & bit) == 0) {
                entry.u_prepare_ok_mask |= bit;
                entry.u_prepare_ok_count++;
            }

            if (entry.u_prepare_ok_count == Q)
            {
                Debug("Got quorum!");
                for (opnum_t i = msg.batchstart(); i <= msg.opnum(); i++)
                {
                    ASSERT(i > 0 && i <= entryStore.size());
                    IoclEntry &entry = Entry(i-1);
                    /* Progress state to Persisted */
                    entry.state = IOCL_STATE_PERSISTED;

                    /* Assign Arrival Timestamp */
                    auto ts_it = lastReadyTS.find(entry.intkey);
                    uint64_t ts = (ts_it == lastReadyTS.end()) ? 0 : ts_it->second;
                    entry.arrivalTs = std::max(shardTS, ts);
                    entry.finalTs = entry.arrivalTs; // will be updated later
                    shardTS++;
                    Debug("After UnorderedPrepareOK, arrivalTs = %lu and finalTs = %lu for entry with shardtag %lu", entry.arrivalTs, entry.finalTs, entry.myShardTag);

                    /* If it has any pending successor requests in
                    outstandingCoordinationReqs, respond to them now */
                    auto it = outstandingCoordinationReqs.find(entry.myShardTag);
                    if (it != outstandingCoordinationReqs.end()) {
                        Debug("Ih ave outstansing successors!");
                        preplySend.set_arrivalts(entry.arrivalTs);
                        std::vector<outCoordReq> &ocr = it->second;
                        for (const auto& succ : ocr) {
                            Debug("Responding to successor with shardtag %lu on shard %u", succ.s, succ.shardidx);
                            preplySend.set_s(succ.s);
                            preplySend.set_predidx(succ.predidx);
                            if (!(transport->SendMessageToReplica(this, succ.shardidx, 0, MsgType::COORD_RESP_TYPE, preplySend)))
                            {
                                RWarning("Failed to send SuccessorReply message to client");
                            }
                            /* And save the successor ! */
                            SuccessorKey succ_key{succ.s, static_cast<uint32_t>(succ.shardidx)};
                            auto result = entry.successors.emplace(succ_key, SuccessorInfo{static_cast<uint16_t>(succ.predidx), false});
                            auto succ_it = result.first;
                            bool inserted = result.second;
                            if (!inserted) {
                                Warning("Duplicate successor request received for successor %lu on shard %u", succ.s, succ.shardidx);
                                ASSERT(succ_it->second.predidx == succ.predidx);
                            }
                        }
                        outstandingCoordinationReqs.erase(it);
                    }

                    bool readyNow = (entry.ACKs == entry.num_predecessors);
                    Debug("Are we readyNow? ACKs = %d, predList size = %d, so readyNow = %d", entry.ACKs, entry.num_predecessors, readyNow);
                    auto sq_it = perKeySubqueues.find(entry.intkey);
                    bool no_subqueue = (sq_it == perKeySubqueues.end());
                    bool insertedAtHead = false;
                    uint64_t candidateFinalTs = readyNow ? std::max(entry.arrivalTs, FoldL(entry.predecessorArrivalTs)) : 0;
                    if (!no_subqueue && readyNow) {
                        // Make sure we're not in the log already (i-1 is the idx of the entry in the entryStore)
                        ASSERT(std::find(sq_it->second.begin(), sq_it->second.end(), i-1) == sq_it->second.end());
                        ASSERT(sq_it->second.find(i-1) == sq_it->second.end());
                        auto &sq = sq_it->second;
                        ASSERT(!sq.empty());
                        uint32_t head_idx = *sq.begin();
                        IoclEntry &head = Entry(head_idx);
                        insertedAtHead = (candidateFinalTs < head.finalTs || (candidateFinalTs == head.finalTs && entry.myShardTag < head.myShardTag));
                        if (insertedAtHead) {
                            ASSERT(head.ACKs < head.num_predecessors);
                            ASSERT(head.state != IOCL_STATE_READY);
                        }
                    }
                    /* FAST PATH: Check if we should never use the subqueue structure anyway */
                    // Coordinated < Replicated
                    if (readyNow &&
                        (no_subqueue || insertedAtHead)) {
                        Debug("In the fast path!!!");
                        /* Assign a final TS */
                        entry.finalTs = candidateFinalTs;
                        lastReadyTS[entry.intkey] = entry.finalTs + 1;
                        /* Assign it ready state */
                        entry.state = IOCL_STATE_READY;
                        /* Send out the Final ACK to all successors */
                        for (auto& kv : entry.successors) {
                            const SuccessorKey &succ = kv.first;
                            SuccessorInfo &succ_info = kv.second;
                            if (!succ_info.final_sent) {
                                predFinalSend.set_s(succ.s_shardtag);
                                predFinalSend.set_predidx(succ_info.predidx);
                                if (!(transport->SendMessageToReplica(this, succ.s_shardidx, 0, MsgType::COORD_FINAL_TYPE, predFinalSend)))
                                {
                                    RWarning("Failed to send SuccessorRequest message to client");
                                }
                                // Mark that we've sent to this successor
                                succ_info.final_sent = true;
                            }
                        }
                        /* Assign it a real opnum for this view in the ordered log */
                        ASSERT(entry.viewstamp.opnum - 1 == i-1);
                        viewstamp_t v;
                        ++this->lastOp;
                        v.view = this->view;
                        v.opnum = this->lastOp;
                        entry.viewstamp = v;
                        /* Set it as Prepared (since it isn't quite committed yet ) */
                        entry.state = IOCL_STATE_PREPARED;

                        /* Add the request to my log */
                        AppendToLog(entry.viewstamp.opnum, i-1);

                        if (lastOp - lastBatchEnd + 1 > batchSize)
                        {
                            Debug("Added to ordered log and replicating to replicas!");
                            CloseBatch();
                        }
                        else
                        {
                            Panic("should always be batching with IOCL protocol");
                            if (!closeBatchTimeout->Active())
                            {
                                closeBatchTimeout->Start();
                            }
                        }
                    } else { /* Otherwise, we need to wait for more ACKs before we can mark it ready */
                        /* Insert into the perKeySubqueue so that Head Of Line Blocking begins! */
                        Debug("in the slow path -- inserting into subqueue for HOL!");
                        InsertInSubqueue(entry.intkey, i-1);
                    }
                }
                nullCommitTimeout->Reset();
            }
        }

        void IOCL_CTReplica::HandleUnloggedRequest(const TransportAddress &remote,
                                              const UnloggedRequestMessage &msg)
        {
            if (status != STATUS_NORMAL)
            {
                // Not clear if we should ignore this or just let the request
                // go ahead, but this seems reasonable.
                RNotice("Ignoring unlogged request due to abnormal status");
                return;
            }

            UnloggedReplyMessage reply;

            Debug("Received unlogged request %s", (char *)msg.req().op().c_str());

            ExecuteUnlogged(msg.req(), reply);
            reply.set_clientreqid(msg.req().clientreqid());

            if (!(transport->SendMessage(this, remote, reply)))
                Warning("Failed to send reply message");
        }

        void IOCL_CTReplica::HandlePrepare(const TransportAddress &remote,
                                      const PrepareMessage &msg)
        {
            RDebug("Received PREPARE <" FMT_VIEW "," FMT_OPNUM "-" FMT_OPNUM ">",
                   msg.view(), msg.batchstart(), msg.opnum());

            if (this->status != STATUS_NORMAL)
            {
                RDebug("Ignoring PREPARE due to abnormal status");
                return;
            }

            if (msg.view() < this->view)
            {
                RDebug("Ignoring PREPARE due to stale view");
                return;
            }

            if (msg.view() > this->view)
            {
                RequestStateTransfer();
                pendingPrepares.push_back(
                    std::pair<TransportAddress *, PrepareMessage>(remote.clone(), msg));
                return;
            }

            if (AmLeader())
            {
                RPanic("Unexpected PREPARE: I'm the leader of this view");
            }

            ASSERT(msg.batchstart() <= msg.opnum());
            ASSERT((msg.opnum() - msg.batchstart() + 1) ==
                   (unsigned int)msg.shardtags_size());

            viewChangeTimeout->Reset();
            int leaderIdx = configuration.GetLeaderIndex(view);

            if (msg.opnum() <= this->lastOp)
            {
                RDebug("Ignoring PREPARE; already prepared that operation");
                // Resend the prepareOK message
                PrepareOKMessage reply;
                reply.set_view(msg.view());
                reply.set_opnum(msg.opnum());
                reply.set_replicaidx(myIdx);
                if (!(transport->SendMessageToReplica(
                        this, leaderIdx, MsgType::PREPARE_OK_TYPE, reply)))
                {
                    RWarning("Failed to send PrepareOK message to leader");
                }
                return;
            }

            if (msg.batchstart() > this->lastOp + 1)
            {
                RequestStateTransfer();
                pendingPrepares.push_back(
                    std::pair<TransportAddress *, PrepareMessage>(remote.clone(), msg));
                return;
            }

            /* Add operations to the log */
            opnum_t op = msg.batchstart() - 1;
            size_t chain_idx = 0;
            for (auto &shardtag : msg.shardtags())
            {
                op++;

                /* Find the entry */
                auto it = shardtagToEntryIdx.find(shardtag);
                if (it == shardtagToEntryIdx.end()) {
                    Panic("Replica didn't have request with shardtag %lu in unorderedBag during Prepare",
                        shardtag);
                }
                uint32_t idx = it->second;
                IoclEntry &entry = Entry(idx);
                size_t N = entry.num_predecessors + 1;

                if (op <= lastOp)
                {
                    chain_idx += N;
                    continue;
                }
                this->lastOp++;

                /* Update its state */
                ASSERT(entry.viewstamp.opnum - 1 == idx);
                entry.viewstamp.view = msg.view();
                entry.viewstamp.opnum = op;
                entry.state = IOCL_STATE_PREPARED;
                // loop through timestamp_chains and add to predecessorArrivalTs
                ASSERT(chain_idx + N <= msg.timestamp_chains_size());
                for (size_t j = 0; j < (N-1); j++) {
                    entry.predecessorArrivalTs[j] = msg.timestamp_chains(chain_idx);
                    chain_idx++;
                }
                entry.finalTs = msg.timestamp_chains(chain_idx); // last one is the final TS of the predecessor chain, which is the one we use for ordering in the log and comparing against other entries
                chain_idx++;
                /* Add the request to my log */
                AppendToLog(entry.viewstamp.opnum, idx);
                Debug("Just Prepared operation with shardtag %lu and intkey %lu, and finalTs = %lu", entry.myShardTag, entry.intkey, entry.finalTs);
            }
            ASSERT(op == msg.opnum());
            ASSERT(chain_idx == msg.timestamp_chains_size());

            /* Build reply and send it to the leader */
            PrepareOKMessage reply;
            reply.set_view(msg.view());
            reply.set_opnum(msg.opnum());
            reply.set_replicaidx(myIdx);

            if (!(transport->SendMessageToReplica(
                    this, leaderIdx, MsgType::PREPARE_OK_TYPE, reply)))
            {
                RWarning("Failed to send PrepareOK message to leader");
            }
        }

        void IOCL_CTReplica::HandleUnorderedPrepare(const TransportAddress &remote,
                                      UnorderedPrepareMessage &msg)
        {
            RDebug("Received PREPARE <" FMT_VIEW "," FMT_OPNUM ">",
                   msg.view(), msg.opnum());

            if (this->status != STATUS_NORMAL)
            {
                RDebug("Ignoring UNORDERED_PREPARE due to abnormal status");
                return;
            }

            if (msg.view() < this->view)
            {
                RDebug("Ignoring UNORDERED_PREPARE due to stale view");
                return;
            }

            if (msg.view() > this->view)
            {
                // RequestStateTransfer();
                Panic("not implemented");
                // pendingPrepares.push_back(
                //     std::pair<TransportAddress *, PrepareMessage>(remote.clone(), msg));
                return;
            }

            if (AmLeader())
            {
                RPanic("Unexpected UNORDERED_PREPARE: I'm the leader of this view");
            }

            ASSERT(msg.batchstart() <= msg.opnum());
            ASSERT((msg.opnum() - msg.batchstart() + 1) ==
                   (unsigned int)msg.request_size());

            viewChangeTimeout->Reset();
            int leaderIdx = configuration.GetLeaderIndex(view);

            if (msg.opnum() <= this->lastUnorderedOp)
            {
                RDebug("Ignoring UNORDERED_PREPARE; already prepared that operation");
                // Resend the prepareOK message
                UnorderedPrepareOKMessage reply;
                reply.set_view(msg.view());
                reply.set_opnum(msg.opnum());
                reply.set_replicaidx(myIdx);
                reply.set_batchstart(msg.batchstart());
                if (!(transport->SendMessageToReplica(
                        this, leaderIdx, MsgType::UNORDERED_PREPARE_OK_TYPE, reply)))
                {
                    RWarning("Failed to send PrepareOK message to leader");
                }
                return;
            }

            // Add operations to the unordered bag
            int i = -1;
            opnum_t op = msg.batchstart() - 1;
            for (const auto &req : msg.request())
            {
                op++;
                i++;
                if (op <= lastUnorderedOp)
                {
                    continue;
                }
                this->lastUnorderedOp++;
                /* Add the request to the unordered bag */
                uint64_t shardtag = req.shardtag();
                uint64_t intkey = req.intkey();
                uint16_t num_predecessors = req.predlist().size();

                /* For now we don't replicate the intkey at replicas
                Instead, if a new leader takes over, it can get its key from 
                the string in the Request */
                uint32_t idx = entryStore.size();
                entryStore.emplace_back(viewstamp_t(msg.view(), op), IOCL_STATE_PERSISTED, std::move(req), shardtag, intkey, num_predecessors);

                IoclEntry &entry = Entry(idx);

                ASSERT(shardtagToEntryIdx.find(shardtag) == shardtagToEntryIdx.end());
                shardtagToEntryIdx[shardtag] = idx;
            }

            /* Build reply and send it to the leader */
            UnorderedPrepareOKMessage reply;
            reply.set_view(msg.view());
            reply.set_opnum(msg.opnum());
            reply.set_batchstart(msg.batchstart());
            reply.set_replicaidx(myIdx);

            if (!(transport->SendMessageToReplica(
                    this, leaderIdx, MsgType::UNORDERED_PREPARE_OK_TYPE, reply)))
            {
                RWarning("Failed to send PrepareOK message to leader");
            }
        }

        void IOCL_CTReplica::HandlePrepareOK(const TransportAddress &remote,
                                        const PrepareOKMessage &msg)
        {
            RDebug("Received PREPAREOK <" FMT_VIEW ", " FMT_OPNUM "> from replica %d",
                   msg.view(), msg.opnum(), msg.replicaidx());

            if (this->status != STATUS_NORMAL)
            {
                RDebug("Ignoring PREPAREOK due to abnormal status");
                return;
            }

            if (msg.view() < this->view)
            {
                RDebug("Ignoring PREPAREOK due to stale view");
                return;
            }

            if (msg.view() > this->view)
            {
                RequestStateTransfer();
                return;
            }

            if (!AmLeader())
            {
                RWarning("Ignoring PREPAREOK because I'm not the leader");
                return;
            }

            IoclEntry *entry = FindInLog(msg.opnum());
            if (entry == nullptr)
            {
                RPanic("Did not find operation " FMT_OPNUM " in log",
                           msg.opnum());
            }
            uint64_t bit = 1ULL << msg.replicaidx();
            if ((entry->prepare_ok_mask & bit) == 0) {
                entry->prepare_ok_mask |= bit;
                entry->prepare_ok_count++;
            }

            if (entry->prepare_ok_count == Q)
            {
                Debug("Got quorum!");
                /*
                 * We have a quorum of PrepareOK messages for this
                 * opnumber. Execute it and all previous operations.
                 *
                 * (Note that we might have already executed it. That's fine,
                 * we just won't do anything.)
                 *
                 * This also notifies the client of the result.
                 */
                CommitUpTo(msg.opnum());

                /*
                 * Send COMMIT message to the other replicas.
                 *
                 * This can be done asynchronously, so it really ought to be
                 * piggybacked on the next PREPARE or something.
                 */
                Debug("And now issueing Commit message to replicas for opnum " FMT_OPNUM, msg.opnum());
                CommitMessage cm;
                cm.set_view(this->view);
                cm.set_opnum(this->lastCommitted);

                if (!(transport->SendMessageToAll(this, MsgType::COMMIT_TYPE, cm)))
                {
                    RWarning("Failed to send COMMIT message to all replicas");
                }

                nullCommitTimeout->Reset();
            }
        }

        void IOCL_CTReplica::HandleCoordinationFinal(const TransportAddress &remote,
                                                const proto::PredecessorFinalMessage &msg)
        {
            Debug("hadnelCoordiantonFINAL from predecessorIDX%u-->successor %lu",
                msg.predidx(), msg.s());
            auto it = shardtagToEntryIdx.find(msg.s());
            if (it == shardtagToEntryIdx.end()) {
                uint64_t curr_finals_mask = outstandingCoordinationFinals[msg.s()];
                uint64_t bit = 1ULL << msg.predidx();
                if ((curr_finals_mask & bit) == 0) {
                    curr_finals_mask |= bit;
                } else {
                    Warning("Duplicate final ACK received from predecessor IDX %u for my shardtag %lu",
                            msg.predidx(), msg.s());
                }
                outstandingCoordinationFinals[msg.s()] = curr_finals_mask;
                return;
            }
            IoclEntry &entry = Entry(it->second);

            /* assert that there are no outstanding
               responses for this successor in the
               outstandingCoordinationFinals -- should
               have been drained when upon arrival */
            ASSERT(outstandingCoordinationFinals.find(entry.myShardTag) == outstandingCoordinationFinals.end());
            /* Mark that this predecessor has finalized */
            uint64_t bit = 1ULL << msg.predidx();
            if ((entry.final_ack_mask & bit) == 0) {
                entry.final_ack_mask |= bit;
                entry.final_ack_count++;
            } else {
                Warning("Duplicate final ACK received from predecessor IDX %u for my shardtag %lu",
                        msg.predidx(), entry.myShardTag);
                return;
            }
            /* Now it is safe to Execute this operation! */
            /* Check if it is waiting to be executed */
            if ((entry.state == IOCL_STATE_COMMITTED) && (entry.final_ack_count == entry.num_predecessors)) {
                ASSERT(perKeySubLogs.find(entry.intkey) != perKeySubLogs.end());
                // TODO ASSERT WE ARE IN THE LOG
                Debug("All final ACKs received for entry with shardtag %lu, so now executing +ReadyFinalRoutine!", entry.myShardTag);
                ReadyFinalRoutine(entry.intkey);
            }
        }

        void IOCL_CTReplica::HandleCoordination(const SuccessorRequestMessage &msg)
        {
            Debug("Received client Coordination with msg = {succ_shardtag = %lu, shardidx = %u, predidx = %u, pred_shardtag = %lu}",
                msg.s(), msg.shardidx(), msg.predidx(), msg.p());
            auto it = shardtagToEntryIdx.find(msg.p());
            if (it == shardtagToEntryIdx.end()) {
                std::vector<outCoordReq> &vec = outstandingCoordinationReqs[msg.p()];
                outCoordReq ocr{msg.s(), static_cast<uint32_t>(msg.shardidx()), static_cast<uint16_t>(msg.predidx())};
                vec.emplace_back(ocr);
                return;
            }
            IoclEntry &entry = Entry(it->second);
            /* If not yet persisted, can't respond yet */
            if (entry.state == IOCL_STATE_ARRIVED) {
                std::vector<outCoordReq> &vec = outstandingCoordinationReqs[msg.p()];
                outCoordReq ocr{msg.s(), static_cast<uint32_t>(msg.shardidx()), static_cast<uint16_t>(msg.predidx())};
                vec.emplace_back(ocr);
                return;
            }
            /* assert that there are no outstanding
               requests for this predecessor in the
               outstandingCoordinationReqs -- should
               have been drained when state changed */
            ASSERT(outstandingCoordinationReqs.find(entry.myShardTag) == outstandingCoordinationReqs.end());

            /* Send reply now */
            preplySend.set_arrivalts(entry.arrivalTs);
            preplySend.set_s(msg.s());
            preplySend.set_predidx(msg.predidx());
            if (!(transport->SendMessageToReplica(this, msg.shardidx(), 0, MsgType::COORD_RESP_TYPE, preplySend)))
            {
                RWarning("Failed to send SuccessorReply message to client");
            }
            // NOTE the shardidx is int32
            /* Store the successor for the final TS */
            SuccessorKey succ{msg.s(), static_cast<uint32_t>(msg.shardidx())};
            auto result = entry.successors.emplace(succ, SuccessorInfo{static_cast<uint16_t>(msg.predidx()), false});
            auto succ_it = result.first;
            bool inserted = result.second;
            if (!inserted) {
                Warning("Duplicate successor request received for successor %lu on shard %u", msg.s(), msg.shardidx());
                ASSERT(succ_it->second.predidx == msg.predidx());
            }
            /* Reply to the successor if we've already been added to the ordered log */
            if (entry.state == IOCL_STATE_READY ||
                entry.state == IOCL_STATE_PREPARED ||
                entry.state == IOCL_STATE_COMMITTED) {
                /* Send out the Final ACK to all successors */
                predFinalSend.set_predidx(msg.predidx());
                predFinalSend.set_s(msg.s());
                succ_it->second.final_sent = true; // Mark that we've sent final ACK to this successor
                if (!(transport->SendMessageToReplica(this, msg.shardidx(), 0, MsgType::COORD_FINAL_TYPE, predFinalSend)))
                {
                    RWarning("Failed to send SuccessorRequest message to client");
                }
            }

            return;
        }

        void IOCL_CTReplica::HandleCoordinationReply(const TransportAddress &remote,
                                                const proto::PredecessorReplyMessage &msg)
        {
            Debug("Received Coordination Reply from predecessor-->successor %lu with arrivalTs = %lu and predidx = %u",
                msg.s(), msg.arrivalts(), msg.predidx());
            // NOTE the shardidx is int32
            auto it = shardtagToEntryIdx.find(msg.s());
            if (it == shardtagToEntryIdx.end()) {
                std::vector<outCoordResp> &vec = outstandingCoordinationResps[msg.s()];
                outCoordResp ocr{msg.arrivalts(), static_cast<uint16_t>(msg.predidx())};
                vec.emplace_back(ocr);
                return;
            }
            uint32_t idx = it->second;
            IoclEntry &entry = Entry(idx);
            /* assert that there are no outstanding
               responses for this successor in the
               outstandingCoordinationResps -- should
               have been drained when upon arrival */
            ASSERT(outstandingCoordinationResps.find(entry.myShardTag) == outstandingCoordinationResps.end());
            ASSERT(msg.predidx() < entry.num_predecessors);
            // ASSERT(entry.predecessorArrivalTs[msg.predidx()] == 0); --> OTHERWISE DEBUG DUPLICATION MESSAGE
            entry.predecessorArrivalTs[msg.predidx()] = msg.arrivalts();
            entry.ACKs++;
            // Might remove this for dedup
            ASSERT(entry.state == IOCL_STATE_ARRIVED || entry.state == IOCL_STATE_PERSISTED);

            // Replicated < Coordinated
            if (entry.state == IOCL_STATE_PERSISTED &&
                     entry.ACKs == entry.num_predecessors) {
                /* Now can progress to READY state */
                Debug("Replicated < Coordinated for %lu", entry.myShardTag);
                // ASSERT it is in here in the first place
                auto it = perKeySubqueues.find(entry.intkey);
                ASSERT(it != perKeySubqueues.end());
                ASSERT(std::find(it->second.begin(), it->second.end(), idx) != it->second.end());
                ASSERT(it->second.find(idx) != it->second.end());
                Debug("Found it in the perKeySubqueue!");
                // Remove it and reinsert it to update its position in the subqueue based on the new finalTs that will be assigned
                it->second.erase(idx);
                Debug("just erased it!");
                /* Assign a final TS */
                entry.finalTs = std::max(entry.arrivalTs, FoldL(entry.predecessorArrivalTs));
                lastReadyTS[entry.intkey] = entry.finalTs + 1;
                /* Assign it ready state */
                entry.state = IOCL_STATE_READY;
                // Reinsert
                Debug("REEinserting into SUBQUEUE.... for idx = %u and intkey %lu and shardtag %lu and finalTs = %lu", idx, entry.intkey, entry.myShardTag, entry.finalTs);
                it->second.insert(idx);
                Debug("All ACKs received for entry with shardtag %lu, so now ready!", entry.myShardTag);
                ReadyRoutine(entry.intkey);
            }

            return;
        }

        void IOCL_CTReplica::HandleCommit(const TransportAddress &remote,
                                     const CommitMessage &msg)
        {
            RDebug("Received COMMIT " FMT_VIEWSTAMP, msg.view(), msg.opnum());

            if (this->status != STATUS_NORMAL)
            {
                RDebug("Ignoring COMMIT due to abnormal status");
                return;
            }

            if (msg.view() < this->view)
            {
                RDebug("Ignoring COMMIT due to stale view");
                return;
            }

            if (msg.view() > this->view)
            {
                RequestStateTransfer();
                return;
            }

            if (AmLeader())
            {
                RPanic("Unexpected COMMIT: I'm the leader of this view");
            }

            viewChangeTimeout->Reset();

            if (msg.opnum() <= this->lastCommitted)
            {
                RDebug("Ignoring COMMIT; already committed that operation");
                return;
            }

            if (msg.opnum() > this->lastOp)
            {
                RequestStateTransfer();
                return;
            }

            CommitUpTo(msg.opnum());
        }

        void IOCL_CTReplica::HandleRequestStateTransfer(
            const TransportAddress &remote, const RequestStateTransferMessage &msg)
        {
            RDebug("Received REQUESTSTATETRANSFER " FMT_VIEWSTAMP, msg.view(),
                   msg.opnum());
            Panic("Shouldn't be calling HandleRequestStateTransfer");

            if (status != STATUS_NORMAL)
            {
                RDebug("Ignoring REQUESTSTATETRANSFER due to abnormal status");
                return;
            }

            if (msg.view() > view)
            {
                RequestStateTransfer();
                return;
            }

            RNotice("Sending state transfer from " FMT_VIEWSTAMP " to " FMT_VIEWSTAMP,
                    msg.view(), msg.opnum(), view, lastCommitted);

            StateTransferMessage reply;
            reply.set_view(view);
            reply.set_opnum(lastCommitted);

            //log.Dump(msg.opnum() + 1, reply.mutable_entries());

            transport->SendMessage(this, remote, reply);
        }

        void IOCL_CTReplica::HandleStateTransfer(const TransportAddress &remote,
                                            const StateTransferMessage &msg)
        {
            RDebug("Received STATETRANSFER " FMT_VIEWSTAMP, msg.view(), msg.opnum());
            Panic("shouldn't be caling handle state transfer");

            if (msg.view() < view)
            {
                RWarning("Ignoring state transfer for older view");
                return;
            }

            opnum_t oldLastOp = lastOp;

            /* Install the new log entries */
            for (auto newEntry : msg.entries())
            {
                if (newEntry.opnum() <= lastCommitted)
                {
                    // Already committed this operation; nothing to be done.
#if PARANOID
                    const LogEntry *entry = log.Find(newEntry.opnum());
                    ASSERT(entry->viewstamp.opnum == newEntry.opnum());
                    ASSERT(entry->viewstamp.view == newEntry.view());
//          ASSERT(entry->request == newEntry.request());
#endif
                }
                else if (newEntry.opnum() <= lastOp)
                {
                    // We already have an entry with this opnum, but maybe
                    // it's from an older view?
                    const IoclEntry *entry = FindInLog(newEntry.opnum());
                    ASSERT(entry->viewstamp.opnum == newEntry.opnum());
                    ASSERT(entry->viewstamp.view <= newEntry.view());

                    if (entry->viewstamp.view == newEntry.view())
                    {
                        // We already have this operation in our log.
                        ASSERT(entry->state == IOCL_STATE_PREPARED);
#if PARANOID
//              ASSERT(entry->request == newEntry.request());
#endif
                    }
                    else
                    {
                        // Our operation was from an older view, so obviously
                        // it didn't survive a view change. Throw out any
                        // later log entries and replace with this one.
                        ASSERT(entry->state != IOCL_STATE_COMMITTED);
                        //log.RemoveAfter(newEntry.opnum());
                        lastOp = newEntry.opnum();
                        oldLastOp = lastOp;

                        viewstamp_t vs = {newEntry.view(), newEntry.opnum()};
                        //AppendInLog(vs, newEntry.request(), IOCL_STATE_PREPARED);
                    }
                }
                else
                {
                    // This is a new operation to us. Add it to the log.
                    ASSERT(newEntry.opnum() == lastOp + 1);

                    lastOp++;
                    viewstamp_t vs = {newEntry.view(), newEntry.opnum()};
                    //log.Append(vs, newEntry.request(), IOCL_STATE_PREPARED);
                }
            }

            if (msg.view() > view)
            {
                EnterView(msg.view());
            }

            /* Execute committed operations */
            ASSERT(msg.opnum() <= lastOp);
            CommitUpTo(msg.opnum());
            SendPrepareOKs(oldLastOp);

            // Process pending prepares
            std::list<std::pair<TransportAddress *, PrepareMessage>> pending =
                pendingPrepares;
            pendingPrepares.clear();
            for (auto &msgpair : pendingPrepares)
            {
                RDebug("Processing pending prepare message");
                HandlePrepare(*msgpair.first, msgpair.second);
                delete msgpair.first;
            }
        }

        void IOCL_CTReplica::HandleStartViewChange(const TransportAddress &remote,
                                              const StartViewChangeMessage &msg)
        {
            RDebug("Received STARTVIEWCHANGE " FMT_VIEW " from replica %d", msg.view(),
                   msg.replicaidx());
            Panic("Shouldn't be calling HandleStartView");

            if (msg.view() < view)
            {
                RDebug("Ignoring STARTVIEWCHANGE for older view");
                return;
            }

            if ((msg.view() == view) && (status != STATUS_VIEW_CHANGE))
            {
                RDebug("Ignoring STARTVIEWCHANGE for current view");
                return;
            }

            if ((status != STATUS_VIEW_CHANGE) || (msg.view() > view))
            {
                StartViewChange(msg.view());
            }

            ASSERT(msg.view() == view);

            viewstamp_t vs = {msg.view(), 0};
            if (startViewChangeQuorum.AddAndCheckForQuorum(
                    vs, msg.replicaidx()))
            {
                int leader = configuration.GetLeaderIndex(view);
                // Don't try to send a DoViewChange message to ourselves
                if (leader != myIdx)
                {
                    DoViewChangeMessage dvc;
                    dvc.set_view(view);
                    dvc.set_lastnormalview(LastViewstampOfLog().view);
                    dvc.set_lastop(lastOp);
                    dvc.set_lastcommitted(lastCommitted);
                    dvc.set_replicaidx(myIdx);

                    // Figure out how much of the log to include
                    // opnum_t minCommitted =
                    //     std::min_element(
                    //         msgs->begin(), msgs->end(),
                    //         [](decltype(*msgs->begin()) a, decltype(*msgs->begin()) b)
                    //         {
                    //             return a.second.lastcommitted() <
                    //                    b.second.lastcommitted();
                    //         })
                    //         ->second.lastcommitted();
                    // minCommitted = std::min(minCommitted, lastCommitted);

                    // log.Dump(minCommitted, dvc.mutable_entries());

                    if (!(transport->SendMessageToReplica(this, leader, dvc)))
                    {
                        RWarning(
                            "Failed to send DoViewChange message to leader of new "
                            "view");
                    }
                }
            }
        }

        void IOCL_CTReplica::HandleDoViewChange(const TransportAddress &remote,
                                           const DoViewChangeMessage &msg)
        {
            RDebug("Received DOVIEWCHANGE " FMT_VIEW
                   " from replica %d, "
                   "lastnormalview=" FMT_VIEW " op=" FMT_OPNUM " committed=" FMT_OPNUM,
                   msg.view(), msg.replicaidx(), msg.lastnormalview(), msg.lastop(),
                   msg.lastcommitted());
            Panic("Shouldn't be calling HandleDoViewChange");

            if (msg.view() < view)
            {
                RDebug("Ignoring DOVIEWCHANGE for older view");
                return;
            }

            if ((msg.view() == view) && (status != STATUS_VIEW_CHANGE))
            {
                RDebug("Ignoring DOVIEWCHANGE for current view");
                return;
            }

            if ((status != STATUS_VIEW_CHANGE) || (msg.view() > view))
            {
                // It's superfluous to send the StartViewChange messages here,
                // but harmless...
                StartViewChange(msg.view());
            }

            ASSERT(configuration.GetLeaderIndex(msg.view()) == myIdx);

            viewstamp_t vs = {msg.view(), 0};
            auto quorum_reached = doViewChangeQuorum.AddAndCheckForQuorum(vs,
                                                                msg.replicaidx());
            if (quorum_reached)
            {
                // Find the response with the most up to date log, i.e. the
                // one with the latest viewstamp
                view_t latestView = LastViewstampOfLog().view;
                opnum_t latestOp = LastViewstampOfLog().opnum;
                DoViewChangeMessage *latestMsg = NULL;

                // for (auto kv : *msgs)
                // {
                //     DoViewChangeMessage &x = kv.second;
                //     if ((x.lastnormalview() > latestView) ||
                //         (((x.lastnormalview() == latestView) &&
                //           (x.lastop() > latestOp))))
                //     {
                //         latestView = x.lastnormalview();
                //         latestOp = x.lastop();
                //         latestMsg = &x;
                //     }
                // }

                // Install the new log. We might not need to do this, if our
                // log was the most current one.
                if (latestMsg != NULL)
                {
                    RDebug("Selected log from replica %d with lastop=" FMT_OPNUM,
                           latestMsg->replicaidx(), latestMsg->lastop());
                    if (latestMsg->entries_size() == 0)
                    {
                        // There weren't actually any entries in the
                        // log. That should only happen in the corner case
                        // that everyone already had the entire log, maybe
                        // because it actually is empty.
                        ASSERT(lastCommitted == msg.lastcommitted());
                        ASSERT(msg.lastop() == msg.lastcommitted());
                    }
                    else
                    {
                        if (latestMsg->entries(0).opnum() > lastCommitted + 1)
                        {
                            RPanic(
                                "Received log that didn't include enough entries "
                                "to "
                                "install it");
                        }

                        // log.RemoveAfter(latestMsg->lastop() + 1);
                        // log.Install(latestMsg->entries().begin(),
                                    // latestMsg->entries().end());
                    }
                }
                else
                {
                    RDebug("My log is most current, lastnormalview=" FMT_VIEW
                           " lastop=" FMT_OPNUM,
                           LastViewstampOfLog().view, lastOp);
                }

                // How much of the log should we include when we send the
                // STARTVIEW message? Start from the lowest committed opnum of
                // any of the STARTVIEWCHANGE or DOVIEWCHANGE messages we got.
                //
                // We need to compute this before we enter the new view
                // because the saved messages will go away.
                // auto svcs = startViewChangeQuorum.GetMessages(view);
                // opnum_t minCommittedSVC =
                //     std::min_element(
                //         svcs.begin(), svcs.end(),
                //         [](decltype(*svcs.begin()) a, decltype(*svcs.begin()) b)
                //         {
                //             return a.second.lastcommitted() < b.second.lastcommitted();
                //         })
                //         ->second.lastcommitted();
                // opnum_t minCommittedDVC =
                //     std::min_element(
                //         msgs->begin(), msgs->end(),
                //         [](decltype(*msgs->begin()) a, decltype(*msgs->begin()) b)
                //         {
                //             return a.second.lastcommitted() < b.second.lastcommitted();
                //         })
                //         ->second.lastcommitted();
                // opnum_t minCommitted = std::min(minCommittedSVC, minCommittedDVC);
                // minCommitted = std::min(minCommitted, lastCommitted);

                EnterView(msg.view());

                ASSERT(AmLeader());

                lastOp = latestOp;
                if (latestMsg != NULL)
                {
                    CommitUpTo(latestMsg->lastcommitted());
                }

                // Send a STARTVIEW message with the new log
                StartViewMessage sv;
                sv.set_view(view);
                sv.set_lastop(lastOp);
                sv.set_lastcommitted(lastCommitted);

                // log.Dump(minCommitted, sv.mutable_entries());

                if (!(transport->SendMessageToAll(this, sv)))
                {
                    RWarning("Failed to send StartView message to all replicas");
                }
            }
        }

        void IOCL_CTReplica::HandleStartView(const TransportAddress &remote,
                                        const StartViewMessage &msg)
        {
            RDebug("Received STARTVIEW " FMT_VIEW " op=" FMT_OPNUM
                   " committed=" FMT_OPNUM " entries=%d",
                   msg.view(), msg.lastop(), msg.lastcommitted(), msg.entries_size());
            RDebug("Currently in view " FMT_VIEW " op " FMT_OPNUM
                   " committed " FMT_OPNUM,
                   view, lastOp, lastCommitted);
            Panic("Shouldn't be calling HandleStartView");

            if (msg.view() < view)
            {
                RWarning("Ignoring STARTVIEW for older view");
                return;
            }

            if ((msg.view() == view) && (status != STATUS_VIEW_CHANGE))
            {
                RWarning("Ignoring STARTVIEW for current view");
                return;
            }

            ASSERT(configuration.GetLeaderIndex(msg.view()) != myIdx);

            if (msg.entries_size() == 0)
            {
                ASSERT(msg.lastcommitted() == lastCommitted);
                ASSERT(msg.lastop() == msg.lastcommitted());
            }
            else
            {
                if (msg.entries(0).opnum() > lastCommitted + 1)
                {
                    RPanic(
                        "Not enough entries in STARTVIEW message to install new "
                        "log");
                }

                // Install the new log
                // log.RemoveAfter(msg.lastop() + 1);
                // log.Install(msg.entries().begin(), msg.entries().end());
            }

            EnterView(msg.view());
            opnum_t oldLastOp = lastOp;
            lastOp = msg.lastop();

            ASSERT(!AmLeader());

            CommitUpTo(msg.lastcommitted());
            SendPrepareOKs(oldLastOp);
        }

        void IOCL_CTReplica::Close()
        {
            Debug("IOCL_CTReplica::Close called, closing batch if any");
            std::cerr << "==== IOCL Per-Key Queue Length Dump ====\n";

            for (const auto &kv : perKeyQueueLengths) {
                uint64_t key = kv.first;
                const std::vector<size_t> &lens = kv.second;

                std::cerr << "key=" << key << ": [";

                for (size_t i = 0; i < lens.size(); ++i) {
                    std::cerr << lens[i];
                    if (i + 1 < lens.size()) {
                        std::cerr << ",";
                    }
                }
                std::cerr << "]\n";
            }

            std::cerr << "==== End of Dump ====\n";
        }

    } // namespace iocl_ct
} // namespace replication
