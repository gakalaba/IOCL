// -*- mode: c++; c-file-style: "k&r"; c-basic-offset: 4 -*-
/***********************************************************************
 *
 * craq-test.cc:
 *   test cases for CRAQ protocol
 *
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

#include "lib/configuration.h"
#include "lib/message.h"
#include "lib/transport.h"
#include "lib/simtransport.h"

#include "replication/common/client.h"
#include "replication/common/replica.h"
#include "replication/craq/client.h"
#include "replication/craq/replica.h"
#include "replication/craq/craq-proto.pb.h"
#include "store/common/backend/versionstore.h"
#include "store/common/transaction.h"
#include "store/strongstore/strong-proto.pb.h"

using LinearizeableReply = strongstore::proto::LinearizeableReply;

#include <stdlib.h>
#include <stdio.h>
#include <gtest/gtest.h>
#include <vector>
#include <sstream>
#include <random>

const string COMMIT_MESSAGE_TYPE = "replication.craq.proto.CommitMessage";
const string VERSION_REQUEST_MESSAGE_TYPE = "replication.craq.proto.VersionRequestMessage";

using google::protobuf::Message;
using namespace replication;
using namespace replication::craq;
using namespace replication::craq::proto;

class CRAQApp : public AppReplica {

    std::vector<string> *ops;
    std::vector<string> *unloggedOps;

public:
    CRAQApp(std::vector<string> *o, std::vector<string> *u) : ops(o), unloggedOps(u) { }

    void ReplicaUpcallAppRequest(opnum_t opnum, LinearizeableOperation &req, string &response){
       Panic("Replica update app request should not be called");
    };

    void ReplicaUpcall(opnum_t opnum, const string &req, string &reply) {
        Panic("Replica upcall should not be called");
    }

    void ReplicaUpcall(const Timestamp &timestamp, const string &op, string &res) 
    {
        Notice("Replica upcall with timestamp");

        LinearizeableOperation req;
        LinearizeableReply reply;
        req.ParseFromString(op);
        uint64_t transaction_id = req.transaction_id();
        std::pair<uint64_t, uint64_t> value;

        string retval;
        int status = REPLY_OK;
        if (req.op() == "get")
        {
            Debug("the request is get for timestamp %d", timestamp.getTimestamp());
            if (!store.get(req.key(), timestamp.getTimestamp(), value))
            {
                Debug("value does not exist");
                status = REPLY_FAIL;
            };
            Debug("Get value %d from %s", value.second, req.key().c_str());
        }
        else if (req.op() == "put")
        {
            Debug("the request is put for timestamp %d",  timestamp.getTimestamp());
            store.put(req.key(), std::stoi(req.value()), timestamp.getTimestamp());
            Debug("put key %s and val %s", req.key().c_str(), req.value().c_str());
        }
        else
        {
            Panic("Unrecognized operation.");
        }
        reply.set_status(status);
        reply.set_return_value(std::to_string(value.second));
        reply.set_transaction_id(transaction_id);
        reply.mutable_rid()->set_client_id(req.rid().client_id());
        reply.mutable_rid()->set_client_req_id(req.rid().client_req_id());
        reply.SerializeToString(&res);

        Debug("pushing op");
        ops->push_back(op);
    }

    void UnloggedUpcall(const string &req, string &reply) {
        Panic("Should not be called");
    }

    void LeaderUpcall(opnum_t opnum, const string &str1,
                              bool &replicate, string &str2) 
    {
        Notice("Making craq leader upcall");
        replicate = true;
        str2 = str1;
    }

    VersionedKVStore<uint64_t, uint64_t> store;
};

struct CRAQTestParam
{
    int batchSize;
    int shards;
    int replicasPerShard;
    int clientsPerShard;
};


class CRAQTest : public  ::testing::TestWithParam<CRAQTestParam>
{
    struct ClientStruct
    {
        std::vector<CRAQClient *> clientList;
        string key;
    };

    using ShardType = int;

protected:
    std::unordered_map<ShardType, std::vector<CRAQReplica *>> replicas;
    std::unordered_map<ShardType, std::vector<CRAQApp>> apps;
    CRAQClient *client;
    SimulatedTransport *transport;
    transport::Configuration *config;
    std::vector<std::string> clientOps;
    std::vector<std::vector<string>> ops;
    std::vector<std::vector<string> > unloggedOps;
    std::map<ShardType, std::vector<transport::ReplicaAddress>> replicaAddrs; 
    std::unordered_map<ShardType, ClientStruct> clients; 
    int requestNum = 0;
    int shards;
    int clientsPerShard;

    virtual void SetUp() {
        CRAQTestParam param = GetParam();
        shards = param.shards; 
        int replicasPerShard = param.replicasPerShard;
        clientsPerShard = param.clientsPerShard;

        if (shards < 1 || replicasPerShard < 1 || clientsPerShard < 1) Panic("Shards and clients/replicas per shard must be at least 1");

        int faultTolerance = 1;
        int keys = 1;

        int totalReplicas = shards * replicasPerShard;
        int currentLocalHostAddress = 12345;

        for (ShardType shard = 0; shard < shards; shard++)
        {
           replicaAddrs[shard] = {};
           for (int replicaIndex = 0; replicaIndex < replicasPerShard; replicaIndex++)
           {
                replicaAddrs[shard].push_back({"localhost", std::to_string(currentLocalHostAddress)});
                currentLocalHostAddress++;
           }
            
        }

        config = new transport::Configuration(shards, replicasPerShard, faultTolerance, replicaAddrs);

        transport = new SimulatedTransport();

        ops.resize(totalReplicas);
        unloggedOps.resize(totalReplicas);
        for (ShardType shard = 0; shard < shards; shard++)
        {
           replicas[shard].reserve(replicasPerShard);
           apps[shard].reserve(replicasPerShard);
           for (int replicaIndex = 0; replicaIndex < replicasPerShard; replicaIndex++)
           {
                int appIndex = shard * replicasPerShard + replicaIndex;
                ops[appIndex].reserve(100);

                apps[shard].emplace_back(&ops[appIndex], &unloggedOps[appIndex]);
                replicas[shard].push_back(new CRAQReplica(*config, shard, replicaIndex, transport, param.batchSize, &apps[shard][replicaIndex], true));
           }

            string key = "key" + std::to_string(shard);
            Notice("key is set to %s", key);
            ClientStruct clientStruct;
            clientStruct.key = key;
            clients[shard] = clientStruct;

            for (int clientIndex = 0; clientIndex < clientsPerShard; clientIndex++)
            {
                clients[shard].clientList.push_back(new CRAQClient(*config, transport, shard, shard * clientsPerShard + clientIndex));
            }
        }

        string request_str;

        for (ShardType shard = 0; shard < shards; shard++)
        {
            auto &clientInfo = clients[shard];

            ClientSendNext(shard, putUpcall, "put");

            transport->Run();

            for (int replicaIndex = 0; replicaIndex < replicasPerShard; replicaIndex++) 
            {
                std::pair<size_t, uint64_t> val;
                string key = clientInfo.key;
                apps[shard][replicaIndex].store.get(key, val);
                
                EXPECT_EQ(val.second, requestNum);
            }
        }

        // Only let tests run for a simulated minute. This prevents infinite retry loops, etc.
       transport->Timer(60000, [&]() {
               transport->CancelAllTimers();
           });
    }

    virtual void ClientSendNext(int shard, Client::continuation_t upcall, std::string op, int clientIndex = 0, int replicaIndex = -1) {
        string request_str;

        auto &clientInfo = clients[shard];

        LinearizeableOperation linop;
        linop.mutable_rid()->set_client_id(shard * clientsPerShard + clientIndex);
        // must be fixed!
        linop.mutable_rid()->set_client_req_id(requestNum);
        linop.set_transaction_id(requestNum);
        linop.set_op(op);
        linop.set_key(clientInfo.key);
        Debug("Putting val %d", requestNum);
        linop.set_value(std::to_string(requestNum));

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

    // Drain all buffered messages into the live queue without running transport.
    void FlushBufferedQueue() {
        while (!transport->IsBufferedQueueEmpty())
        {
            transport->PopBufferedEvent();
        }
    }

    // Block until a message of `type` is popped from the event queue.
    void RunUntilMessageType(const string &type) {
        while (transport->PopEvent() != type) {}
    }

    // Assert every replica in `shard` stores `expectedValue` for that shard's key.
    void ExpectAllReplicasHaveValue(int shard, int expectedValue) {
        int replicasPerShard = GetParam().replicasPerShard;
        for (int r = 0; r < replicasPerShard; r++) {
            std::pair<size_t, uint64_t> val;
            EXPECT_TRUE(apps[shard][r].store.get(clients[shard].key, val));
            EXPECT_EQ(val.second, (uint64_t)expectedValue);
        }
    }

    // Put upcall that does NOT cancel timers — use when multiple puts will
    // complete inside a single transport->Run() call.
    std::function<bool(const std::string &, const std::string &)>
    MakeSilentPutUpcall(int &putsCompleted) {
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

    virtual void TearDown() {
        for (auto kv : replicas) {
            for (auto replica : kv.second)
            {
                delete replica;
            }
        }

        replicas.clear();
        ops.clear();
        unloggedOps.clear();

        for (auto &kv : clients) {
            for (CRAQClient *c : kv.second.clientList) {
                delete c;
            }
            kv.second.clientList.clear();
        }
        clients.clear();
        delete transport;
        delete config;
    }

public:
    bool validateUpcall(const std::string &req, const std::string &reply, int expectedValue) {
        LinearizeableOperation linop;
        LinearizeableReply linreply;
        
        // 1. Parsing and basic sanity checks
        EXPECT_TRUE(linop.ParseFromString(req));
        EXPECT_TRUE(linreply.ParseFromString(reply));
        EXPECT_EQ(linreply.status(), REPLY_OK);
        EXPECT_EQ(linop.transaction_id(), linreply.transaction_id());

        // 2. Value validation
        int actualValue = std::stoi(linreply.return_value());
        Notice("Expected %d, got %d", expectedValue, actualValue);
        EXPECT_EQ(expectedValue, actualValue);

        // 3. Side effects
        transport->CancelAllTimers();
        return true;
    }

    std::function<bool(const std::string &, const std::string &)>
        MakeGetUpcall(int expectedValue)
        {
            return [this, expectedValue](const std::string &req,
                                        const std::string &reply) -> bool {
                return validateUpcall(req, reply, expectedValue);
            };
        }
    
    std::function<bool(const std::string &req,
                const std::string &reply)> putUpcall =
    [this](const std::string &req,
        const std::string &reply) -> bool {

        LinearizeableOperation linop;
        LinearizeableReply linreply;
        bool parsed;

        parsed = linop.ParseFromString(req);
        EXPECT_TRUE(parsed);
        parsed = linreply.ParseFromString(reply);
        EXPECT_TRUE(parsed);

        Notice("client upcall is called for put",
            linreply.return_value().c_str());

        EXPECT_EQ(linreply.status(), REPLY_OK);

        transport->CancelAllTimers();

        return true;
    };
};

TEST_P(CRAQTest, SimpleGet)
{
    int prevWriteRequestNum = requestNum;
    for (int shard = 0; shard < shards; shard++)
    {
        requestNum++;
        ClientSendNext(shard, MakeGetUpcall(prevWriteRequestNum), "get");
    }
    transport->Run();

    // copy log logic once gap logic is fixed, then check logs through op
}

TEST_P(CRAQTest, AllClientsWrite)
{
    int iterations = 1;
    for (int requestPerShard = 0; requestPerShard < iterations; requestPerShard++)
    {
        for (int shard = 0; shard < shards; shard++)
        {
            for (int writingClientIndex = 0; writingClientIndex < GetParam().clientsPerShard; writingClientIndex++)
            {
                int prevWritingRequestNum = ++requestNum;
                ClientSendNext(shard, putUpcall, "put", writingClientIndex);
                transport->Run();

                // check write can be read by all clients
                for (int clientIndex = 0; clientIndex < GetParam().clientsPerShard; clientIndex++)
                {
                    requestNum++;
                    ClientSendNext(shard, MakeGetUpcall(prevWritingRequestNum), "get");
                    transport->Run();
                }

                // check write can be read by all replicas
                for (int replicaIndex = 0; replicaIndex < config->n; replicaIndex++)
                {
                    std::pair<size_t, uint64_t> val;
                    string key = clients[shard].key;
                    bool success = apps[shard][replicaIndex].store.get(key, val);
                    EXPECT_TRUE(success);
                    EXPECT_EQ(val.second, prevWritingRequestNum);
                }

            }
        }
    }
}

TEST_P(CRAQTest, BasicVersionRequestIgnorePendingWrite)
{
    auto params = GetParam();
    // only run once with basic input
    if (params.clientsPerShard > 1 || params.shards > 1)
    {
        GTEST_SKIP();
    }

    int prevWriteRequestNum = ++requestNum;
    ClientSendNext(0, putUpcall, "put"); 
    
    transport->SetBufferingMessage(COMMIT_MESSAGE_TYPE);

    // run events just before back-propagating commit message
    RunUntilMessageType(COMMIT_MESSAGE_TYPE);
    Notice("Finished propagating write and commiting at tail, not sending commit messages yet");
   
    requestNum++;
    ClientSendNext(0, MakeGetUpcall(prevWriteRequestNum - 1), "get"); 

    transport->Run();

    transport->ResetBufferingMessage();
    // pop the commitMessage
    FlushBufferedQueue();
    // resume runnning craq
    transport->Run();
    EXPECT_TRUE(transport->IsQueueEmpty());
    EXPECT_TRUE(transport->IsBufferedQueueEmpty());
}

TEST_P(CRAQTest, BasicVersionRequestReadPendingWrite)
{
    auto params = GetParam();
    // only run once with basic input
    if (params.clientsPerShard > 1 || params.shards > 1)
    {
        GTEST_SKIP();
    }
    string messageType;

    int prevWriteRequestNum = ++requestNum;
    ClientSendNext(0, putUpcall, "put"); 
    
    transport->SetBufferingMessage(COMMIT_MESSAGE_TYPE);

    // run events just before back-propagating commit message
    RunUntilMessageType(COMMIT_MESSAGE_TYPE);
    Notice("Finished propagating write and commiting at tail, not sending commit messages yet");
   
    transport->SetBufferingMessage(VERSION_REQUEST_MESSAGE_TYPE);
    requestNum++;
    ClientSendNext(0, MakeGetUpcall(prevWriteRequestNum), "get"); 
    RunUntilMessageType(VERSION_REQUEST_MESSAGE_TYPE);
    Notice("Buffer version request for read");

    // backpropagate write up chain
    messageType = transport->PopBufferedEvent();
    EXPECT_EQ(COMMIT_MESSAGE_TYPE, messageType);
    transport->Run();

    Notice("Propagated commits");
    // pop the version request
    FlushBufferedQueue();
    transport->Run();
    EXPECT_TRUE(transport->IsQueueEmpty());
    EXPECT_TRUE(transport->IsBufferedQueueEmpty());
}

TEST_P(CRAQTest, StressRandomReadsWrites)
{
    Notice("Starting test");
    auto params = GetParam();
    // Dirty-read scenarios require exactly 1 shard and 1 client per shard,
    // but we need >= 2 replicas so there's a non-tail node that can be dirty.
    // if (params.shards > 1 || params.clientsPerShard > 1 || params.replicasPerShard < 2) {
    //     GTEST_SKIP();
    // }

    const int NUM_ITERATIONS = 50;
    const int replicasPerShard = params.replicasPerShard;

    std::mt19937 rng(42);
    std::uniform_int_distribution<int> dist(0, 3);

    // committedValue is the value any correct read must return.
    // SetUp did one put, so requestNum is 0 and committedValue == 0.
    int committedValue = requestNum;

    for (int iter = 0; iter < NUM_ITERATIONS; iter++) {
        int scenario = dist(rng);
        Notice("Stress iter %d: scenario=%d committedValue=%d requestNum=%d",
               iter, scenario, committedValue, requestNum);

        if (scenario == 0) {
            // ------------------------------------------------------------------
            // Scenario 0: clean write followed by a clean read.
            // The chain is fully committed after Run(), so the read must see
            // the new value.
            // ------------------------------------------------------------------
            requestNum++;
            committedValue = requestNum;
            ClientSendNext(0, putUpcall, "put");
            transport->Run();

            requestNum++;
            ClientSendNext(0, MakeGetUpcall(committedValue), "get");
            transport->Run();

            // All replicas must agree on committedValue in their stores.
            ExpectAllReplicasHaveValue(0, committedValue);

        } else if (scenario == 1) {
            // ------------------------------------------------------------------
            // Scenario 1: dirty read — commit ack is buffered then discarded.
            //
            // The write propagates HEAD→…→TAIL.  The tail commits and starts
            // sending acks backward (COMMIT_MESSAGE_TYPE).  We intercept and
            // discard those acks, so non-tail nodes never learn the write is
            // committed — they stay dirty.
            //
            // A read that lands on a dirty non-tail node issues a version
            // request to the tail.  The tail's committed version is the
            // *previous* write (it committed pendingValue, but the ack never
            // propagated, so from the perspective of the version query the
            // answer depends on implementation — in this codebase the test
            // proves the old value is returned).
            //
            // After discarding the acks, we issue a recovery write so that
            // all nodes receive a fresh write + commit, cleaning up the
            // leftover dirty state before the next iteration.
            // ------------------------------------------------------------------
            int pendingValue = ++requestNum;
            Notice("Scenario 1: issuing pending write %d", pendingValue);
            ClientSendNext(0, putUpcall, "put");

            // Intercept the first commit ack leaving the tail.
            transport->SetBufferingMessage(COMMIT_MESSAGE_TYPE);
            RunUntilMessageType(COMMIT_MESSAGE_TYPE);
            Notice("Scenario 1: write reached tail, commit ack now buffered");

            // Read hits a dirty non-tail node → version request → old value.
            requestNum++;
            ClientSendNext(0, MakeGetUpcall(committedValue), "get");
            transport->Run();

            // Throw away the buffered ack — pendingValue is abandoned.
            transport->ResetBufferingMessage();
            FlushBufferedQueue();
            transport->Run();

            // Recovery write: this propagates cleanly through all nodes and
            // commits, guaranteeing every node's dirty state is resolved
            // before the next iteration begins.
            requestNum++;
            committedValue = requestNum;
            Notice("Scenario 1: recovery write %d", committedValue);
            ClientSendNext(0, putUpcall, "put");
            transport->Run();

            // Confirm every replica stored the recovery value.
            ExpectAllReplicasHaveValue(0, committedValue);

        } else if (scenario == 2) {
            // ------------------------------------------------------------------
            // Scenario 2: dirty read — version request is delayed until AFTER
            //   the commit ack backpropagates, so the read sees the NEW value.
            //
            // Sequence:
            //   1. Write pendingValue — stops at the tail (commit buffered).
            //   2. Issue a read to the dirty HEAD node.  HEAD sends a version
            //      request to the tail — buffer that too.
            //   3. Release the commit ack.  All non-tail nodes learn
            //      pendingValue is now committed (clean).
            //   4. Release the version request.  The tail replies with the
            //      *new* committed version, so the read returns pendingValue.
            // ------------------------------------------------------------------
            int pendingValue = ++requestNum;
            Notice("Scenario 2: issuing write %d", pendingValue);
            ClientSendNext(0, putUpcall, "put");

            // Step 1: buffer the commit ack so non-tail nodes stay dirty.
            transport->SetBufferingMessage(COMMIT_MESSAGE_TYPE);
            RunUntilMessageType(COMMIT_MESSAGE_TYPE);
            Notice("Scenario 2: commit ack buffered");

            // Step 2: issue a read (goes to dirty HEAD, which must ask the
            // tail for the committed version).  Buffer that version request
            // before it reaches the tail.  SetBufferingMessage replaces the
            // previous buffering type; the already-buffered COMMIT stays in
            // the buffered queue.
            transport->SetBufferingMessage(VERSION_REQUEST_MESSAGE_TYPE);
            requestNum++;
            ClientSendNext(0, MakeGetUpcall(pendingValue), "get");
            RunUntilMessageType(VERSION_REQUEST_MESSAGE_TYPE);
            Notice("Scenario 2: version request buffered");

            // Buffered queue now contains (in order): COMMIT, VERSION_REQUEST.

            // Step 3: pop and deliver COMMIT first, let the transport process
            // all resulting events (acks travel up the chain; nodes go clean).
            string firstBuffered = transport->PopBufferedEvent();
            EXPECT_EQ(COMMIT_MESSAGE_TYPE, firstBuffered);
            transport->Run();
            Notice("Scenario 2: commit backpropagated, all nodes clean");

            // Step 4: deliver the version request now that the tail's
            // committed version reflects pendingValue.  The tail replies with
            // pendingValue, the HEAD returns it to the client.
            FlushBufferedQueue();
            transport->Run();

            committedValue = pendingValue;
            Notice("Scenario 2: committed value advanced to %d", committedValue);

            // All replicas must have pendingValue in their stores.
            ExpectAllReplicasHaveValue(0, committedValue);
        }
        else {
            // ------------------------------------------------------------------
            // Scenario 3: single write — verifies a write commits cleanly and
            // all replicas converge to the new value.
            // ------------------------------------------------------------------
            requestNum++;
            committedValue = requestNum;
            Notice("Scenario 3: issuing write %d", committedValue);
            ClientSendNext(0, putUpcall, "put");
            transport->Run();

            ExpectAllReplicasHaveValue(0, committedValue);
            }
    }

    EXPECT_TRUE(transport->IsQueueEmpty());
    EXPECT_TRUE(transport->IsBufferedQueueEmpty());
}

TEST_P(CRAQTest, CommitLogOrdering)
{
    auto params = GetParam();
    if (params.clientsPerShard > 1 || params.shards > 1)
    {
        GTEST_SKIP();
    }

    int replicasPerShard = params.replicasPerShard;

    auto parseEntry = [](const LogEntry *entry) -> LinearizeableOperation {
        LinearizeableOperation linop;
        linop.ParseFromString(entry->request.op());
        linop.mutable_rid()->set_client_id(entry->request.clientid());
        linop.mutable_rid()->set_client_req_id(entry->request.clientreqid());
        return linop;
    };

    // Check commitLog entry at pos on replica r has expectedOp.
    auto expectCommitLogEntry = [&](int r, opnum_t pos, const string &expectedOp) {
        const Log &clog = replicas[0][r]->GetCommitLog();

        EXPECT_FALSE(clog.Empty())
            << "commitLog is empty on replica " << r;
        if (clog.Empty()) return;

        EXPECT_GE(clog.LastOpnum(), pos)
            << "commitLog too short on replica " << r
            << " (last=" << clog.LastOpnum() << " want pos=" << pos << ")";
        if (clog.LastOpnum() < pos) return;

        const LogEntry *entry = clog.Find(pos);
        EXPECT_NE(entry, nullptr)
            << "commitLog entry " << pos << " missing on replica " << r;
        if (entry == nullptr) return;

        EXPECT_EQ(entry->state, LOG_STATE_CLEAN)
            << "commitLog entry " << pos << " not clean on replica " << r;

        LinearizeableOperation linop = parseEntry(entry);
        EXPECT_EQ(linop.op(), expectedOp)
            << "commitLog entry " << pos << " op mismatch on replica " << r
            << " (got=" << linop.op() << " want=" << expectedOp << ")";
    };

    // -----------------------------------------------------------------------
    // Phase 1: clean write.
    // SetUp did one put → commitLog[1]. This write is commitLog[2].
    // -----------------------------------------------------------------------
    int writeValue = ++requestNum;
    Notice("CommitLogOrdering: clean write %d", writeValue);
    ClientSendNext(0, putUpcall, "put");
    transport->Run();

    for (int r = 0; r < replicasPerShard; r++)
    {
        expectCommitLogEntry(r, 1, "put");
        expectCommitLogEntry(r, 2, "put");
        EXPECT_EQ(replicas[0][r]->GetCommitLog().LastOpnum(), (opnum_t)2)
            << "commitLog should have exactly 2 entries on replica " << r;
    }

    // -----------------------------------------------------------------------
    // Phase 2: clean read.
    // Chain is clean so read executes immediately → commitLog[3] on head only.
    // Non-head replicas never see reads directly so their log stays at [2].
    // -----------------------------------------------------------------------
    requestNum++;
    Notice("CommitLogOrdering: clean read");
    ClientSendNext(0, MakeGetUpcall(writeValue), "get");
    transport->Run();

    // Head (replica 0) handles the read directly and logs it.
    expectCommitLogEntry(0, 3, "get");
    EXPECT_EQ(replicas[0][0]->GetCommitLog().LastOpnum(), (opnum_t)3)
        << "head commitLog should have 3 entries after 2 writes + 1 read";

    // Non-head replicas do not see reads — their commitLog stays at 2.
    for (int r = 1; r < replicasPerShard; r++)
    {
        EXPECT_EQ(replicas[0][r]->GetCommitLog().LastOpnum(), (opnum_t)2)
            << "non-head replica " << r << " should not log reads it didn't serve";
    }

    // -----------------------------------------------------------------------
    // Phase 3: dirty read — commit ack buffered then discarded.
    // -----------------------------------------------------------------------
    int dirtyWriteValue = ++requestNum;
    Notice("CommitLogOrdering: dirty write %d (ack will be discarded)", dirtyWriteValue);
    ClientSendNext(0, putUpcall, "put");

    transport->SetBufferingMessage(COMMIT_MESSAGE_TYPE);
    RunUntilMessageType(COMMIT_MESSAGE_TYPE);
    Notice("CommitLogOrdering: commit ack buffered, chain dirty");

    requestNum++;
    ClientSendNext(0, MakeGetUpcall(writeValue), "get");
    transport->Run();

    transport->ResetBufferingMessage();
    FlushBufferedQueue();
    transport->Run();

    // HEAD: FlushWritesUpTo in HandleVersionResponse flushed the dirty write
    // before the read — so [4]=dirty write, [5]=read.
    expectCommitLogEntry(0, 4, "put");
    expectCommitLogEntry(0, 5, "get");
    EXPECT_EQ(replicas[0][0]->GetCommitLog().LastOpnum(), (opnum_t)5)
        << "head should have 5 entries: 2 writes + 1 clean read + 1 dirty write + 1 dirty read";

    // Mid replicas: FlushBufferedQueue released the commit ack, CommitUpTo
    // ran, dirty write was flushed. They never log reads.
    for (int r = 1; r < replicasPerShard - 1; r++)
    {
        expectCommitLogEntry(r, 3, "put");
        EXPECT_EQ(replicas[0][r]->GetCommitLog().LastOpnum(), (opnum_t)3)
            << "mid replica " << r << " should have 3 entries after dirty write committed";
    }

    // Tail: committed the dirty write in HandlePrepare, flushed it immediately.
    // Never logs reads.
    int tail = replicasPerShard - 1;
    expectCommitLogEntry(tail, 3, "put");
    EXPECT_EQ(replicas[0][tail]->GetCommitLog().LastOpnum(), (opnum_t)3)
        << "tail should have 3 entries after dirty write committed";

    // Recovery write.
    requestNum++;
    ClientSendNext(0, putUpcall, "put");
    transport->Run();

    ExpectAllReplicasHaveValue(0, requestNum);

    // HEAD: recovery write flushed -> [6]=put
    expectCommitLogEntry(0, 6, "put");
    EXPECT_EQ(replicas[0][0]->GetCommitLog().LastOpnum(), (opnum_t)6)
        << "head should have 6 entries after recovery write";

    // Non-head: recovery write flushed -> [4]=put
    for (int r = 1; r < replicasPerShard; r++)
    {
        expectCommitLogEntry(r, 4, "put");
        EXPECT_EQ(replicas[0][r]->GetCommitLog().LastOpnum(), (opnum_t)4)
            << "replica " << r << " should have 4 entries after recovery write";
    }

    // -----------------------------------------------------------------------
    // Phase 4: dirty read — commit backpropagates BEFORE version request,
    // so the read sees the new value and the write precedes the read in
    // commitLog on HEAD.
    // -----------------------------------------------------------------------
    int pendingValue = ++requestNum;
    Notice("CommitLogOrdering: write %d (dirty read will see this)", pendingValue);
    ClientSendNext(0, putUpcall, "put");

    transport->SetBufferingMessage(COMMIT_MESSAGE_TYPE);
    RunUntilMessageType(COMMIT_MESSAGE_TYPE);
    Notice("CommitLogOrdering: commit ack buffered");

    transport->SetBufferingMessage(VERSION_REQUEST_MESSAGE_TYPE);
    requestNum++;
    ClientSendNext(0, MakeGetUpcall(pendingValue), "get");
    RunUntilMessageType(VERSION_REQUEST_MESSAGE_TYPE);
    Notice("CommitLogOrdering: version request buffered");

    string first = transport->PopBufferedEvent();
    EXPECT_EQ(COMMIT_MESSAGE_TYPE, first);
    transport->Run();
    Notice("CommitLogOrdering: commits backpropagated");

    FlushBufferedQueue();
    transport->Run();

    // HEAD: commit flushed pendingWrite[5] -> [7]=put, then version response
    // appended read -> [8]=get. Write must precede read.
    {
        const Log &clog = replicas[0][0]->GetCommitLog();
        opnum_t last = clog.LastOpnum();

        EXPECT_GE(last, (opnum_t)2)
            << "head commitLog too short for phase 4 check";
        if (last >= 2)
        {
            const LogEntry *writeEntry = clog.Find(last - 1);
            const LogEntry *readEntry  = clog.Find(last);

            EXPECT_NE(writeEntry, nullptr) << "write entry missing on head";
            EXPECT_NE(readEntry,  nullptr) << "read entry missing on head";
            if (writeEntry != nullptr && readEntry != nullptr)
            {
                EXPECT_EQ(parseEntry(writeEntry).op(), string("put"))
                    << "expected write before read on head";
                EXPECT_EQ(parseEntry(readEntry).op(), string("get"))
                    << "expected read as last entry on head";
                EXPECT_LT(writeEntry->viewstamp.opnum, readEntry->viewstamp.opnum)
                    << "write opnum must be less than read opnum on head";
            }
        }
    }

    // Non-HEAD replicas never log reads — their last entry is the write.
    for (int r = 1; r < replicasPerShard; r++)
    {
        const Log &clog = replicas[0][r]->GetCommitLog();
        opnum_t last = clog.LastOpnum();

        EXPECT_GE(last, (opnum_t)1)
            << "commitLog unexpectedly empty on replica " << r;
        if (last < 1) continue;

        const LogEntry *lastEntry = clog.Find(last);
        EXPECT_NE(lastEntry, nullptr) << "last entry null on replica " << r;
        if (lastEntry == nullptr) continue;

        EXPECT_EQ(parseEntry(lastEntry).op(), string("put"))
            << "non-head replica " << r << " last entry should be a write (reads not logged here)";
    }
}

INSTANTIATE_TEST_CASE_P(test,
                        CRAQTest,
                        ::testing::Values(
                            CRAQTestParam{1, 1, 3, 1},      // basic setup
                            CRAQTestParam{1, 10, 3, 1},     // many shards
                            CRAQTestParam{1, 2, 3, 10},       // many clients
                            CRAQTestParam{1, 10, 10, 10}       // many all
                        ));