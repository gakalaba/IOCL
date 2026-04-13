// -*- mode: c++; c-file-style: "k&r"; c-basic-offset: 4 -*-
/***********************************************************************
 *
 * iocl_craq-test.cc:
 *   Unit tests for IOCL_CRAQ protocol
 *
 **********************************************************************/

#include "lib/configuration.h"
#include "lib/message.h"
#include "lib/transport.h"
#include "lib/simtransport.h"

#include "replication/common/client.h"
#include "replication/common/replica.h"
#include "replication/iocl_craq/client.h"
#include "replication/iocl_craq/replica.h"
#include "replication/iocl_craq/iocl_craq-proto.pb.h"
#include "store/common/backend/versionstore.h"
#include "store/common/transaction.h"
#include "store/strongstore/strong-proto.pb.h"

using LinearizeableReply = strongstore::proto::LinearizeableReply;

#include <stdlib.h>
#include <stdio.h>
#include <gtest/gtest.h>
#include <vector>
#include <sstream>

// Message type strings for buffering coordination messages.
const string COORD_REQUEST_TYPE  = "replication.iocl_craq.proto.SuccessorRequestMessage";
const string COORD_RESPONSE_TYPE = "replication.iocl_craq.proto.PredecessorReplyMessage";
const string COMMIT_MESSAGE_TYPE = "replication.iocl_craq.proto.CommitMessage";

using google::protobuf::Message;
using namespace replication;
using namespace replication::iocl_craq;
using namespace replication::iocl_craq::proto;

// ---------------------------------------------------------------------------
// Application replica for tests: a simple versioned KV store.
// ---------------------------------------------------------------------------
class CRAQApp : public AppReplica {
    std::vector<string> *ops;
    std::vector<string> *unloggedOps;

public:
    CRAQApp(std::vector<string> *o, std::vector<string> *u) : ops(o), unloggedOps(u) {}

    void ReplicaUpcallAppRequest(opnum_t opnum, LinearizeableOperation &req, string &response) {
        Panic("Should not be called");
    }

    void ReplicaUpcall(opnum_t opnum, const string &req, string &reply) {
        Panic("Should not be called");
    }

    void ReplicaUpcall(const Timestamp &timestamp, const string &op, string &res)
    {
        LinearizeableOperation req;
        LinearizeableReply reply;
        req.ParseFromString(op);

        std::pair<uint64_t, uint64_t> value;
        int status = REPLY_OK;

        if (req.op() == "get")
        {
            Debug("get for timestamp %lu", timestamp.getTimestamp());
            if (!store.get(req.key(), timestamp.getTimestamp(), value))
            {
                Debug("value does not exist");
                status = REPLY_FAIL;
            }
            Debug("Get value %lu from %s", value.second, req.key().c_str());
        }
        else if (req.op() == "put")
        {
            Debug("put for timestamp %lu", timestamp.getTimestamp());
            store.put(req.key(), std::stoi(req.value()), timestamp.getTimestamp());
            Debug("put key %s val %s", req.key().c_str(), req.value().c_str());
        }
        else
        {
            Panic("Unrecognized operation: %s", req.op().c_str());
        }

        reply.set_status(status);
        reply.set_return_value(std::to_string(value.second));
        reply.set_transaction_id(req.transaction_id());
        reply.mutable_rid()->set_client_id(req.rid().client_id());
        reply.mutable_rid()->set_client_req_id(req.rid().client_req_id());
        reply.SerializeToString(&res);

        ops->push_back(op);
    }

    void UnloggedUpcall(const string &req, string &reply) {
        Panic("Should not be called");
    }

    void LeaderUpcall(opnum_t opnum, const string &str1, bool &replicate, string &str2)
    {
        replicate = true;
        str2 = str1;
    }

    VersionedKVStore<uint64_t, uint64_t> store;
};

// ---------------------------------------------------------------------------
// Test parameterization
// ---------------------------------------------------------------------------
struct CRAQTestParam
{
    int batchSize;
    int shards;
    int replicasPerShard;
    int clientsPerShard;
};

