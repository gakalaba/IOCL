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
            viewstamp_t viewstamp;
            IoclEntryState state;
            Request request;
            uint64_t myShardTag;
            proto::PredListHolder predList; // we copied the predlist out of the RPC message via Swap()
            uint64_t arrivalTs;
            uint64_t finalTs;
            std::vector<uint64_t> predecessorArrivalTs;
            int ACKs;
            const uint64_t intkey;
            std::unordered_set<uint64_t> finalAcks; // tracking all unique final ACKs from predecessors
            std::unordered_map<uint64_t, int32_t> successors; // keep track of all your successors to send the final ACK!
            // string hash;
            // // Speculative client table stuff
            // opnum_t prevClientReqOpnum;
            // ::google::protobuf::Message *replyMessage;

            IoclEntry(viewstamp_t viewstamp, IoclEntryState state,
                    const Request &request, uint64_t shardtag, uint64_t intkey)
                : viewstamp(viewstamp),
                  state(state),
                  request(request),
                  myShardTag(shardtag),
                  ACKs(0),
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
            ska::flat_hash_map<uint64_t, std::vector<proto::SuccessorRequestMessage>> outstandingCoordinationReqs;
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

            QuorumSet<viewstamp_t, proto::UnorderedPrepareOKMessage> unorderedPrepareOKQuorum;
            QuorumSet<viewstamp_t, proto::PrepareOKMessage> prepareOKQuorum;
            QuorumSet<view_t, proto::StartViewChangeMessage> startViewChangeQuorum;
            QuorumSet<view_t, proto::DoViewChangeMessage> doViewChangeQuorum;

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
            void UpdateClientTable(const Request &req);
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

            void HandleRequest(const TransportAddress &remote,
                               proto::RequestMessage &msg);
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
            void HandleCoordination(const TransportAddress &remote,
                                 const proto::SuccessorRequestMessage &msg);
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
