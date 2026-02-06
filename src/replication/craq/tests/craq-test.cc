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

        for (int i = 0; i < config->n; i++) {
            replicas.push_back(new CRAQReplica(*config, group, i, transport, GetParam(), new CRAQApp(&ops[i], &unloggedOps[i]), true));
        }

        client = new CRAQClient(*config, transport, group, clientid);
        requestNum = -1;

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

    virtual void ClientSendNext(Client::continuation_t upcall) {
        requestNum++;
        client->Invoke(LastRequestOp(), upcall);
    }

    virtual void ClientSendNextUnlogged(int idx, Client::continuation_t upcall,
                                        Client::error_continuation_t error_continuation = nullptr,
                                        uint32_t timeout = Client::DEFAULT_UNLOGGED_OP_TIMEOUT) {
        requestNum++;
        client->InvokeUnlogged(idx, LastRequestOp(), upcall, error_continuation, timeout);
    }

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
        EXPECT_EQ(req, LastRequestOp());
        EXPECT_EQ(reply, "reply: "+LastRequestOp());

        // Not guaranteed that any replicas except the leader have
        // executed this request.
        EXPECT_EQ(ops[0].back(), req);
        transport->CancelAllTimers();
        return true;
    };

    ClientSendNext(upcall);
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
