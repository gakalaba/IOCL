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
#include "replication/common/log.h"
#include "replication/common/quorumset.h"
#include "replication/common/replica.h"
#include "replication/iocl_ct/iocl_ct-proto.pb.h"

namespace replication
{
    namespace iocl_ct
    {

        class IOCL_CTReplica : public Replica
        {
        public:
            IOCL_CTReplica(transport::Configuration config, int groupIdx, int myIdx,
                           Transport *transport, unsigned int batchSize, AppReplica *app,
                           bool debug_stats);
            ~IOCL_CTReplica();

            void ReceiveMessage(const TransportAddress &remote, const string &type,
                                const string &data, void *meta_data);

        private:
            view_t view;
            opnum_t lastCommitted;
            opnum_t lastOp;
            view_t lastRequestStateTransferView;
            opnum_t lastRequestStateTransferOpnum;
            std::list<std::pair<TransportAddress *, proto::PrepareMessage>> pendingPrepares;
            proto::PrepareMessage2 lastPrepare2;
            unsigned int batchSize;
            opnum_t lastBatchEnd2;
            // maps lastBatchId to a tuple of <acks, set of pointers to entries>
            std::unordered_map<int, std::tuple<int, std::unordered_set<LogEntry *>>> thebatchs;
            int lastBatch;
            int lastBatchEnd;
            // IOCL specifics
            uint64_t shardTimestamp;
            uint64_t lastExecutedTimestamp;

            Log log;
            std::map<uint64_t, std::unique_ptr<TransportAddress>> clientAddresses;
            struct ClientTableEntry
            {
                uint64_t lastReqId;
                bool replied;
                proto::ReplyMessage reply;
            };
            std::map<uint64_t, ClientTableEntry> clientTable;
            // IOCL specific
            std::unordered_map<uint64_t, std::vector<Successor *>> outstandingSuccessors;
            // Because predecessors, unlike successors, have an invocation order associated with them, the inner map maps predIdx to predecessor
            std::unordered_map<uint64_t, std::unordered_map<uint64_t, Predecessor *>> outstandingPredecessors;

            QuorumSet<viewstamp_t, proto::PrepareOKMessage> prepareOKQuorum;
            QuorumSet<view_t, proto::StartViewChangeMessage> startViewChangeQuorum;
            QuorumSet<view_t, proto::DoViewChangeMessage> doViewChangeQuorum;

            Timeout *viewChangeTimeout;
            Timeout *nullCommitTimeout;
            Timeout *stateTransferTimeout;
            Timeout *resendPrepareTimeout;
            Timeout *closeBatch2Timeout;

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
            void CloseBatch2();
            void CloseBatch();

            void HandleRequest(const TransportAddress &remote,
                               const proto::RequestMessage &msg);
            void HandleUnloggedRequest(const TransportAddress &remote,
                                       const proto::UnloggedRequestMessage &msg);
            // IOCL specifics
            void HandleCoordination(const TransportAddress &remote,
                                    const proto::SuccessorRequestMessage &msg);

            void HandleCoordinationResp(const TransportAddress &remote,
                                        const proto::PredecessorReplyMessage &msg);

            void HandleCoordinationResp2(const TransportAddress &remote,
                                         const proto::PredecessorReplyMessage2 &msg);

            void HandlePrepare(const TransportAddress &remote,
                               const proto::PrepareMessage &msg);
            void HandlePrepareOK(const TransportAddress &remote,
                                 const proto::PrepareOKMessage &msg);
            void HandlePrepare2(const TransportAddress &remote,
                                const proto::PrepareMessage2 &msg);
            void HandlePrepareOK2(const TransportAddress &remote,
                                  const proto::PrepareOKMessage2 &msg);
            void assignSortedTs(LogEntry &entry);
            void finalizeEntry(LogEntry &entry);
            // void addOutstandingPredecessor(const google::protobuf::Message &msg, bool arrival);
            void addOutstandingPredecessor(const proto::PredecessorReplyMessage &msg);
            void addOutstandingPredecessor2(const proto::PredecessorReplyMessage2 &msg);
            // void sendMessageToSuccessorList(std::vector<replication::Successor *> &successors, google::protobuf::Message &msg);
            //
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
