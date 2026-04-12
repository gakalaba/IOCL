// -*- mode: c++; c-file-style: "k&r"; c-basic-offset: 4 -*-
/***********************************************************************
 *
 * iocl_iocl_craq/replica.h:
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

#ifndef _IOCL_CRAQ_REPLICA_H_
#define _IOCL_CRAQ_REPLICA_H_

#include <algorithm>
#include <list>
#include <map>
#include <memory>
#include <unordered_map>
#include <vector>

#include "lib/configuration.h"
#include "lib/latency.h"
#include "replication/common/log.h"
#include "replication/common/replica.h"
#include "replication/iocl_craq/iocl_craq-proto.pb.h"
#include <string_view>

namespace replication
{
    namespace iocl_craq
    {

        class IOCL_CRAQReplica : public Replica
        {
        public:
            static constexpr const char *PUT_OPERATION = "put";
            static constexpr const char *GET_OPERATION = "get";

            IOCL_CRAQReplica(transport::Configuration config, int groupIdx, int myIdx,
                        Transport *transport, unsigned int batchSize, AppReplica *app,
                        bool debug_stats);
            ~IOCL_CRAQReplica();
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
                    return h1 ^ (h2 + 0x9e3779b97f4a7c15ULL + (h1 << 6) + (h1 >> 2));
                }
            };

            std::unordered_map<std::pair<uint64_t, uint64_t>, replication::LinearizeableOperation, PairHash> pendingReads;

            std::vector<uint64_t> vectorClock;

            // Tail-side: predecessor shardtag -> list of CoordRequests from successors
            // that arrived before the predecessor committed.
            std::map<uint64_t, std::vector<proto::SuccessorRequestMessage>> pendingCoordRequests;

            // Tail-side: successor shardtag -> CoordResponse that arrived before
            // the successor write reached the tail (or before it could commit).
            std::map<uint64_t, proto::PredecessorReplyMessage> pendingCoordResponses;

            // Tail-side: count of CoordResponses received per successor shardtag.
            // A write's gate opens when this count reaches its predlist_size().
            std::map<uint64_t, int> coordResponseCount_;

            // Tail-side: writes whose store commit is done but whose client reply
            // is deferred pending all CoordResponses.  Keyed by the write's shardtag.
            struct DeferredGateReply {
                LinearizeableOperation linRequest;
                opnum_t                opnum;   // lastCommitted captured at commit time
                int                    expectedCount;
            };
            std::map<uint64_t, DeferredGateReply> pendingGateReplies_;

            // Tail-side: shardtag -> true for writes that have committed.
            // Used to answer late CoordRequests immediately.
            std::unordered_map<uint64_t, bool> committedForCoord;

            // Shardtags of writes that committed at the tail during the current
            // CommitUpTo call, whose pendingCoordRequests should be drained
            // AFTER BroadcastCommit (so CommitMessages are queued first).
            std::vector<uint64_t> pendingCoordDrain_;

            // Any replica: read's shardtag -> linOp, for reads awaiting their
            // CoordResponse before they can proceed.
            std::map<uint64_t, LinearizeableOperation> readsWaitingForCoord;

            // Non-tail: reads that have gotten their CoordResponse (or have no
            // predecessor) and sent a VersionRequest, then received a VersionResponse,
            // but are waiting for lastCommitted to reach the VC threshold.
            // Pair: (required_vc_threshold, linOp).
            std::vector<std::pair<uint64_t, LinearizeableOperation>> readsWaitingForVC;

            // ---------------------------------

            Timeout *resendPrepareTimeout;
            Timeout *closeBatchTimeout;

            Latency_t rec_to_upcall_lat_;
            Latency_t upcall_to_exec_lat_;
            Latency_t exec_to_sent_lat_;

            bool debug_stats_;

            // Middle read-outcome counters (accumulated, dumped in Close())
            uint64_t cleanReadCount_{0};
            uint64_t dirtyReadCount_{0};
            std::map<uint64_t, uint64_t> dirtyDepthHist_;        // depth -> count
            std::map<uint64_t, std::pair<uint64_t,uint64_t>> perClientReads_; // client_id -> {clean, dirty}

            // Tail queueing instrumentation
            uint64_t tailTotalOps_{0};
            uint64_t tailTotalBatches_{0};

            [[nodiscard]] inline bool AmHead() const {return myIdx == 0;}
            [[nodiscard]] inline bool AmTail() const {return myIdx == numReplicas - 1;}
            [[nodiscard]] bool ForwardPropagateMessageInChain(const Message &m);
            [[unused]] bool BackwardsPropagateMessageInChain(const Message &m);
            [[nodiscard]] bool SendMessageToAllPreviousReplicasInChain(const Message &m);
            Request ToRequest(const replication::LinearizeableOperation &linRequest);
            replication::LinearizeableOperation ToLinearizableRequest(const Request &request);
            void ExecuteWriteOperation(const replication::LinearizeableOperation &linRequest);
            void ExecuteReadOperation(const replication::LinearizeableOperation &linRequest);
            // opnum defaults to 0, which means "use current lastCommitted".
            // Pass the opnum captured at commit time for deferred gate replies.
            void SendReplyToClient(const replication::LinearizeableOperation &entry,
                                   proto::ReplyMessage &reply, opnum_t opnum = 0);
            void CommitUpTo(opnum_t upto);
            void FlushWritesUpTo(opnum_t upto);
            void SendVersionRequest(const replication::LinearizeableOperation &linRequest);
            void UpdateClientTable(const replication::LinearizeableOperation &linRequest);
            [[nodiscard]] bool IsDuplicateRequest(const TransportAddress &remote,
                                const replication::LinearizeableOperation &linRequest);
            void UpdateClientAddresses(const TransportAddress &remote, 
                                const replication::LinearizeableOperation &linRequest);
            void CloseBatch();

            // Sync vectorClock component-wise max from a remote VC.
            void SyncVC(const google::protobuf::RepeatedField<google::protobuf::uint64> &remoteVC);

            // After CommitUpTo advances lastCommitted, serve any reads in
            // readsWaitingForVC whose VC threshold is now satisfied.
            void TryServeWaitingReads();

            // Send a CoordResponse (PredecessorReplyMessage) to the successor
            // identified in coordReq. Uses current vectorClock.
            void SendCoordResponseMsg(const proto::SuccessorRequestMessage &coordReq);

            // Drain pendingCoordRequests for shardtags collected in pendingCoordDrain_.
            // Must be called AFTER BroadcastCommit so CommitMessages are queued first.
            void DrainPendingCoordRequests();

            // Send CommitMessage to all non-tail replicas, carrying the current
            // vectorClock and lastCommitted.
            void BroadcastCommit(const string &key = "");

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

            void HandleCoordination(const TransportAddress &remote,
                                    const proto::SuccessorRequestMessage &msg);

            void HandleCoordinationReply(const TransportAddress &remote,
                                         const proto::PredecessorReplyMessage &msg);
        };

    } // namespace iocl_craq
} // namespace replication

#endif /* _IOCL_CRAQ_REPLICA_H_ */
