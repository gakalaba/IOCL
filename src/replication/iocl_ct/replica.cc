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

#define RDebug(fmt, ...) Debug("[replica index = %d][group index = %d] " fmt, myIdx, groupIdx, ##__VA_ARGS__)
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
              prepareOKQuorum(config.QuorumSize() - 1),
              startViewChangeQuorum(config.QuorumSize() - 1),
              doViewChangeQuorum(config.QuorumSize() - 1),
              debug_stats_{debug_stats}
        {
            this->status = STATUS_NORMAL;
            this->view = 0;
            this->lastOp = 0;
            this->lastCommitted = 0;
            this->lastRequestStateTransferView = 0;
            this->lastRequestStateTransferOpnum = 0;
            lastBatchEnd2 = 0;
            lastBatch = 0;
            lastBatchEnd = 0;
            lastBatch2 = 0;

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
            this->closeBatch2Timeout =
                new Timeout(transport, 300, [this]()
                            { CloseBatch2(); });

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
        }

        // Destructor
        IOCL_CTReplica::~IOCL_CTReplica()
        {
            delete viewChangeTimeout;
            delete nullCommitTimeout;
            delete stateTransferTimeout;
            delete resendPrepareTimeout;
            delete closeBatch2Timeout;

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

        bool IOCL_CTReplica::AmLeader() const
        {
            return (configuration.GetLeaderIndex(view) == myIdx);
        }

        uint64_t FoldL(const std::vector<Predecessor *> &predecessors, bool arrival)
        {
            if (predecessors.empty())
            {
                Debug("Called FoldL on empty predecessor list");
                return 0;
            }
            auto v = arrival ? predecessors.front()->arrivalTimestamp : predecessors.front()->sortedTimestamp;
            for (auto it = predecessors.begin(); it != predecessors.end(); ++it)
            {
                auto e = arrival ? (*it)->arrivalTimestamp : (*it)->sortedTimestamp;
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

        void PrintBatches(uint64_t batchid, std::unordered_map<int, std::tuple<int, std::unordered_set<replication::LogEntry *>>> b)
        {
            if (b.find(batchid) == b.end())
            {
                Debug("the batch doesn't have a batch for batchid = %d", batchid);
            }
            else
            {
                auto s = std::get<1>(b[batchid]);
                Debug("number of entries in the batch for batchid = %d is %d", batchid, s.size());
                for (auto p : s)
                {
                    Debug("printing entry: %d, entry.tag = %d", p, p->myShardTag);
                }
            }
        }

        void IOCL_CTReplica::CommitUpTo(uint64_t batchId)
        {
            auto s = std::get<1>(thebatchs2[batchId]);
            PredecessorReplyMessage2 aa;
            Debug("hopefully gonna execute some stuff");
            for (auto entry : s)
            {
                // If any of these requests were fast pathd, they need to respond NOW
                if (entry->state == LOG_STATE_FASTPATH)
                {
                    notifySuccessorsACK2(*entry);
                }

                lastCommitted++;
                // TODO Anja update LastExecuted in here!!

                const Request request = entry->request;

                /* Execute it */
                RDebug("Executing request with tag %d", entry->myShardTag);
                ReplyMessage reply;
                Execute(entry->myShardTag, entry->request, reply);

                reply.set_view(entry->viewstamp.view);
                reply.set_opnum(entry->viewstamp.opnum);
                reply.set_clientreqid(entry->request.clientreqid());
                reply.set_shardtag(entry->myShardTag);

                /* Mark it as committed */
                log.SetStatus(*entry, LOG_STATE_COMMITTED);

                // Store reply in the client table
                ClientTableEntry &cte = clientTable[entry->request.clientid()];
                if (cte.lastReqId <= entry->request.clientreqid())
                {
                    // TODO i think since clients now have outstanding reqs... we can't do this anymore ha
                    cte.lastReqId = entry->request.clientreqid();
                    cte.replied = true;
                    cte.reply = reply;
                }
                else
                {
                    // We've subsequently prepared another operation from the
                    // same client. So this request must have been completed
                    // at the client, and there's no need to record the
                    // result.
                }

                /* Send reply */
                Debug("sending reply!");
                auto iter = clientAddresses.find(entry->request.clientid());
                if (iter != clientAddresses.end())
                {
                    transport->SendMessage(this, *iter->second, reply);
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

                const LogEntry *entry = log.Find(i);
                if (!entry)
                {
                    RPanic("Did not find operation " FMT_OPNUM " in log", i);
                }
                ASSERT(entry->state == LOG_STATE_PREPARED);
                UpdateClientTable(entry->request);

                PrepareOKMessage2 reply;
                reply.set_view(view);
                reply.set_replicaidx(myIdx);

                RDebug("Sending PREPAREOK for new uncommitted operation",
                       reply.view());

                if (!(transport->SendMessageToReplica(
                        this, configuration.GetLeaderIndex(view), reply)))
                {
                    RWarning("Failed to send PrepareOK message to leader");
                }
            }
        }

        void IOCL_CTReplica::RequestStateTransfer()
        {
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
            lastBatchEnd2 = lastOp;

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
                closeBatch2Timeout->Stop();
            }

            prepareOKQuorum.Clear();
            startViewChangeQuorum.Clear();
            doViewChangeQuorum.Clear();
        }

        void IOCL_CTReplica::StartViewChange(view_t newview)
        {
            RNotice("Starting view change for view " FMT_VIEW, newview);

            view = newview;
            status = STATUS_VIEW_CHANGE;

            viewChangeTimeout->Reset();
            nullCommitTimeout->Stop();
            resendPrepareTimeout->Stop();
            closeBatch2Timeout->Stop();

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
            cm.set_batchid(this->lastCommitted);

            ASSERT(AmLeader());

            if (!(transport->SendMessageToAll(this, cm)))
            {
                RWarning("Failed to send null COMMIT message to all replicas");
            }

            nullCommitTimeout->Reset();
        }

        void IOCL_CTReplica::UpdateClientTable(const Request &req)
        {
            ClientTableEntry &entry = clientTable[req.clientid()];

            ASSERT(entry.lastReqId <= req.clientreqid());

            if (entry.lastReqId == req.clientreqid())
            {
                return;
            }

            entry.lastReqId = req.clientreqid();
            entry.replied = false;
            entry.reply.Clear();
        }

        void IOCL_CTReplica::ResendPrepare()
        {
            ASSERT(AmLeader());
            if (lastOp == lastCommitted)
            {
                return;
            }
            RNotice("Resending prepare");
            if (!(transport->SendMessageToAll(this, lastPrepare2)))
            {
                RWarning("Failed to ressend prepare message to all replicas");
            }
        }

        void IOCL_CTReplica::CloseBatch()
        {
            Debug("Inside CloseBatch");
            ASSERT(AmLeader());
            ASSERT(lastBatchEnd < lastBatch);
            auto s = std::get<1>(thebatchs[lastBatchEnd]);
            ASSERT(s.size() == batchSize);

            RDebug("Sending batched prepare!! for unsorted batchid = %d", lastBatchEnd);
            /* Send prepare messages */
            PrepareMessage p;
            p.set_batchid(lastBatchEnd);

            for (const auto &entry_ptr : s)
            {
                Request *r = p.add_requests();
                *r = entry_ptr->request;
                // add an arrival timestamp for each request we replicate in the first round
                p.add_arrivalts(entry_ptr->arrivalTimestamp);
                p.add_shardtags(entry_ptr->myShardTag);
            }

            if (!(transport->SendMessageToAll(this, p)))
            {
                RWarning("Failed to send prepare message to all replicas");
            }
            lastBatchEnd = lastBatch;

            resendPrepareTimeout->Reset();
        }

        void IOCL_CTReplica::CloseBatch2()
        {
            Debug("Inside CloseBatch2");
            ASSERT(AmLeader());
            auto s = std::get<1>(thebatchs2[lastBatchEnd2]);
            // ASSERT(lastBatchEnd2 < lastOp);

            RDebug("Sending batched prepare2!! for sorted batchid = %d",
                   lastBatchEnd2);
            /* Send prepare messages */
            PrepareMessage2 pp;
            pp.set_view(view);
            pp.set_batchid(lastBatchEnd2);

            ASSERT(s.size() > 0);

            for (const auto &entry_ptr : s)
            {
                Request *r = pp.add_requests();
                ASSERT(entry_ptr->viewstamp.view == view);
                *r = entry_ptr->request;
                pp.add_shardtags(entry_ptr->myShardTag);
                pp.add_sortedts(entry_ptr->sortTimestamp);
            }
            lastPrepare2 = pp;

            if (!(transport->SendMessageToAll(this, pp)))
            {
                RWarning("Failed to send prepare message to all replicas");
            }
            lastBatchEnd2 = lastBatch2;
            Debug("Setting lastBatchEnd2 to %d", lastBatchEnd2);

            resendPrepareTimeout->Reset();
            closeBatch2Timeout->Stop();
        }

        void IOCL_CTReplica::ReceiveMessage(const TransportAddress &remote,
                                            const string &type, const string &data,
                                            void *meta_data)
        {
            RequestMessage request;
            /* Acks for timestamps ****************/
            SuccessorRequestMessage succReq;     ///
            PredecessorReplyMessage predReply;   ///
            PredecessorReplyMessage2 predReply2; ///
            /**************************************/
            UnloggedRequestMessage unloggedRequest;
            PrepareMessage prepare;
            PrepareOKMessage prepareOK;
            /* Second round of replication */
            PrepareMessage2 prepare2;     ///
            PrepareOKMessage2 prepareOK2; ///
            /*******************************/
            CommitMessage commit;
            RequestStateTransferMessage requestStateTransfer;
            StateTransferMessage stateTransfer;
            StartViewChangeMessage startViewChange;
            DoViewChangeMessage doViewChange;
            StartViewMessage startView;

            if (type == request.GetTypeName())
            {
                request.ParseFromString(data);
                HandleRequest(remote, request);
            }
            else if (type == unloggedRequest.GetTypeName())
            {
                unloggedRequest.ParseFromString(data);
                HandleUnloggedRequest(remote, unloggedRequest);
            }
            else if (type == succReq.GetTypeName())
            {
                succReq.ParseFromString(data);
                HandleCoordination(remote, succReq);
            }
            else if (type == predReply.GetTypeName())
            {
                predReply.ParseFromString(data);
                HandleCoordinationResp(remote, predReply);
            }
            else if (type == predReply2.GetTypeName())
            {
                predReply2.ParseFromString(data);
                HandleCoordinationResp2(remote, predReply2);
            }
            else if (type == prepare.GetTypeName())
            {
                prepare.ParseFromString(data);
                HandlePrepare(remote, prepare);
            }
            else if (type == prepareOK.GetTypeName())
            {
                prepareOK.ParseFromString(data);
                HandlePrepareOK(remote, prepareOK);
            }
            else if (type == prepare2.GetTypeName())
            {
                prepare2.ParseFromString(data);
                HandlePrepare2(remote, prepare2);
            }
            else if (type == prepareOK2.GetTypeName())
            {
                prepareOK2.ParseFromString(data);
                HandlePrepareOK2(remote, prepareOK2);
            }
            else if (type == commit.GetTypeName())
            {
                commit.ParseFromString(data);
                HandleCommit(remote, commit);
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
            else
            {
                RPanic("Received unexpected message type in IOCL_CT proto: %s",
                       type.c_str());
            }
        }

        void IOCL_CTReplica::HandleRequest(const TransportAddress &remote,
                                           const RequestMessage &msg)
        {
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

            RDebug("Handling Request with tag %d--I AM the leader", msg.shardtag());
            // Save the client's address
            clientAddresses.erase(msg.req().clientid());
            clientAddresses.insert(
                std::pair<uint64_t, std::unique_ptr<TransportAddress>>(
                    msg.req().clientid(),
                    std::unique_ptr<TransportAddress>(remote.clone())));

            // Check the client table to see if this is a duplicate request
            auto kv = clientTable.find(msg.req().clientid());
            if (kv != clientTable.end())
            {
                const ClientTableEntry &entry = kv->second;
                if (msg.req().clientreqid() < entry.lastReqId)
                {
                    RNotice("Ignoring stale request");
                    return;
                }
                if (msg.req().clientreqid() == entry.lastReqId)
                {
                    // This is a duplicate request. Resend the reply if we
                    // have one. We might not have a reply to resend if we're
                    // waiting for the other replicas; in that case, just
                    // discard the request.
                    if (entry.replied)
                    {
                        RNotice("Received duplicate request; resending reply");
                        if (!(transport->SendMessage(this, remote, entry.reply)))
                        {
                            RWarning("Failed to resend reply to client");
                        }
                        return;
                    }
                    else
                    {
                        RNotice(
                            "Received duplicate request but no reply available; "
                            "ignoring");
                        return;
                    }
                }
            }

            // Update the client table
            UpdateClientTable(msg.req());

            // Leader Upcall
            bool replicate = false;
            string res;
            LeaderUpcall(lastCommitted, msg.req().op(), replicate, res);
            ClientTableEntry &cte = clientTable[msg.req().clientid()];

            // Check whether this request should be committed to replicas
            if (!replicate)
            {
                Panic("why isn't replicate on???");
                ReplyMessage reply;
                reply.set_reply(res);
                reply.set_view(0);
                reply.set_opnum(0);
                reply.set_clientreqid(msg.req().clientreqid());
                cte.replied = true;
                cte.reply = reply;
                transport->SendMessage(this, remote, reply);
            }
            else
            {
                // RDebug("replicating to other replicas!");
                Request request;
                request.set_op(res);
                request.set_clientid(msg.req().clientid());
                request.set_clientreqid(msg.req().clientreqid());

                /* Assign it an arrival timestamp */
                Debug("Assign arrival timestamp = %d", shardTimestamp);
                uint64_t arrivalTimestamp = shardTimestamp;
                shardTimestamp++;

                /* Add outstanding successors that asked for my timestamp */
                std::vector<Successor *> successors = std::vector<Successor *>{};
                auto it = outstandingSuccessors.find(msg.shardtag());
                if (it != outstandingSuccessors.end())
                {
                    successors = it->second;
                    outstandingSuccessors.erase(msg.shardtag());
                };

                /* Add outstanding Predecessors that provided their timestamps */
                std::vector<Predecessor *> predecessors = std::vector<Predecessor *>{};
                uint64_t acks = 0;
                uint64_t acks2 = 0;
                RDebug("adding %d predecessors in predlist", msg.predlist_size());
                for (int i = 0; i < msg.predlist_size(); ++i)
                {
                    Predecessor *newp = new Predecessor{msg.predlist(i), -1, -1};
                    predecessors.push_back(newp);
                }
                if (outstandingPredecessors.find(msg.shardtag()) != outstandingPredecessors.end())
                {
                    auto predsmap = outstandingPredecessors.find(msg.shardtag())->second;
                    for (const auto &entry : predsmap)
                    {
                        const uint64_t &predIdx = entry.first;
                        Predecessor *p = entry.second;
                        predecessors[predIdx]->arrivalTimestamp = p->arrivalTimestamp;
                        predecessors[predIdx]->sortedTimestamp = p->sortedTimestamp;
                        acks = (p->arrivalTimestamp != -1) ? acks + 1 : acks;
                        acks2 = (p->sortedTimestamp != -1) ? acks2 + 1 : acks2;
                        delete p;
                    }
                    outstandingPredecessors.erase(msg.shardtag());
                }

                if (acks2 == predecessors.size())
                // Accounts for 0th requests and requests on fast path
                {
                    RDebug("ready to add to the SORTED log on FASTPATH!");
                    /* Add the request to my log(s) */
                    LogEntry &entry = log.AppendUnsorted(request, msg.shardtag(), LOG_STATE_ARRIVED, arrivalTimestamp, std::move(successors), std::move(predecessors), acks, acks2);
                    IOCL_CTReplica::finalizeEntry(entry, LOG_STATE_FASTPATH);
                }
                else
                // Ready to add to unsorted log!
                {
                    RDebug("Received REQUEST, adding to Unsorted log");
                    /* Add the request to my unorderedLog OR sorted log, depending */
                    LogEntry &entry = log.AppendUnsorted(request, msg.shardtag(), LOG_STATE_ARRIVED, arrivalTimestamp, std::move(successors), std::move(predecessors), acks, acks2);
                    // Add the request to the current pending batch
                    IOCL_CTReplica::addToPendingBatch(entry);
                    // Flush out the batch if it's hit batchSize
                    if (lastBatch - lastBatchEnd + 1 > batchSize)
                    {
                        CloseBatch();
                    }
                }
                Debug("at the end of handle request the log looks like ....");
                log.PrintSortedLog();

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
            RDebug("Received PREPARE");

            if (this->status != STATUS_NORMAL)
            {
                RDebug("Ignoring PREPARE due to abnormal status");
                return;
            }

            if (AmLeader())
            {
                RPanic("Unexpected PREPARE: I'm the leader of this view");
            }

            viewChangeTimeout->Reset();
            /* Build reply and send it to the leader */
            PrepareOKMessage reply;
            reply.set_replicaidx(myIdx);

            /* Add operations to the log */
            int i;
            for (auto &req : msg.requests())
            {
                log.AppendUnsorted(req, msg.shardtags(i), LOG_STATE_PREPARED, msg.arrivalts(i), std::vector<Successor *>{}, std::vector<Predecessor *>{}, 0, 0);
                UpdateClientTable(req);
                i++;
            }
            ASSERT(i == msg.requests().size());
            reply.set_batchid(msg.batchid());

            if (!(transport->SendMessageToReplica(
                    this, configuration.GetLeaderIndex(view), reply)))
            {
                RWarning("Failed to send PrepareOK message to leader");
            }
        }

        void IOCL_CTReplica::HandlePrepareOK(const TransportAddress &remote,
                                             const PrepareOKMessage &msg)
        {
            RDebug("Received PREPAREOK from replica %d",
                   msg.replicaidx());

            if (this->status != STATUS_NORMAL)
            {
                RDebug("Ignoring PREPAREOK due to abnormal status");
                return;
            }

            if (!AmLeader())
            {
                RWarning("Ignoring PREPAREOK because I'm not the leader");
                return;
            }

            uint64_t batchId = msg.batchid();

            ASSERT(batchId >= 0 && batchId < lastBatchEnd);
            if (thebatchs.find(batchId) == thebatchs.end())
            {
                RDebug("ignoring this ack");
                // gonna assume this means we're getting acks past the quorum
                return;
            }
            auto t = thebatchs[batchId];
            std::get<0>(t)++;

            // If this batch got a quorum of ACKs/is replicated!!
            if (std::get<0>(t) >= prepareOKQuorum.NumRequired())
            {
                PredecessorReplyMessage a;
                PredecessorReplyMessage2 aa;
                /*
                 * We have a quorum of PrepareOK messages for this
                 * batch.
                 *
                 *
                 * Loop through the batch
                 *
                 * Mark the entries as replicated
                 *
                 * Check if we've gotten any timestamps back from our predecessors
                 *
                 * Check if we've got any successors registered
                 */
                auto s = std::get<1>(t);
                for (const auto &entry_ptr : s)
                {
                    log.SetStatus(*entry_ptr, LOG_STATE_PREPARED);
                    if (entry_ptr->acks == entry_ptr->predecessors.size())
                    {
                        RDebug("Got all acks!!");
                        // Send sorted timestamp!
                        IOCL_CTReplica::assignSortedTs(*entry_ptr);
                        if (entry_ptr->acks2 == entry_ptr->predecessors.size())
                        {
                            RDebug("Got all acks2!!");
                            // Do final sort and replicated it
                            ASSERT(log.InSorted(entry_ptr->myShardTag));
                            IOCL_CTReplica::finalizeEntry(*entry_ptr);
                        }
                    }
                    else
                    {
                        ASSERT(entry_ptr->acks2 < entry_ptr->predecessors.size());
                        /* Send arrival timestamp in PredecessorReply messages to the registered successors */
                        a.set_arrivalts(entry_ptr->arrivalTimestamp);
                        a.set_p(entry_ptr->myShardTag);
                        for (auto it = entry_ptr->successors.begin(); it != entry_ptr->successors.end(); it++)
                        {
                            a.set_s((*it)->perShardTag);
                            // Sending to shard id, replicaIdx = 0 since that's where the leader is when there's no failures
                            if (!(transport->SendMessageToReplica(this, (*it)->shardId, 0, a)))
                            {
                                RWarning("Failed to send PredecessorReply to shard %d", (*it)->shardId);
                            }
                        }
                    }
                }
                thebatchs.erase(batchId);

                nullCommitTimeout->Reset();
            }
        }

        void IOCL_CTReplica::HandlePrepare2(const TransportAddress &remote,
                                            const PrepareMessage2 &msg)
        {
            RDebug("Received PREPARE2 < view = %d, batchid = %d",
                   msg.view(), msg.batchid());

            if (this->status != STATUS_NORMAL)
            {
                RDebug("Ignoring PREPARE2 due to abnormal status");
                return;
            }

            if (msg.view() < this->view)
            {
                RDebug("Ignoring PREPARE2 due to stale view");
                return;
            }

            if (msg.view() > this->view)
            {
                RequestStateTransfer();
                pendingPrepares.push_back(
                    std::pair<TransportAddress *, PrepareMessage2>(remote.clone(), msg));
                return;
            }

            if (AmLeader())
            {
                RPanic("Unexpected PREPARE2: I'm the leader of this view");
            }
            viewChangeTimeout->Reset();

            // if (msg.batchid() > this->lastOp + 1)
            // {
            //     RequestStateTransfer();
            //     pendingPrepares.push_back(
            //         std::pair<TransportAddress *, PrepareMessage2>(remote.clone(), msg));
            //     return;
            // }

            /* Add operations to the log */
            uint64_t op = msg.batchid();
            int i = 0;
            std::unordered_set<LogEntry *> s = {};
            for (auto &req : msg.requests())
            {
                // TODO ANJA this is supposed to be ApendUnsorted
                log.AppendUnsorted(req, msg.shardtags(i), LOG_STATE_ARRIVED, msg.sortedts(i), {}, {}, 0, 0);
                log.ResortSorted(viewstamp_t(msg.view(), i + op), LOG_STATE_READY, msg.shardtags(i), msg.sortedts(i));
                UpdateClientTable(req);
                s.insert(log.FindUnsorted(msg.shardtags(i)));
                i++;
            }
            ASSERT(i == msg.requests().size());
            thebatchs2[msg.batchid()] = {0, s};

            /* Build reply and send it to the leader */
            PrepareOKMessage2 reply;
            reply.set_view(msg.view());
            reply.set_batchid(msg.batchid());
            reply.set_replicaidx(myIdx);

            if (!(transport->SendMessageToReplica(
                    this, configuration.GetLeaderIndex(view), reply)))
            {
                RWarning("Failed to send PrepareOK2 message to leader");
            }
        }

        void IOCL_CTReplica::HandlePrepareOK2(const TransportAddress &remote,
                                              const PrepareOKMessage2 &msg)
        {
            RDebug("Received PREPAREOK2 for view=%d and batchid=%d from replica %d",
                   msg.view(), msg.batchid(), msg.replicaidx());

            if (this->status != STATUS_NORMAL)
            {
                RDebug("Ignoring PREPAREOK2 due to abnormal status");
                return;
            }

            if (msg.view() < this->view)
            {
                RDebug("Ignoring PREPAREOK2 due to stale view");
                return;
            }

            if (msg.view() > this->view)
            {
                RequestStateTransfer();
                return;
            }

            if (!AmLeader())
            {
                RWarning("Ignoring PREPAREOK2 because I'm not the leader");
                return;
            }
            uint64_t batchId = msg.batchid();

            ASSERT(batchId >= 0 && batchId <= lastBatchEnd2);
            if (thebatchs2.find(batchId) == thebatchs2.end())
            {
                Debug("Ignoring this prepareok2");
                // Assuming this is an ack for something that already got a quorum
                return;
            }
            auto t = thebatchs2[batchId];
            std::get<0>(t)++;
            // If this batch got a quorum of ACKs/is replicated!!
            if (std::get<0>(t) >= prepareOKQuorum.NumRequired())
            {
                /*
                 * We have a quorum of PrepareOK2 messages for this
                 * opnumber. Execute it and all previous operations.
                 *
                 * (Note that we might have already executed it. That's fine,
                 * we just won't do anything.)
                 *
                 * This also notifies the client of the result.
                 */
                CommitUpTo(msg.batchid());
                /*
                 * Send COMMIT message to the other replicas.
                 *
                 * This can be done asynchronously, so it really ought to be
                 * piggybacked on the next PREPARE or something.
                 */
                CommitMessage cm;
                cm.set_view(this->view);
                cm.set_batchid(msg.batchid());

                if (!(transport->SendMessageToAll(this, cm)))
                {
                    RWarning("Failed to send COMMIT message to all replicas");
                }

                thebatchs2.erase(batchId);

                nullCommitTimeout->Reset();
            }
        }

        void IOCL_CTReplica::HandleCommit(const TransportAddress &remote,
                                          const CommitMessage &msg)
        {
            RDebug("Received COMMIT for view %d and batchdi %d", msg.view(), msg.batchid());

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

            // if (msg.opnum() <= this->lastCommitted)
            // {
            //     RDebug("Ignoring COMMIT; already committed that operation");
            //     return;
            // }

            // if (msg.opnum() > this->lastOp)
            // {
            //     RequestStateTransfer();
            //     return;
            // }

            CommitUpTo(msg.batchid());
        }

        void IOCL_CTReplica::HandleCoordination(const TransportAddress &remote,
                                                const proto::SuccessorRequestMessage &msg)
        {
            ASSERT(AmLeader());
            Debug("Inside handle coordination! successor tag %d is looking for predecessor tag %d", msg.s(), msg.p());
            LogEntry *entry = log.FindUnsorted(msg.p());
            bool inSorted = log.InSorted(msg.p());
            log.PrintSortedLog();
            PredecessorReplyMessage a;
            a.set_p(msg.p());
            a.set_s(msg.s());
            a.set_predidx(msg.predidx());
            Successor *s = new Successor{msg.s(), msg.shardidx(), msg.predidx()};

            if (inSorted)
            {
                // Entry is in the sorted log already
                // Have already sent sortedTs to successors, there is no successor list anymore
                // This is probably a late successor
                // Fast path to second round
                // Send sortedTs to this successor
                RDebug("The predecessor is in the sorted log with state....");
                Debug("%s", log.PrintState(entry->state).c_str());
                ASSERT(entry->state == LOG_STATE_ASSIGNED || entry->state == LOG_STATE_READY || entry->state == LOG_STATE_COMMITTED || entry->state == LOG_STATE_FASTPATH);
                if (entry->state != LOG_STATE_FASTPATH)
                {
                    PredecessorReplyMessage2 aa;
                    aa.set_p(msg.p());
                    aa.set_s(msg.s());
                    aa.set_predidx(msg.predidx());
                    aa.set_sortedts(entry->sortTimestamp);
                    Debug("We are sending the predeessor's sorted timestamp which is is %d", entry->sortTimestamp);
                    if (!(transport->SendMessageToReplica(this, msg.shardidx(), 0, aa)))
                    {
                        RWarning("Failed to send PredecessorReplyMessage2 from HandleCoordination");
                    }
                }
                else
                {
                    Debug("The predecessor hasn't been replicated yet, so not sending anything to the successor at this time");
                    // else we'll send it from HandlePrepareOK2
                    entry->successors.push_back(s);
                }
            }
            else if (!inSorted && !entry)
            {
                // If the request isn't there yet, add it to the outstandingSuccessors map
                // Add this successor for the next round of timestamp replies
                RDebug("The predecessor has not arrived at this shard yet, not sending anything");
                outstandingSuccessors[msg.p()].push_back(s);
            }
            else if (!inSorted && entry)
            {
                RDebug("The predecessor is in the UNsorted log");
                Debug("%s", log.PrintState(entry->state).c_str());
                ASSERT(entry->state == LOG_STATE_ARRIVED || entry->state == LOG_STATE_PREPARED);
                // Add this successor for the next round of timestamp replies
                entry->successors.push_back(s);
                // If it's been prepared, send the correct timestamp to this asking successor
                if (entry->state == LOG_STATE_PREPARED)
                {
                    Debug("we are sending the predecessor's arrival timestamp which is %d", entry->arrivalTimestamp);
                    a.set_arrivalts(entry->arrivalTimestamp);
                    // Only send the ack once it's replicated the arrival timestamp!
                    if (!(transport->SendMessageToReplica(this, msg.shardidx(), 0, a)))
                    {
                        RWarning("Failed to send PredecessorReplyMessage from HandleCoordination to successor at shard %d", msg.shardidx());
                    }
                }
                // else we'll send it from HandlePrepareOK
            }
        }
        void IOCL_CTReplica::HandleCoordinationResp(const TransportAddress &remote,
                                                    const proto::PredecessorReplyMessage &msg)
        {
            ASSERT(AmLeader());
            Debug("Inside handleCoordinationResp! predecessor tag %d is ack2ing successor tag %d", msg.p(), msg.s());
            // The entry should be in the unsorted log
            LogEntry *entry = log.FindUnsorted(msg.s());
            bool inSorted = log.InSorted(msg.s());
            if (inSorted)
            {
                ASSERT(entry->state == LOG_STATE_ASSIGNED || entry->state == LOG_STATE_READY || entry->state == LOG_STATE_COMMITTED);
                // This request is in the sorted log... so it doesn't really need this response
                return;
            }
            else if (!inSorted && !entry)
            {
                // Entry never arrived yet
                IOCL_CTReplica::addOutstandingPredecessor(msg);
            }
            else if (!inSorted && entry)
            {
                ASSERT(entry->state == LOG_STATE_ARRIVED || entry->state == LOG_STATE_PREPARED);

                // entry is in the unsorted log!
                if (entry->predecessors[msg.predidx()]->arrivalTimestamp != -1 && entry->predecessors[msg.predidx()]->sortedTimestamp == -1)
                {
                    Panic("Duplicate arrival timestamp for this predecessor");
                }
                // Set the arrival timestamp
                entry->predecessors[msg.predidx()]->arrivalTimestamp = msg.arrivalts();
                entry->acks++;

                // If this is the nth predecessor ACK, compute a new timestamp
                if (entry->acks == entry->predecessors.size() && entry->state == LOG_STATE_PREPARED)
                {
                    IOCL_CTReplica::assignSortedTs(*entry);
                }
            }
        }

        void IOCL_CTReplica::HandleCoordinationResp2(const TransportAddress &remote,
                                                     const proto::PredecessorReplyMessage2 &msg)
        {
            ASSERT(AmLeader());
            Debug("Inside handleCoordinationREsp2! predecessor tag %d is ack2ing successor tag %d", msg.p(), msg.s());
            // This could be ariving for an entry that never came yet or for an entry that never got the first round ACK!
            LogEntry *entry = log.FindUnsorted(msg.s());
            bool inSorted = log.InSorted(msg.s());
            if (inSorted)
            {
                Debug("The successor is in the sorted log");
                ASSERT(entry->state == LOG_STATE_ASSIGNED || entry->state == LOG_STATE_READY || entry->state == LOG_STATE_COMMITTED);

                // Entry is in the sorted log!
                entry->acks2++;
                entry->predecessors[msg.predidx()]->sortedTimestamp = msg.sortedts();
                if (entry->acks2 == entry->predecessors.size())
                {
                    ASSERT(log.InSorted(entry->myShardTag));
                    IOCL_CTReplica::finalizeEntry(*entry);
                }
            }
            else if (!inSorted && !entry)
            {
                Debug("the successor has not arrived at this shard yet");
                // Entry has never arrived yet
                IOCL_CTReplica::addOutstandingPredecessor2(msg);
            }
            else if (!inSorted && entry)
            {
                Debug("the successor is in the UNsorted log");
                ASSERT(entry->state == LOG_STATE_ARRIVED || entry->state == LOG_STATE_PREPARED);

                // Entry is in the unsorted map... probably has been replicated...
                // Set the arrival timestamp
                entry->predecessors[msg.predidx()]->sortedTimestamp = msg.sortedts();
                entry->acks2++;
                Debug("incremented acks2");
                if (entry->predecessors[msg.predidx()]->arrivalTimestamp == -1)
                {
                    // Fast path or OoO
                    entry->predecessors[msg.predidx()]->arrivalTimestamp = msg.sortedts();
                    entry->acks++;
                    Debug("also incremeented acks1 ---> on the fast path!");
                }

                // If this is the nth predecessor ACK, compute a new timestamp
                if (entry->acks == entry->predecessors.size() && entry->state == LOG_STATE_PREPARED)
                {
                    Debug("nth predecessor ack, computing new timestamp");
                    IOCL_CTReplica::assignSortedTs(*entry);
                }

                // If this is the nth predecessor ACK2, compute a new timestamp as well
                if (entry->acks2 == entry->predecessors.size() && entry->state == LOG_STATE_ASSIGNED)
                {
                    Debug("finalizing entry");
                    ASSERT(entry->acks == entry->acks2);
                    ASSERT(log.InSorted(entry->myShardTag));
                    IOCL_CTReplica::finalizeEntry(*entry);
                }
            }
        }

        void IOCL_CTReplica::notifySuccessorsACK2(LogEntry &entry)
        {
            PredecessorReplyMessage2 aa;
            aa.set_p(entry.myShardTag);
            aa.set_sortedts(entry.sortTimestamp);
            for (auto it = entry.successors.begin(); it != entry.successors.end(); it++)
            {
                aa.set_s((*it)->perShardTag);
                aa.set_predidx((*it)->predIdx);
                // Sending to shard id, replicaIdx = 0 since that's where the leader is when there's no failures
                if (!(transport->SendMessageToReplica(this, (*it)->shardId, 0, aa)))
                {
                    RWarning("Failed to send PredecessorReply to shard %d", (*it)->shardId);
                }
            }

            // delete the successor list!!!!!!
            for (auto ptr : entry.successors)
            {
                delete ptr;
            }
            entry.successors.clear();
        }

        /* This function does:
         * 1. assigns a sorted timestamp
         * 2. modifies the entry and adds it to the sorted log
         * 3. sends the sorted timestamp (PredecessorReplyMessage2) to all registered successors
         */
        void IOCL_CTReplica::assignSortedTs(LogEntry &entry)
        {
            // Step 2.
            ASSERT(entry.state == LOG_STATE_PREPARED);
            LogEntry *p = log.Find(lastCommitted);
            uint64_t lastCommittedTimestamp = 0;
            if (p)
            {
                lastCommittedTimestamp = p->sortTimestamp;
            }
            entry.sortTimestamp = std::max(FoldL(entry.predecessors, true), lastCommittedTimestamp + 1);
            Debug("new sorted timestamp is %d", entry.sortTimestamp);
            // Insert into orderedLog, sorted by sortedTimestamp
            Debug("inserting into sorted log");
            log.AppendSorted(LOG_STATE_ASSIGNED, entry.myShardTag, entry.sortTimestamp);
            // Send ACK to all successors
            IOCL_CTReplica::notifySuccessorsACK2(entry);
        }

        void IOCL_CTReplica::finalizeEntry(LogEntry &entry, LogEntryState logstate)
        {
            // Step 3.
            LogEntry *p = log.Find(lastCommitted);
            uint64_t lastCommittedTimestamp = 0;
            if (p)
            {
                lastCommittedTimestamp = p->sortTimestamp;
            }
            entry.sortTimestamp = std::max(FoldL(entry.predecessors, false), lastCommittedTimestamp + 1);
            Debug("final timestamp is %d", entry.sortTimestamp);
            /* Assign it an opnum */
            viewstamp_t v;
            Debug("this->lastOp = %d", this->lastOp);
            ++this->lastOp;
            v.view = this->view;
            v.opnum = this->lastOp;
            Debug("after ++: this->lastOp = %d", this->lastOp);
            RDebug("For this request, assigning v.view = %d, v.opnum = %d", v.view, v.opnum);
            Debug("entry.viewstamp.opnum = %d", entry.viewstamp.opnum);
            log.PrintSortedLog();
            log.ResortSorted(v, logstate, entry.myShardTag, entry.sortTimestamp);
            Debug("entry.viewstamp.opnum after = %d", entry.viewstamp.opnum);
            Debug("now after resortSorted on %d, here's the sorted log:", entry.myShardTag);
            log.PrintSortedLog();
            // Add if it is the head, send out contiguous run of ready entries!!
            int count = log.MoveSortedToLog(entry.myShardTag);
            if (count > 0)
            {
                IOCL_CTReplica::addToPendingBatch2(count);
                // Flush out the batch if it's hit batchSize
                if (lastBatch2 - lastBatchEnd2 + 1 > batchSize)
                {
                    CloseBatch2();
                }
            }
        }

        void IOCL_CTReplica::addToPendingBatch(LogEntry &entry)
        {
            // Add the request to the current pending batch
            if (thebatchs.find(lastBatchEnd) != thebatchs.end())
            {
                auto t = thebatchs[lastBatchEnd];
                std::get<1>(t).insert(&entry);
            }
            else
            {
                std::unordered_set<LogEntry *> s = {};
                s.insert(&entry);
                thebatchs[lastBatchEnd] = std::make_tuple(1, s);
            }
            lastBatch++;
        }

        void IOCL_CTReplica::addToPendingBatch2(int count)
        {
            Debug("addToPendingBatch2 adding %d batched requests", count);
            std::tuple<int, std::unordered_set<replication::LogEntry *>> t;
            for (int i = 0; i < count; i++)
            {
                lastBatch2++;
                // TODO Anja: make sure this index is right oofgh
                if (thebatchs2.find(lastBatchEnd2) != thebatchs2.end())
                {
                    Debug("adding to existing batch for batchid %d", lastBatchEnd2);
                    t = thebatchs2[lastBatchEnd2];
                    LogEntry *batchedEntry = log.Find(lastBatch2 + i);
                    ASSERT(batchedEntry != NULL);
                    std::get<1>(t).insert(batchedEntry);
                }
                else
                {
                    Debug("starting new batch for batchid = %d, and looking in log entries for opnum %d", lastBatchEnd2, lastBatch2 + i);
                    std::unordered_set<LogEntry *> s = {};
                    LogEntry *batchedEntry = log.Find(lastBatch2 + i);
                    ASSERT(batchedEntry != NULL);
                    s.insert(batchedEntry);
                    t = std::make_tuple(1, s);
                    thebatchs2[lastBatchEnd2] = t;
                }
            }
        }

        void IOCL_CTReplica::addOutstandingPredecessor(const proto::PredecessorReplyMessage &msg)
        {
            auto arrivalTs = msg.arrivalts();
            auto sortedTs = -1;
            auto preds_map = outstandingPredecessors.find(msg.s());
            if (preds_map != outstandingPredecessors.end())
            {
                // The outstandingPredecessors map has an entry for this successor
                // so *some* predecessor to this yet unarrived successor has already replied
                auto thisp = preds_map->second.find(msg.predidx());
                if (thisp != (preds_map->second).end())
                {
                    // This is the second ACK from the same predecessor for this successor
                    Panic("why are we getting duplicate acks from the same pred to the same coord for arrival of Ack%d", 1);
                }
                else
                {
                    // This is the first time this predecessor ACKd
                    Predecessor *newp = new Predecessor{msg.p(), arrivalTs, sortedTs};
                    preds_map->second[msg.predidx()] = newp;
                }
            }
            else
            {
                // The outstandingPredecessors map doesn't have an entry for this successor at all
                std::unordered_map<uint64_t, replication::Predecessor *> m;
                Predecessor *newp = new Predecessor{msg.p(), arrivalTs, sortedTs};
                m[msg.predidx()] = newp;
                outstandingPredecessors[msg.s()] = m;
            }
        }

        void IOCL_CTReplica::addOutstandingPredecessor2(const proto::PredecessorReplyMessage2 &msg)
        {
            auto arrivalTs = -1;
            auto sortedTs = msg.sortedts();
            auto preds_map = outstandingPredecessors.find(msg.s());
            if (preds_map != outstandingPredecessors.end())
            {
                // The outstandingPredecessors map has an entry for this successor
                // so *some* predecessor to this yet unarrived successor has already replied
                auto thisp = preds_map->second.find(msg.predidx());
                if (thisp != preds_map->second.end())
                {
                    // This is the second ACK from the same predecessor for this successor
                    Panic("why are we getting duplicate acks from the same pred to the same coord for arrival of Ack%d", 2);
                }
                else
                {
                    // This is the first time this predecessor ACKd
                    Predecessor *newp = new Predecessor{msg.p(), arrivalTs, sortedTs};
                    preds_map->second[msg.predidx()] = newp;
                }
            }
            else
            {
                // The outstandingPredecessors map doesn't have an entry for this successor at all
                std::unordered_map<uint64_t, replication::Predecessor *> m;
                Predecessor *newp = new Predecessor{msg.p(), arrivalTs, sortedTs};
                m[msg.predidx()] = newp;
                outstandingPredecessors[msg.s()] = m;
            }
        }

        void IOCL_CTReplica::HandleRequestStateTransfer(
            const TransportAddress &remote, const RequestStateTransferMessage &msg)
        {
            RDebug("Received REQUESTSTATETRANSFER " FMT_VIEWSTAMP, msg.view(),
                   msg.opnum());

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

            log.Dump(msg.opnum() + 1, reply.mutable_entries());

            transport->SendMessage(this, remote, reply);
        }

        void IOCL_CTReplica::HandleStateTransfer(const TransportAddress &remote,
                                                 const StateTransferMessage &msg)
        {
            RDebug("Received STATETRANSFER " FMT_VIEWSTAMP, msg.view(), msg.opnum());

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
                    const LogEntry *entry = log.Find(newEntry.opnum());
                    ASSERT(entry->viewstamp.opnum == newEntry.opnum());
                    ASSERT(entry->viewstamp.view <= newEntry.view());

                    if (entry->viewstamp.view == newEntry.view())
                    {
                        // We already have this operation in our log.
                        ASSERT(entry->state == LOG_STATE_PREPARED);
#if PARANOID
//              ASSERT(entry->request == newEntry.request());
#endif
                    }
                    else
                    {
                        // Our operation was from an older view, so obviously
                        // it didn't survive a view change. Throw out any
                        // later log entries and replace with this one.
                        ASSERT(entry->state != LOG_STATE_COMMITTED);
                        log.RemoveAfter(newEntry.opnum());
                        lastOp = newEntry.opnum();
                        oldLastOp = lastOp;

                        viewstamp_t vs = {newEntry.view(), newEntry.opnum()};
                        log.Append(vs, newEntry.request(), LOG_STATE_PREPARED);
                    }
                }
                else
                {
                    // This is a new operation to us. Add it to the log.
                    ASSERT(newEntry.opnum() == lastOp + 1);

                    lastOp++;
                    viewstamp_t vs = {newEntry.view(), newEntry.opnum()};
                    log.Append(vs, newEntry.request(), LOG_STATE_PREPARED);
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
            std::list<std::pair<TransportAddress *, PrepareMessage2>> pending =
                pendingPrepares;
            pendingPrepares.clear();
            for (auto &msgpair : pendingPrepares)
            {
                RDebug("Processing pending prepare message");
                HandlePrepare2(*msgpair.first, msgpair.second);
                delete msgpair.first;
            }
        }

        void IOCL_CTReplica::HandleStartViewChange(const TransportAddress &remote,
                                                   const StartViewChangeMessage &msg)
        {
            RDebug("Received STARTVIEWCHANGE " FMT_VIEW " from replica %d", msg.view(),
                   msg.replicaidx());

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

            if (auto msgs = startViewChangeQuorum.AddAndCheckForQuorum(
                    msg.view(), msg.replicaidx(), msg))
            {
                int leader = configuration.GetLeaderIndex(view);
                // Don't try to send a DoViewChange message to ourselves
                if (leader != myIdx)
                {
                    DoViewChangeMessage dvc;
                    dvc.set_view(view);
                    dvc.set_lastnormalview(log.LastViewstamp().view);
                    dvc.set_lastop(lastOp);
                    dvc.set_lastcommitted(lastCommitted);
                    dvc.set_replicaidx(myIdx);

                    // Figure out how much of the log to include
                    opnum_t minCommitted =
                        std::min_element(
                            msgs->begin(), msgs->end(),
                            [](decltype(*msgs->begin()) a, decltype(*msgs->begin()) b)
                            {
                                return a.second.lastcommitted() <
                                       b.second.lastcommitted();
                            })
                            ->second.lastcommitted();
                    minCommitted = std::min(minCommitted, lastCommitted);

                    log.Dump(minCommitted, dvc.mutable_entries());

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

            auto msgs = doViewChangeQuorum.AddAndCheckForQuorum(msg.view(),
                                                                msg.replicaidx(), msg);
            if (msgs != NULL)
            {
                // Find the response with the most up to date log, i.e. the
                // one with the latest viewstamp
                view_t latestView = log.LastViewstamp().view;
                opnum_t latestOp = log.LastViewstamp().opnum;
                DoViewChangeMessage *latestMsg = NULL;

                for (auto kv : *msgs)
                {
                    DoViewChangeMessage &x = kv.second;
                    if ((x.lastnormalview() > latestView) ||
                        (((x.lastnormalview() == latestView) &&
                          (x.lastop() > latestOp))))
                    {
                        latestView = x.lastnormalview();
                        latestOp = x.lastop();
                        latestMsg = &x;
                    }
                }

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

                        log.RemoveAfter(latestMsg->lastop() + 1);
                        log.Install(latestMsg->entries().begin(),
                                    latestMsg->entries().end());
                    }
                }
                else
                {
                    RDebug("My log is most current, lastnormalview=" FMT_VIEW
                           " lastop=" FMT_OPNUM,
                           log.LastViewstamp().view, lastOp);
                }

                // How much of the log should we include when we send the
                // STARTVIEW message? Start from the lowest committed opnum of
                // any of the STARTVIEWCHANGE or DOVIEWCHANGE messages we got.
                //
                // We need to compute this before we enter the new view
                // because the saved messages will go away.
                auto svcs = startViewChangeQuorum.GetMessages(view);
                opnum_t minCommittedSVC =
                    std::min_element(
                        svcs.begin(), svcs.end(),
                        [](decltype(*svcs.begin()) a, decltype(*svcs.begin()) b)
                        {
                            return a.second.lastcommitted() < b.second.lastcommitted();
                        })
                        ->second.lastcommitted();
                opnum_t minCommittedDVC =
                    std::min_element(
                        msgs->begin(), msgs->end(),
                        [](decltype(*msgs->begin()) a, decltype(*msgs->begin()) b)
                        {
                            return a.second.lastcommitted() < b.second.lastcommitted();
                        })
                        ->second.lastcommitted();
                opnum_t minCommitted = std::min(minCommittedSVC, minCommittedDVC);
                minCommitted = std::min(minCommitted, lastCommitted);

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

                log.Dump(minCommitted, sv.mutable_entries());

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
                log.RemoveAfter(msg.lastop() + 1);
                log.Install(msg.entries().begin(), msg.entries().end());
            }

            EnterView(msg.view());
            opnum_t oldLastOp = lastOp;
            lastOp = msg.lastop();

            ASSERT(!AmLeader());

            CommitUpTo(msg.lastcommitted());
            SendPrepareOKs(oldLastOp);
        }

    } // namespace iocl_ct
} // namespace replication
