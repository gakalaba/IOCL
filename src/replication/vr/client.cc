// -*- mode: c++; c-file-style: "k&r"; c-basic-offset: 4 -*-
/***********************************************************************
 *
 * vr/client.cc:
 *   Viewstamped Replication clinet
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
#include "replication/vr/client.h"
#include "replication/vr/vr-proto.pb.h"
#include "store/common/backend/timingdebug.h"

namespace replication
{
    namespace vr
    {

        VRClient::VRClient(const transport::Configuration &config, Transport *transport,
                           int group, uint64_t clientid)
            : Client(config, transport, group, clientid)
        {
            lastReqId = 0;
        }

        VRClient::~VRClient()
        {
            for (auto kv : pendingReqs)
            {
                delete kv.second;
            }
        }

        void VRClient::Invoke(const string &request, continuation_t continuation,
                              error_continuation_t error_continuation)
        {
            // TODO: Currently, invocations never timeout and error_continuation is
            // never called. It may make sense to set a timeout on the invocation.
            (void)error_continuation;
            // Notice("                (D) Received Op on Replica CLIENT side %lu", now_us());

            uint64_t reqId = ++lastReqId;
            // Timeout *timer =
            //     new Timeout(transport, 500, [this, reqId]()
            //                 { ResendRequest(reqId); });

            /* Call the HandleRequest function on the leader */
            replica_->HandleRequest(request, clientid, reqId);
        }

        void VRClient::InvokeLinOp(LinearizeableOperation &msg, continuation_t continuation,
                              error_continuation_t error_continuation)
        {
            // TODO: Currently, invocations never timeout and error_continuation is
            // never called. It may make sense to set a timeout on the invocation.
            (void)error_continuation;
            // Notice("                (D) Received Op on Replica CLIENT side %lu", now_us());

            uint64_t reqId = ++lastReqId;
            // Timeout *timer =
            //     new Timeout(transport, 500, [this, reqId]()
            //                 { ResendRequest(reqId); });

            /* Call the HandleRequest function on the leader */
            replica_->HandleOperation(msg, clientid, reqId);
        }

        void VRClient::SetReplica(Replica *replica)
        {
            this->replica_ = replica;
        }

        void VRClient::InvokeUnlogged(int replicaIdx, const string &request,
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

        void VRClient::InvokeUnloggedAll(const string &request,
                                         continuation_t continuation,
                                         error_continuation_t error_continuation,
                                         uint32_t timeout)
        {
            Panic("Unimplemented.");
            return;
        }

        void VRClient::SendRequest(const PendingRequest *req)
        {
            replica_->HandleRequest(req->request, clientid, req->clientReqId);
        }

        void VRClient::ResendRequest(const uint64_t reqId)
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

        void VRClient::ReceiveMessage(const TransportAddress &remote,
                                      const string &type, char *data,
                                      size_t size,
                                      void *meta_data)
        {
            proto::ReplyMessage reply;
            proto::UnloggedReplyMessage unloggedReply;

            if (type == reply.GetTypeName())
            {
                reply.ParseFromArray(data, size);
                HandleReply(remote, reply);
            }
            else if (type == unloggedReply.GetTypeName())
            {
                unloggedReply.ParseFromArray(data, size);
                HandleUnloggedReply(remote, unloggedReply);
            }
            else
            {
                Client::ReceiveMessage(remote, type, data, size, meta_data);
            }
        }

        void VRClient::HandleReply(const TransportAddress &remote,
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
            Debug("Client received reply: %lu", reqId);
            // req->timer->Stop();
            pendingReqs.erase(it);
            req->continuation(req->request, msg.reply());
            delete req;
        }

        void VRClient::HandleUnloggedReply(const TransportAddress &remote,
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

        void VRClient::UnloggedRequestTimeoutCallback(const uint64_t reqId)
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

    } // namespace vr
} // namespace replication
