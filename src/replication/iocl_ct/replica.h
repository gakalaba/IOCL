// -*- mode: c++; c-file-style: "k&r"; c-basic-offset: 4 -*-
/***********************************************************************
 *
 * iocl_ct/replica.h:
 *   IOCL Constant Time protocol
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

#ifndef _IOCL_CT_REPLICA_H_
#define _IOCL_CT_REPLICA_H_

#include <list>
#include <map>
#include <memory>

#include "lib/configuration.h"
#include "lib/latency.h"
// #include "replication/common/log.h"
#include "replication/common/quorumset.h"
#include "replication/common/replica.h"
#include "replication/iocl_ct/iocl_ct-proto.pb.h"
#include "replication/common/flat_hash_map.hpp"


namespace replication
{
    namespace iocl_ct
    {
        enum IoclEntryState {
            IOCL_STATE_ARRIVED,
            IOCL_STATE_PERSISTED,
            IOCL_STATE_READY,
            IOCL_STATE_PREPARED,
            IOCL_STATE_COMMITTED
        };

        struct SuccessorKey {
            uint64_t s_shardtag;
            uint32_t s_shardidx;

            bool operator==(const SuccessorKey &other) const noexcept {
                return s_shardtag == other.s_shardtag &&
                    s_shardidx == other.s_shardidx;
            }
        };

        struct SuccessorKeyHash {
            std::size_t operator()(const SuccessorKey &x) const noexcept {
                std::size_t h1 = std::hash<uint64_t>{}(x.s_shardtag);
                std::size_t h2 = std::hash<uint32_t>{}(x.s_shardidx);

                std::size_t h = h1;
                h ^= h2 + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
                return h;
            }
        };

        struct SuccessorInfo {
            uint16_t predidx;
            bool final_sent;
        };

        struct outCoordResp {
            uint64_t arrivalTs;
            uint16_t predidx;
        };

        struct outCoordReq {
            uint64_t s;
            uint32_t shardidx;
            uint16_t predidx;
        };

        struct IoclEntry {
            viewstamp_t viewstamp;
            IoclEntryState state;
            const LinearizeableOperation request; // op, key, value, slot_idx, clientid, clientreqid, shardtag, predList
            const uint64_t myShardTag; // redundant but for caching and less request.shardtag access
            const uint64_t intkey; // redundant but for caching and less request.intkey access
            const uint16_t num_predecessors;
            uint64_t arrivalTs;
            uint64_t finalTs;
            std::vector<uint64_t> predecessorArrivalTs;
            int ACKs;
            uint64_t final_ack_mask = 0; // num_predecessors <= 64, so can fit in a 64-bit int
            uint8_t final_ack_count = 0;
            // keep track of all your successors to send the final ACK! (successor shardtag, successor shardidx, predix) -> have we sent the final ACK to this successor already?
            std::unordered_map<SuccessorKey, SuccessorInfo, SuccessorKeyHash> successors;
            // string hash;
            // // Speculative client table stuff
            // opnum_t prevClientReqOpnum;
            // ::google::protobuf::Message *replyMessage;
            // Quorum tracking stuff
            uint64_t prepare_ok_mask = 0;
            uint8_t prepare_ok_count = 0;
            uint64_t u_prepare_ok_mask = 0;
            uint8_t u_prepare_ok_count = 0;

            IoclEntry(viewstamp_t viewstamp, IoclEntryState state,
                    LinearizeableOperation request, uint64_t shardtag, uint64_t intkey, uint16_t num_predecessors)
                : viewstamp(viewstamp),
                  state(state),
                  request(std::move(request)),
                  myShardTag(shardtag),
                  num_predecessors(num_predecessors),
                  predecessorArrivalTs(num_predecessors),
                  ACKs(0),
                  prepare_ok_count(0),
                  prepare_ok_mask(0),
                  u_prepare_ok_count(0),
                  u_prepare_ok_mask(0),
                  arrivalTs(0),
                  finalTs(0),
                  intkey(intkey) {
                    successors.reserve(16);
                  }
        };
        // Comparison operator for ordering IoclEntries
        struct EntryReadyCompareIdx {
            const std::vector<IoclEntry> *store;
            bool operator()(uint32_t a, uint32_t b) const {
                const IoclEntry &ea = (*store)[a];
                const IoclEntry &eb = (*store)[b];

                // First by final timestamp
                if (ea.finalTs < eb.finalTs) return true;
                if (ea.finalTs > eb.finalTs) return false;

                // Then by Tag (unique)
                return ea.myShardTag < eb.myShardTag;
                /* EntryReadyCompareIdx using (finalTs, myShardTag)
                is fine as long as myShardTag is truly unique
                per entry and finalTs is only changed while the
                entry is out of the set. */
            }
        };

        class IOCL_CTReplica : public Replica
        {
        public:
            IOCL_CTReplica(transport::Configuration config, int groupIdx, int myIdx,
                      Transport *transport, unsigned int batchSize, AppReplica *app,
                      bool debug_stats);
            ~IOCL_CTReplica();
            void Close();

            void ReceiveMessage(const TransportAddress &remote, const string &type,
                                const string &data, void *meta_data);
            void ReceiveMessage(const TransportAddress &remote, MsgType type,
                                const string &data, void *meta_data);
            virtual void HandleRequest(LinearizeableOperation &msg);
            virtual void HandleCoordination(const SuccessorRequestMessage &msg);
            inline IoclEntry &Entry(uint32_t idx) { return entryStore[idx]; }
            inline const IoclEntry &Entry(uint32_t idx) const { return entryStore[idx]; }

        private:
            view_t view;
            opnum_t lastCommitted;
            opnum_t lastOp;
            opnum_t lastUnorderedOp;
            view_t lastRequestStateTransferView;
            opnum_t lastRequestStateTransferOpnum;
            std::list<std::pair<TransportAddress *, proto::PrepareMessage>>
                pendingPrepares;
            proto::PrepareMessage lastPrepare;
            proto::UnorderedPrepareMessage lastUnorderedPrepare;
            unsigned int batchSize;
            opnum_t lastBatchEnd;
            opnum_t lastUnorderedBatchEnd;
            uint8_t Q;
            // cached message objects for ReceiveMessage()
            proto::UnorderedPrepareMessage unorderedPrepareRecv;
            proto::UnorderedPrepareOKMessage unorderedPrepareOKRecv;
            proto::PrepareMessage prepareRecv;
            proto::PrepareOKMessage prepareOKRecv;
            proto::CommitMessage commitRecv;
            proto::PredecessorReplyMessage coordRespRecv;
            proto::PredecessorFinalMessage coordFinalRecv;
            // cached message objects for transmission
            proto::PredecessorFinalMessage predFinalSend;
            proto::PredecessorReplyMessage preplySend;

            struct PerKeySubLog {
                std::vector<opnum_t> ops;
                size_t head = 0;
            };
            ska::flat_hash_map<uint64_t, PerKeySubLog> perKeySubLogs;

            /*******************************/
            /* IOCL_CT specific structures */
            /*******************************/
            std::vector<IoclEntry> entryStore;
            ska::flat_hash_map<uint64_t, uint32_t> shardtagToEntryIdx;
            std::vector<uint32_t> log;                           // ordered opnum offset -> entry index
            /* Getting rid of unorderedBagByOpnum because 
               all of the following will always hold true:
               1. every unordered request gets exactly one IoclEntry
               2. entries are appended to entryStore in the same order unordered opnums are assigned
               3. nothing else gets inserted into entryStore anywhere other than at HandleRequest
               4. you never remove/recycle entries in a way that changes indices 
            */
            ska::flat_hash_map<uint64_t, std::set<uint32_t, EntryReadyCompareIdx>> perKeySubqueues;
            std::map<uint64_t, std::unique_ptr<TransportAddress>> clientAddresses;
            uint64_t shardTS;
            std::unordered_map<uint64_t, uint64_t> lastReadyTS; // last ready TS per Key
            ska::flat_hash_map<uint64_t, std::vector<outCoordReq>> outstandingCoordinationReqs;
            ska::flat_hash_map<uint64_t, std::vector<outCoordResp>> outstandingCoordinationResps;
            ska::flat_hash_map<uint64_t, uint64_t> outstandingCoordinationFinals;
            std::unordered_map<uint64_t, std::vector<size_t>> perKeyQueueLengths;

            // struct ClientTableEntry
            // {
            //     uint64_t lastReqId;
            //     bool replied;
            //     proto::ReplyMessage reply;
            // };
            // std::map<uint64_t, ClientTableEntry> clientTable;

            replication::QuorumSet<viewstamp_t, replication::ViewstampHash, replication::ViewstampEq> startViewChangeQuorum;
            replication::QuorumSet<viewstamp_t, replication::ViewstampHash, replication::ViewstampEq> doViewChangeQuorum;

            Timeout *viewChangeTimeout;
            Timeout *nullCommitTimeout;
            Timeout *stateTransferTimeout;
            Timeout *resendPrepareTimeout;
            Timeout *resendUnorderedPrepareTimeout;
            Timeout *closeBatchTimeout;
            Timeout *closeUnorderedBatchTimeout;

            Latency_t rec_to_upcall_lat_;
            Latency_t upcall_to_exec_lat_;
            Latency_t exec_to_sent_lat_;

            bool debug_stats_;

            bool AmLeader() const;
            void CommitUpTo(opnum_t upto);
            void SendPrepareOKs(opnum_t oldLastOp);
            void RequestStateTransfer();
            void EnterView(view_t newview);
            void StartViewChange(view_t newview);
            void SendNullCommit();
            // void UpdateClientTable(const Request &req);
            void ResendPrepare();
            void ResendUnorderedPrepare();
            void CloseBatch();
            void CloseUnorderedBatch();
            void ReadyRoutine(uint64_t intkey);
            void ReadyFinalRoutine(uint64_t intkey);
            void AppendToLog(opnum_t new_entry_opnum, uint32_t idx);
            IoclEntry *FindInLog(opnum_t opnum);
            viewstamp_t LastViewstampOfLog() const;
            uint64_t FoldL(const std::vector<uint64_t> &pl);
            void InsertInSubqueue(uint64_t intkey, uint32_t idx);

            void HandleUnloggedRequest(const TransportAddress &remote,
                                       const proto::UnloggedRequestMessage &msg);

            void HandlePrepare(const TransportAddress &remote,
                               const proto::PrepareMessage &msg);
            void HandlePrepareOK(const TransportAddress &remote,
                                 const proto::PrepareOKMessage &msg);
            void HandleUnorderedPrepare(const TransportAddress &remote,
                               proto::UnorderedPrepareMessage &msg);
            void HandleUnorderedPrepareOK(const TransportAddress &remote,
                                 const proto::UnorderedPrepareOKMessage &msg);
            void HandleCoordinationReply(const TransportAddress &remote,
                                const proto::PredecessorReplyMessage &msg);
            void HandleCoordinationFinal(const TransportAddress &remote,
                                const proto::PredecessorFinalMessage &msg);
            void HandleCommit(const TransportAddress &remote,
                              const proto::CommitMessage &msg);
            void HandleRequestStateTransfer(
                const TransportAddress &remote,
                const proto::RequestStateTransferMessage &msg);
            void HandleStateTransfer(const TransportAddress &remote,
                                     const proto::StateTransferMessage &msg);
            void HandleStartViewChange(const TransportAddress &remote,
                                       const proto::StartViewChangeMessage &msg);
            void HandleDoViewChange(const TransportAddress &remote,
                                    const proto::DoViewChangeMessage &msg);
            void HandleStartView(const TransportAddress &remote,
                                 const proto::StartViewMessage &msg);
        };

    } // namespace iocl_ct
} // namespace replication

#endif /* _IOCL_CT_REPLICA_H_ */
