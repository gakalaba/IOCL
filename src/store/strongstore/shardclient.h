/***********************************************************************
 *
 * store/strongstore/shardclient.h:
 *
 * Copyright 2022 Jeffrey Helt, Matthew Burke, Amit Levy, Wyatt Lloyd
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
#ifndef _STRONG_SHARDCLIENT_H_
#define _STRONG_SHARDCLIENT_H_

// Timeouts for various operations
#define GET_TIMEOUT 250
#define GET_RETRIES 3
// Only used for QWStore
#define PUT_TIMEOUT 250
#define PREPARE_TIMEOUT 1000
#define PREPARE_RETRIES 5

#define COMMIT_TIMEOUT 1000
#define COMMIT_RETRIES 5

#define ABORT_TIMEOUT 1000
#define RETRY_TIMEOUT 500000

#define OPERATION_TIMEOUT 1000
#define OPERATION_RETRIES 5

#include <unordered_set>
#include <vector>

#include "lib/assert.h"
#include "lib/latency.h"
#include "lib/message.h"
#include "lib/transport.h"
#include "store/common/promise.h"
#include "store/common/timestamp.h"
#include "store/common/transaction.h"
#include "store/strongstore/preparedtransaction.h"
#include "store/strongstore/strong-proto.pb.h"
#include "replication/common/request.pb.h"
#include "store/common/slot_pool.h"

namespace strongstore
{

    enum Mode
    {
        MODE_UNKNOWN,
        MODE_OCC,
        MODE_LOCK,
        MODE_SPAN_OCC,
        MODE_SPAN_LOCK,
        MODE_MVTSO
    };
    struct OutstandingPred {
        uint64_t tag;
        uint32_t shardid;
        uint16_t refcount;
    };

    typedef std::function<void(int, const std::string &, const std::string &, Timestamp)> get_callback;
    typedef std::function<void(int, const std::string &)> get_timeout_callback;

    typedef std::function<void(int, const std::string &, const std::string &)> put_callback;
    typedef std::function<void(int, const std::string &, const std::string &)> put_timeout_callback;

    typedef std::function<void(int, const std::string &, const std::vector<std::pair<uint64_t, uint32_t>> &)> op_callback;
    typedef std::function<void(int, const std::string &, const std::vector<std::pair<uint64_t, uint32_t>> &)> op_timeout_callback;

    typedef std::function<void(int, Timestamp)> prepare_callback;
    typedef std::function<void(int, Timestamp)> prepare_timeout_callback;

    typedef std::function<void(uint64_t, int, Timestamp)> prepare_ok_callback;

    typedef std::function<void(int)> rw_coord_commit_callback;
    typedef std::function<void(int)> rw_coord_commit_timeout_callback;

    typedef std::function<void(int)> rw_part_commit_callback;
    typedef std::function<void(int)> rw_part_commit_timeout_callback;

    typedef std::function<void()> abort_callback;
    typedef std::function<void()> abort_timeout_callback;

    typedef std::function<void(int, const std::vector<Value> &, const std::vector<PreparedTransaction> &)> ro_commit_callback;
    typedef std::function<void(int, uint64_t, const Timestamp &, bool)> ro_commit_slow_callback;
    typedef std::function<void()> ro_commit_timeout_callback;

    typedef std::function<void(uint64_t)> wound_callback;

    class ShardClient : public TransportReceiver
    {
    public:
        /* Constructor needs path to shard config. */
        ShardClient(
            const transport::Configuration &config, Transport *transport, uint64_t client_id,
            int shard, uint64_t fanout, wound_callback wcb = [](uint64_t transaction_id) {}, prepare_ok_callback pokcb = [](uint64_t transaction_id, int participant_shard, Timestamp prepare_timestamp) {});

        ~ShardClient();

        void ReceiveMessage(const TransportAddress &remote,
                            const std::string &type,
                            const std::string &data,
                            void *meta_data);
        void ReceiveMessage(const TransportAddress &remote,
                            MsgType type,
                            const std::string &data,
                            void *meta_data);
        void Close();

        void Begin(uint64_t transaction_id, const Timestamp &start_time);
        void Get(uint64_t id, const std::string &key, get_callback gcb,
                 get_timeout_callback gtcb, uint32_t timeout);

        void GetForUpdate(uint64_t transaction_id, const std::string &key,
                          get_callback gcb, get_timeout_callback gtcb,
                          uint32_t timeout);

        void Put(uint64_t transaction_id, const std::string &key, const std::string &value,
                 put_callback pcb, put_timeout_callback ptcb,
                 uint32_t timeout);
        
        void SendOperation(uint64_t app_request_id, const std::string op,
                         const std::string &key, const std::string &value,
                         op_callback ocb, op_timeout_callback otcb,
                         uint32_t timeout,
                         std::vector<OutstandingPred> &outstandingOperationVec,
                         bool isIOCL);

        void ROCommit(uint64_t transaction_id, const std::vector<std::string> &keys,
                      const Timestamp &commit_timestamp,
                      const Timestamp &min_read_timestamp,
                      ro_commit_callback ccb, ro_commit_slow_callback cscb,
                      ro_commit_timeout_callback ctcb, uint32_t timeout);

        void RWCommitCoordinator(uint64_t transaction_id,
                                 const std::unordered_set<int> participants,
                                 Timestamp &nonblock_timestamp,
                                 rw_coord_commit_callback ccb,
                                 rw_coord_commit_timeout_callback ctcb, uint32_t timeout);
        void RWCommitParticipant(uint64_t transaction_id,
                                 int coordinator_shard,
                                 Timestamp &nonblock_timestamp);

        void PrepareOK(uint64_t transaction_id, int participant_shard,
                       const Timestamp &prepare_timestamp, const Timestamp &nonblock_ts);

        void PrepareAbort(uint64_t transaction_id, int participant_shard,
                          prepare_callback pcb, prepare_timeout_callback ptcb,
                          uint32_t timeout);

        void Abort(uint64_t transaction_id, abort_callback acb,
                   abort_timeout_callback atcb, uint32_t timeout);

        void Wound(uint64_t transaction_id);
        void AbortGet(uint64_t transaction_id);
        void AbortPut(uint64_t transaction_id);

    private:
        struct PendingRequest
        {
            PendingRequest(uint64_t transaction_id, uint64_t req_id) : transaction_id{transaction_id}, req_id(req_id) {}
            uint64_t transaction_id;
            uint64_t req_id;
        };
        struct PendingPrepareAbort : public PendingRequest
        {
            PendingPrepareAbort(uint64_t transaction_id, uint64_t req_id) : PendingRequest(transaction_id, req_id) {}
            prepare_callback pcb;
            prepare_timeout_callback ptcb;
        };
        struct PendingROCommit : public PendingRequest
        {
            PendingROCommit(uint64_t transaction_id, uint64_t req_id) : PendingRequest(transaction_id, req_id) {}
            ro_commit_callback ccb;
            ro_commit_slow_callback cscb;
            ro_commit_timeout_callback ctcb;
            uint64_t n_slow_replies;
        };

        bool CheckPriorReadsAndWrites(uint64_t transaction_id, const std::string &key, get_callback gcb);

        void Get(uint64_t transaction_id, const std::string &key,
                 get_callback gcb, get_timeout_callback gtcb,
                 uint32_t timeout, bool for_update);

        void HandleGetReply(const proto::GetReply &reply);
        void HandleSendOperationReply(const proto::LinearizeableReply &reply);
        void HandleRWCommitCoordinatorReply(const proto::RWCommitCoordinatorReply &reply);
        // void HandleRWCommitParticipantReply(const proto::RWCommitParticipantReply &reply);
        void HandlePrepareOKReply(const proto::PrepareOKReply &reply);
        void HandlePrepareAbortReply(const proto::PrepareAbortReply &reply);
        void HandleROCommitReply(const proto::ROCommitReply &reply);
        void HandleROCommitSlowReply(const proto::ROCommitSlowReply &reply);
        void HandleAbortReply(const proto::AbortReply &reply);
        void HandleWound(const proto::Wound &wound);

        Transaction the_transaction_;
        bool server_shard_client_;
        std::unordered_map<std::string, std::string> the_read_set_;

        std::unordered_map<uint64_t, PendingPrepareAbort *> pendingPrepareAborts;
        std::unordered_map<uint64_t, PendingROCommit *> pendingROCommits;

        proto::Get get_;
        replication::LinearizeableOperation op_;
        proto::RWCommitCoordinator rw_commit_c_;
        proto::RWCommitParticipant rw_commit_p_;
        proto::PrepareOK prepare_ok_;
        proto::PrepareAbort prepare_abort_;
        proto::ROCommit ro_commit_;
        proto::Abort abort_;
        proto::Wound wound_;

        proto::GetReply get_reply_;
        proto::LinearizeableReply op_reply_;
        proto::RWCommitCoordinatorReply rw_commit_c_reply_;
        proto::RWCommitParticipantReply rw_commit_p_reply_;
        proto::PrepareOKReply prepare_ok_reply_;
        proto::PrepareAbortReply prepare_abort_reply_;
        proto::ROCommitReply ro_commit_reply_;
        proto::ROCommitSlowReply ro_commit_slow_reply_;
        proto::AbortReply abort_reply_;

        uint64_t last_req_id_;

        const transport::Configuration &config_;
        Transport *transport_; // Transport layer.
        uint64_t client_id_;   // Unique ID for this client.
        int shard_idx_;        // which shard this client accesses
        int replica_;          // which replica to use for reads
        wound_callback wcb_;
        prepare_ok_callback pokcb_;
        uint64_t fanout_;

        // IOCL Operation Metadata
        uint64_t seqno;

        struct PendingReplySlot {
            bool in_use = false;
            op_callback ocb;
            std::vector<std::pair<uint64_t, uint32_t>> pred_list;
        };
        std::vector<PendingReplySlot> slots_;

        struct PendingGetSlot {
            bool in_use = false;
            uint64_t transaction_id;
            get_callback gcb;
            std::string key;
        };
        SlotPool<PendingGetSlot> *get_slots_ = nullptr;

        struct PendingRWCoordCommitSlot {
            bool in_use = false;
            rw_coord_commit_callback ccb;
            uint64_t transaction_id;
        };
        SlotPool<PendingRWCoordCommitSlot> *pending_rw_coord_commit_slot_ = nullptr;

        struct PendingPrepareOKSlot {
            bool in_use = false;
        };
        SlotPool<PendingPrepareOKSlot> *pending_prepare_ok_slot_ = nullptr;

        struct PendingAbortSlot {
            bool in_use = false;
            abort_callback acb;
            uint64_t transaction_id;
        };
        SlotPool<PendingAbortSlot> *pending_abort_slot_ = nullptr;
    };

} // namespace strongstore

#endif /* _STRONG_SHARDCLIENT_H_ */
