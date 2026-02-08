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

    void ReplicaUpcall(opnum_t opnum, const string &req, string &reply) {
        ops->push_back(req);
        reply = "reply: " + req;
    }

    void UnloggedUpcall(const string &req, string &reply) {
        unloggedOps->push_back(req);
        reply = "unlreply: " + req;
    }
};

class CRAQTest : public  ::testing::TestWithParam<int>
{
protected:
    std::vector<CRAQReplica *> replicas;
    CRAQClient *client;
    SimulatedTransport *transport;
    transport::Configuration *config;
    std::vector<std::vector<string> > ops;
    std::vector<std::vector<string> > unloggedOps;
    int requestNum;
    int group = 0;

    virtual void SetUp() {
        int clientid = 0;
        int groups = 3; 
        int replicasPerGroup = 3;
        int faultTolerance = 1;
        // TODO: make this configurable
        int keys = 5;

        std::map<int, std::vector<transport::ReplicaAddress>> replicaAddrs = 
        {
            {
                0,
                std::vector<transport::ReplicaAddress>{
                    {"localhost", "12345"},
                    {"localhost", "12346"},
                    {"localhost", "12347"}
                }
            },
            {
                1,
                std::vector<transport::ReplicaAddress>{
                    {"localhost", "12348"},
                    {"localhost", "12349"},
                    {"localhost", "12350"}
                }
            },
            {
                2,
                std::vector<transport::ReplicaAddress>{
                    {"localhost", "12351"},
                    {"localhost", "12352"},
                    {"localhost", "12353"}
                }
            }
        };

        config = new transport::Configuration(groups, replicasPerGroup, faultTolerance, replicaAddrs);

        transport = new SimulatedTransport();

        ops.resize(config->n);
        unloggedOps.resize(config->n);

        // TODO: make this multiple clients
        client = new CRAQClient(*config, transport, group, clientid);
        requestNum = -1; 

        for (int i = 0; i < config->n; i++) {
            replicas.push_back(new CRAQReplica(*config, group, i, transport, GetParam(), new CRAQApp(&ops[i], &unloggedOps[i]), true));
            // TODO: preload
            // for (int j = 0; j < keys; j++)
            // {
            //     client->Invoke()
            // }
        }

        // Only let tests run for a simulated minute. This prevents
        // infinite retry loops, etc.
//        transport->Timer(60000, [&]() {
//                transport->CancelAllTimers();
//            });
    }

    virtual string RequestOp(int n) {
        std::ostringstream stream;
        stream << "test: " << n;
        return stream.str();
    }

    virtual string LastRequestOp() {
        return RequestOp(requestNum);
    }

    virtual void ClientSendNext(Client::continuation_t upcall, std::string op, std::string key) {
        string request_str;

        LinearizeableOperation linop;
        // only one client rn
        linop.mutable_rid()->set_client_id(requestNum);
        linop.mutable_rid()->set_client_req_id(requestNum);
        linop.set_transaction_id(requestNum);
        linop.set_transaction_id(requestNum);
        linop.set_key(key);
        linop.set_value(key);
        linop.set_op(op);

        linop.SerializeToString(&request_str);

        client->Invoke(request_str, upcall);
    }

    // void ExecuteNextAppRequestOperation()
    // {
    //     int session_id = 0;
    //     // Generic Operation Callback
    //     auto ocb = std::bind(&BenchmarkClient::ReceiveOperationResponse, this, session_id, std::placeholders::_1, std::placeholders::_2);
    //     auto otcb = std::bind(&BenchmarkClient::SendOperationTimeout, this, session_id, std::placeholders::_1, std::placeholders::_2);

    //     auto client_index = ss.current_client_index();
    //     auto &client = *clients_[client_index];

    //     Debug("opindex == %lu and ss.fanout() == %lu", op_index, ss.fanout());
    //     if (op_index == ss.fanout())
    //     {
    //         Debug("we've sent fanout number of requests, no longer sending more");
    //         return;
    //     }

    //     LinearizeableOperation op = appreq->GetNextOperation(op_index);
    //     ss.incr_op_index();
    //     std::string op_str = "GET";

    //     client.SendOperation(session, op_str, op.key, op.value, ocb, otcb, timeout_);

    //     if (issueConcurrent)
    //     {
    //         Debug("we're about to issue the next operation within this app request without having gotten a response!!!");
    //         // TODO ANJA should these just be added to the event queue?? or actually issued next
    //         ExecuteNextAppRequestOperation(session_id);
    //     } else {
    //         Debug("Not issueing next op from this fn");
    //     }
    // }

    virtual void TearDown() {
        for (auto x : replicas) {
            delete x;
        }

        replicas.clear();
        ops.clear();
        unloggedOps.clear();

        delete client;
        delete transport;
        delete config;
    }
};

TEST_P(CRAQTest, OneOp)
{
    auto upcall = [this](const string &req, const string &reply) {
        // EXPECT_EQ(req, LastRequestOp());
        // EXPECT_EQ(reply, "reply: "+LastRequestOp());

        // // Not guaranteed that any replicas except the leader have
        // // executed this request.
        // EXPECT_EQ(ops[0].back(), req);
        // transport->CancelAllTimers();
        return true;
    };

    ClientSendNext(upcall, "get", "1");
    transport->Run();

    // By now, they all should have executed the last request.
    for (int i = 0; i < config->n; i++) {
        EXPECT_EQ(ops[i].size(), 1);
        EXPECT_EQ(ops[i].back(),  LastRequestOp());
    }
}

INSTANTIATE_TEST_CASE_P(Batching,
                        CRAQTest,
                        ::testing::Values(1, 8));
