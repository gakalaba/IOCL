// -*- mode: c++; c-file-style: "k&r"; c-basic-offset: 4 -*-
/***********************************************************************
 *
 * common/transaction.cc
 *   A transaction implementation.
 *
 **********************************************************************/

#include "store/common/transaction.h"

using namespace std;

Transaction::Transaction() : readSet{}, writeSet{}, start_time_{} {}

Transaction::Transaction(const TransactionMessage &msg)
    : start_time_{msg.starttime()} {
    readSet.reserve(msg.readset_size());
    writeSet.reserve(msg.writeset_size());
    for (int i = 0; i < msg.readset_size(); i++) {
        const auto &readMsg = msg.readset(i);
        readSet.emplace_back(readMsg.key(), Timestamp(readMsg.readtime()));
    }

    for (int i = 0; i < msg.writeset_size(); i++) {
        const auto &writeMsg = msg.writeset(i);
        writeSet.emplace_back(writeMsg.key(), writeMsg.value());
    }
}

Transaction::~Transaction() {}

uint64_t Transaction::transaction_id() {
    return transaction_id_;
}

const std::vector<std::pair<std::string, Timestamp>> &Transaction::getReadSet() const {
    return readSet;
}

const std::vector<std::pair<std::string, std::string>> &Transaction::getWriteSet() const {
    return writeSet;
}

std::vector<std::pair<std::string, std::string>> &Transaction::getWriteSet() {
    return writeSet;
}

const Timestamp &Transaction::start_time() const { return start_time_; }

void Transaction::clear() {
    readSet.clear();
    writeSet.clear();
}

void Transaction::set_start_time(const Timestamp &ts) { start_time_ = ts; }

void Transaction::addReadSet(const string &key, const Timestamp &readTime) {
    readSet.emplace_back(key, readTime);
}

void Transaction::addWriteSet(const string &key, const string &value) {
    writeSet.emplace_back(key, value);
}

void Transaction::set_transaction_id(uint64_t transaction_id) {
    transaction_id_ = transaction_id;
}

void Transaction::add_read_write_sets(const Transaction &other) {
    readSet.insert(readSet.end(), other.readSet.begin(), other.readSet.end());
    writeSet.insert(writeSet.end(), other.writeSet.begin(), other.writeSet.end());
}

void Transaction::serialize(TransactionMessage *msg) const {
    start_time_.serialize(msg->mutable_starttime());
    msg->mutable_readset()->Reserve(readSet.size());
    msg->mutable_writeset()->Reserve(writeSet.size());
    for (const auto &read : readSet) {
        ReadMessage *readMsg = msg->add_readset();
        readMsg->set_key(read.first);
        read.second.serialize(readMsg->mutable_readtime());
    }

    for (const auto &write : writeSet) {
        WriteMessage *writeMsg = msg->add_writeset();
        writeMsg->set_key(write.first);
        writeMsg->set_value(write.second);
    }
}
