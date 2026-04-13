// -*- mode: c++; c-file-style: "k&r"; c-basic-offset: 4 -*-
/***********************************************************************
 *
 * replication/iocl_craq/client.h:
 *   IOCL_CRAQ replication client
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

#ifndef _IOCL_CRAQ_CLIENT_H_
#define _IOCL_CRAQ_CLIENT_H_

#include <unordered_map>
#include <vector>

#include "lib/configuration.h"
#include "replication/common/client.h"
#include "replication/iocl_craq/iocl_craq-proto.pb.h"

namespace replication
{
    namespace iocl_craq
    {

        class IOCL_CRAQClient : public Client
        {
        public:
            static constexpr const char *PUT_OPERATION = "put";
            static constexpr const char *GET_OPERATION = "get";

            IOCL_CRAQClient(const transport::Configuration &config, Transport *transport,
                     int group, uint64_t clientid);
            virtual ~IOCL_CRAQClient();

            // Default Invoke: assigns a shardtag and performs IOCL predecessor
            // tracking before sending. replicaIndex = 0 (head for writes, or
            // any replica for reads).
            virtual void Invoke(const string &request, continuation_t continuation,
                                error_continuation_t error_continuation = nullptr) override;

            // Invoke to a specific replica index (e.g. for reads).
            void Invoke(const string &request, continuation_t continuation, int replicaIndex,
                        error_continuation_t error_continuation = nullptr);

            void SetSendCoordRequests(bool enabled) { sendCoordRequests = enabled; }

            virtual void InvokeUnlogged(
                int replicaIdx, const string &request, continuation_t continuation,
                error_continuation_t error_continuation = nullptr,
                uint32_t timeout = DEFAULT_UNLOGGED_OP_TIMEOUT) override;
            virtual void InvokeUnloggedAll(
                const string &request, continuation_t continuation,
                error_continuation_t error_continuation = nullptr,
                uint32_t timeout = DEFAULT_UNLOGGED_OP_TIMEOUT) override;

            virtual void ReceiveMessage(const TransportAddress &remote,
                                        const string &type, const string &data,
                                        void *meta_data) override;

        protected:
            int opnumber;
            uint64_t lastReqId;

            // IOCL predecessor tracking state.
            uint64_t shardTagCounter;       // monotonically increasing shardtag generator
            uint64_t lastIssuedShardTag;    // shardtag of last issued op (0 = none)
            int      lastIssuedGroupIdx;    // group of last issued op
            int      lastIssuedReplicaIdx;  // handler replica of last issued op
                                            //   (tail for writes, read-replica for reads)
            std::vector<uint64_t> clientVectorClock;  // synced from ReplyMessage.vector_clock
            bool sendCoordRequests;

            struct PendingRequest
            {
                string request;
                uint64_t clientReqId;
                continuation_t continuation;
                int replicaIndex;

                inline PendingRequest(string request, uint64_t clientReqId,
                                      continuation_t continuation, int replicaIndex)
                    : request(request),
                      clientReqId(clientReqId),
                      continuation(continuation),
                      replicaIndex{replicaIndex} {}
                inline ~PendingRequest() {}
            };

            struct PendingUnloggedRequest : public PendingRequest
            {
                error_continuation_t error_continuation;
                inline PendingUnloggedRequest(string request, uint64_t clientReqId,
                                              continuation_t continuation,
                                              error_continuation_t error_continuation)
                    : PendingRequest(request, clientReqId, continuation, -1),
                      error_continuation(error_continuation) {}
            };

            std::unordered_map<uint64_t, PendingRequest *> pendingReqs;

            // Core invocation: assigns shardtag, sends CoordRequest if predecessor
            // exists, then sends the operation to replicaIndex.
            void InvokeHelper(const string &request, continuation_t continuation,
                              int replicaIndex,
                              error_continuation_t error_continuation = nullptr);

            void SendRequest(const PendingRequest *req);
            void ResendRequest(const uint64_t reqId);
            void HandleReply(const TransportAddress &remote,
                             const proto::ReplyMessage &msg);
            void HandleUnloggedReply(const TransportAddress &remote,
                                     const proto::UnloggedReplyMessage &msg);
            void UnloggedRequestTimeoutCallback(const uint64_t reqId);
        };

    } // namespace iocl_craq
} // namespace replication

#endif /* _IOCL_CRAQ_CLIENT_H_ */