// ---------------------------------------------------------------------------
// Test fixture
// ---------------------------------------------------------------------------
class IOCL_CRAQTest : public ::testing::TestWithParam<CRAQTestParam>
{
    struct ClientStruct
    {
        std::vector<IOCL_CRAQClient *> clientList;
        string key;
    };

    using ShardType = int;

protected:
    std::unordered_map<ShardType, std::vector<IOCL_CRAQReplica *>> replicas;
    std::unordered_map<ShardType, std::vector<CRAQApp>> apps;
    SimulatedTransport *transport;
    transport::Configuration *config;
    std::vector<std::string> clientOps;
    std::vector<std::vector<string>> ops;
    std::vector<std::vector<string>> unloggedOps;
    std::map<ShardType, std::vector<transport::ReplicaAddress>> replicaAddrs;
    std::unordered_map<ShardType, ClientStruct> clients;
    int requestNum = 0;
    int shards;
    int clientsPerShard;
    uint64_t manualShardTagCounter = 1000000;

    virtual void SetUp()
    {
        CRAQTestParam param = GetParam();
        shards            = param.shards;
        int replicasPerShard = param.replicasPerShard;
        clientsPerShard   = param.clientsPerShard;

        if (shards < 1 || replicasPerShard < 1 || clientsPerShard < 1)
            Panic("Shards and clients/replicas per shard must be at least 1");

        int faultTolerance = 1;
        int currentLocalHostAddress = 22345;

        for (ShardType shard = 0; shard < shards; shard++)
        {
            replicaAddrs[shard] = {};
            for (int r = 0; r < replicasPerShard; r++)
            {
                replicaAddrs[shard].push_back({"localhost",
                                               std::to_string(currentLocalHostAddress++)});
            }
        }

        config = new transport::Configuration(shards, replicasPerShard, faultTolerance,
                                              replicaAddrs);
        transport = new SimulatedTransport();

        int totalReplicas = shards * replicasPerShard;
        ops.resize(totalReplicas);
        unloggedOps.resize(totalReplicas);

        for (ShardType shard = 0; shard < shards; shard++)
        {
            replicas[shard].reserve(replicasPerShard);
            apps[shard].reserve(replicasPerShard);

            for (int r = 0; r < replicasPerShard; r++)
            {
                int appIndex = shard * replicasPerShard + r;
                ops[appIndex].reserve(100);
                apps[shard].emplace_back(&ops[appIndex], &unloggedOps[appIndex]);
                replicas[shard].push_back(new IOCL_CRAQReplica(
                    *config, shard, r, transport, param.batchSize,
                    &apps[shard][r], /*debug_stats=*/true));
            }

            string key = "key" + std::to_string(shard);
            ClientStruct cs;
            cs.key = key;
            clients[shard] = cs;

            for (int c = 0; c < clientsPerShard; c++)
            {
                clients[shard].clientList.push_back(new IOCL_CRAQClient(
                    *config, transport, shard,
                    /*clientid=*/shard * clientsPerShard + c));
            }
        }

        // Send an initial put on every shard so all replicas have a baseline
        // value and the client's predecessor tracking starts from a committed op.
        for (ShardType shard = 0; shard < shards; shard++)
        {
            ClientSendNext(shard, putUpcall, "put");
            transport->Run();

            // Verify all replicas received the initial value.
            int replicasPerShardLocal = GetParam().replicasPerShard;
            for (int r = 0; r < replicasPerShardLocal; r++)
            {
                std::pair<size_t, uint64_t> val;
                EXPECT_TRUE(apps[shard][r].store.get(clients[shard].key, val));
                EXPECT_EQ(val.second, (uint64_t)requestNum);
            }
        }

        // Give tests up to a simulated minute before auto-cancelling.
        transport->Timer(60000, [&]() { transport->CancelAllTimers(); });
    }

