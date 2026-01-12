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

#include <list>
#include <map>
#include <memory>

#include "lib/configuration.h"
#include "lib/latency.h"
#include "replication/common/log.h"
#include "replication/common/replica.h"
#include "replication/craq/craq-proto.pb.h"

namespace replication
{
    namespace craq
    {

        class CRAQReplica : public Replica
        {
        public:
            CRAQReplica(transport::Configuration config, int groupIdx, int myIdx,
                        Transport *transport, unsigned int batchSize, AppReplica *app,
                        bool debug_stats);
            ~CRAQReplica();
            void Close();

            void ReceiveMessage(const TransportAddress &remote, const string &type,
                                const string &data, void *meta_data);

        private:
            view_t view;
            int myIdx;
            int numReplicas;
            opnum_t lastCommitted;
            opnum_t lastOp;
            opnum_t lastRequestStateTransferOpnum;
            std::list<std::pair<TransportAddress *, proto::PrepareMessage>>
                pendingPrepares;
            proto::PrepareMessage lastPrepare;
            unsigned int batchSize;
            opnum_t lastBatchEnd;

            Log log;
            std::map<uint64_t, std::unique_ptr<TransportAddress>> clientAddresses;
            struct ClientTableEntry
            {
                uint64_t lastReqId;
                bool replied;
                proto::ReplyMessage reply;
            };
            std::map<uint64_t, ClientTableEntry> clientTable;

            Timeout *resendPrepareTimeout;
            Timeout *closeBatchTimeout;

            Latency_t rec_to_upcall_lat_;
            Latency_t upcall_to_exec_lat_;
            Latency_t exec_to_sent_lat_;

            bool debug_stats_;

            [[nodiscard]] inline bool AmHead() const {return myIdx == 0;}
            [[nodiscard]] inline bool AmTail() const {return myIdx == numReplicas - 1;}
            [[nodiscard]] bool ForwardPropagateMessageInChain(const Message &m);
            [[nodiscard]] bool BackwardsPropagateMessageInChain(const Message &m);
            void ExecuteOperation(const Request &entry);
            void CommitUpTo(opnum_t upto);
            void SendPrepareOKs(opnum_t oldLastOp);
            void RequestStateTransfer();
            void UpdateClientTable(const Request &req);
            void ResendPrepare();
            [[nodiscard]] bool IsDuplicateRequest(const TransportAddress &remote,
                                const proto::RequestMessage &msg);
            void AddToClientTable(const TransportAddress &remote, 
                                const proto::RequestMessage &msg);
            void CloseBatch();

            void HandleRequest(const TransportAddress &remote,
                               const proto::RequestMessage &msg);
            void HandleWriteRequest(const TransportAddress &remote,
                               const proto::RequestMessage &msg);
            void HandleReadRequest(const TransportAddress &remote,
                               const proto::RequestMessage &msg);
            void HandleUnloggedRequest(const TransportAddress &remote,
                                       const proto::UnloggedRequestMessage &msg);
            void HandlePrepare(const TransportAddress &remote,
                               const proto::PrepareMessage &msg);
            void HandlePrepareOK(const TransportAddress &remote,
                                 const proto::PrepareOKMessage &msg);
            void HandleCommit(const TransportAddress &remote,
                              const proto::CommitMessage &msg);
            void HandleRequestStateTransfer(
                const TransportAddress &remote,
                const proto::RequestStateTransferMessage &msg);
            void HandleStateTransfer(const TransportAddress &remote,
                                     const proto::StateTransferMessage &msg);
        };

    } // namespace craq
} // namespace replication

#endif /* _CRAQ_REPLICA_H_ */
