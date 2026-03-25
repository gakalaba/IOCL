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
        }

        void VRClient::Invoke(LinearizeableOperation &msg)
        {
            // XXX Try sending only to (what we think is) the leader first
            if (!(transport->SendMessageToReplica(this, group, 0, msg)))
            {
                Warning("Could not send request to replicas.");
            }
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

            if (!(transport->SendMessageToReplica(this, group, replicaIdx, reqMsg)))
            {
                Panic("Could not send unlogged request to replica %u.", replicaIdx);
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

        void VRClient::SendRequest(uint64_t tid, uint32_t idx)
        {
            LinearizeableOperation reqMsg;
            reqMsg.mutable_rid()->set_client_id(clientid);
            reqMsg.mutable_rid()->set_client_req_id(tid);

            if (!(transport->SendMessageToReplica(this, group, 0, reqMsg)))
            {
                Panic("Could not send request to replicas.");
            }
        }

        void VRClient::ReceiveMessage(const TransportAddress &remote,
                                      const string &type, const string &data,
                                      void *meta_data)
        {
            Panic("shoud have no responses from replicas, all traffic should go through upcall mechanism");
            // proto::ReplyMessage reply;
            // proto::UnloggedReplyMessage unloggedReply;
            // proto::DummyReply dummyReply;

            // if (type == reply.GetTypeName())
            // {
            //     reply.ParseFromString(data);
            //     HandleReply(remote, reply);
            // }
            // else if (type == dummyReply.GetTypeName())
            // {
            //     // This is a reply to an unlogged request, but we don't care about
            //     // the contents. Just stop the timer and remove the pending request.
            //     dummyReply.ParseFromString(data);
            //     HandleDummyReply(remote, dummyReply);
            // }
            // else if (type == unloggedReply.GetTypeName())
            // {
            //     unloggedReply.ParseFromString(data);
            //     HandleUnloggedReply(remote, unloggedReply);
            // }
            // else
            // {
            //     Client::ReceiveMessage(remote, type, data, meta_data);
            // }
        }

        void VRClient::HandleReply(const TransportAddress &remote,
                                   const proto::ReplyMessage &msg)
        {
            Panic("Shouldn't be getting reply");
            // uint64_t reqId = msg.clientreqid();
            // auto it = pendingReqs.find(reqId);
            // if (it == pendingReqs.end())
            // {
            //     Debug("Received reply when no request was pending");
            //     return;
            // }

            // PendingRequest *req = it->second;
            // Debug("Client received reply: %lu", reqId);
            // // req->timer->Stop();
            // pendingReqs.erase(it);
            // req->continuation(req->request, msg.reply());
            // // delete req;
        }

        void VRClient::HandleUnloggedReply(const TransportAddress &remote,
                                           const proto::UnloggedReplyMessage &msg)
        {
            Panic("Shouldn't be getting unlogged reply");
            // uint64_t reqId = msg.clientreqid();
            // auto it = pendingReqs.find(reqId);
            // if (it == pendingReqs.end())
            // {
            //     Debug("Received reply when no request was pending");
            //     return;
            // }

            // PendingUnloggedRequest *req =
            //     static_cast<PendingUnloggedRequest *>(it->second);

            // Debug("Client received unloggedReply %lu", reqId);
            // // req->timer->Stop();
            // pendingReqs.erase(it);
            // req->continuation(req->request, msg.reply());
            // delete req;
        }

    } // namespace vr
} // namespace replication
