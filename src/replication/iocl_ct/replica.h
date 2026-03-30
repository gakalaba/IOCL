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

        struct IoclEntry {
            struct PairHash {
                std::size_t operator()(const std::pair<uint64_t, int32_t>& p) const noexcept {
                    uint64_t h1 = std::hash<uint64_t>()(p.first);
                    uint64_t h2 = std::hash<int32_t>()(p.second);

                    // Very good hash mixing (from boost::hash_combine)
                    return h1 ^ (h2 + 0x9e3779b97f4a7c15ULL + (h1 << 6) + (h1 >> 2));
                }
            };
            viewstamp_t viewstamp;
            IoclEntryState state;
            Request request; // op, key, value, slot_idx, clientid, clientreqid
            uint64_t myShardTag;
            proto::PredListHolder predList; // we copied the predlist out of the RPC message via Swap()
            uint64_t arrivalTs;
            uint64_t finalTs;
            std::vector<uint64_t> predecessorArrivalTs;
            int ACKs;
            const uint64_t intkey;
            std::unordered_set<std::pair<uint64_t,int32_t>, PairHash> finalAcks; // tracking all unique final ACKs from predecessors
            std::unordered_map<std::pair<uint64_t,int32_t>, int, PairHash> successors; // keep track of all your successors to send the final ACK! (shardtag -> shardidx)
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
                    const Request &request, uint64_t shardtag, uint64_t intkey)
                : viewstamp(viewstamp),
                  state(state),
                  request(request),
                  myShardTag(shardtag),
                  ACKs(0),
                  prepare_ok_count(0),
                  prepare_ok_mask(0),
                  u_prepare_ok_count(0),
                  u_prepare_ok_mask(0),
                  intkey(intkey) {}
            virtual ~IoclEntry() {}
        };
        // Comparison operator for ordering IoclEntries
        struct EntryReadyCompare {
            bool operator()(const IoclEntry* a, const IoclEntry* b) const {
                // First by final timestamp
                if (a->finalTs < b->finalTs) return true;
                if (a->finalTs > b->finalTs) return false;

                // Then by Tag (unique)
                return a->myShardTag < b->myShardTag;
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

            std::vector<IoclEntry *> log;
            ska::flat_hash_map<uint64_t, std::vector<opnum_t>> perKeySubLogs;

            /*******************************/
            /* IOCL_CT specific structures */
            /*******************************/
            ska::flat_hash_map<uint64_t, std::unique_ptr<IoclEntry>> unorderedBag;
            std::unordered_map<opnum_t, IoclEntry *> unorderedBagByOpnum; // For Batching
            ska::flat_hash_map<uint64_t, std::set<IoclEntry*, EntryReadyCompare>> perKeySubqueues;
            std::map<uint64_t, std::unique_ptr<TransportAddress>> clientAddresses;
            uint64_t shardTS;
            std::unordered_map<uint64_t, uint64_t> lastReadyTS; // last ready TS per Key
            ska::flat_hash_map<uint64_t, std::vector<SuccessorRequestMessage>> outstandingCoordinationReqs;
            ska::flat_hash_map<uint64_t, std::vector<proto::PredecessorReplyMessage>> outstandingCoordinationResps;
            ska::flat_hash_map<uint64_t, std::vector<proto::PredecessorFinalMessage>> outstandingCoordinationFinals;
            std::unordered_map<uint64_t, std::vector<size_t>> perKeyQueueLengths;

            struct ClientTableEntry
            {
                uint64_t lastReqId;
                bool replied;
                proto::ReplyMessage reply;
            };
            std::map<uint64_t, ClientTableEntry> clientTable;

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
            void ReadyRoutine(IoclEntry *entry);
            void ReadyFinalRoutine(IoclEntry *entry);
            void AppendToLog(IoclEntry *entry);
            IoclEntry *FindInLog(opnum_t opnum);
            viewstamp_t LastViewstampOfLog() const;
            uint64_t FoldL(const proto::PredListHolder &pl);

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
