// -*- mode: c++; c-file-style: "k&r"; c-basic-offset: 4 -*-
/***********************************************************************
 *
 * craq/replica.cc:
 *   CRAQ protocol
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
#include "replication/craq/replica.h"
#include "replication/craq/craq-proto.pb.h"

#define RDebug(fmt, ...) Debug("[%d] " fmt, myIdx, ##__VA_ARGS__)
#define RNotice(fmt, ...) Notice("[%d] " fmt, myIdx, ##__VA_ARGS__)
#define RWarning(fmt, ...) Warning("[%d] " fmt, myIdx, ##__VA_ARGS__)
#define RPanic(fmt, ...) Panic("[%d] " fmt, myIdx, ##__VA_ARGS__)

namespace replication
{
    namespace craq
    {

        using namespace proto;

        CRAQReplica::CRAQReplica(transport::Configuration config, int groupIdx, int myIdx,
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

        CRAQReplica::~CRAQReplica()
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

       bool CRAQReplica::ForwardPropagateMessageInChain(const Message &m)
       {
        if (AmTail())
        {
            Panic("Tail can't forward propagate message, it's the last node in the chain");
        }
        Debug("Forward propagating message to %d", myIdx + 1);
        return transport->SendMessageToReplica(this, myIdx + 1, m);
       } 

       bool CRAQReplica::BackwardsPropagateMessageInChain(const Message &m)
       {
        if (AmHead())
        {
            Panic("Head can't backwards propagate message, it's the first node in the chain");
        }
        Debug("Backwards propagates message to %d", myIdx - 1);
        return transport->SendMessageToReplica(this, myIdx - 1, m);
       } 
       
       bool CRAQReplica::SendMessageToAllPreviousReplicasInChain(const Message &m)
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
       
        Request CRAQReplica::ToRequest(const replication::LinearizeableOperation &linRequest)
        {
            Request request;
            string linop_string;

            linRequest.SerializeToString(&linop_string);
            request.set_op(linop_string);
            request.set_clientid(linRequest.rid().client_id());
            request.set_clientreqid(linRequest.rid().client_req_id());

            return request;
        }

        LinearizeableOperation CRAQReplica::ToLinearizableRequest(const Request &request)
        {
            LinearizeableOperation linRequest;
            linRequest.ParseFromString(request.op());
            linRequest.mutable_rid()->set_client_id(request.clientid());
            linRequest.mutable_rid()->set_client_req_id(request.clientreqid());

            return linRequest;
        }

        void CRAQReplica::ExecuteWriteOperation(const LinearizeableOperation &linRequest)
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

        void CRAQReplica::ExecuteReadOperation(const LinearizeableOperation &linRequest)
        {
            RDebug("Executing read request " FMT_OPNUM, lastCommitted);
            ReplyMessage reply;

            Request request = ToRequest(linRequest);

            Execute(Timestamp{keyToVersionNumber[linRequest.key()]}, request, reply);
            SendReplyToClient(linRequest, reply);
        }

        void CRAQReplica::SendReplyToClient(const LinearizeableOperation &request, ReplyMessage &reply)
        {
            reply.set_view(this->view);
            reply.set_opnum(lastCommitted);
            reply.set_clientreqid(request.rid().client_req_id());

            // Store reply in the client table
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

            /* Send reply */
            auto iter = clientAddresses.find(request.rid().client_id());
            if (iter != clientAddresses.end())
            {
                Debug("Found message, sending to client");
                transport->SendMessage(this, *iter->second, reply);
            }
        }

        void CRAQReplica::FlushWritesUpTo(opnum_t upto)
        {
            auto it = pendingWrites.begin();
            while (it != pendingWrites.end() && it->first <= upto)
            {
                // Monotonicity: commitLogOpnum must strictly increase on every
                // append, and must never be less than what is already in commitLog.
                ASSERT(++commitLogOpnum > 0);
                ASSERT(commitLogOpnum > commitLog.LastOpnum());
                commitLog.Append(
                    viewstamp_t(view, commitLogOpnum),
                    it->second,
                    LOG_STATE_CLEAN);
                it = pendingWrites.erase(it);
            }
        }

        void CRAQReplica::CommitUpTo(opnum_t upto)
        {
            while (lastCommitted < upto)
            {
                lastCommitted++;

                /* Find operation in log */
                const LogEntry *entry = log.Find(lastCommitted);
                if (entry == nullptr)
                {
                    RPanic("Did not find operation " FMT_OPNUM " in log",
                           lastCommitted);
                }

                const Request request = entry->request;

                /* Mark it as committed */
                bool status = log.SetStatus(lastCommitted, LOG_STATE_CLEAN);
                Debug("Status is %d", status);

                replication::LinearizeableOperation linRequest = ToLinearizableRequest(request);
                const string op = linRequest.op();

                Debug("Commiting entry with op %s", op.c_str());

                if (op == PUT_OPERATION)
                {
                    ExecuteWriteOperation(linRequest);

                    // Commit ack received — flush this write from pendingWrites
                    // into commitLog now that it is known to be committed.
                    FlushWritesUpTo(lastCommitted);
                }
            }
        }

        void CRAQReplica::SendVersionRequest(const LinearizeableOperation &request)
        {
            VersionRequestMessage msg;
            msg.set_key(request.key());
            msg.set_clientid(request.rid().client_id());
            msg.set_clientreqid(request.rid().client_req_id());
            msg.set_replicaidx(myIdx);

            Debug("Sending version request for key %s for client %d and client request id %d", request.key().c_str(), request.rid().client_id(), request.rid().client_req_id());

            pendingReads[{request.rid().client_id(), request.rid().client_req_id()}] = request;

            transport->SendMessageToReplica(this, numReplicas - 1, msg);
        }

        void CRAQReplica::UpdateClientTable(const LinearizeableOperation &req)
        {
            ClientTableEntry &entry = clientTable[req.rid().client_id()];

            Debug("for clientid %d, last req id is %d while current req id is %d", req.rid().client_id(), entry.lastReqId, req.rid().client_req_id());
            ASSERT(entry.lastReqId <= req.rid().client_req_id());

            if (entry.lastReqId == req.rid().client_req_id())
            {
                return;
            }

            entry.replied = false;
            entry.reply.Clear();
        }

        bool CRAQReplica::IsDuplicateRequest(const TransportAddress &remote, const LinearizeableOperation &linRequest)
        {
            // Check the client table to see if this is a duplicate request
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
                        return true;
                    }
                    else
                    {
                        RNotice(
                            "Received duplicate request but no reply available; "
                            "ignoring");
                        return true;
                    }
                }
            }

            return false;
        }

        void CRAQReplica::UpdateClientAddresses(const TransportAddress &remote, const LinearizeableOperation &linRequest)
        {
            clientAddresses.erase(linRequest.rid().client_id());
            clientAddresses.insert(
                std::pair<uint64_t, std::unique_ptr<TransportAddress>>(
                    linRequest.rid().client_id(),
                    std::unique_ptr<TransportAddress>(remote.clone())));
        }

        void CRAQReplica::CloseBatch()
        {
            ASSERT(!AmTail());
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

        void CRAQReplica::ReceiveMessage(const TransportAddress &remote,
                                         const string &type, const string &data,
                                         void *meta_data)
        {
            LinearizeableOperation request;
            UnloggedRequestMessage unloggedRequest;
            PrepareMessage prepare;
            CommitMessage commit;
            VersionRequestMessage versionRequest;
            VersionResponseMessage versionResponse;

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
            else
            {
                RPanic("Received unexpected message type in CRAQ proto: %s",
                       type.c_str());
            }
        }

        void CRAQReplica::HandleRequest(const TransportAddress &remote,
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
                RPanic("Received unexpected op in CRAQ proto: %s",
                       op);
            }
        }

        void CRAQReplica::HandleReadRequest(const TransportAddress &remote, const LinearizeableOperation &linRequest)
        {
            Debug("Handling read request for client id %lu and client request id %lu", linRequest.rid().client_id(), linRequest.rid().client_req_id());
            if (status != STATUS_NORMAL)
            {
                RNotice("Ignoring request due to abnormal status");
                return;
            }

            UpdateClientAddresses(remote, linRequest);

            if (IsDuplicateRequest(remote, linRequest))
                return;

            UpdateClientTable(linRequest);

            // Leader Upcall
            bool replicate = false;
            string res;
            string messageString;
            linRequest.SerializeToString(&messageString);
            LeaderUpcall(lastCommitted, messageString, replicate, res);
            ClientTableEntry &cte = clientTable[linRequest.rid().client_id()];

            // Check whether this request should be committed to replicas
            if (!replicate)
            {
                RPanic("Should always replicate when using CRAQ");
            }

            if (this->lastOp != lastCommitted && !AmTail())
            {
                SendVersionRequest(linRequest);
                return;
            }

            ExecuteReadOperation(linRequest);

            // Clean read (chain is clean or we are the tail) — append to
            // commitLog immediately since all writes up to lastCommitted
            // are already flushed.
            ASSERT(++commitLogOpnum > 0);
            ASSERT(commitLogOpnum > commitLog.LastOpnum());
            commitLog.Append(
                viewstamp_t(view, commitLogOpnum),
                ToRequest(linRequest),
                LOG_STATE_CLEAN);
        }

        void CRAQReplica::HandleWriteRequest(const TransportAddress &remote,
                                             const LinearizeableOperation &linRequest)
        {
            // Latency_Start(&rec_to_upcall_lat_);
            Debug("Handling write request for client id %lu and client request id %lu", linRequest.rid().client_id(), linRequest.rid().client_req_id());
            viewstamp_t v;

            if (status != STATUS_NORMAL)
            {
                RNotice("Ignoring request due to abnormal status");
                return;
            }

            if (!AmHead())
            {
                RDebug("Ignoring write request because I'm not the head");
                return;
            }

            UpdateClientAddresses(remote, linRequest);

            if (IsDuplicateRequest(remote, linRequest))
                return;

            UpdateClientTable(linRequest);

            // Leader Upcall: will always be true, can comment out
            bool replicate = false;
            string res;
            string messageString;
            linRequest.SerializeToString(&messageString);
            LeaderUpcall(lastCommitted, messageString, replicate, res);
            ClientTableEntry &cte = clientTable[linRequest.rid().client_id()];

            // Check whether this request should be committed to replicas
            if (!replicate)
            {
                RPanic("Should always replicate when using CRAQ");
            }

            Request request;
            request.set_op(messageString);
            request.set_clientid(linRequest.rid().client_id());
            request.set_clientreqid(linRequest.rid().client_req_id());

            /* Assign it an opnum */
            ++this->lastOp;
            v.view = this->view;
            v.opnum = this->lastOp;

            RDebug("Received REQUEST, assigning " FMT_VIEWSTAMP, VA_VIEWSTAMP(v));

            /* Add the request to my log */
            log.Append(v, request, LOG_STATE_DIRTY);

            // Buffer write in pendingWrites — will be flushed to commitLog
            // when the commit ack arrives. Invariant: new opnum must be
            // strictly greater than anything already buffered since lastOp
            // is monotonically increasing.
            ASSERT(pendingWrites.empty() || this->lastOp > pendingWrites.rbegin()->first);
            pendingWrites[this->lastOp] = request;

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

        void CRAQReplica::HandleUnloggedRequest(const TransportAddress &remote,
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

        void CRAQReplica::HandlePrepare(const TransportAddress &remote,
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
                // RequestStateTransfer();
                pendingPrepares.push_back(
                    std::pair<TransportAddress *, PrepareMessage>(remote.clone(), msg));
                return;
            }

            /* Add operations to the log */
            opnum_t op = msg.batchstart() - 1;
            for (const auto &req : msg.request())
            {
                op++;
                if (op <= lastOp)
                {
                    continue;
                }
                this->lastOp++;
                // TODO: if tail and write (prepare only sent for write), then we can just set the state to committed since its event driven and no locks i believe
                log.Append(viewstamp_t(msg.view(), op), req, LOG_STATE_DIRTY);

                // Buffer write in pendingWrites on receipt of prepare —
                // non-head replicas buffer here since they receive writes
                // via prepare rather than directly from the client.
                ASSERT(pendingWrites.empty() || this->lastOp > pendingWrites.rbegin()->first);
                pendingWrites[this->lastOp] = req;

                LinearizeableOperation linRequest = ToLinearizableRequest(req);

                UpdateClientTable(linRequest);
            }
            ASSERT(op == msg.opnum());

            if (!AmTail())
            {
                ForwardPropagateMessageInChain(msg);
            }
            else
            {
                CommitUpTo(lastOp);

                CommitMessage cm;
                cm.set_view(this->view);
                cm.set_opnum(this->lastCommitted);

                auto requests = msg.request();
                auto requestCount = requests.size();
                if (requestCount > 0)
                {
                    LinearizeableOperation linRequest = ToLinearizableRequest(msg.request(requestCount - 1));
                    cm.set_key(linRequest.key());
                }
                else
                {
                    cm.set_key("");
                }

                if (!SendMessageToAllPreviousReplicasInChain(cm))
                {
                    RWarning("Failed to backward propagate COMMIT message from tail");
                }
                Debug("Sending commit for write from tail");
            }
        }

        void CRAQReplica::HandleCommit(const TransportAddress &remote,
                                       const CommitMessage &msg)
        {
            RDebug("Received COMMIT " FMT_VIEWSTAMP " with key %s", msg.view(), msg.opnum(), msg.key().c_str());

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
                RDebug("Ignoring COMMIT; already committed that operation, opnum is %lu and lastCommitted is %lu", msg.opnum(), this->lastCommitted);
                return;
            }

            if (msg.opnum() > this->lastOp)
            {
                // RequestStateTransfer();
                return;
            }

            CommitUpTo(msg.opnum());
        }

        void CRAQReplica::HandleVersionRequest(const TransportAddress &remote, const VersionRequestMessage &msg)
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

            Notice("Sending message to replica via version response, timestamp to read is %d", keyToVersionNumber[msg.key()]);
            transport->SendMessageToReplica(this, msg.replicaidx(), response);
        }

        void CRAQReplica::HandleVersionResponse(const TransportAddress &remote, const VersionResponseMessage &msg)
        {
            std::pair<uint64_t, uint64_t> requestClientId = {msg.clientid(), msg.clientreqid()};
            auto it = pendingReads.find(requestClientId);
            if (it == pendingReads.end())
            {
                Debug("Old version request for clientid %d and client request id %d, no longer pending", msg.clientid(), msg.clientreqid());
                return;
            }

            LinearizeableOperation linRequest = it->second;
            Debug("Handling version response for key %s and opnum %d", linRequest.key().c_str(), msg.opnum());

            keyToVersionNumber[linRequest.key()] = std::max(keyToVersionNumber[linRequest.key()], msg.opnum());

            // The version response tells us the tail has committed up to
            // msg.opnum(). Flush any buffered writes up to that point into
            // commitLog before appending the read — this preserves execution
            // order: all writes the dirty read observed appear before it.
            FlushWritesUpTo(msg.opnum());

            ExecuteReadOperation(linRequest);

            // Dirty read — append to commitLog after writes are flushed so
            // the log reflects the order in which operations were observed.
            ASSERT(++commitLogOpnum > 0);
            ASSERT(commitLogOpnum > commitLog.LastOpnum());
            commitLog.Append(
                viewstamp_t(view, commitLogOpnum),
                ToRequest(linRequest),
                LOG_STATE_CLEAN);

            pendingReads.erase({msg.clientid(), msg.clientreqid()});
        }

        void CRAQReplica::Close()
        {
        }

    } // namespace craq
} // namespace replication