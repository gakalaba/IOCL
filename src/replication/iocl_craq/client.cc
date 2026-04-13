// -*- mode: c++; c-file-style: "k&r"; c-basic-offset: 4 -*-
/***********************************************************************
 *
 * iocl_craq/client.cc:
 *   IOCL_CRAQ client
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

#include "replication/common/client.h"

#include "lib/assert.h"
#include "lib/message.h"
#include "lib/transport.h"
#include "replication/common/request.pb.h"
#include "replication/iocl_craq/client.h"
#include "replication/iocl_craq/iocl_craq-proto.pb.h"

namespace replication
{
    namespace iocl_craq
    {
        IOCL_CRAQClient::IOCL_CRAQClient(const transport::Configuration &config,
                                          Transport *transport,
                                          int group, uint64_t clientid)
            : Client(config, transport, group, clientid),
              lastReqId(0),
              shardTagCounter(0),
              lastIssuedShardTag(0),
              lastIssuedGroupIdx(group),
              lastIssuedReplicaIdx(0)
        {
            clientVectorClock.assign(config.g, 0);
        }

        IOCL_CRAQClient::~IOCL_CRAQClient()
        {
            for (auto kv : pendingReqs)
            {
                delete kv.second;
            }
        }

        void IOCL_CRAQClient::Invoke(const string &request, continuation_t continuation,
                                error_continuation_t error_continuation)
        {
            InvokeHelper(request, continuation, 0, error_continuation);
        }

        void IOCL_CRAQClient::Invoke(const string &request, continuation_t continuation, int replicaIndex,
                                error_continuation_t error_continuation)
        {
            InvokeHelper(request, continuation, replicaIndex, error_continuation);
        }

        void IOCL_CRAQClient::InvokeHelper(const string &request, continuation_t continuation, int replicaIndex,
                                error_continuation_t error_continuation)
        {
            (void)error_continuation;

            LinearizeableOperation linOp;
            linOp.ParseFromString(request);

            // If the store layer (ShardClient) already assigned a shardtag, honor it
            // so that predlist entries (which reference ShardClient's CreateTag values)
            // match committedForCoord on the replica.  Only fall back to our own counter
            // when no shardtag has been assigned (e.g. unit-test ops).
            uint64_t shardtag;
            if (linOp.shardtag() != 0) {
                shardtag = linOp.shardtag();
            } else {
                shardtag = ++shardTagCounter;
                linOp.set_shardtag(shardtag);
            }

            bool isWrite = (linOp.op() == PUT_OPERATION);

            // For write successors, the gate is at the tail (numReplicas-1).
            // For read successors, the gate is at the read-serving replica.
            int gateReplicaIdx = isWrite ? (config.n - 1) : replicaIndex;

            // Do not auto-chain writes here. For StrongStore/IOCL_CRAQ, cross-op
            // dependencies are built in ShardClient, which also sends the matching
            // CoordRequests. Adding an implicit predecessor here creates a gate
            // dependency with no corresponding CoordRequest, so the tail waits
            // forever for a CoordResponse that will never arrive.

            // Preserve explicit predecessors already attached by the caller and
            // send the matching CoordRequests. This is still required for the
            // embedded server-side IOCL_CRAQClient path.
            for (int i = 0; i < linOp.predlist_size() && i < linOp.shardlist_size(); i++)
            {
                uint64_t predShardtag = linOp.predlist(i);
                if (predShardtag == 0) continue;
                int predGroupIdx = (int)linOp.shardlist(i);
                int predReplicaIdx = (i < linOp.pred_replicalist_size())
                                     ? linOp.pred_replicalist(i)
                                     : (config.n - 1);

                proto::SuccessorRequestMessage coordReq;
                coordReq.set_p(predShardtag);
                coordReq.set_s(shardtag);
                coordReq.set_succ_groupidx(group);
                coordReq.set_succ_replicaidx(gateReplicaIdx);
                for (uint64_t v : clientVectorClock)
                {
                    coordReq.add_vector_clock(v);
                }

                Notice("Sending CoordRequest: p=%lu s=%lu to group=%d replica=%d",
                       predShardtag, shardtag, predGroupIdx, predReplicaIdx);

                if (!transport->SendMessageToReplica(this, predGroupIdx, predReplicaIdx, coordReq))
                {
                    Warning("Could not send CoordRequest to predecessor's handler.");
                }
            }

            // Re-serialize the modified linOp (with shardtag and predlist set).
            string modifiedRequest;
            linOp.SerializeToString(&modifiedRequest);

            uint64_t reqId = ++lastReqId;
            PendingRequest *req =
                new PendingRequest(modifiedRequest, reqId, continuation, replicaIndex);
            pendingReqs[reqId] = req;
            SendRequest(req);
        }

        void IOCL_CRAQClient::InvokeUnlogged(int replicaIdx, const string &request,
                                              continuation_t continuation,
                                              error_continuation_t error_continuation,
                                              uint32_t timeout)
        {
            uint64_t reqId = ++lastReqId;
            proto::UnloggedRequestMessage reqMsg;
            reqMsg.mutable_req()->set_op(request);
            reqMsg.mutable_req()->set_clientid(clientid);
            reqMsg.mutable_req()->set_clientreqid(reqId);

            if (transport->SendMessageToReplica(this, group, replicaIdx, reqMsg))
            {
                PendingUnloggedRequest *req = new PendingUnloggedRequest(
                    request, reqId, continuation, error_continuation);
                pendingReqs[reqId] = req;
            }
            else
            {
                Warning("Could not send unlogged request to replica %u.", replicaIdx);
            }
        }

        void IOCL_CRAQClient::InvokeUnloggedAll(const string &request,
                                                  continuation_t continuation,
                                                  error_continuation_t error_continuation,
                                                  uint32_t timeout)
        {
            Panic("Unimplemented.");
        }

        void IOCL_CRAQClient::SendRequest(const PendingRequest *req)
        {
            LinearizeableOperation linRequest;
            linRequest.ParseFromString(req->request);
            linRequest.set_origin_client_id(linRequest.rid().client_id());
            linRequest.set_origin_client_req_id(linRequest.rid().client_req_id());
            linRequest.mutable_rid()->set_client_id(clientid);
            linRequest.mutable_rid()->set_client_req_id(req->clientReqId);

            Notice("Sending client request with id %lu (shardtag=%lu predlist_size=%d)",
                   req->clientReqId,
                   linRequest.has_shardtag() ? linRequest.shardtag() : 0,
                   linRequest.predlist_size());

            if (!transport->SendMessageToReplica(this, group, req->replicaIndex, linRequest))
            {
                Warning("Could not send request to replica.");
                pendingReqs.erase(req->clientReqId);
                delete req;
                return;
            }

            // Also send directly to the tail so it can register the client address
            // via UpdateClientAddresses. The tail returns early (AmHead() false) without
            // processing the write, but clientAddresses is populated so SendReplyToClient
            // can reply after commit.
            if (linRequest.op() == PUT_OPERATION)
            {
                int tailIdx = config.n - 1;
                if (req->replicaIndex != tailIdx)
                {
                    transport->SendMessageToReplica(this, group, tailIdx, linRequest);
                }
            }
        }

        void IOCL_CRAQClient::ResendRequest(const uint64_t reqId)
        {
            Panic("Shouldn't be resending");
            if (pendingReqs.find(reqId) == pendingReqs.end())
            {
                Debug("Received resend request when no request was pending");
                return;
            }
            Warning("Client timeout; resending request: %lu", reqId);
            SendRequest(pendingReqs[reqId]);
        }

        void IOCL_CRAQClient::ReceiveMessage(const TransportAddress &remote,
                                              const string &type, const string &data,
                                              void *meta_data)
        {
            proto::ReplyMessage reply;
            proto::UnloggedReplyMessage unloggedReply;

            if (type == reply.GetTypeName())
            {
                reply.ParseFromString(data);
                HandleReply(remote, reply);
            }
            else if (type == unloggedReply.GetTypeName())
            {
                unloggedReply.ParseFromString(data);
                HandleUnloggedReply(remote, unloggedReply);
            }
            else
            {
                Client::ReceiveMessage(remote, type, data, meta_data);
            }
        }

        void IOCL_CRAQClient::HandleReply(const TransportAddress &remote,
                                           const proto::ReplyMessage &msg)
        {
            uint64_t reqId = msg.clientreqid();
            auto it = pendingReqs.find(reqId);
            if (it == pendingReqs.end())
            {
                Debug("Received reply when no request was pending");
                return;
            }

            PendingRequest *req = it->second;
            Debug("IOCL_CRAQ Client received reply: %lu", reqId);
            pendingReqs.erase(it);
            req->continuation(req->request, msg.reply());
            delete req;
        }

        void IOCL_CRAQClient::HandleUnloggedReply(const TransportAddress &remote,
                                                    const proto::UnloggedReplyMessage &msg)
        {
            uint64_t reqId = msg.clientreqid();
            auto it = pendingReqs.find(reqId);
            if (it == pendingReqs.end())
            {
                Debug("Received reply when no request was pending");
                return;
            }

            PendingUnloggedRequest *req =
                static_cast<PendingUnloggedRequest *>(it->second);

            Debug("Client received unloggedReply %lu", reqId);
            pendingReqs.erase(it);
            req->continuation(req->request, msg.reply());
            delete req;
        }

        void IOCL_CRAQClient::UnloggedRequestTimeoutCallback(const uint64_t reqId)
        {
            auto it = pendingReqs.find(reqId);
            if (it == pendingReqs.end())
            {
                Debug("Received reply when no request was pending");
                return;
            }
            Warning("Unlogged request timed out");
            PendingUnloggedRequest *req =
                static_cast<PendingUnloggedRequest *>(it->second);
            pendingReqs.erase(it);
            if (req->error_continuation)
            {
                req->error_continuation(req->request, ErrorCode::TIMEOUT);
            }
            delete req;
        }

    } // namespace iocl_craq
} // namespace replication
