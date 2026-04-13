// -*- mode: c++; c-file-style: "k&r"; c-basic-offset: 4 -*-
/***********************************************************************
 *
 * iocl_craq/replica.cc:
 *   IOCL_CRAQ protocol
 *
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
#include "replication/iocl_craq/replica.h"
#include "replication/iocl_craq/iocl_craq-proto.pb.h"

#define RDebug(fmt, ...) Debug("[%d] " fmt, myIdx, ##__VA_ARGS__)
#define RNotice(fmt, ...) Notice("[%d] " fmt, myIdx, ##__VA_ARGS__)
#define RWarning(fmt, ...) Warning("[%d] " fmt, myIdx, ##__VA_ARGS__)
#define RPanic(fmt, ...) Panic("[%d] " fmt, myIdx, ##__VA_ARGS__)

namespace replication
{
    namespace iocl_craq
    {

        using namespace proto;

        IOCL_CRAQReplica::IOCL_CRAQReplica(transport::Configuration config, int groupIdx, int myIdx,
                                 Transport *transport, unsigned int batchSize,
                                 AppReplica *app, bool debug_stats)
            : myIdx{myIdx},
              numReplicas{config.n},
              Replica(config, groupIdx, myIdx, transport, app),
              batchSize(batchSize),
              log(false),
              commitLog(false),
              commitLogOpnum(0),
              debug_stats_{debug_stats}
        {
            this->status = STATUS_NORMAL;
            this->view = 0;
            this->lastOp = 0;
            this->lastCommitted = 0;
            lastBatchEnd = 0;

            vectorClock.assign(configuration.g, 0);

            if (batchSize > 1)
            {
                Notice("Batching enabled; batch size %d", batchSize);
            }

            this->closeBatchTimeout =
                new Timeout(transport, 300, [this]()
                            { CloseBatch(); });

            if (debug_stats_)
            {
                _Latency_Init(&rec_to_upcall_lat_, "rec_to_upcall");
                _Latency_Init(&upcall_to_exec_lat_, "upcall_to_exec");
                _Latency_Init(&exec_to_sent_lat_, "exec_to_sent");
            }
        }

        IOCL_CRAQReplica::~IOCL_CRAQReplica()
        {
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

       bool IOCL_CRAQReplica::ForwardPropagateMessageInChain(const Message &m)
       {
        if (AmTail())
        {
            Panic("Tail can't forward propagate message, it's the last node in the chain");
        }
        Debug("Forward propagating message to %d", myIdx + 1);
        return transport->SendMessageToReplica(this, myIdx + 1, m);
       } 

       bool IOCL_CRAQReplica::BackwardsPropagateMessageInChain(const Message &m)
       {
        if (AmHead())
        {
            Panic("Head can't backwards propagate message, it's the first node in the chain");
        }
        Debug("Backwards propagates message to %d", myIdx - 1);
        return transport->SendMessageToReplica(this, myIdx - 1, m);
       } 
       
       bool IOCL_CRAQReplica::SendMessageToAllPreviousReplicasInChain(const Message &m)
       {
        if (!AmTail())
        {
            Panic("Shouldn't call from a non-tail replica");
        }

        bool success = true;
        for (int replicaIndex = 0; replicaIndex < myIdx; ++replicaIndex)
        {
            if (!transport->SendMessageToReplica(this, replicaIndex, m))
            {
               success = false; 
            }
        }
        return success;
       }
       
        Request IOCL_CRAQReplica::ToRequest(const replication::LinearizeableOperation &linRequest)
        {
            Request request;
            string linop_string;

            linRequest.SerializeToString(&linop_string);
            request.set_op(linop_string);
            request.set_clientid(linRequest.rid().client_id());
            request.set_clientreqid(linRequest.rid().client_req_id());

            return request;
        }

        LinearizeableOperation IOCL_CRAQReplica::ToLinearizableRequest(const Request &request)
        {
            LinearizeableOperation linRequest;
            linRequest.ParseFromString(request.op());
            linRequest.mutable_rid()->set_client_id(request.clientid());
            linRequest.mutable_rid()->set_client_req_id(request.clientreqid());

            return linRequest;
        }

        void IOCL_CRAQReplica::SyncVC(const google::protobuf::RepeatedField<google::protobuf::uint64> &remoteVC)
        {
            Debug("Syncing Vc");
            for (int i = 0; i < remoteVC.size() && i < (int)vectorClock.size(); ++i)
            {
                if (remoteVC[i] > vectorClock[i])
                {
                    vectorClock[i] = remoteVC[i];
                }
            }
        }

        void IOCL_CRAQReplica::SendCoordResponseMsg(const proto::SuccessorRequestMessage &coordReq)
        {
            Debug("Sending coordination response");
            PredecessorReplyMessage preply;
            preply.set_s(coordReq.s());
            for (uint64_t v : vectorClock)
            {
                preply.add_vector_clock(v);
            }
            if (!transport->SendMessageToReplica(this, coordReq.succ_groupidx(),
                                                  coordReq.succ_replicaidx(), preply))
            {
                RWarning("Failed to send CoordResponse for successor shardtag %lu", coordReq.s());
            }
        }

        void IOCL_CRAQReplica::DrainPendingCoordRequests()
        {
            for (uint64_t shardTag : pendingCoordDrain_)
            {
                auto pendIt = pendingCoordRequests.find(shardTag);
                if (pendIt != pendingCoordRequests.end())
                {
                    for (const auto &coordReq : pendIt->second)
                    {
                        SendCoordResponseMsg(coordReq);
                    }
                    pendingCoordRequests.erase(pendIt);
                }
            }
            pendingCoordDrain_.clear();
        }

        void IOCL_CRAQReplica::BroadcastCommit(const string &key)
        {
            ASSERT(AmTail());
            CommitMessage cm;
            cm.set_view(this->view);
            cm.set_opnum(this->lastCommitted);
            cm.set_key(key);
            for (uint64_t v : vectorClock)
            {
                cm.add_vector_clock(v);
            }
            if (!SendMessageToAllPreviousReplicasInChain(cm))
            {
                RWarning("Failed to send CommitMessage from tail");
            }
        }

        void IOCL_CRAQReplica::ExecuteWriteOperation(const LinearizeableOperation &linRequest)
        {
            RDebug("Executing write request " FMT_OPNUM, lastCommitted);
            ReplyMessage reply;

            const string key = linRequest.key();

            keyToVersionNumber[key] = std::max(keyToVersionNumber[key], lastCommitted);

            Request request = ToRequest(linRequest);

            Execute(Timestamp{lastCommitted}, request, reply);
            if (AmTail())
            {
                SendReplyToClient(linRequest, reply);
            }
        }

        void IOCL_CRAQReplica::ExecuteReadOperation(const LinearizeableOperation &linRequest)
        {
            RDebug("Executing read request " FMT_OPNUM, lastCommitted);
            ReplyMessage reply;

            Request request = ToRequest(linRequest);

            Execute(Timestamp{keyToVersionNumber[linRequest.key()]}, request, reply);
            SendReplyToClient(linRequest, reply);

            // Mark this read as "committed" for coordination purposes so that
            // successor operations which declared this read as a predecessor can
            // be unblocked via CoordResponse.
            if (linRequest.has_shardtag() && linRequest.shardtag() != 0)
            {
                uint64_t myShardTag = linRequest.shardtag();
                committedForCoord[myShardTag] = true;
                if (pendingCoordRequests.count(myShardTag))
                {
                    pendingCoordDrain_.push_back(myShardTag);
                    DrainPendingCoordRequests();
                }
            }
        }

        void IOCL_CRAQReplica::SendReplyToClient(const LinearizeableOperation &request,
                                                   ReplyMessage &reply, opnum_t opnum)
        {
            reply.set_view(this->view);
            reply.set_opnum(opnum != 0 ? opnum : lastCommitted);
            reply.set_clientreqid(request.rid().client_req_id());

            ClientTableEntry &cte = clientTable[request.rid().client_id()];
            if (cte.lastReqId <= request.rid().client_req_id())
            {
                if (request.op() == PUT_OPERATION)
                {
                    cte.lastReqId = request.rid().client_req_id();
                }
                cte.replied = true;
                cte.reply = reply;
            }

            auto iter = clientAddresses.find(request.rid().client_id());
            if (iter != clientAddresses.end())
            {
                Debug("Found message, sending to client");
                transport->SendMessage(this, *iter->second, reply);
            }
        }

        void IOCL_CRAQReplica::FlushWritesUpTo(opnum_t upto)
        {
            auto it = pendingWrites.begin();
            while (it != pendingWrites.end() && it->first <= upto)
            {
                ASSERT(++commitLogOpnum > 0);
                ASSERT(commitLogOpnum > commitLog.LastOpnum());
                commitLog.Append(
                    viewstamp_t(view, commitLogOpnum),
                    ToRequest(it->second),
                    LOG_STATE_CLEAN);
                it = pendingWrites.erase(it);
            }
        }

        void IOCL_CRAQReplica::CommitUpTo(opnum_t upto)
        {
            while (lastCommitted < upto)
            {
                lastCommitted++;

                const LogEntry *entry = log.Find(lastCommitted);
                if (entry == nullptr)
                {
                    RPanic("Did not find operation " FMT_OPNUM " in log",
                           lastCommitted);
                }

                bool status = log.SetStatus(lastCommitted, LOG_STATE_CLEAN);
                Debug("Status is %d", status);

                auto cacheIt = linOpCache_.find(lastCommitted);
                ASSERT(cacheIt != linOpCache_.end());
                const LinearizeableOperation &linRequest = cacheIt->second;
                const string &op = linRequest.op();

                Debug("Commiting entry with op %s", op.c_str());

                if (op == PUT_OPERATION)
                {
                    // IOCL gate: at the tail, a write with predecessors must
                    // wait for all CoordResponses before sending its client reply.
                    //
                    // FIX: Never block CommitUpTo. Apply the write to the store
                    // immediately (preserving log-order store consistency) and
                    // mark committedForCoord so cross-shard CoordResponses can
                    // flow.  If not all CoordResponses have arrived yet, defer
                    // the client reply in pendingGateReplies_ until they do.
                    // This breaks the deadlock where blocking the log at position N
                    // prevents later-positioned ops (needed to unblock N) from
                    // committing.
                    if (AmTail() && linRequest.predlist_size() > 0 &&
                        linRequest.predlist(0) != 0)
                    {
                        uint64_t myShardTag = linRequest.has_shardtag() ? linRequest.shardtag() : 0;
                        if (myShardTag != 0)
                        {
                            // Count non-zero predlist entries: one CoordRequest is
                            // sent (and one CoordResponse expected) per entry.
                            int expected = 0;
                            for (int pi = 0; pi < linRequest.predlist_size(); pi++)
                            {
                                if (linRequest.predlist(pi) != 0) expected++;
                            }

                            int received = 0;
                            auto cntIt = coordResponseCount_.find(myShardTag);
                            if (cntIt != coordResponseCount_.end())
                                received = cntIt->second;

                            // Update version index eagerly for VersionRequest correctness.
                            keyToVersionNumber[linRequest.key()] =
                                std::max(keyToVersionNumber[linRequest.key()], lastCommitted);

                            // Do NOT set committedForCoord eagerly here.
                            // committedForCoord must be set only when Execute fires so that
                            // the cascade is preserved: a successor's CoordResponse arrives
                            // only after this write's Execute (its gate-fire), not as soon
                            // as its PrepareMessage chain completes.  Setting it early causes
                            // all CoordResponses to arrive at ~300ms regardless of predlist
                            // depth, collapsing the 400/500/600/700ms staircase to flat ~500ms.
                            //
                            // NOTE: this means circular cross-shard deps can deadlock at the
                            // gate level (not the log level, since the log is non-blocking).
                            // Avoid circular deps by keeping client count low relative to the
                            // number of shards, or by limiting predlist depth.
                            vectorClock[groupIdx]++;

                            if (received >= expected)
                            {
                                // All CoordResponses already arrived: open gate now.
                                auto vcIt = pendingCoordResponses.find(myShardTag);
                                if (vcIt != pendingCoordResponses.end())
                                {
                                    SyncVC(vcIt->second.vector_clock());
                                    pendingCoordResponses.erase(vcIt);
                                }
                                coordResponseCount_.erase(myShardTag);
                                // Execute applies store write AND sends reply to ShardClient.
                                // Set committedForCoord AFTER Execute so downstream CoordRequests
                                // are drained only after this write is truly visible.
                                ReplyMessage reply;
                                Request request = ToRequest(linRequest);
                                Execute(Timestamp{lastCommitted}, request, reply);
                                committedForCoord[myShardTag] = true;
                                if (pendingCoordRequests.count(myShardTag))
                                    pendingCoordDrain_.push_back(myShardTag);
                                SendReplyToClient(linRequest, reply, lastCommitted);
                            }
                            else
                            {
                                // Defer Execute (and thus the ShardClient reply) until
                                // all CoordResponses arrive in HandleCoordinationReply.
                                // committedForCoord set there, after Execute.
                                pendingGateReplies_[myShardTag] =
                                    {linRequest, lastCommitted, expected};
                            }

                            FlushWritesUpTo(lastCommitted);
                            linOpCache_.erase(cacheIt);
                            continue;
                        }
                    }

                    ExecuteWriteOperation(linRequest);

                    // After committing a write at the tail: update VC and record
                    // in committedForCoord. Defer draining pendingCoordRequests
                    // into pendingCoordDrain_ so the caller can send CoordResponses
                    // AFTER BroadcastCommit (ensuring CommitMessages are queued
                    // before CoordResponses in SimulatedTransport's FIFO queue).
                    if (AmTail() && linRequest.has_shardtag() && linRequest.shardtag() != 0)
                    {
                        vectorClock[groupIdx]++;

                        uint64_t myShardTag = linRequest.shardtag();
                        committedForCoord[myShardTag] = true;

                        if (pendingCoordRequests.count(myShardTag))
                        {
                            pendingCoordDrain_.push_back(myShardTag);
                        }
                    }

                    FlushWritesUpTo(lastCommitted);
                }

                linOpCache_.erase(cacheIt);
            }
        }

        void IOCL_CRAQReplica::TryServeWaitingReads()
        {
            auto it = readsWaitingForVC.begin();
            while (it != readsWaitingForVC.end())
            {
                if (lastCommitted >= it->first)
                {
                    ExecuteReadOperation(it->second);
                    ASSERT(++commitLogOpnum > 0);
                    ASSERT(commitLogOpnum > commitLog.LastOpnum());
                    commitLog.Append(
                        viewstamp_t(view, commitLogOpnum),
                        ToRequest(it->second),
                        LOG_STATE_CLEAN);
                    it = readsWaitingForVC.erase(it);
                }
                else
                {
                    ++it;
                }
            }
        }

        void IOCL_CRAQReplica::SendVersionRequest(const LinearizeableOperation &request)
        {
            VersionRequestMessage msg;
            msg.set_key(request.key());
            msg.set_clientid(request.rid().client_id());
            msg.set_clientreqid(request.rid().client_req_id());
            msg.set_replicaidx(myIdx);

            Debug("Sending version request for key %s for client %d and client request id %d",
                  request.key().c_str(), request.rid().client_id(), request.rid().client_req_id());

            pendingReads[{request.rid().client_id(), request.rid().client_req_id()}] = request;

            transport->SendMessageToReplica(this, numReplicas - 1, msg);
        }

        void IOCL_CRAQReplica::UpdateClientTable(const LinearizeableOperation &req)
        {
            ClientTableEntry &entry = clientTable[req.rid().client_id()];

            Debug("for clientid %d, last req id is %d while current req id is %d",
                  req.rid().client_id(), entry.lastReqId, req.rid().client_req_id());
            ASSERT(entry.lastReqId <= req.rid().client_req_id());

            if (entry.lastReqId == req.rid().client_req_id())
            {
                return;
            }

            entry.replied = false;
            entry.reply.Clear();
        }

        bool IOCL_CRAQReplica::IsDuplicateRequest(const TransportAddress &remote,
                                                    const LinearizeableOperation &linRequest)
        {
            auto kv = clientTable.find(linRequest.rid().client_id());
            if (kv != clientTable.end())
            {
                const ClientTableEntry &entry = kv->second;
                if (linRequest.rid().client_req_id() < entry.lastReqId)
                {
                    RNotice("Ignoring stale request");
                    return true;
                }
                if (linRequest.rid().client_req_id() == entry.lastReqId)
                {
                    if (entry.replied)
                    {
                        RNotice("Received duplicate request; resending reply");
                        if (!(transport->SendMessage(this, remote, entry.reply)))
                        {
                            RWarning("Failed to resend reply to client");
                        }
                        return true;
                    }
                    else
                    {
                        RNotice("Received duplicate request but no reply available; ignoring");
                        return true;
                    }
                }
            }

            return false;
        }

        void IOCL_CRAQReplica::UpdateClientAddresses(const TransportAddress &remote,
                                                      const LinearizeableOperation &linRequest)
        {
            clientAddresses.erase(linRequest.rid().client_id());
            clientAddresses.insert(
                std::pair<uint64_t, std::unique_ptr<TransportAddress>>(
                    linRequest.rid().client_id(),
                    std::unique_ptr<TransportAddress>(remote.clone())));
        }

        void IOCL_CRAQReplica::CloseBatch()
        {
            ASSERT(!AmTail());
            ASSERT(lastBatchEnd < lastOp);

            opnum_t batchStart = lastBatchEnd + 1;

            RDebug("Sending batched prepare from " FMT_OPNUM " to " FMT_OPNUM,
                   batchStart, lastOp);
            PrepareMessage p;
            p.set_view(view);
            p.set_opnum(lastOp);
            p.set_batchstart(batchStart);

            for (opnum_t i = batchStart; i <= lastOp; i++)
            {
                Request *r = p.add_request();
                const LogEntry *entry = log.Find(i);
                ASSERT(entry != NULL);
                ASSERT(entry->viewstamp.view == view);
                ASSERT(entry->viewstamp.opnum == i);
                *r = entry->request;
            }

            if (!ForwardPropagateMessageInChain(p))
            {
                RWarning("Failed to send prepare message to next replica from head");
            }
            else
            {
                Debug("Sent message from idx %d to idx %d", myIdx, myIdx + 1);
            }
            Debug("Propagated prepare message (write batch) from idx %d", myIdx);
            lastBatchEnd = lastOp;

            closeBatchTimeout->Stop();
        }

        void IOCL_CRAQReplica::ReceiveMessage(const TransportAddress &remote,
                                         const string &type, const string &data,
                                         void *meta_data)
        {
            LinearizeableOperation request;
            UnloggedRequestMessage unloggedRequest;
            PrepareMessage prepare;
            CommitMessage commit;
            VersionRequestMessage versionRequest;
            VersionResponseMessage versionResponse;
            SuccessorRequestMessage coordReq;
            PredecessorReplyMessage coordResp;

            if (type == request.GetTypeName())
            {
                request.ParseFromString(data);
                HandleRequest(remote, request);
            }
            else if (type == prepare.GetTypeName())
            {
                prepare.ParseFromString(data);
                HandlePrepare(remote, prepare);
            }
            else if (type == commit.GetTypeName())
            {
                commit.ParseFromString(data);
                HandleCommit(remote, commit);
            }
            else if (type == versionRequest.GetTypeName())
            {
                versionRequest.ParseFromString(data);
                HandleVersionRequest(remote, versionRequest);
            }
            else if (type == versionResponse.GetTypeName())
            {
                versionResponse.ParseFromString(data);
                HandleVersionResponse(remote, versionResponse);
            }
            else if (type == coordReq.GetTypeName())
            {
                coordReq.ParseFromString(data);
                HandleCoordination(remote, coordReq);
            }
            else if (type == coordResp.GetTypeName())
            {
                coordResp.ParseFromString(data);
                HandleCoordinationReply(remote, coordResp);
            }
            else
            {
                RPanic("Received unexpected message type in IOCL_CRAQ proto: %s",
                       type.c_str());
            }
        }

        void IOCL_CRAQReplica::HandleRequest(const TransportAddress &remote,
                                        const LinearizeableOperation &linRequest)
        {
            const string op = linRequest.op();

            if (op == PUT_OPERATION)
            {
                HandleWriteRequest(remote, linRequest);
            }
            else if (op == GET_OPERATION)
            {
                HandleReadRequest(remote, linRequest);
            }
            else
            {
                RPanic("Received unexpected op in IOCL_CRAQ proto: %s", op.c_str());
            }
        }

        void IOCL_CRAQReplica::HandleReadRequest(const TransportAddress &remote,
                                                  const LinearizeableOperation &linRequest)
        {
            Debug("Handling read request for client id %lu and client request id %lu",
                  linRequest.rid().client_id(), linRequest.rid().client_req_id());
            if (status != STATUS_NORMAL)
            {
                RNotice("Ignoring request due to abnormal status");
                return;
            }

            UpdateClientAddresses(remote, linRequest);

            if (IsDuplicateRequest(remote, linRequest))
                return;

            UpdateClientTable(linRequest);

            // If this read has predecessors, ensure all CoordResponses have arrived.
            if (linRequest.predlist_size() > 0 && linRequest.predlist(0) != 0)
            {
                uint64_t myShardTag = linRequest.has_shardtag() ? linRequest.shardtag() : 0;
                if (myShardTag != 0)
                {
                    int expected = 0;
                    for (int pi = 0; pi < linRequest.predlist_size(); pi++)
                        if (linRequest.predlist(pi) != 0) expected++;

                    int received = coordResponseCount_.count(myShardTag)
                                   ? coordResponseCount_[myShardTag] : 0;

                    if (received >= expected)
                    {
                        // All CoordResponses already arrived before this read.
                        Debug("All %d CoordResponses already arrived for read shardtag %lu", expected, myShardTag);
                        auto vcIt = pendingCoordResponses.find(myShardTag);
                        if (vcIt != pendingCoordResponses.end())
                        {
                            SyncVC(vcIt->second.vector_clock());
                            pendingCoordResponses.erase(vcIt);
                        }
                        coordResponseCount_.erase(myShardTag);
                        // Fall through to serve the read normally.
                    }
                    else
                    {
                        Debug("Buffering read shardtag %lu waiting for CoordResponses (%d/%d)", myShardTag, received, expected);
                        readsWaitingForCoord[myShardTag] = linRequest;
                        readsExpectedCoordCount_[myShardTag] = expected;
                        return;
                    }
                }
            }

            bool replicate = false;
            string res;
            string messageString;
            linRequest.SerializeToString(&messageString);
            LeaderUpcall(lastCommitted, messageString, replicate, res);

            if (!replicate)
            {
                RPanic("Should always replicate when using IOCL_CRAQ");
            }

            if (this->lastOp != lastCommitted && !AmTail())
            {
                uint64_t depth = this->lastOp - lastCommitted;
                dirtyReadCount_++;
                dirtyDepthHist_[depth]++;
                perClientReads_[linRequest.rid().client_id()].second++;
                SendVersionRequest(linRequest);
                return;
            }

            cleanReadCount_++;
            perClientReads_[linRequest.rid().client_id()].first++;
            ExecuteReadOperation(linRequest);

            ASSERT(++commitLogOpnum > 0);
            ASSERT(commitLogOpnum > commitLog.LastOpnum());
            commitLog.Append(
                viewstamp_t(view, commitLogOpnum),
                ToRequest(linRequest),
                LOG_STATE_CLEAN);
        }

        void IOCL_CRAQReplica::HandleWriteRequest(const TransportAddress &remote,
                                             const LinearizeableOperation &linRequest)
        {
            Debug("Handling write request for client id %lu and client request id %lu",
                  linRequest.rid().client_id(), linRequest.rid().client_req_id());
            viewstamp_t v;

            if (status != STATUS_NORMAL)
            {
                RNotice("Ignoring request due to abnormal status");
                return;
            }

            UpdateClientAddresses(remote, linRequest);

            if (!AmHead())
            {
                RDebug("Ignoring write request because I'm not the head");
                return;
            }

            if (IsDuplicateRequest(remote, linRequest))
                return;

            UpdateClientTable(linRequest);

            bool replicate = false;
            string res;
            string messageString;
            linRequest.SerializeToString(&messageString);
            LeaderUpcall(lastCommitted, messageString, replicate, res);

            if (!replicate)
            {
                RPanic("Should always replicate when using IOCL_CRAQ");
            }

            Request request;
            request.set_op(messageString);
            request.set_clientid(linRequest.rid().client_id());
            request.set_clientreqid(linRequest.rid().client_req_id());

            ++this->lastOp;
            v.view = this->view;
            v.opnum = this->lastOp;

            RDebug("Received REQUEST, assigning " FMT_VIEWSTAMP, VA_VIEWSTAMP(v));

            log.Append(v, request, LOG_STATE_DIRTY);

            linOpCache_[this->lastOp] = linRequest;

            ASSERT(pendingWrites.empty() || this->lastOp > pendingWrites.rbegin()->first);
            pendingWrites[this->lastOp] = linRequest;

            if (lastOp - lastBatchEnd + 1 > batchSize)
            {
                CloseBatch();
            }
            else
            {
                RDebug("Keeping in batch");
                if (!closeBatchTimeout->Active())
                {
                    closeBatchTimeout->Start();
                }
            }
        }

        void IOCL_CRAQReplica::HandleUnloggedRequest(const TransportAddress &remote,
                                                const UnloggedRequestMessage &msg)
        {
            if (status != STATUS_NORMAL)
            {
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

        void IOCL_CRAQReplica::HandlePrepare(const TransportAddress &remote,
                                        const PrepareMessage &msg)
        {
            RDebug("Received PREPARE <" FMT_VIEW "," FMT_OPNUM "-" FMT_OPNUM ">",
                   msg.view(), msg.batchstart(), msg.opnum());

            if (this->status != STATUS_NORMAL)
            {
                RDebug("Ignoring PREPARE due to abnormal status");
                return;
            }

            if (AmHead())
            {
                RPanic("Unexpected PREPARE: I'm the head of this chain");
            }

            ASSERT(msg.batchstart() <= msg.opnum());
            ASSERT((msg.opnum() - msg.batchstart() + 1) ==
                   (unsigned int)msg.request_size());

            if (msg.opnum() <= this->lastOp)
            {
                RDebug("Ignoring PREPARE; already prepared that operation");
                return;
            }

            if (msg.batchstart() > this->lastOp + 1)
            {
                Debug("Calling state transfer due to gap between last operation seen and start of batch received");
                pendingPrepares.push_back(
                    std::pair<TransportAddress *, PrepareMessage>(remote.clone(), msg));
                return;
            }

            opnum_t op = msg.batchstart() - 1;
            for (const auto &req : msg.request())
            {
                op++;
                if (op <= lastOp)
                {
                    continue;
                }
                this->lastOp++;
                log.Append(viewstamp_t(msg.view(), op), req, LOG_STATE_DIRTY);

                LinearizeableOperation linRequest = ToLinearizableRequest(req);
                linOpCache_[this->lastOp] = linRequest;

                ASSERT(pendingWrites.empty() || this->lastOp > pendingWrites.rbegin()->first);
                pendingWrites[this->lastOp] = linRequest;

                UpdateClientTable(linRequest);
            }
            ASSERT(op == msg.opnum());

            if (!AmTail())
            {
                ForwardPropagateMessageInChain(msg);
            }
            else
            {
                // Capture the key of the last op in this batch before CommitUpTo
                // erases it from linOpCache_. Used only for the CommitMessage key
                // field (informational, not functionally required).
                string lastKey = "";
                if (msg.request_size() > 0)
                {
                    auto keyIt = linOpCache_.find(this->lastOp);
                    if (keyIt != linOpCache_.end())
                    {
                        lastKey = keyIt->second.key();
                    }
                }

                tailTotalOps_ += msg.request_size();
                tailTotalBatches_++;
                RNotice("Tail batch: size=%d pending_ooo=%lu avg_batch_size=%.2f total_ops=%lu",
                        msg.request_size(), pendingPrepares.size(),
                        (double)tailTotalOps_ / tailTotalBatches_,
                        tailTotalOps_);

                CommitUpTo(lastOp);

                BroadcastCommit(lastKey);

                DrainPendingCoordRequests();
            }
        }

        void IOCL_CRAQReplica::HandleCommit(const TransportAddress &remote,
                                       const CommitMessage &msg)
        {
            RDebug("Received COMMIT " FMT_VIEWSTAMP " with key %s",
                   msg.view(), msg.opnum(), msg.key().c_str());

            if (this->status != STATUS_NORMAL)
            {
                RDebug("Ignoring COMMIT due to abnormal status");
                return;
            }

            if (AmTail())
            {
                RPanic("Unexpected COMMIT: I'm the tail of this chain");
            }

            if (msg.opnum() <= this->lastCommitted)
            {
                RDebug("Ignoring COMMIT; already committed that operation, opnum is %lu and lastCommitted is %lu",
                       msg.opnum(), this->lastCommitted);
                return;
            }

            if (msg.opnum() > this->lastOp)
            {
                return;
            }

            SyncVC(msg.vector_clock());

            CommitUpTo(msg.opnum());

            TryServeWaitingReads();
        }

        void IOCL_CRAQReplica::HandleVersionRequest(const TransportAddress &remote,
                                                      const VersionRequestMessage &msg)
        {
            if (!AmTail())
            {
                Panic("Received version request at a non-tail node");
            }

            Debug("Handling version request for key %s", msg.key().c_str());
            VersionResponseMessage response;
            response.set_clientid(msg.clientid());
            response.set_clientreqid(msg.clientreqid());
            response.set_opnum(keyToVersionNumber[msg.key()]);

            for (uint64_t v : vectorClock)
            {
                response.add_vector_clock(v);
            }

            Notice("Sending version response to replica %d, timestamp to read is %lu, VC[%d]=%lu",
                   msg.replicaidx(), keyToVersionNumber[msg.key()], groupIdx, vectorClock[groupIdx]);
            transport->SendMessageToReplica(this, msg.replicaidx(), response);
        }

        void IOCL_CRAQReplica::HandleVersionResponse(const TransportAddress &remote,
                                                       const VersionResponseMessage &msg)
        {
            std::pair<uint64_t, uint64_t> requestClientId = {msg.clientid(), msg.clientreqid()};
            auto it = pendingReads.find(requestClientId);
            if (it == pendingReads.end())
            {
                Debug("Old version request for clientid %d and client request id %d, no longer pending",
                      msg.clientid(), msg.clientreqid());
                return;
            }

            LinearizeableOperation linRequest = it->second;
            pendingReads.erase(it);

            Debug("Handling version response for key %s and opnum %d",
                  linRequest.key().c_str(), msg.opnum());

            keyToVersionNumber[linRequest.key()] =
                std::max(keyToVersionNumber[linRequest.key()], msg.opnum());

            uint64_t requiredVC = 0;
            if (msg.vector_clock_size() > groupIdx)
            {
                requiredVC = msg.vector_clock(groupIdx);
            }

            SyncVC(msg.vector_clock());

            FlushWritesUpTo(msg.opnum());

            // If we haven't committed up to the tail's VC[groupIdx], buffer
            // the read until HandleCommit catches us up.
            if (lastCommitted < requiredVC)
            {
                Debug("Buffering read for key %s waiting for VC threshold %lu (lastCommitted=%lu)",
                      linRequest.key().c_str(), requiredVC, lastCommitted);
                readsWaitingForVC.push_back({requiredVC, linRequest});
                return;
            }

            ExecuteReadOperation(linRequest);

            ASSERT(++commitLogOpnum > 0);
            ASSERT(commitLogOpnum > commitLog.LastOpnum());
            commitLog.Append(
                viewstamp_t(view, commitLogOpnum),
                ToRequest(linRequest),
                LOG_STATE_CLEAN);
        }

        void IOCL_CRAQReplica::HandleCoordination(const TransportAddress &remote,
                                                   const proto::SuccessorRequestMessage &msg)
        {
            // CoordRequests may arrive at any replica: tail for write predecessors,
            // or the read-serving replica (e.g. MIDDLE) for read predecessors.
            uint64_t p = msg.p();  // predecessor's shardtag

            // Sync client's VC.
            SyncVC(msg.vector_clock());

            Debug("Received CoordRequest: p=%lu s=%lu succ_groupidx=%d succ_replicaidx=%d",
                  p, msg.s(), msg.succ_groupidx(), msg.succ_replicaidx());

            if (committedForCoord.count(p))
            {
                // Predecessor already committed: send CoordResponse immediately.
                SendCoordResponseMsg(msg);
            }
            else
            {
                // Buffer until predecessor commits.
                pendingCoordRequests[p].push_back(msg);
            }
        }

        void IOCL_CRAQReplica::HandleCoordinationReply(const TransportAddress &remote,
                                                        const proto::PredecessorReplyMessage &msg)
        {
            uint64_t successorShardTag = msg.s();  

            Debug("Received CoordResponse: s=%lu", successorShardTag);

            // Sync VC from the predecessor's handler.
            SyncVC(msg.vector_clock());

            // Check if a READ is waiting for this CoordResponse.
            auto readIt = readsWaitingForCoord.find(successorShardTag);
            if (readIt != readsWaitingForCoord.end())
            {
                // Count this response and check if all expected have arrived.
                pendingCoordResponses[successorShardTag] = msg;
                int nowReceived = ++coordResponseCount_[successorShardTag];

                auto expIt = readsExpectedCoordCount_.find(successorShardTag);
                int expected = (expIt != readsExpectedCoordCount_.end()) ? expIt->second : 1;

                if (nowReceived < expected)
                {
                    Debug("Read shardtag %lu: %d/%d CoordResponses received, still waiting",
                          successorShardTag, nowReceived, expected);
                    return;
                }

                // All CoordResponses received — unblock the read.
                LinearizeableOperation linRequest = readIt->second;
                readsWaitingForCoord.erase(readIt);
                readsExpectedCoordCount_.erase(successorShardTag);
                coordResponseCount_.erase(successorShardTag);
                pendingCoordResponses.erase(successorShardTag);

                Debug("Unblocking read shardtag %lu (%d/%d CoordResponses)", successorShardTag, nowReceived, expected);

                // Now proceed with the normal read logic: dirty check.
                if (this->lastOp != lastCommitted && !AmTail())
                {
                    SendVersionRequest(linRequest);
                }
                else
                {
                    ExecuteReadOperation(linRequest);
                    ASSERT(++commitLogOpnum > 0);
                    ASSERT(commitLogOpnum > commitLog.LastOpnum());
                    commitLog.Append(
                        viewstamp_t(view, commitLogOpnum),
                        ToRequest(linRequest),
                        LOG_STATE_CLEAN);
                }
                return;
            }

            if (!AmTail())
            {
                // CoordResponse arrived before the read was buffered in readsWaitingForCoord
                // (race: predecessor was already committed when CoordRequest arrived).
                // Store it so HandleReadRequest can consume it when the read arrives.
                RDebug("Buffering CoordResponse for shardtag %lu at non-tail (read not yet arrived)", successorShardTag);
                pendingCoordResponses[successorShardTag] = msg;
                coordResponseCount_[successorShardTag]++;
                return;
            }

            // At tail: record the response and count it.
            pendingCoordResponses[successorShardTag] = msg;  // kept for VC sync
            int nowReceived = ++coordResponseCount_[successorShardTag];

            // If a write is waiting for its deferred reply, check whether all
            // expected CoordResponses have now arrived.
            auto deferIt = pendingGateReplies_.find(successorShardTag);
            if (deferIt != pendingGateReplies_.end())
            {
                DeferredGateReply &deferred = deferIt->second;
                if (nowReceived >= deferred.expectedCount)
                {
                    // All predecessors have committed.  Fire the deferred gate.
                    SyncVC(msg.vector_clock());
                    // Execute applies the store write AND sends the direct ShardClient
                    // reply via ReplicaUpcall.  opnum captured at commit time is used
                    // so the reply carries the correct version number.
                    ReplyMessage reply;
                    Request req = ToRequest(deferred.linRequest);
                    Execute(Timestamp{deferred.opnum}, req, reply);
                    // Set committedForCoord AFTER Execute so that downstream CoordRequests
                    // (from ops that depend on this write) see it as visible only now.
                    // This is what creates the staircase: each step's CoordResponse is sent
                    // only after that step's own gate fires (its Execute runs).
                    committedForCoord[successorShardTag] = true;
                    if (pendingCoordRequests.count(successorShardTag))
                        pendingCoordDrain_.push_back(successorShardTag);
                    SendReplyToClient(deferred.linRequest, reply, deferred.opnum);
                    pendingGateReplies_.erase(deferIt);
                    pendingCoordResponses.erase(successorShardTag);
                    coordResponseCount_.erase(successorShardTag);
                    DrainPendingCoordRequests();
                }
            }

            // Advance commit state (may commit newly-prepared ops).
            CommitUpTo(lastOp);

            // Broadcast updated commit state to non-tail replicas.
            BroadcastCommit();

            // Drain CoordRequests for writes newly committed.
            DrainPendingCoordRequests();
        }

        void IOCL_CRAQReplica::Close()
        {
            if (!AmTail() && (cleanReadCount_ > 0 || dirtyReadCount_ > 0))
            {
                uint64_t total = cleanReadCount_ + dirtyReadCount_;
                Notice("[%d] ReadStatsSummary clean=%lu dirty=%lu total=%lu",
                       myIdx, cleanReadCount_, dirtyReadCount_, total);
                for (auto &kv : dirtyDepthHist_)
                    Notice("[%d] ReadStatsDepth depth=%lu count=%lu", myIdx, kv.first, kv.second);
                for (auto &kv : perClientReads_)
                    Notice("[%d] ReadStatsClient client=%lu clean=%lu dirty=%lu",
                           myIdx, kv.first, kv.second.first, kv.second.second);
            }
        }

    } // namespace iocl_craq
} // namespace replication
