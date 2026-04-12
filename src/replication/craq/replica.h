// -*- mode: c++; c-file-style: "k&r"; c-basic-offset: 4 -*-
/***********************************************************************
 *
 * craq/replica.h:
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

#ifndef _CRAQ_REPLICA_H_
#define _CRAQ_REPLICA_H_

#include <algorithm>
#include <list>
#include <map>
#include <memory>

#include "lib/configuration.h"
#include "lib/latency.h"
#include "replication/common/log.h"
#include "replication/common/replica.h"
#include "replication/craq/craq-proto.pb.h"
#include <string_view>

namespace replication
{
    namespace craq
    {

        class CRAQReplica : public Replica
        {
        public:
            static constexpr const char *PUT_OPERATION = "put";
            static constexpr const char *GET_OPERATION = "get";

            CRAQReplica(transport::Configuration config, int groupIdx, int myIdx,
                        Transport *transport, unsigned int batchSize, AppReplica *app,
                        bool debug_stats);
            ~CRAQReplica();
            void Close();

            void ReceiveMessage(const TransportAddress &remote, const string &type,
                                const string &data, void *meta_data);

            const Log &GetCommitLog() const { return commitLog; }

        private:
            view_t view;
            int myIdx;
            int numReplicas;
            opnum_t lastCommitted;
            opnum_t lastOp;
            std::list<std::pair<TransportAddress *, proto::PrepareMessage>>
                pendingPrepares;
            unsigned int batchSize;
            opnum_t lastBatchEnd;
            std::unordered_map<std::string, opnum_t> keyToVersionNumber;

            Log log;

            // commitLog records all operations in execution order — writes
            // are flushed from pendingWrites when their commit ack arrives,
            // reads are appended at execution time. Uses its own sequential
            // counter independent of lastOp/lastCommitted.
            Log commitLog;
            opnum_t commitLogOpnum;

            // Writes buffered on arrival, keyed by lastOp at the time of
            // receipt. Flushed into commitLog when the commit ack arrives
            // (CommitUpTo) or when a version response reveals the tail has
            // committed past them (HandleVersionResponse).
            std::map<opnum_t, LinearizeableOperation> pendingWrites;
            std::unordered_map<opnum_t, LinearizeableOperation> linOpCache_;

            std::map<uint64_t, std::unique_ptr<TransportAddress>> clientAddresses;
            struct ClientTableEntry
            {
                // last ack'd request: should only be updated when a write is committed
                uint64_t lastReqId;
                bool replied;
                proto::ReplyMessage reply;
            };
            std::map<uint64_t, ClientTableEntry> clientTable;

            struct PairHash
            {
                std::size_t operator()(const std::pair<uint64_t, uint64_t> &p) const noexcept
                {
                    uint64_t h1 = std::hash<uint64_t>()(p.first);
                    uint64_t h2 = std::hash<uint64_t>()(p.second);

                    // Very good hash mixing (from boost::hash_combine)
                    return h1 ^ (h2 + 0x9e3779b97f4a7c15ULL + (h1 << 6) + (h1 >> 2));
                }
            };

            std::unordered_map<std::pair<uint64_t, uint64_t>, replication::LinearizeableOperation, PairHash> pendingReads; // contain reads waiting on version responses

            Timeout *resendPrepareTimeout;
            Timeout *closeBatchTimeout;

            Latency_t rec_to_upcall_lat_;
            Latency_t upcall_to_exec_lat_;
            Latency_t exec_to_sent_lat_;

            bool debug_stats_;

            // Tail queueing instrumentation
            uint64_t tailTotalOps_{0};
            uint64_t tailTotalBatches_{0};

            // Middle read-outcome counters (accumulated, dumped in Close())
            uint64_t cleanReadCount_{0};
            uint64_t dirtyReadCount_{0};
            std::map<uint64_t, uint64_t> dirtyDepthHist_;        // depth -> count
            std::map<uint64_t, std::pair<uint64_t,uint64_t>> perClientReads_; // client_id -> {clean, dirty}

            [[nodiscard]] inline bool AmHead() const {return myIdx == 0;}
            [[nodiscard]] inline bool AmTail() const {return myIdx == numReplicas - 1;}
            [[nodiscard]] bool ForwardPropagateMessageInChain(const Message &m);
            [[unused]] bool BackwardsPropagateMessageInChain(const Message &m);
            [[nodiscard]] bool SendMessageToAllPreviousReplicasInChain(const Message &m);
            Request ToRequest(const replication::LinearizeableOperation &linRequest);
            replication::LinearizeableOperation ToLinearizableRequest(const Request &request);
            void ExecuteWriteOperation(const replication::LinearizeableOperation &linRequest);
            void ExecuteReadOperation(const replication::LinearizeableOperation &linRequest);
            void SendReplyToClient(const replication::LinearizeableOperation &entry, proto::ReplyMessage &reply);
            void CommitUpTo(opnum_t upto);
            void FlushWritesUpTo(opnum_t upto);
            void SendVersionRequest(const replication::LinearizeableOperation &linRequest);
            void UpdateClientTable(const replication::LinearizeableOperation &linRequest);
            [[nodiscard]] bool IsDuplicateRequest(const TransportAddress &remote,
                                const replication::LinearizeableOperation &linRequest);
            void UpdateClientAddresses(const TransportAddress &remote, 
                                const replication::LinearizeableOperation &linRequest);
            void CloseBatch();

            void HandleRequest(const TransportAddress &remote,
                               const replication::LinearizeableOperation &linRequest);
            void HandleWriteRequest(const TransportAddress &remote,
                               const replication::LinearizeableOperation &linRequest);
            void HandleReadRequest(const TransportAddress &remote,
                               const replication::LinearizeableOperation &linRequest);
            void HandleUnloggedRequest(const TransportAddress &remote,
                                       const proto::UnloggedRequestMessage &msg);
            void HandlePrepare(const TransportAddress &remote,
                               const proto::PrepareMessage &msg);
            void HandleCommit(const TransportAddress &remote,
                              const proto::CommitMessage &msg);
            void HandleVersionRequest(const TransportAddress &remote,
                                const proto::VersionRequestMessage &msg);
            void HandleVersionResponse(const TransportAddress &remote,
                                const proto::VersionResponseMessage &msg);
        };

    } // namespace craq
} // namespace replication

#endif /* _CRAQ_REPLICA_H_ */
