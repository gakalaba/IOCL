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
#include "store/common/backend/timingdebug.h"

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
              unorderedPrepareOKQuorum(config.QuorumSize() - 1),
              prepareOKQuorum(config.QuorumSize() - 1),
              startViewChangeQuorum(config.QuorumSize() - 1),
              doViewChangeQuorum(config.QuorumSize() - 1),
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
            unorderedBag.reserve(200000);
            unorderedBagByOpnum.reserve(200000);
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

        void IOCL_CTReplica::AppendToLog(IoclEntry *entry)
        {
            opnum_t start = 1;
            if (log.empty()) {
                ASSERT(entry->viewstamp.opnum == start);
            } else {
                ASSERT(entry->viewstamp.opnum == log.back()->viewstamp.opnum+1);
            }

            log.push_back(entry);
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

            IoclEntry *entry = log[opnum-start];
            ASSERT(entry->viewstamp.opnum == opnum);
            return entry;
        }

        viewstamp_t IOCL_CTReplica::LastViewstampOfLog() const
        {
            if (log.empty()) {
                return viewstamp_t(0, 0);
            } else {
                return log.back()->viewstamp;
            }
        }

        bool IOCL_CTReplica::AmLeader() const
        {
            return (configuration.GetLeaderIndex(view) == myIdx);
        }

        void IOCL_CTReplica::CommitUpTo(opnum_t upto)
        {
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

                const Request request = entry->request;

                /* Mark it as committed */
                entry->state = IOCL_STATE_COMMITTED;

                /* If there are other ops that haven't received their 
                   N final ACKs, then this op must block behind them.
                   Similarly, if there is no sublog, but this op hasn't
                   received all N final ACKs, insert it to the log and
                   do NOT execute it (for now replicas do, since we don't
                   track acks there, and i don't want to implement the final
                   ACK probably via piggybacking mechanism)*/ 
                if ((perKeySubLogs.find(entry->intkey) != perKeySubLogs.end()) || 
                    (AmLeader() && (entry->finalAcks.size() != entry->predList.predlist_size()))) {
                    // Warning("Not committing operation " FMT_OPNUM " because not all predecessor final ACKs have arrived (%d/%d)",
                    //         lastCommitted, entry->finalAcks.size(), entry->predList.predlist_size());
                    if (entry->finalAcks.size() > entry->predList.predlist_size()) {
                        Panic("Should not be getting more final ACKs than predecessors?");
                    }
                    // Add ourselves to the perKeySubLog to be executed when all N Acks arrive
                    auto &vec = perKeySubLogs[entry->intkey];
                    vec.push_back(lastCommitted);
                    return;
                }
                if (AmLeader()) {
                    ASSERT(entry->finalAcks.size() == entry->predList.predlist_size());
                    ASSERT(perKeySubLogs.find(entry->intkey) == perKeySubLogs.end());

                    /* Execute it */
                    ReadyFinalRoutine(entry);
                    return;
                }

                /* Replica path: */
                /* Execute it */
                ReplyMessage reply;
                Execute(entry->viewstamp.opnum, entry->request, reply);

                reply.set_view(entry->viewstamp.view);
                reply.set_opnum(entry->viewstamp.opnum);
                reply.set_clientreqid(entry->request.clientreqid());

                // Store reply in the client table
                // ClientTableEntry &cte = clientTable[entry->request.clientid()];
                // if (cte.lastReqId <= entry->request.clientreqid())
                // {
                //     cte.lastReqId = entry->request.clientreqid();
                //     cte.replied = true;
                //     cte.reply = reply;
                // }
                // else
                // {
                //     // We've subsequently prepared another operation from the
                //     // same client. So this request must have been completed
                //     // at the client, and there's no need to record the
                //     // result.
                // }

                // /* Send reply */
                // auto iter = clientAddresses.find(entry->request.clientid());
                // if (iter != clientAddresses.end())
                // {
                //     transport->SendMessage(this, *iter->second, reply);
                // }
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
                UpdateClientTable(entry->request);

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

            prepareOKQuorum.Clear();
            startViewChangeQuorum.Clear();
            doViewChangeQuorum.Clear();
            unorderedPrepareOKQuorum.Clear();
        }

        void IOCL_CTReplica::StartViewChange(view_t newview)
        {
            RNotice("Starting view change for view " FMT_VIEW, newview);

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

            if (!(transport->SendMessageToAll(this, cm)))
            {
                RWarning("Failed to send null COMMIT message to all replicas");
            }

            nullCommitTimeout->Reset();
        }

        void IOCL_CTReplica::UpdateClientTable(const Request &req)
        {
            Panic("Shouldn't be calling this right now");
            ClientTableEntry &entry = clientTable[req.clientid()];
            Debug("the request has clientid %lu and clientreqid %lu",
                   req.clientid(), req.clientreqid());
            Debug("we are checking entry.lastReqId (= %lu) < req.clientreqid (= %lu)",
                   entry.lastReqId, req.clientreqid());

            if (entry.lastReqId > req.clientreqid()) {

                Panic("we are checking entry.lastReqId (= %lu) < req.clientreqid (= %lu)",
                   entry.lastReqId, req.clientreqid());
            }

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
            if (!(transport->SendMessageToAll(this, lastPrepare)))
            {
                RWarning("Failed to ressend prepare message to all replicas");
            }
            // Keep retrying
            resendPrepareTimeout->Reset();
        }

        void IOCL_CTReplica::ResendUnorderedPrepare()
        {
            ASSERT(AmLeader());
            if (unorderedBagByOpnum.empty())
            {
                return;
            }
            RNotice("Resending unordered prepare for last message with shardtag = %lu", lastUnorderedPrepare.shardtags(0));
            if (!(transport->SendMessageToAll(this, lastUnorderedPrepare)))
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

            UnorderedPrepareMessage up;
            up.set_view(view);
            up.set_opnum(lastUnorderedOp);
            up.set_batchstart(unorderedBatchStart);

            for (opnum_t i = unorderedBatchStart; i <= lastUnorderedOp; i++)
            {
                Request *r = up.add_request();
                const IoclEntry& entry = *unorderedBagByOpnum[i];
                ASSERT(entry.viewstamp.view == view);
                *r = entry.request;
                up.add_shardtags(entry.myShardTag);
                PredListHolder* pl = up.add_predlists();
                pl->CopyFrom(entry.predList);
            }
            lastUnorderedPrepare = up;

            if (!(transport->SendMessageToAll(this, up)))
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

            RDebug("Sending batched prepare from " FMT_OPNUM " to " FMT_OPNUM,
                   batchStart, lastOp);
            /* Send prepare messages */
            PrepareMessage p;
            p.set_view(view);
            p.set_opnum(lastOp);
            p.set_batchstart(batchStart);

            for (opnum_t i = batchStart; i <= lastOp; i++)
            {
                Request *r = p.add_request();
                const IoclEntry *entry = FindInLog(i);
                ASSERT(entry != NULL);
                ASSERT(entry->viewstamp.view == view);
                ASSERT(entry->viewstamp.opnum == i);
                *r = entry->request;
                p.add_shardtags(entry->myShardTag);
                PredListHolder* ts_chain = p.add_timestamp_chains();
                // loop through predecessorArrivalTs and add to timestamp chain
                for (auto ts : entry->predecessorArrivalTs) {
                    ts_chain->add_predlist(ts);
                }
                // Add my finalTs at the end
                ts_chain->add_predlist(entry->finalTs);
                // Debug("The final added ts_chain looks like this:");
                // for (int idx = 0; idx < ts_chain->predlist_size(); idx++) {
                //     Warning("TO DELETE!!!!!!!! ts_chain predlist[%d] = %lu", idx, ts_chain->predlist(idx));
                // }
            }

            lastPrepare = p;

            if (!(transport->SendMessageToAll(this, p)))
            {
                RWarning("Failed to send prepare message to all replicas");
            }
            lastBatchEnd = lastOp;

            resendPrepareTimeout->Reset();
            closeBatchTimeout->Stop();
        }

        void IOCL_CTReplica::ReceiveMessage(const TransportAddress &remote,
                                       const string &type, const string &data,
                                       void *meta_data)
        {
            RequestMessage request;
            UnloggedRequestMessage unloggedRequest;
            PrepareMessage prepare;
            PrepareOKMessage prepareOK;
            UnorderedPrepareMessage unorderedPrepare;
            UnorderedPrepareOKMessage unorderedPrepareOK;
            CommitMessage commit;
            RequestStateTransferMessage requestStateTransfer;
            StateTransferMessage stateTransfer;
            StartViewChangeMessage startViewChange;
            DoViewChangeMessage doViewChange;
            StartViewMessage startView;
            SuccessorRequestMessage coordReq;
            PredecessorReplyMessage coordResp;
            PredecessorFinalMessage coordFinal;


            if (type == request.GetTypeName())
            {
                // Request arrived -- issue unordered prepare
                request.ParseFromString(data);
                Notice("                (E) Received Op on Leader Replica %lu", now_us());
                HandleRequest(remote, request);
            }
            else if (type == coordReq.GetTypeName())
            {
                // Successor request arrived
                coordReq.ParseFromString(data);
                HandleCoordination(remote, coordReq);
            }
            else if (type == coordResp.GetTypeName())
            {
                // Predecessor reply arrived
                coordResp.ParseFromString(data);
                HandleCoordinationReply(remote, coordResp);
            }
            else if (type == coordFinal.GetTypeName())
            {
                // Predecessor final ACK arrived
                coordFinal.ParseFromString(data);
                HandleCoordinationFinal(remote, coordFinal);
            }
            else if (type == unorderedPrepare.GetTypeName())
            {
                unorderedPrepare.ParseFromString(data);
                HandleUnorderedPrepare(remote, unorderedPrepare);
            }
            else if (type == unorderedPrepareOK.GetTypeName())
            {
                // Request persisted at quorum -- ensue regular VR prepare
                unorderedPrepareOK.ParseFromString(data);
                HandleUnorderedPrepareOK(remote, unorderedPrepareOK);
            }
            else if (type == unloggedRequest.GetTypeName())
            {
                unloggedRequest.ParseFromString(data);
                HandleUnloggedRequest(remote, unloggedRequest);
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
                RPanic("Received unexpected message type in iocl_ct proto: %s",
                       type.c_str());
            }
        }

        void IOCL_CTReplica::HandleRequest(const TransportAddress &remote,
                                      RequestMessage &msg)
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

            // Save the client's address
            clientAddresses.erase(msg.req().clientid());
            clientAddresses.insert(
                std::pair<uint64_t, std::unique_ptr<TransportAddress>>(
                    msg.req().clientid(),
                    std::unique_ptr<TransportAddress>(remote.clone())));

            // Check the client table to see if this is a duplicate request
            /*
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
            }*/

            // Update the client table
            //UpdateClientTable(msg.req());

            // Leader Upcall
            bool replicate = false;
            string res;
            LeaderUpcall(lastCommitted, msg.req().op(), replicate, res);

            Request request;
            request.set_op(res);
            request.set_clientid(msg.req().clientid());
            request.set_clientreqid(msg.req().clientreqid());

            /* Assign it an opnum within this view --> this is 
                strictly to compy with quorum checking which 
                currently is unique per viewstamp_t */
            ++this->lastUnorderedOp;
            v.view = this->view;
            v.opnum = this->lastUnorderedOp;

            /* Add the request to the unordered bag */
            uint64_t shardtag = msg.shardtag();

            auto result = unorderedBag.emplace(
                shardtag,
                std::make_unique<IoclEntry>(
                    v, IOCL_STATE_ARRIVED, request, shardtag, msg.intkey()
                )
            );
            auto it = result.first;
            bool inserted = result.second;
            if (!inserted) {
                IoclEntry *existingEntry = it->second.get();
                Warning("here's everything i know abotu the existing entry: state = %d, myShardTag = %lu, intkey = %lu, ACKs = %d arrivalTs = %lu finalTs = %lu, num_preds = %d, clientreqid = %lu",
                        existingEntry->state, existingEntry->myShardTag, existingEntry->intkey, existingEntry->ACKs, existingEntry->arrivalTs, existingEntry->finalTs, existingEntry->predList.predlist_size(), existingEntry->request.clientreqid());
                Panic("ok");
            }
            IoclEntry *entryPtr = it->second.get();

            // Grab the msg.predlist() efficiently and store
            entryPtr->predList.mutable_predlist()->Swap(msg.mutable_predlist());
            entryPtr->predecessorArrivalTs.resize(entryPtr->predList.predlist_size());

            /* Add entry to "ordered" unorderedBag (for batching) */
            if (replicate) unorderedBagByOpnum.emplace(v.opnum, entryPtr);

            /* Go through any outstanding predecessor replies and add them in */
            auto pit = outstandingCoordinationResps.find(shardtag);
            if (pit != outstandingCoordinationResps.end()) {
                auto &predAcks = pit->second;
                for (const auto& resp : predAcks) {
                    ASSERT(resp.predidx() < entryPtr->predList.predlist_size());
                    // ASSERT(entryPtr->predecessorArrivalTs[resp.predidx()] == 0);
                    entryPtr->predecessorArrivalTs[resp.predidx()] = resp.arrivalts();
                    entryPtr->ACKs++;
                }
                outstandingCoordinationResps.erase(pit);
            }
            /* Also go through any outstanding predecessor final ACKs and add them in */
            auto fit = outstandingCoordinationFinals.find(shardtag);
            if (fit != outstandingCoordinationFinals.end()) {
                auto &predFinals = fit->second;
                for (const auto& finalAck : predFinals) {
                    auto facks_it = entryPtr->finalAcks.find({finalAck.p(), finalAck.shardidx()});
                    if (facks_it != entryPtr->finalAcks.end()) {
                        Warning("Duplicate final ACK received from predecessor with shardtag %lu and shardidx %lu for my shardtag %lu",
                            finalAck.p(), finalAck.shardidx(), entryPtr->myShardTag);
                    } else {
                        /* ASSERT THIS IS A LEGAL PREDECESSOR */
                        entryPtr->finalAcks.emplace(finalAck.p(), finalAck.shardidx());
                    }
                }
                outstandingCoordinationFinals.erase(fit);
            }

            // Check whether this request should be committed to replicas
            if (!replicate)
            {
                entryPtr->replicate = false;
                RDebug("Not replicating to replicas");


                /* Progress state to Persisted */
                entryPtr->state = IOCL_STATE_PERSISTED;

                /* Assign Arrival Timestamp */
                auto ts_it = lastReadyTS.find(entryPtr->intkey);
                uint64_t ts = (ts_it == lastReadyTS.end()) ? 0 : ts_it->second;
                entryPtr->arrivalTs = std::max(shardTS, ts);
                entryPtr->finalTs = entryPtr->arrivalTs; // will be updated later
                shardTS++;

                /* Insert into the perKeySubqueue so that Head Of Line Blocking begins! */
                perKeySubqueues[entryPtr->intkey].insert(entryPtr);

                auto it = outstandingCoordinationReqs.find(entryPtr->myShardTag);
                if (it != outstandingCoordinationReqs.end()) {
                    PredecessorReplyMessage preply;
                    preply.set_arrivalts(entryPtr->arrivalTs);
                    for (const auto& succ : it->second) {
                        preply.set_s(succ.s());
                        preply.set_predidx(succ.predidx());
                        if (!(transport->SendMessageToReplica(this, succ.shardidx(), 0, preply)))
                        {
                            RWarning("Failed to send SuccessorReply message to client");
                        }
                        /* And save the successor ! */
                        auto succ_it = entryPtr->successors.find({succ.s(), succ.shardidx()});
                        if (succ_it == entryPtr->successors.end()) {
                            // Map the successor shardtag to its shardidx
                            entryPtr->successors.emplace(std::make_pair(succ.s(), succ.shardidx()), 0);
                        } else {
                            Warning("Duplicate successor request received for successor %lu on shard %lu", succ.s(), succ.shardidx());
                        }
                    }
                    outstandingCoordinationReqs.erase(it);
                }
                
                if (entryPtr->state == IOCL_STATE_PERSISTED &&
                        entryPtr->ACKs == entryPtr->predList.predlist_size()) {
                    /* Now can progress to READY state */
                    ReadyRoutine(entryPtr);
                }
                return;
            } else {
                entryPtr->replicate = true;
                RDebug("Replicating to replicas");
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

        uint64_t IOCL_CTReplica::FoldL(const proto::PredListHolder &pl)
        {
            // check if empty
            if (pl.predlist_size() == 0)
            {
                return 0;
            }
            auto v = pl.predlist(0);
            for (uint64_t e : pl.predlist())
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

        void IOCL_CTReplica::ReadyFinalRoutine(IoclEntry *entry)
        {
            if (perKeySubLogs.find(entry->intkey) == perKeySubLogs.end()) {
                /* We can immediately execute this entry */
                auto &vec = perKeySubLogs[entry->intkey];
                vec.push_back(entry->viewstamp.opnum);
            }

            /* Execute as many head entries from the sublog as are ready */
            auto &vec = perKeySubLogs[entry->intkey];
            while (true) {
                if (vec.empty()) {
                    /* Delete it and return */
                    perKeySubLogs.erase(entry->intkey);
                    ASSERT(perKeySubLogs.find(entry->intkey) == perKeySubLogs.end());
                    break;
                }
                opnum_t headOpnum = vec.front();
                const IoclEntry *entry = FindInLog(headOpnum);
                ASSERT(entry->state == IOCL_STATE_COMMITTED);
                if (entry->finalAcks.size() != entry->predList.predlist_size()) {
                    /* Still waiting on final ACKs, done with loop */
                    if (entry->finalAcks.size() > entry->predList.predlist_size()) {
                        Panic("Should not be getting more final ACKs than predecessors?");
                    }
                    break;
                }
                /* Remove from sublog */
                vec.erase(vec.begin());
                /* Execute it */
                ReplyMessage reply;
                Execute(entry->viewstamp.opnum, entry->request, reply);

                reply.set_view(entry->viewstamp.view);
                reply.set_opnum(entry->viewstamp.opnum);
                reply.set_clientreqid(entry->request.clientreqid());

                // Store reply in the client table
                // ClientTableEntry &cte = clientTable[entry->request.clientid()];
                // if (cte.lastReqId <= entry->request.clientreqid())
                // {
                //     cte.lastReqId = entry->request.clientreqid();
                //     cte.replied = true;
                //     cte.reply = reply;
                // }
                // else
                // {
                //     // We've subsequently prepared another operation from the
                //     // same client. So this request must have been completed
                //     // at the client, and there's no need to record the
                //     // result.
                // }

                /* Send reply */
                auto iter = clientAddresses.find(entry->request.clientid());
                if (iter != clientAddresses.end())
                {
                    transport->SendMessage(this, *iter->second, reply);
                }
            }
        }

        void IOCL_CTReplica::ReadyRoutine(IoclEntry *entry)
        {
            /* Remove from subqueue */
            perKeySubqueues[entry->intkey].erase(entry);    // Erase by pointer identity            
            /* Assign a final TS */
            entry->finalTs = std::max(entry->arrivalTs, FoldL(entry->predList));
            lastReadyTS[entry->intkey] = entry->finalTs + 1;
            /* Reinsert as newly sorted */
            perKeySubqueues[entry->intkey].insert(entry);
            /* Assign it ready state */
            entry->state = IOCL_STATE_READY;

            auto &sq = perKeySubqueues[entry->intkey];
            while (true) {
                if (sq.empty()) {
                    break;
                }
                IoclEntry* head = *sq.begin();
                if (head->state != IOCL_STATE_PERSISTED && head->state != IOCL_STATE_READY) {
                    Warning("the head (shardtag = %lu) is currently neither PERSISTED nor READY, instead it is in state %d", head->myShardTag,
                            head->state);
                    ASSERT(head->state == IOCL_STATE_PERSISTED || head->state == IOCL_STATE_READY);
                }
                if (head->state != IOCL_STATE_READY) {
                    break;
                }
                /* Progress to REQUEST ordered */
                sq.erase(sq.begin());
                /* Send out the Final ACK to all successors */
                PredecessorFinalMessage predFinal;
                predFinal.set_p(head->myShardTag);
                predFinal.set_shardidx(groupIdx);
                for (const auto& kv : head->successors) {
                    if (kv.second > 0) {
                        continue;
                    }
                    predFinal.set_s(kv.first.first);
                    if (!(transport->SendMessageToReplica(this, kv.first.second, 0, predFinal)))
                    {
                        RWarning("Failed to send SuccessorRequest message to client");
                    }
                    // Mark that we've sent to this successor
                    head->successors[kv.first] = 1;
                }

                /* Assign it a real opnum for this view in the ordered log */
                viewstamp_t v;
                ++this->lastOp;
                v.view = this->view;
                v.opnum = this->lastOp;
                head->viewstamp = v;
                /* Set it as Prepared (since it isn't quite committed yet ) */
                head->state = IOCL_STATE_PREPARED;

                /* Add the request to my log */
                AppendToLog(head);
                if (!(head->replicate))
                {
                    const Request request = head->request;

                    /* Mark it as committed */
                    head->state = IOCL_STATE_COMMITTED;

                    /* If there are other ops that haven't received their 
                    N final ACKs, then this op must block behind them.
                    Similarly, if there is no sublog, but this op hasn't
                    received all N final ACKs, insert it to the sublog and
                    do NOT execute it (for now replicas do, since we don't
                    track acks there, and i don't want to implement the final
                    ACK probably via piggybacking mechanism)*/ 
                    if ((perKeySubLogs.find(head->intkey) != perKeySubLogs.end()) || 
                        (AmLeader() && (head->finalAcks.size() != head->predList.predlist_size()))) {
                        // Warning("Not committing operation " FMT_OPNUM " because not all predecessor final ACKs have arrived (%d/%d)",
                        //         lastCommitted, head->finalAcks.size(), head->predList.predlist_size());
                        if (head->finalAcks.size() > head->predList.predlist_size()) {
                            Panic("Should not be getting more final ACKs than predecessors?");
                        }
                        // Add ourselves to the perKeySubLog to be executed when all N Acks arrive
                        auto &vec = perKeySubLogs[head->intkey];
                        vec.push_back(lastCommitted);
                        return;
                    }
                    ASSERT(head->finalAcks.size() == head->predList.predlist_size());
                    ASSERT(perKeySubLogs.find(head->intkey) == perKeySubLogs.end());

                    /* Execute it */
                    ReadyFinalRoutine(head);
                    return;
                }

                if (lastOp - lastBatchEnd + 1 > batchSize)
                {
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

            viewstamp_t vs = {msg.view(), msg.opnum()};
            if (auto msgs =
                    (unorderedPrepareOKQuorum.AddAndCheckForQuorum(vs, msg.replicaidx(), msg)))
            {
                if (msgs->size() >= (unsigned int)configuration.QuorumSize())
                {
                    return;
                }
                for (opnum_t i = msg.batchstart(); i <= msg.opnum(); i++)
                {
                    auto pair = unorderedBagByOpnum.find(i);
                    if (pair == unorderedBagByOpnum.end())
                    {
                        RPanic("Did not find unordered operation with tag");
                    }
                    IoclEntry *entry = pair->second;
                    /* Progress state to Persisted */
                    entry->state = IOCL_STATE_PERSISTED;

                    /* Assign Arrival Timestamp */
                    auto ts_it = lastReadyTS.find(entry->intkey);
                    uint64_t ts = (ts_it == lastReadyTS.end()) ? 0 : ts_it->second;
                    entry->arrivalTs = std::max(shardTS, ts);
                    entry->finalTs = entry->arrivalTs; // will be updated later
                    shardTS++;

                    /* Insert into the perKeySubqueue so that Head Of Line Blocking begins! */
                    perKeySubqueues[entry->intkey].insert(entry);
                    // /* Code Instrumentation ! */
                    // perKeyQueueLengths[entry->intkey].push_back(perKeySubqueues[entry->intkey].size());

                    /* If it has any pending successor requests in
                    outstandingCoordinationReqs, respond to them now */
                    auto it = outstandingCoordinationReqs.find(entry->myShardTag);
                    if (it != outstandingCoordinationReqs.end()) {
                        PredecessorReplyMessage preply;
                        preply.set_arrivalts(entry->arrivalTs);
                        for (const auto& succ : it->second) {
                            preply.set_s(succ.s());
                            preply.set_predidx(succ.predidx());
                            if (!(transport->SendMessageToReplica(this, succ.shardidx(), 0, preply)))
                            {
                                RWarning("Failed to send SuccessorReply message to client");
                            }
                            /* And save the successor ! */
                            auto succ_it = entry->successors.find({succ.s(), succ.shardidx()});
                            if (succ_it == entry->successors.end()) {
                                // Map the successor shardtag to its shardidx
                                entry->successors.emplace(std::make_pair(succ.s(), succ.shardidx()), 0);
                            } else {
                                Warning("Duplicate successor request received for successor %lu on shard %lu", succ.s(), succ.shardidx());
                            }
                        }
                        outstandingCoordinationReqs.erase(it);
                    }
                    /* If it has been persisted, it doesn't need to be retried, 
                    remove from unorderedBagByOpnum tracker */
                    unorderedBagByOpnum.erase(entry->viewstamp.opnum);
                    if (entry->state == IOCL_STATE_PERSISTED &&
                            entry->ACKs == entry->predList.predlist_size()) {
                        /* Now can progress to READY state */
                        ReadyRoutine(entry);
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
                   (unsigned int)msg.request_size());

            viewChangeTimeout->Reset();

            if (msg.opnum() <= this->lastOp)
            {
                RDebug("Ignoring PREPARE; already prepared that operation");
                // Resend the prepareOK message
                PrepareOKMessage reply;
                reply.set_view(msg.view());
                reply.set_opnum(msg.opnum());
                reply.set_replicaidx(myIdx);
                if (!(transport->SendMessageToReplica(
                        this, configuration.GetLeaderIndex(view), reply)))
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
            int i = 0;
            for (auto &req : msg.request())
            {
                op++;
                if (op <= lastOp)
                {
                    continue;
                }
                this->lastOp++;
                uint64_t shardtag = msg.shardtags(i);

                /* Find the entry */
                auto it = unorderedBag.find(shardtag);
                if (it == unorderedBag.end()) {
                    Panic("Replica didn't have request with shardtag %lu in unorderedBag during Prepare",
                        shardtag);
                }
                IoclEntry *entry = it->second.get();
                /* Update its state */
                entry->viewstamp.view = msg.view();
                entry->viewstamp.opnum = op;
                entry->state = IOCL_STATE_PREPARED;
                // loop through timestamp_chains and add to predecessorArrivalTs
                const proto::PredListHolder& ts_chain = msg.timestamp_chains(i);
                uint64_t N = ts_chain.predlist_size();
                entry->predecessorArrivalTs.resize(N);
                for (int j = 0; j < (N-1); j++) {
                    uint64_t ts = ts_chain.predlist(j);
                    entry->predecessorArrivalTs[j] = ts;
                }
                entry->finalTs = ts_chain.predlist(N-1);
                /* Add the request to my log */
                AppendToLog(entry);
                /* Remove from the batched unorderdBagByOpnum */
                unorderedBagByOpnum.erase(entry->viewstamp.opnum);

                // UpdateClientTable(req);
            }
            ASSERT(op == msg.opnum());

            /* Build reply and send it to the leader */
            PrepareOKMessage reply;
            reply.set_view(msg.view());
            reply.set_opnum(msg.opnum());
            reply.set_replicaidx(myIdx);

            if (!(transport->SendMessageToReplica(
                    this, configuration.GetLeaderIndex(view), reply)))
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
                RequestStateTransfer();
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

            if (msg.opnum() <= this->lastUnorderedOp)
            {
                Panic("hopefully won't be going through this case");
                RDebug("Ignoring UNORDERED_PREPARE; already prepared that operation");
                // Resend the prepareOK message
                PrepareOKMessage reply;
                reply.set_view(msg.view());
                reply.set_opnum(msg.opnum());
                reply.set_replicaidx(myIdx);
                if (!(transport->SendMessageToReplica(
                        this, configuration.GetLeaderIndex(view), reply)))
                {
                    RWarning("Failed to send PrepareOK message to leader");
                }
                return;
            }

            // Add operations to the unordered bag
            int i = 0;
            opnum_t op = msg.batchstart() - 1;
            for (const auto &req : msg.request())
            {
                op++;
                if (op <= lastUnorderedOp)
                {
                    continue;
                }
                this->lastUnorderedOp++;
                /* Add the request to the unordered bag */
                uint64_t shardtag = msg.shardtags(i);

                /* For now we don't replicate the intkey at replicas
                Instead, if a new leader takes over, it can get its key from 
                the string in the Request */
                auto result = unorderedBag.emplace(
                    shardtag,
                    std::make_unique<IoclEntry>(
                        viewstamp_t(msg.view(), op), IOCL_STATE_PERSISTED, req, shardtag, 0
                    )
                );
                auto it = result.first;
                bool inserted = result.second;
                ASSERT(inserted);
                IoclEntry *entryPtr = it->second.get();

                // Grab the msg.predlist() efficiently and store
                entryPtr->predList.Swap(msg.mutable_predlists(i));
                unorderedBagByOpnum.emplace(op, entryPtr);
                i++;
            }

            /* Build reply and send it to the leader */
            UnorderedPrepareOKMessage reply;
            reply.set_view(msg.view());
            reply.set_opnum(msg.opnum());
            reply.set_batchstart(msg.batchstart());
            reply.set_replicaidx(myIdx);

            if (!(transport->SendMessageToReplica(
                    this, configuration.GetLeaderIndex(view), reply)))
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

            viewstamp_t vs = {msg.view(), msg.opnum()};
            if (auto msgs =
                    (prepareOKQuorum.AddAndCheckForQuorum(vs, msg.replicaidx(), msg)))
            {
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

                if (msgs->size() >= (unsigned int)configuration.QuorumSize())
                {
                    return;
                }

                /*
                 * Send COMMIT message to the other replicas.
                 *
                 * This can be done asynchronously, so it really ought to be
                 * piggybacked on the next PREPARE or something.
                 */
                CommitMessage cm;
                cm.set_view(this->view);
                cm.set_opnum(this->lastCommitted);

                if (!(transport->SendMessageToAll(this, cm)))
                {
                    RWarning("Failed to send COMMIT message to all replicas");
                }

                nullCommitTimeout->Reset();
            }
        }

        void IOCL_CTReplica::HandleCoordinationFinal(const TransportAddress &remote,
                                                const proto::PredecessorFinalMessage &msg)
        {
            auto it = unorderedBag.find(msg.s());
            if (it == unorderedBag.end()) {

                auto &vec = outstandingCoordinationFinals[msg.s()];
                vec.emplace_back(std::move(msg));
                return;
            }
            IoclEntry *entry = it->second.get();

            /* assert that there are no outstanding
               responses for this successor in the
               outstandingCoordinationFinals -- should
               have been drained when upon arrival */
            ASSERT(outstandingCoordinationFinals.find(entry->myShardTag) == outstandingCoordinationFinals.end());
            /* Mark that this predecessor has finalized */
            auto facks_it = entry->finalAcks.find({msg.p(), msg.shardidx()});
            if (facks_it != entry->finalAcks.end()) {
                Warning("Duplicate final ACK received from predecessor with shardtag %lu on shard %lu for my shardtag %lu",
                        msg.p(), msg.shardidx(), entry->myShardTag);
                return;
            }
            /* ASSERT THIS IS A LEGAL PREDECESSOR */
            entry->finalAcks.emplace(msg.p(), msg.shardidx());
            /* Now it is safe to Execute this operation! */
            /* Check if it is waiting to be executed */
            if ((entry->state == IOCL_STATE_COMMITTED) && (entry->finalAcks.size() == entry->predList.predlist_size())) {
                ASSERT(perKeySubLogs.find(entry->intkey) != perKeySubLogs.end());
                ReadyFinalRoutine(entry);
            }
        }

        void IOCL_CTReplica::HandleCoordination(const TransportAddress &remote,
                                                const proto::SuccessorRequestMessage &msg)
        {
            auto it = unorderedBag.find(msg.p());
            if (it == unorderedBag.end()) {
                auto &vec = outstandingCoordinationReqs[msg.p()];
                vec.emplace_back(std::move(msg));
                return;
            }
            IoclEntry *entry = it->second.get();
            /* If not yet persisted, can't respond yet */
            if (entry->state == IOCL_STATE_ARRIVED) {
                auto &vec = outstandingCoordinationReqs[msg.p()];
                vec.emplace_back(std::move(msg));
                return;
            }
            /* assert that there are no outstanding
               requests for this predecessor in the
               outstandingCoordinationReqs -- should
               have been drained when state changed */
            ASSERT(outstandingCoordinationReqs.find(entry->myShardTag) == outstandingCoordinationReqs.end());

            /* Send reply now */
            PredecessorReplyMessage preply;
            preply.set_arrivalts(entry->arrivalTs);
            preply.set_s(msg.s());
            preply.set_predidx(msg.predidx());
            if (!(transport->SendMessageToReplica(this, msg.shardidx(), 0, preply)))
            {
                RWarning("Failed to send SuccessorReply message to client");
            }
            // NOTE the shardidx is int32
            /* Store the successor for the final TS */

            auto succ_it = entry->successors.find({msg.s(), msg.shardidx()});
            if (succ_it == entry->successors.end()) {
                entry->successors.emplace(std::make_pair(msg.s(), msg.shardidx()), 0);
            } else {
                Warning("Duplicate successor request received for successor %lu on shard %lu", msg.s(), msg.shardidx());
            }
            /* Reply to the successor if we've already been added to the ordered log */
            if (entry->state == IOCL_STATE_READY || entry->state == IOCL_STATE_PREPARED || entry->state == IOCL_STATE_COMMITTED) {
                /* Send out the Final ACK to all successors */
                PredecessorFinalMessage predFinal;
                predFinal.set_p(entry->myShardTag);
                predFinal.set_s(msg.s());
                predFinal.set_shardidx(groupIdx);
                entry->successors[{msg.s(), msg.shardidx()}] = 1; // Mark that we've sent final ACK to this successor
                if (!(transport->SendMessageToReplica(this, msg.shardidx(), 0, predFinal)))
                {
                    RWarning("Failed to send SuccessorRequest message to client");
                }
            }

            return;
        }

        void IOCL_CTReplica::HandleCoordinationReply(const TransportAddress &remote,
                                                const proto::PredecessorReplyMessage &msg)
        {
            // NOTE the shardidx is int32
            auto it = unorderedBag.find(msg.s());
            if (it == unorderedBag.end()) {
                auto &vec = outstandingCoordinationResps[msg.s()];
                vec.emplace_back(std::move(msg));
                return;
            }
            IoclEntry *entry = it->second.get();
            /* assert that there are no outstanding
               responses for this successor in the
               outstandingCoordinationResps -- should
               have been drained when upon arrival */
            ASSERT(outstandingCoordinationResps.find(entry->myShardTag) == outstandingCoordinationResps.end());
            ASSERT(msg.predidx() < entry->predList.predlist_size());
            // ASSERT(entry->predecessorArrivalTs[msg.predidx()] == 0); --> OTHERWISE DEBUG DUPLICATION MESSAGE
            entry->predecessorArrivalTs[msg.predidx()] = msg.arrivalts();
            entry->ACKs++;
            // Might remove this for dedup
            ASSERT(entry->state == IOCL_STATE_ARRIVED || entry->state == IOCL_STATE_PERSISTED);
            if (entry->state == IOCL_STATE_PERSISTED &&
                     entry->ACKs == entry->predList.predlist_size()) {
                /* Now can progress to READY state */
                ReadyRoutine(entry);
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

            if (auto msgs = startViewChangeQuorum.AddAndCheckForQuorum(
                    msg.view(), msg.replicaidx(), msg))
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

            auto msgs = doViewChangeQuorum.AddAndCheckForQuorum(msg.view(),
                                                                msg.replicaidx(), msg);
            if (msgs != NULL)
            {
                // Find the response with the most up to date log, i.e. the
                // one with the latest viewstamp
                view_t latestView = LastViewstampOfLog().view;
                opnum_t latestOp = LastViewstampOfLog().opnum;
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
