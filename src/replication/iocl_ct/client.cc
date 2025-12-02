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
            for (auto kv : pendingReqs)
            {
                delete kv.second;
            }
        }

        void IOCL_CTClient::Invoke(const string &request, continuation_t continuation,
                              error_continuation_t error_continuation)
        {
            Panic("Should never call this");
        }

        void IOCL_CTClient::InvokeIOCL(LinearizeableOperation &msg,
                                continuation_t continuation,
                                error_continuation_t error_continuation)
        {
            // TODO: Currently, invocations never timeout and error_continuation is
            // never called. It may make sense to set a timeout on the invocation.
            (void)error_continuation;

            Debug("Inside InvokeIOCL: shardtag is %lu and predlist size is %d",
                  msg.shardtag(), msg.predlist().size());
            Debug("size of the message before: %lu", msg.ByteSizeLong());
            string request_str;
            proto::RequestMessage reqMsg;
            // We only want to stringify the operation, not the IOCL metadata
            reqMsg.mutable_predlist()->Swap(msg.mutable_predlist());
            uint64_t theshardtag = msg.shardtag();
            uint64_t theintkey = msg.intkey();
            reqMsg.set_shardtag(msg.shardtag());
            reqMsg.set_intkey(msg.intkey());
            msg.clear_shardtag();
            msg.clear_predlist();
            msg.clear_intkey();
            // Issue coordination requests
            proto::SuccessorRequestMessage coordReqMsg;
            coordReqMsg.set_s(reqMsg.shardtag()); // my shard tag
            coordReqMsg.set_shardidx(group); // who pred should return to??
            for (uint32_t i = 0; i < reqMsg.predlist().size(); i++)
            {
                uint64_t sendTo = msg.shardlist(i);
                // if (sendTo == group)
                // {
                //     Debug("Skipping sending COORD REQUEST to self for predecessor_tag %u",
                //           reqMsg.predlist(i));
                //     // Append this index to the same_shards field
                //     reqMsg.add_same_shards(i);
                //     continue;
                // }
                uint64_t predShardTag = reqMsg.predlist(i);
                coordReqMsg.set_p(predShardTag);
                coordReqMsg.set_predidx(i);
                Debug("SENDING %dth COORD REQUEST for predecessor_tag %lu to shard %lu",
                      i, predShardTag, sendTo);
                // XXX Try sending only to (what we think is) the leader first
                if (!transport->SendMessageToReplica(this, sendTo, 0, coordReqMsg))
                {
                    Warning("Could not send request to replicas.");
                }
            }
            msg.clear_shardlist();
            Debug("size of the message after (right before stringify): %lu", msg.ByteSizeLong());

            msg.SerializeToString(&request_str);

            // uint64_t reqId = (reqMsg.shardtag() & 0xFFFFFFFF);
            uint64_t reqId = ++lastReqId;
            // Timeout *timer =
            //     new Timeout(transport, 15000, [this, reqId]()
            //                 { ResendRequest(reqId); });
            PendingRequest *req =
                new PendingRequest(request_str, reqId, theshardtag, theintkey, continuation);

            pendingReqs[reqId] = req;

            /*------------------ Send Request ------------------*/
            // req->request is the string type of LinearizeableOperation without IOCL metadata
            reqMsg.mutable_req()->set_op(request_str);
            // uint64_t pid = (reqMsg.shardtag() >> 32) & 0xFFFFFFFF;
            // reqMsg.mutable_req()->set_clientid(pid);
            reqMsg.mutable_req()->set_clientid(clientid);
            reqMsg.mutable_req()->set_clientreqid(req->clientReqId);

            // Debug("SENDING REQUEST: %lu %lu", clientid, pendingRequest->clientReqId);
            // XXX Try sending only to (what we think is) the leader first
            if (transport->SendMessageToReplica(this, group, 0, reqMsg))
            // if (transport->SendMessageToGroup(this, group, reqMsg))
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
            proto::RequestMessage reqMsg;
            // req->request is the string type of LinearizeableOperation
            reqMsg.mutable_req()->set_op(req->request);
            reqMsg.mutable_req()->set_clientid(clientid);
            reqMsg.mutable_req()->set_clientreqid(req->clientReqId);
            reqMsg.set_shardtag(req->shardtag);
            reqMsg.set_intkey(req->intkey);

            // Debug("SENDING REQUEST: %lu %lu", clientid, pendingRequest->clientReqId);
            // XXX Try sending only to (what we think is) the leader first
            if (transport->SendMessageToReplica(this, group, 0, reqMsg))
            // if (transport->SendMessageToGroup(this, group, reqMsg))
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

        void IOCL_CTClient::ResendRequest(const uint64_t reqId)
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

        void IOCL_CTClient::ReceiveMessage(const TransportAddress &remote,
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

        void IOCL_CTClient::HandleReply(const TransportAddress &remote,
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

        void IOCL_CTClient::HandleUnloggedReply(const TransportAddress &remote,
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

        void IOCL_CTClient::UnloggedRequestTimeoutCallback(const uint64_t reqId)
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

    } // namespace iocl_ct
} // namespace replication
