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

static string replicaLastOp;
static string clientLastOp;
static string clientLastReply;

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
        std::pair<uint64_t, std::string> value;

        string retval;
        int status = REPLY_OK;
        if (req.op() == "get")
        {
            Debug("the request is get");
            if (!store.get(req.key(), timestamp.getTimestamp(), value))
            {
                Debug("value does not exist");
                status = REPLY_FAIL;
            };
            Debug("Get value %s from %s", value.second.c_str(), req.key().c_str());
        }
        else if (req.op() == "put")
        {
            Debug("the request is put");
            store.put(req.key(), req.value(), transaction_id);
            Debug("put key %s and val %s", req.key().c_str(), req.value().c_str());
        }
        else
        {
            Panic("Unrecognized operation.");
        }
        reply.set_status(status);
        reply.set_return_value(value.second);
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

    VersionedKVStore<uint64_t, std::string> store;
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

protected:
    std::vector<CRAQReplica *> replicas;
    std::vector<CRAQApp> apps;
    CRAQClient *client;
    SimulatedTransport *transport;
    transport::Configuration *config;
    std::vector<std::string> clientOps;
    std::vector<std::vector<string>> ops;
    std::vector<std::vector<string> > unloggedOps;
    std::map<int, std::vector<transport::ReplicaAddress>> replicaAddrs; 
    std::unordered_map<int, ClientStruct> clients; 
    int requestNum = 0;
    int groups;

    virtual void SetUp() {
        CRAQTestParam param = GetParam();
        groups = param.shards; 
        int replicasPerGroup = param.replicasPerShard;

        if (groups < 1 || replicasPerGroup < 1) Panic("Groups and replicas per group must be at least 1");

        int faultTolerance = 1;
        int keys = 1;

        int totalReplicas = groups * replicasPerGroup;
        int currentLocalHostAddress = 12345;

        for (int group = 0; group < groups; group++)
        {
           replicaAddrs[group] = {};
           for (int replicaIndex = 0; replicaIndex < replicasPerGroup; replicaIndex++)
           {
                replicaAddrs[group].push_back({"localhost", std::to_string(currentLocalHostAddress)});
                currentLocalHostAddress++;
           }
            
        }

        config = new transport::Configuration(groups, replicasPerGroup, faultTolerance, replicaAddrs);

        transport = new SimulatedTransport();

        ops.resize(totalReplicas);
        unloggedOps.resize(totalReplicas);
        apps.reserve(totalReplicas);
        for (int group = 0; group < groups; group++)
        {
           for (int replicaIndex = 0; replicaIndex < replicasPerGroup; replicaIndex++)
           {
                int appIndex = group * replicasPerGroup + replicaIndex;
                ops[appIndex].reserve(100);
                apps.emplace_back(&ops[appIndex], &unloggedOps[appIndex]);
                replicas.push_back(new CRAQReplica(*config, group, replicaIndex, transport, param.batchSize, &apps[appIndex], true));
           }

            string key = "key" + std::to_string(group);
            Notice("key is set to %s", key);
            ClientStruct clientStruct;
            clientStruct.key = key;
            clients[group] = clientStruct;
            clients[group].clientList.push_back(new CRAQClient(*config, transport, group, group));
        }

        string request_str;

        for (int clientIndex = 0; clientIndex < groups; clientIndex++)
        {
            auto clientInfo = clients[clientIndex];

            LinearizeableOperation linop;
            // only one client rn
            linop.mutable_rid()->set_client_id(requestNum);
            linop.mutable_rid()->set_client_req_id(requestNum);
            linop.set_transaction_id(requestNum);
            linop.set_op("put");
            linop.set_key(clientInfo.key);
            linop.set_value(clientInfo.key);

            linop.SerializeToString(&request_str);
            auto upcall = [this](const string &req, const string &reply) {return true;};

            clientInfo.clientList[0]->Invoke(request_str, upcall);
            clientOps.push_back(request_str);
            transport->Run();

            for (int i = 0; i < replicasPerGroup; i++) 
            {
                int appIndex = (clientIndex * replicasPerGroup) + i;
                std::pair<size_t, std::string> val;
                string key = clientInfo.key;
                apps[appIndex].store.get(key, val);
                
                EXPECT_EQ(val.second, clientInfo.key);
            }
        }

        // Only let tests run for a simulated minute. This prevents infinite retry loops, etc.
       transport->Timer(60000, [&]() {
               transport->CancelAllTimers();
           });
    }

    virtual void ClientSendNext(int clientIndex, Client::continuation_t upcall, std::string op, std::string key, int clientListIndex = 0) {
        string request_str;

        auto clientInfo = clients[clientIndex];

        LinearizeableOperation linop;
        // only one client rn
        linop.mutable_rid()->set_client_id(requestNum);
        linop.mutable_rid()->set_client_req_id(requestNum);
        linop.set_transaction_id(requestNum);
        linop.set_key(key);
        linop.set_value(key);
        linop.set_op(op);

        linop.SerializeToString(&request_str);

        clientInfo.clientList[clientListIndex]->Invoke(request_str, upcall);
        clientOps.push_back(request_str);
    }

    virtual void TearDown() {
        for (auto x : replicas) {
            delete x;
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
};

TEST_P(CRAQTest, SimpleGet)
{
    auto simpleGetUpcall = [this](const string &req, const string &reply) {
        LinearizeableOperation linop;
        LinearizeableReply linreply;
        bool parsed;

        parsed = linop.ParseFromString(req);
        EXPECT_TRUE(parsed);
        parsed = linreply.ParseFromString(reply);
        EXPECT_TRUE(parsed);

        Notice("client upcall is called with retval %s", linreply.return_value().c_str()); 

        EXPECT_EQ(linreply.status(), REPLY_OK);
        EXPECT_EQ(linop.transaction_id(), linreply.transaction_id());
        EXPECT_EQ(linop.value(), linreply.return_value());

        transport->CancelAllTimers();
        return true;
    };

    for (int clientIndex = 0; clientIndex < groups; clientIndex++)
    {
        ClientSendNext(clientIndex, simpleGetUpcall, "get", clients[clientIndex].key);
    }
    transport->Run();

    // By now, they all should have executed the last request.
    Notice("config->n = %d", config->n);
    // EXPECT_EQ(clientOps.size(), 2);
    // copy log logic once gap logic is fixed, then check logs through op

    // for (int replicaIdx = 0; replicaIdx < config->n; replicaIdx++) 
    // {
    //     EXPECT_EQ(ops[replicaIdx].size(), 2);
    // }

    // for (int logIdx = 0; logIdx < clientOps.size(); logIdx++)
    // {
    //     for (int replicaIdx = 0; replicaIdx < config->n; replicaIdx++) 
    //     {
    //         EXPECT_EQ(clientOps[logIdx], ops[replicaIdx][logIdx]);
    //     }
    // }
}

INSTANTIATE_TEST_CASE_P(test,
                        CRAQTest,
                        ::testing::Values(
                            CRAQTestParam{1, 1, 3, 0},
                            CRAQTestParam{1, 10, 3, 0}
                        ));