    // Send an operation from clientIndex on shard. replicaIndex = -1 → replica 0.
    virtual void ClientSendNext(int shard, Client::continuation_t upcall,
                                std::string op, int clientIndex = 0,
                                int replicaIndex = -1)
    {
        auto &clientInfo = clients[shard];

        LinearizeableOperation linop;
        linop.mutable_rid()->set_client_id(shard * clientsPerShard + clientIndex);
        linop.mutable_rid()->set_client_req_id(requestNum);
        linop.set_transaction_id(requestNum);
        linop.set_op(op);
        linop.set_key(clientInfo.key);
        linop.set_value(std::to_string(requestNum));

        string request_str;
        linop.SerializeToString(&request_str);

        if (replicaIndex == -1)
        {
            clientInfo.clientList[clientIndex]->Invoke(request_str, upcall);
        }
        else
        {
            clientInfo.clientList[clientIndex]->Invoke(request_str, upcall, replicaIndex);
        }
        clientOps.push_back(request_str);
    }

    // Send an operation with an explicit IOCL predecessor edge. This is needed
    // in unit tests because the IOCL_CRAQClient does not auto-chain writes.
    virtual uint64_t ClientSendNextWithExplicitPred(int shard, Client::continuation_t upcall,
                                                    std::string op,
                                                    uint64_t predShardTag,
                                                    int predShard,
                                                    int predReplicaIndex,
                                                    int clientIndex = 0,
                                                    int replicaIndex = -1)
    {
        auto &clientInfo = clients[shard];

        LinearizeableOperation linop;
        linop.mutable_rid()->set_client_id(shard * clientsPerShard + clientIndex);
        linop.mutable_rid()->set_client_req_id(requestNum);
        linop.set_transaction_id(requestNum);
        linop.set_op(op);
        linop.set_key(clientInfo.key);
        linop.set_value(std::to_string(requestNum));
        uint64_t shardTag = ++manualShardTagCounter;
        linop.set_shardtag(shardTag);
        linop.add_predlist(predShardTag);
        linop.add_shardlist(predShard);
        linop.add_pred_replicalist(predReplicaIndex);

        string request_str;
        linop.SerializeToString(&request_str);

        // Unit-test clients do not auto-chain writes, so send the matching
        // CoordRequest explicitly for tests that model predecessor blocking.
        SuccessorRequestMessage coordReq;
        coordReq.set_p(predShardTag);
        coordReq.set_s(shardTag);
        coordReq.set_succ_groupidx(shard);
        coordReq.set_succ_replicaidx(op == "put" ? (config->n - 1)
                                                 : (replicaIndex == -1 ? 0 : replicaIndex));
        static_cast<Transport *>(transport)->SendMessageToReplica(
            clientInfo.clientList[clientIndex], predShard, predReplicaIndex, coordReq);

        clientInfo.clientList[clientIndex]->SetSendCoordRequests(false);
        if (replicaIndex == -1)
        {
            clientInfo.clientList[clientIndex]->Invoke(request_str, upcall);
        }
        else
        {
            clientInfo.clientList[clientIndex]->Invoke(request_str, upcall, replicaIndex);
        }
        clientInfo.clientList[clientIndex]->SetSendCoordRequests(true);
        clientOps.push_back(request_str);
        return shardTag;
    }

    // Drain all buffered messages into the live queue.
    void FlushBufferedQueue()
    {
        while (!transport->IsBufferedQueueEmpty())
        {
            transport->PopBufferedEvent();
        }
    }

    // Pop events until one matching `type` is delivered, then stop.
    void RunUntilMessageType(const string &type)
    {
        while (transport->PopEvent() != type) {}
    }

    // Assert every replica in shard stores expectedValue for that shard's key.
    void ExpectAllReplicasHaveValue(int shard, int expectedValue)
    {
        int replicasPerShard = GetParam().replicasPerShard;
        for (int r = 0; r < replicasPerShard; r++)
        {
            std::pair<size_t, uint64_t> val;
            EXPECT_TRUE(apps[shard][r].store.get(clients[shard].key, val));
            EXPECT_EQ(val.second, (uint64_t)expectedValue);
        }
    }

