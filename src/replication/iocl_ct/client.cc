// -*- mode: c++; c-file-style: "k&r"; c-basic-offset: 4 -*-
/***********************************************************************
 *
 * iocl_ct/client.cc:
 *   IOCL Constant Time Replication client
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

#include "replication/common/client.h"

#include <chrono>

#include "lib/assert.h"
#include "lib/message.h"
#include "lib/transport.h"
#include "replication/common/request.pb.h"
#include "replication/iocl_ct/client.h"
#include "replication/iocl_ct/iocl_ct-proto.pb.h"

namespace replication
{
    namespace iocl_ct
    {

        IOCL_CTClient::IOCL_CTClient(const transport::Configuration &config, Transport *transport,
                           int group, uint64_t clientid)
            : Client(config, transport, group, clientid)
        {
            lastReqId = 0;
            Debug("IOCL_CTClient created, we are group %u with clientid %lu", group, clientid);
        }

        IOCL_CTClient::~IOCL_CTClient()
        {
        }

        void IOCL_CTClient::Invoke(LinearizeableOperation &msg)
        {
            // TODO: Currently, invocations never timeout and error_continuation is
            // never called. It may make sense to set a timeout on the invocation.

            /*------------------ Send Request ------------------*/
            if (!(transport->SendMessageToReplica(this, group, 0, msg)))
            {
                Panic("Could not send request to replicas.");
            }

        }

        void IOCL_CTClient::InvokeUnlogged(int replicaIdx, const string &request,
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

        void IOCL_CTClient::InvokeUnloggedAll(const string &request,
                                         continuation_t continuation,
                                         error_continuation_t error_continuation,
                                         uint32_t timeout)
        {
            Panic("Unimplemented.");
            return;
        }

        void IOCL_CTClient::SendRequest(const PendingRequest *req)
        {
            LinearizeableOperation reqMsg;
            // req->request is the string type of LinearizeableOperation
            // reqMsg.set_op(req->request);
            reqMsg.mutable_rid()->set_client_id(clientid);
            reqMsg.mutable_rid()->set_client_req_id(req->clientReqId);
            // reqMsg.set_shardtag(req->shardtag);
            // reqMsg.set_intkey(req->intkey);

            if (!(transport->SendMessageToReplica(this, group, 0, reqMsg)))
            {
                Panic("Could not send request to replicas.");
            }
        }

        void IOCL_CTClient::ReceiveMessage(const TransportAddress &remote,
                                      MsgType type, const string &data,
                                      void *meta_data)
        {
            Panic("Unimplemented");
        }

        void IOCL_CTClient::ReceiveMessage(const TransportAddress &remote,
                                      const string &type, const string &data,
                                      void *meta_data)
        {
            Panic("shoud have no responses from replicas, all traffic should go through upcall mechanism");
            // proto::ReplyMessage reply;
            // proto::UnloggedReplyMessage unloggedReply;

            // if (type == reply.GetTypeName())
            // {
            //     reply.ParseFromString(data);
            //     HandleReply(remote, reply);
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

        void IOCL_CTClient::HandleReply(const TransportAddress &remote,
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
            // delete req;
        }

        void IOCL_CTClient::HandleUnloggedReply(const TransportAddress &remote,
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

    } // namespace iocl_ct
} // namespace replication
