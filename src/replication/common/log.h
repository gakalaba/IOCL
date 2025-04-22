// -*- mode: c++; c-file-style: "k&r"; c-basic-offset: 4 -*-
/***********************************************************************
 *
 * log.h:
 *   a replica's log of pending and committed operations
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

#ifndef _COMMON_LOG_H_
#define _COMMON_LOG_H_

#include <google/protobuf/message.h>

#include <map>

#include "lib/assert.h"
#include "lib/message.h"
#include "lib/transport.h"
#include "lib/viewstamp.h"
#include "replication/common/request.pb.h"

namespace replication
{

    enum LogEntryState
    {
        // LOG_STATE_COMMITTED,
        // LOG_STATE_PREPARED,
        LOG_STATE_SPECULATIVE,  // specpaxos only
        LOG_STATE_FASTPREPARED, // fastpaxos only
        /* IOCL specifics */
        LOG_STATE_ARRIVED,
        LOG_STATE_PREPARED,
        LOG_STATE_ASSIGNED,
        LOG_STATE_READY, // all acks have arrived
        LOG_STATE_COMMITTED
    };

    struct Predecessor
    {
        uint64_t perShardTag;
        uint64_t shardId;
        int64_t arrivalTimestamp;
        int64_t sortedTimestamp;
    };

    struct Successor
    {
        uint64_t perShardTag;
        uint64_t shardId;
        uint64_t predIdx;
    };

    struct LogEntry
    {
        viewstamp_t viewstamp;
        LogEntryState state;
        Request request;
        string hash;
        // Speculative client table stuff
        opnum_t prevClientReqOpnum;
        ::google::protobuf::Message *replyMessage;
        // IOCL specifics
        uint64_t arrivalTimestamp;
        uint64_t sortTimestamp;
        uint64_t myShardTag;
        std::vector<Predecessor *> predecessors;
        std::vector<Successor *> successors;
        uint64_t acks;
        uint64_t acks2;

        LogEntry() { replyMessage = NULL; }
        LogEntry(const LogEntry &x)
            : viewstamp(x.viewstamp), state(x.state), request(x.request), hash(x.hash), prevClientReqOpnum(x.prevClientReqOpnum)
        {
            if (x.replyMessage)
            {
                replyMessage = x.replyMessage->New();
                replyMessage->CopyFrom(*x.replyMessage);
            }
            else
            {
                replyMessage = NULL;
            }
        }
        LogEntry(viewstamp_t viewstamp, LogEntryState state,
                 const Request &request, const string &hash)
            : viewstamp(viewstamp), state(state), request(request), hash(hash), replyMessage(NULL) {}
        virtual ~LogEntry()
        {
            if (replyMessage)
            {
                delete replyMessage;
            }
        }
    };

    class Log
    {
    public:
        Log(bool useHash, opnum_t start = 1, string initialHash = EMPTY_HASH);
        LogEntry &Append(viewstamp_t vs, const Request &req, LogEntryState state);
        LogEntry *Find(opnum_t opnum);
        // LogEntry *Find(uint64_t opnum);
        // IOCL specifics
        LogEntry &AppendUnsorted(const Request &req, uint64_t shardTag,
                                 LogEntryState state,
                                 uint64_t arrivalTs,
                                 std::vector<Successor *> &&successors,
                                 std::vector<Predecessor *> &&predecessors,
                                 uint64_t acks, uint64_t acks2);
        LogEntry *FindUnsorted(uint64_t shardTag);
        LogEntry &AppendSorted(LogEntryState state,
                               uint64_t shardTag, uint64_t sortedTs);
        LogEntry *FindSorted(uint64_t shardTag);
        bool IsAtHead(void *it);
        bool InSorted(uint64_t shardTag);
        void *ResortSorted(viewstamp_t vs, LogEntryState state,
                           uint64_t shardTag, uint64_t finalSortedTs);

        bool SetStatus(opnum_t opnum, LogEntryState state);
        void SetStatus(LogEntry &entry, LogEntryState state);
        bool SetRequest(opnum_t op, const Request &req);
        void RemoveAfter(opnum_t opnum);
        LogEntry *Last();
        viewstamp_t LastViewstamp() const; // deprecated
        opnum_t LastOpnum() const;
        opnum_t FirstOpnum() const;
        bool Empty() const;
        template <class T>
        void Dump(opnum_t from, T out);
        template <class iter>
        void Install(iter start, iter end);
        const string &LastHash() const;

        static string ComputeHash(string lastHash, const LogEntry &entry);
        static const string EMPTY_HASH;

    private:
        std::vector<LogEntry> entries;
        string initialHash;
        opnum_t start;
        bool useHash;
        // IOCL specifics
        // .find(), .end(), .insert(), .erase()
        struct CompareBySecond
        {
            bool operator()(const std::tuple<uint64_t, uint64_t> &a, const std::tuple<uint64_t, uint64_t> &b) const
            {
                Debug("hi! we're comparing by the sortedTimestamps wahoo");
                return std::get<1>(a) < std::get<1>(b);
            }
        };

        bool deleteByFirst(std::set<std::tuple<uint64_t, uint64_t>, CompareBySecond> &s, uint64_t firstElement)
        {
            // Create a tuple with the first element you want to search for and a placeholder for the second
            std::tuple<uint64_t, uint64_t> keyToFind = {firstElement, 0}; // The second element doesn't matter

            // Use lower_bound to find the first element greater than or equal to the key
            auto it = s.lower_bound(keyToFind);

            // Check if the iterator points to a valid element and if the first element matches
            if (it != s.end() && std::get<0>(*it) == firstElement)
            {
                s.erase(it); // Erase the element from the set
                return true; // Successfully erased
            }

            return false; // Element not found
        };

        std::set<std::tuple<uint64_t, uint64_t>, CompareBySecond>::iterator findByFirst(
            std::set<std::tuple<uint64_t, uint64_t>, CompareBySecond> &s, uint64_t firstElement)
        {

            // Create a tuple with the first element you want to search for and a placeholder for the second
            std::tuple<uint64_t, uint64_t> keyToFind = {firstElement, 0}; // The second element doesn't matter

            // Use lower_bound to find the first element greater than or equal to the key
            auto it = s.lower_bound(keyToFind);

            // Check if the iterator points to a valid element and if the first element matches
            if (it != s.end() && std::get<0>(*it) == firstElement)
            {
                return it; // Found the element
            }

            return s.end(); // Not found
        };

        std::set<std::tuple<uint64_t, uint64_t>, CompareBySecond> sortedLog; // tuple<tag, sortedTs>
        std::unordered_map<uint64_t, LogEntry> unorderedEntries;
    };

#include "replication/common/log-impl.h"

} // namespace replication

#endif /* _COMMON_LOG_H_ */