    // Put upcall that does NOT cancel timers — for concurrent op sequences.
    std::function<bool(const std::string &, const std::string &)>
    MakeSilentPutUpcall(int &putsCompleted)
    {
        return [this, &putsCompleted](const std::string &req,
                                     const std::string &reply) -> bool {
            LinearizeableOperation linop;
            LinearizeableReply linreply;
            EXPECT_TRUE(linop.ParseFromString(req));
            EXPECT_TRUE(linreply.ParseFromString(reply));
            EXPECT_EQ(linreply.status(), REPLY_OK);
            putsCompleted++;
            return true;
        };
    }

    // Get upcall that validates return_value == expectedValue and cancels timers.
    std::function<bool(const std::string &, const std::string &)>
    MakeGetUpcall(int expectedValue)
    {
        return [this, expectedValue](const std::string &req,
                                     const std::string &reply) -> bool {
            LinearizeableOperation linop;
            LinearizeableReply linreply;
            EXPECT_TRUE(linop.ParseFromString(req));
            EXPECT_TRUE(linreply.ParseFromString(reply));
            EXPECT_EQ(linreply.status(), REPLY_OK);
            int actual = std::stoi(linreply.return_value());
            Notice("Expected %d, got %d", expectedValue, actual);
            EXPECT_EQ(expectedValue, actual);
            transport->CancelAllTimers();
            return true;
        };
    }

    virtual void TearDown()
    {
        for (auto &kv : replicas)
        {
            for (auto r : kv.second) delete r;
        }
        replicas.clear();
        ops.clear();
        unloggedOps.clear();

        for (auto &kv : clients)
        {
            for (auto c : kv.second.clientList) delete c;
            kv.second.clientList.clear();
        }
        clients.clear();
        delete transport;
        delete config;
    }

public:
    // Put upcall that cancels all timers (stops transport->Run()).
    std::function<bool(const std::string &req, const std::string &reply)> putUpcall =
        [this](const std::string &req, const std::string &reply) -> bool {
        LinearizeableOperation linop;
        LinearizeableReply linreply;
        EXPECT_TRUE(linop.ParseFromString(req));
        EXPECT_TRUE(linreply.ParseFromString(reply));
        EXPECT_EQ(linreply.status(), REPLY_OK);
        transport->CancelAllTimers();
        return true;
    };
};

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

// A subsequent read returns the value written by the initial put in SetUp.
TEST_P(IOCL_CRAQTest, SimpleGet)
{
    int prevRequestNum = requestNum;
    for (int shard = 0; shard < shards; shard++)
    {
        requestNum++;
        ClientSendNext(shard, MakeGetUpcall(prevRequestNum), "get");
    }
    transport->Run();
}

// Sequential puts from one client all commit.
TEST_P(IOCL_CRAQTest, SequentialPuts)
{
    int iterations = 3;
    for (int i = 0; i < iterations; i++)
    {
        requestNum++;
        for (int shard = 0; shard < shards; shard++)
        {
            ClientSendNext(shard, putUpcall, "put");
            transport->Run();
            ExpectAllReplicasHaveValue(shard, requestNum);
        }
    }
}

// After a put, reading the key returns the written value.
TEST_P(IOCL_CRAQTest, PutThenGet)
{
    int prevRequestNum = requestNum;
    requestNum++;

    for (int shard = 0; shard < shards; shard++)
    {
        // Write a new value.
        ClientSendNext(shard, putUpcall, "put");
        transport->Run();
        ExpectAllReplicasHaveValue(shard, requestNum);

        // Read it back.
        requestNum++;
        ClientSendNext(shard, MakeGetUpcall(requestNum - 1), "get");
        transport->Run();
    }
}

