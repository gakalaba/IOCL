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

    virtual void ClientSendNext(int shard, Client::continuation_t upcall, std::string op, int clientIndex = 0) {
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

        clientInfo.clientList[clientIndex]->Invoke(request_str, upcall);
        clientOps.push_back(request_str);
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
    
    // std::function<bool(const std::string &req,
    //                 const std::string &reply)> getUpcall =
    //     [this](const std::string &req,
    //         const std::string &reply) -> bool {
    //         return validateUpcall(req, reply, requestNum - 1);
           
    //     };

    // std::function<bool(const std::string &req,
    //                 const std::string &reply)> getUpcallIgnoredPendingWrite =
    //     [this](const std::string &req,
    //         const std::string &reply) -> bool {
    //         return validateUpcall(req, reply, requestNum - 2);
           
    //     };

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
    
    string messageToBuffer = COMMIT_MESSAGE_TYPE;
    transport->SetBufferingMessage(messageToBuffer);

    // run events just before back-propagating commit message
    while (messageToBuffer != transport->PopEvent()){}
    Notice("Finished propagating write and commiting at tail, not sending commit messages yet");
   
    requestNum++;
    ClientSendNext(0, MakeGetUpcall(prevWriteRequestNum - 1), "get"); 

    transport->Run();

    transport->ResetBufferingMessage();
    // pop the commitMessage
    while (!transport->IsBufferedQueueEmpty())
    {
        transport->PopBufferedEvent();
    }
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
    string messageToBuffer;
    string messageType;

    int prevWriteRequestNum = ++requestNum;
    ClientSendNext(0, putUpcall, "put"); 
    
    messageToBuffer = COMMIT_MESSAGE_TYPE;
    transport->SetBufferingMessage(messageToBuffer);

    // run events just before back-propagating commit message
    while (messageToBuffer != transport->PopEvent()){}
    Notice("Finished propagating write and commiting at tail, not sending commit messages yet");
   
    messageToBuffer = VERSION_REQUEST_MESSAGE_TYPE;
    transport->SetBufferingMessage(messageToBuffer);
    requestNum++;
    ClientSendNext(0, MakeGetUpcall(prevWriteRequestNum), "get"); 
    while (messageToBuffer != transport->PopEvent()){}
    Notice("Buffer version request for read");

    // backpropagate write up chain
    messageType = transport->PopBufferedEvent();
    EXPECT_EQ(COMMIT_MESSAGE_TYPE, messageType);
    transport->Run();

    Notice("Propagated commits");
    // pop the version request
    while (!transport->IsBufferedQueueEmpty())
    {
        transport->PopBufferedEvent();
    }
    transport->Run();
    EXPECT_TRUE(transport->IsQueueEmpty());
    EXPECT_TRUE(transport->IsBufferedQueueEmpty());
}

INSTANTIATE_TEST_CASE_P(test,
                        CRAQTest,
                        ::testing::Values(
                            CRAQTestParam{1, 1, 3, 1},      // basic setup
                            CRAQTestParam{1, 10, 3, 1},     // many shards
                            CRAQTestParam{1, 2, 3, 10},       // many clients
                            CRAQTestParam{1, 10, 10, 10}       // many all
                        ));
