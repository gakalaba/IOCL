// -*- mode: c++; c-file-style: "k&r"; c-basic-offset: 4 -*-
/***********************************************************************
 *
 * craq/client.cc:
 *   CRAQ client
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

#include <chrono>

#include "lib/assert.h"
#include "lib/message.h"
#include "lib/transport.h"
#include "replication/common/request.pb.h"
#include "replication/craq/client.h"
#include "replication/craq/craq-proto.pb.h"

namespace replication
{
    namespace craq
    {

        CRAQClient::CRAQClient(const transport::Configuration &config, Transport *transport,
                               int group, uint64_t clientid)
            : Client(config, transport, group, clientid) 
        {
            lastReqId = 0;
        }

        CRAQClient::~CRAQClient()
        {
            for (auto kv : pendingReqs)
            {
                delete kv.second;
            }
        }

        void CRAQClient::Invoke(const string &request, continuation_t continuation,
                                error_continuation_t error_continuation)
        {
            InvokeHelper(request, continuation, 0, error_continuation);
        }

        void CRAQClient::Invoke(const string &request, continuation_t continuation, int replicaIndex,
                                error_continuation_t error_continuation)
        {
            InvokeHelper(request, continuation, replicaIndex, error_continuation);
        }

        void CRAQClient::InvokeHelper(const string &request, continuation_t continuation, int replicaIndex,
                                error_continuation_t error_continuation)
        {
            // TODO: Currently, invocations never timeout and error_continuation is
            // never called. It may make sense to set a timeout on the invocation.
            (void)error_continuation;

            uint64_t reqId = ++lastReqId;
            // Timeout *timer =
            //     new Timeout(transport, 500, [this, reqId]()
            //                 { ResendRequest(reqId); });
            PendingRequest *req =
                new PendingRequest(request, reqId, continuation, replicaIndex);

            pendingReqs[reqId] = req;
            SendRequest(req);
        }

        void CRAQClient::InvokeUnlogged(int replicaIdx, const string &request,
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
                // Timeout *timer = new Timeout(transport, timeout, [this, reqId]()
                //                              { UnloggedRequestTimeoutCallback(reqId); });
                PendingUnloggedRequest *req = new PendingUnloggedRequest(
                    request, reqId, continuation, error_continuation);
                pendingReqs[reqId] = req;
                // req->timer->Start();
            }
            else
            {
                Warning("Could not send unlogged request to replica %u.", replicaIdx);
            }
        }

        void CRAQClient::InvokeUnloggedAll(const string &request,
                                           continuation_t continuation,
                                           error_continuation_t error_continuation,
                                           uint32_t timeout)
        {
            Panic("Unimplemented.");
            return;
        }

        void CRAQClient::SendRequest(const PendingRequest *req)
        {
            LinearizeableOperation linRequest;
            linRequest.ParseFromString(req->request);
            linRequest.set_origin_client_id(linRequest.rid().client_id());
            linRequest.set_origin_client_req_id(linRequest.rid().client_req_id());
            linRequest.mutable_rid()->set_client_id(clientid);
            linRequest.mutable_rid()->set_client_req_id(req->clientReqId);
            string op = linRequest.op();
            Notice("Sending client request with id %d", req->clientReqId);

            if (transport->SendMessageToReplica(this, group, req->replicaIndex, linRequest))
            {
                // req->timer->Reset();
            }
            else
            {
                Warning("Could not send request to replicas.");
                pendingReqs.erase(req->clientReqId);
                delete req;
            }
        }

        void CRAQClient::ResendRequest(const uint64_t reqId)
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

        void CRAQClient::ReceiveMessage(const TransportAddress &remote,
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

        void CRAQClient::HandleReply(const TransportAddress &remote,
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
            Debug("CRAQ Client received reply: %lu", reqId);
            // req->timer->Stop();
            pendingReqs.erase(it);
            req->continuation(req->request, msg.reply());
            delete req;
        }

        void CRAQClient::HandleUnloggedReply(const TransportAddress &remote,
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
            // req->timer->Stop();
            pendingReqs.erase(it);
            req->continuation(req->request, msg.reply());
            delete req;
        }

        void CRAQClient::UnloggedRequestTimeoutCallback(const uint64_t reqId)
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
            // req->timer->Stop();
            pendingReqs.erase(it);
            if (req->error_continuation)
            {
                req->error_continuation(req->request, ErrorCode::TIMEOUT);
            }
            delete req;
        }

    } // namespace craq
} // namespace replication