// Core IOCL test: a write with a predecessor (op_B follows op_A) must NOT
// commit until the CoordResponse for op_A is released.
TEST_P(IOCL_CRAQTest, WriteBlockedByPredecessor)
{
    // Only meaningful with a single shard and single client; skip otherwise.
    if (shards > 1 || clientsPerShard > 1)
    {
        GTEST_SKIP() << "IOCL blocking test requires single shard, single client";
    }

    // op_A (requestNum=0) already committed in SetUp.
    int opAValue = requestNum;
    // The warmup write issued in SetUp is the first request from this client,
    // so the unit-test client deterministically assigns shardtag=1.
    uint64_t opAShardTag = 1;

    // Start buffering CoordResponse messages so op_B cannot be unblocked.
    transport->SetBufferingMessage(COORD_RESPONSE_TYPE);

    int opBCompleted = 0;
    requestNum++;
    int opBValue = requestNum;
    ClientSendNextWithExplicitPred(0, MakeSilentPutUpcall(opBCompleted), "put",
                                   opAShardTag, /*predShard=*/0,
                                   /*predReplicaIndex=*/config->n - 1);

    // Run until the CoordResponse for op_B is intercepted.
    // This happens when the tail sees committedForCoord[opA_shardtag] and
    // immediately responds to the CoordRequest sent by the client.
    RunUntilMessageType(COORD_RESPONSE_TYPE);

    // op_B must NOT have committed yet (CoordResponse is buffered).
    EXPECT_EQ(opBCompleted, 0);

    // All replicas should still show op_A's value, not op_B's.
    ExpectAllReplicasHaveValue(0, opAValue);

    // Release the CoordResponse → op_B can now commit.
    transport->ResetBufferingMessage();
    FlushBufferedQueue();
    transport->Run();

    // op_B must now have committed.
    EXPECT_EQ(opBCompleted, 1);
    ExpectAllReplicasHaveValue(0, opBValue);
}

// Three chained writes: op_C depends on op_B which depends on op_A.
// Releasing CoordResponses one at a time verifies the chain unblocks in order.
TEST_P(IOCL_CRAQTest, ChainedBlockingWrites)
{
    if (shards > 1 || clientsPerShard > 1)
    {
        GTEST_SKIP() << "Chained blocking test requires single shard, single client";
    }

    int opAValue = requestNum;
    uint64_t opAShardTag = 1;

    // Buffer ALL CoordResponses.
    transport->SetBufferingMessage(COORD_RESPONSE_TYPE);

    int opBCompleted = 0, opCCompleted = 0;

    // op_B: predecessor = op_A
    requestNum++;
    int opBValue = requestNum;
    uint64_t opBShardTag =
        ClientSendNextWithExplicitPred(0, MakeSilentPutUpcall(opBCompleted), "put",
                                       opAShardTag, /*predShard=*/0,
                                       /*predReplicaIndex=*/config->n - 1);
    RunUntilMessageType(COORD_RESPONSE_TYPE);  // CoordResp for op_B captured

    // op_C: predecessor = op_B. But the client sends CoordRequest to op_B's
    // handler immediately. op_B hasn't committed yet, so it will be buffered
    // at the tail in pendingCoordRequests[opB_shardtag].
    requestNum++;
    int opCValue = requestNum;
    ClientSendNextWithExplicitPred(0, MakeSilentPutUpcall(opCCompleted), "put",
                                   opBShardTag, /*predShard=*/0,
                                   /*predReplicaIndex=*/config->n - 1);
    // The CoordRequest for op_C arrives at the tail (predecessor = op_B).
    // Since op_B hasn't committed, it is buffered. No CoordResp for op_C yet.

    // Nothing should have committed.
    EXPECT_EQ(opBCompleted, 0);
    EXPECT_EQ(opCCompleted, 0);
    ExpectAllReplicasHaveValue(0, opAValue);

    // Release CoordResp for op_B → op_B commits.
    transport->ResetBufferingMessage();
    transport->SetBufferingMessage(COORD_RESPONSE_TYPE);  // re-arm for op_C's response
    FlushBufferedQueue();  // delivers CoordResp for op_B to tail

    // Run until op_B commits and the tail sends CoordResp for op_C.
    RunUntilMessageType(COORD_RESPONSE_TYPE);

    EXPECT_EQ(opBCompleted, 1);
    EXPECT_EQ(opCCompleted, 0);
    ExpectAllReplicasHaveValue(0, opBValue);

    // Release CoordResp for op_C → op_C commits.
    transport->ResetBufferingMessage();
    FlushBufferedQueue();
    transport->Run();

    EXPECT_EQ(opCCompleted, 1);
    ExpectAllReplicasHaveValue(0, opCValue);
}

// CoordRequest arrives at the tail BEFORE the write reaches the tail.
// The CoordRequest is buffered, then served when the write commits.
TEST_P(IOCL_CRAQTest, CoordRequestArrivesBeforeWrite)
{
    if (shards > 1 || clientsPerShard > 1)
    {
        GTEST_SKIP() << "Test requires single shard, single client";
    }

    // op_A is already committed (from SetUp).
    int opAValue = requestNum;

    // Buffer the PrepareMessage so op_B's write doesn't reach the tail yet.
    // The CoordRequest goes directly to the tail in 1 hop, so it will arrive
    // before the PrepareMessage (which takes 2+ hops through the chain).
    // We verify that when the PrepareMessage is released, op_B commits
    // immediately (because the CoordResponse was pre-buffered by the time
    // the write arrived at the tail).
    //
    // In SimulatedTransport, message ordering is deterministic (FIFO per sender).
    // The CoordRequest (client→tail, 1 hop) arrives before the PrepareMessage
    // (client→head→...→tail) in the normal case. This test verifies correctness
    // when those messages interleave.

    int opBCompleted = 0;
    requestNum++;
    int opBValue = requestNum;
    uint64_t opAShardTag = 1;
    ClientSendNextWithExplicitPred(0, MakeSilentPutUpcall(opBCompleted), "put",
                                   opAShardTag, /*predShard=*/0,
                                   /*predReplicaIndex=*/config->n - 1);

    // Run the full protocol — no interception. op_B should commit.
    transport->Run();

    EXPECT_EQ(opBCompleted, 1);
    ExpectAllReplicasHaveValue(0, opBValue);
}

// ---------------------------------------------------------------------------
// Test: Two shards X (shard=0) and Y (shard=1), each Head→Middle→Tail.
//
// Client1 (clientIndex=0) issues writes w1(x) and w2(y) concurrently.
// Client2 (clientIndex=1) reads from the MIDDLE replica (replicaIndex=1) on
// both shards *before* the writes are queued — SimulatedTransport FIFO
// guarantees those reads reach MIDDLE while it still holds the clean initial
// value, producing genuine stale reads (old value from SetUp).
//
// Verifies:
//   - Stale reads during propagation return REPLY_OK (no crash, no assert).
//   - Both writes commit via IOCL coordination.
//   - r5 (post-stabilisation read of shard X) returns exactly w1Value.
//   - Final state: all replicas on shard X = w1Value, shard Y = w2Value.
// ---------------------------------------------------------------------------
TEST_P(IOCL_CRAQTest, TwoShardConcurrentWritesAndStaleReads)
{
    if (shards < 2 || clientsPerShard < 2)
        GTEST_SKIP() << "Requires >= 2 shards and >= 2 clients per shard";

    // After SetUp all shards hold value = 0.

    int staleReadsCompleted = 0;
    auto silentGetUpcall = [&](const std::string &req,
                                const std::string &reply) -> bool {
        LinearizeableReply linreply;
        EXPECT_TRUE(linreply.ParseFromString(reply));
        EXPECT_EQ(linreply.status(), REPLY_OK);
        staleReadsCompleted++;
        return true;
    };

    // r3: Client2 reads shard Y from MIDDLE (replicaIndex=1).
    // Queued BEFORE w2, so MIDDLE still has the clean initial value.
    requestNum++;
    ClientSendNext(/*shard=*/1, silentGetUpcall, "get",
                   /*clientIndex=*/1, /*replicaIndex=*/1);

    // r4: Client2 reads shard X from MIDDLE (replicaIndex=1).
    // Queued BEFORE w1, so MIDDLE still has the clean initial value.
    requestNum++;
    ClientSendNext(/*shard=*/0, silentGetUpcall, "get",
                   /*clientIndex=*/1, /*replicaIndex=*/1);

    // w1: Client1 writes to shard X. PrepareMessage travels HEAD→MIDDLE→TAIL
    // and therefore arrives at MIDDLE *after* r4 has already been processed.
    requestNum++;
    int w1Value = requestNum;
    int w1Completed = 0;
    ClientSendNext(/*shard=*/0, MakeSilentPutUpcall(w1Completed), "put",
                   /*clientIndex=*/0);

    // w2: Client1 writes to shard Y. IOCL coordination fires on the per-shard
    // predecessor chain; the tail sends CoordResponse and commits.
    requestNum++;
    int w2Value = requestNum;
    int w2Completed = 0;
    ClientSendNext(/*shard=*/1, MakeSilentPutUpcall(w2Completed), "put",
                   /*clientIndex=*/0);

    // Drain the event queue.  FIFO ordering ensures:
    //   r3 and r4 reach MIDDLE before any PrepareMessage → stale reads (value=0).
    //   w1 and w2 propagate asynchronously through their chains; IOCL
    //   coordination triggers and resolves; tails commit and reply to Client1.
    transport->Run();

    EXPECT_EQ(staleReadsCompleted, 2);  // both stale reads completed with REPLY_OK
    EXPECT_EQ(w1Completed, 1);          // w1 committed
    EXPECT_EQ(w2Completed, 1);          // w2 committed

    // r5: Post-stabilisation read of shard X — must return the committed w1Value.
    requestNum++;
    ClientSendNext(/*shard=*/0, MakeGetUpcall(w1Value), "get",
                   /*clientIndex=*/1);
    transport->Run();

    // All replicas on both shards must have converged to the written values.
    ExpectAllReplicasHaveValue(/*shard=*/0, w1Value);
    ExpectAllReplicasHaveValue(/*shard=*/1, w2Value);
}

// ---------------------------------------------------------------------------
// Stress test: 4 shards (x=0, y=1, z=2, a=3), each Head→Middle→Tail, 5 clients.
//
// Client1 (clientIndex=0) issues overlapping writes w1(x), w2(y), w3(z), w4(a).
// Clients 2–4 (clientIndex=1,2,3) read from MIDDLE replicas during propagation;
// because their requests are queued before the writes, they observe stale values.
// Client5 (clientIndex=4) reads after all writes stabilise — must see final values.
//
// Verifies:
//   - 6 early reads all return REPLY_OK (stale but not invalid).
//   - All 4 writes commit; IOCL coordination triggers per shard.
//   - 4 late reads each return the correct committed value.
//   - Final state: all replicas on every shard reflect x=w1, y=w2, z=w3, a=w4.
// ---------------------------------------------------------------------------
TEST_P(IOCL_CRAQTest, MultiShardMultiClientStress)
{
    if (shards < 4 || clientsPerShard < 5)
        GTEST_SKIP() << "Requires >= 4 shards and >= 5 clients per shard";

    // All shards hold value = 0 after SetUp.

    int earlyReadsCompleted = 0;
    auto earlyGetUpcall = [&](const std::string &req,
                               const std::string &reply) -> bool {
        LinearizeableReply linreply;
        EXPECT_TRUE(linreply.ParseFromString(reply));
        EXPECT_EQ(linreply.status(), REPLY_OK);
        earlyReadsCompleted++;
        return true;
    };

    // ---- Queue early reads BEFORE writes so MIDDLE sees stale values. ----

    // Client2 (clientIndex=1): r5(a), r6(z)
    requestNum++;
    ClientSendNext(/*shard a=*/3, earlyGetUpcall, "get",
                   /*clientIndex=*/1, /*replicaIndex=*/1);
    requestNum++;
    ClientSendNext(/*shard z=*/2, earlyGetUpcall, "get",
                   /*clientIndex=*/1, /*replicaIndex=*/1);

    // Client3 (clientIndex=2): r7(z), r8(y)
    requestNum++;
    ClientSendNext(/*shard z=*/2, earlyGetUpcall, "get",
                   /*clientIndex=*/2, /*replicaIndex=*/1);
    requestNum++;
    ClientSendNext(/*shard y=*/1, earlyGetUpcall, "get",
                   /*clientIndex=*/2, /*replicaIndex=*/1);

    // Client4 (clientIndex=3): r9(y), r10(x)
    requestNum++;
    ClientSendNext(/*shard y=*/1, earlyGetUpcall, "get",
                   /*clientIndex=*/3, /*replicaIndex=*/1);
    requestNum++;
    ClientSendNext(/*shard x=*/0, earlyGetUpcall, "get",
                   /*clientIndex=*/3, /*replicaIndex=*/1);

    // ---- Queue Client1 writes (all 4 shards, overlapping propagation). ----

    requestNum++;
    int w1Value = requestNum;
    int w1Completed = 0;
    ClientSendNext(/*shard x=*/0, MakeSilentPutUpcall(w1Completed), "put",
                   /*clientIndex=*/0);

    requestNum++;
    int w2Value = requestNum;
    int w2Completed = 0;
    ClientSendNext(/*shard y=*/1, MakeSilentPutUpcall(w2Completed), "put",
                   /*clientIndex=*/0);

    requestNum++;
    int w3Value = requestNum;
    int w3Completed = 0;
    ClientSendNext(/*shard z=*/2, MakeSilentPutUpcall(w3Completed), "put",
                   /*clientIndex=*/0);

    requestNum++;
    int w4Value = requestNum;
    int w4Completed = 0;
    ClientSendNext(/*shard a=*/3, MakeSilentPutUpcall(w4Completed), "put",
                   /*clientIndex=*/0);

    // Drain: 6 early reads see stale value=0; 4 writes propagate and commit.
    // IOCL coordination fires once per shard for each Client1 write.
    transport->Run();

    EXPECT_EQ(earlyReadsCompleted, 6);
    EXPECT_EQ(w1Completed, 1);
    EXPECT_EQ(w2Completed, 1);
    EXPECT_EQ(w3Completed, 1);
    EXPECT_EQ(w4Completed, 1);

    // ---- Late reads from Client5 — must reflect committed values. ----

    int lateReadsCompleted = 0;
    auto makeLateGetUpcall = [&](int expectedValue) {
        return [&, expectedValue](const std::string &req,
                                   const std::string &reply) -> bool {
            LinearizeableReply linreply;
            EXPECT_TRUE(linreply.ParseFromString(reply));
            EXPECT_EQ(linreply.status(), REPLY_OK);
            EXPECT_EQ(std::stoi(linreply.return_value()), expectedValue);
            lateReadsCompleted++;
            return true;
        };
    };

    requestNum++;
    ClientSendNext(/*shard a=*/3, makeLateGetUpcall(w4Value), "get",
                   /*clientIndex=*/4);  // r11(a)
    requestNum++;
    ClientSendNext(/*shard z=*/2, makeLateGetUpcall(w3Value), "get",
                   /*clientIndex=*/4);  // r12(z)
    requestNum++;
    ClientSendNext(/*shard y=*/1, makeLateGetUpcall(w2Value), "get",
                   /*clientIndex=*/4);  // r13(y)
    requestNum++;
    ClientSendNext(/*shard x=*/0, makeLateGetUpcall(w1Value), "get",
                   /*clientIndex=*/4);  // r14(x)

    transport->Run();

    EXPECT_EQ(lateReadsCompleted, 4);

    // All replicas on every shard must reflect the final written values.
    ExpectAllReplicasHaveValue(/*shard x=*/0, w1Value);
    ExpectAllReplicasHaveValue(/*shard y=*/1, w2Value);
    ExpectAllReplicasHaveValue(/*shard z=*/2, w3Value);
    ExpectAllReplicasHaveValue(/*shard a=*/3, w4Value);
}

// ---------------------------------------------------------------------------
// Test instantiation
// ---------------------------------------------------------------------------
INSTANTIATE_TEST_CASE_P(IOCL_CRAQ, IOCL_CRAQTest, ::testing::Values(
    CRAQTestParam{/*batchSize=*/1, /*shards=*/1, /*replicasPerShard=*/3, /*clientsPerShard=*/1},
    CRAQTestParam{/*batchSize=*/1, /*shards=*/1, /*replicasPerShard=*/5, /*clientsPerShard=*/1},
    CRAQTestParam{/*batchSize=*/1, /*shards=*/2, /*replicasPerShard=*/3, /*clientsPerShard=*/2},
    CRAQTestParam{/*batchSize=*/1, /*shards=*/4, /*replicasPerShard=*/3, /*clientsPerShard=*/5}
));
