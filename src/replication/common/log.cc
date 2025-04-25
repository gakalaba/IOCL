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

#include "replication/common/log.h"
#include "replication/common/request.pb.h"
#include "lib/assert.h"

#include <openssl/sha.h>

namespace replication
{

    const string Log::EMPTY_HASH = string(SHA_DIGEST_LENGTH, '\0');

    Log::Log(bool useHash, opnum_t start, string initialHash)
        : useHash(useHash)
    {
        this->initialHash = initialHash;
        this->start = start;
        firstUncommittedEntry = NULL;
        if (start == 1)
        {
            ASSERT(initialHash == EMPTY_HASH);
        }
    }

    /**************** Old Log ****************/
    LogEntry &
    Log::Append(viewstamp_t vs, const Request &req, LogEntryState state)
    {
        if (entries.empty())
        {
            ASSERT(vs.opnum == start);
        }
        else
        {
            ASSERT(vs.opnum == LastOpnum() + 1);
        }

        LogEntry entry;
        entry.viewstamp = vs;
        entry.request = req;
        entry.state = state;
        if (useHash)
        {
            entry.hash = ComputeHash(LastHash(), entry);
        }

        entries.push_back(entry);
        return *Find(vs.opnum);
    }

    // This really ought to be const
    LogEntry *
    Log::Find(opnum_t opnum)
    {
        if (entries.empty())
        {
            return NULL;
        }

        if (opnum < start)
        {
            return NULL;
        }

        if (opnum - start > entries.size() - 1)
        {
            return NULL;
        }

        LogEntry *entry = &entries[opnum - start];
        ASSERT(entry->viewstamp.opnum == opnum);
        return entry;
    }

    /************** Unordered Log ****************/
    LogEntry &
    Log::AppendUnsorted(const Request &req, uint64_t shardTag, LogEntryState state,
                        uint64_t arrivalTs,
                        std::vector<Successor *> &&successors,
                        std::vector<Predecessor *> &&predecessors,
                        uint64_t acks, uint64_t acks2)
    {
        LogEntry entry;
        entry.request = req;
        entry.state = state;
        entry.arrivalTimestamp = arrivalTs;
        entry.myShardTag = shardTag;
        if (!successors.empty())
        {
            entry.successors = std::move(successors);
        }

        if (!predecessors.empty())
        {
            entry.predecessors = std::move(predecessors);
        }

        if (useHash)
        {
            entry.hash = ComputeHash(LastHash(), entry);
        }

        entry.acks = acks;
        entry.acks2 = acks2;

        unorderedEntries[shardTag] = entry;
        return *FindUnsorted(shardTag);
    }

    LogEntry *Log::FindUnsorted(uint64_t shardTag)
    {
        if (unorderedEntries.find(shardTag) != unorderedEntries.end())
        {
            return &(unorderedEntries[shardTag]);
        }
        else
        {
            return NULL;
        }
    }

    /************** Sorted Log ***************/
    LogEntry &
    Log::AppendSorted(LogEntryState state, uint64_t shardTag, uint64_t sortedTs)
    {
        ASSERT(unorderedEntries.find(shardTag) != unorderedEntries.end());
        LogEntry &entry = unorderedEntries[shardTag];

        entry.state = state;
        entry.sortTimestamp = sortedTs;
        sortedLog.insert(sortedTs, shardTag);
        return *FindUnsorted(shardTag);
    }

    LogEntry *
    Log::FindSorted(uint64_t shardTag)
    {
        LogEntry *ep = FindUnsorted(shardTag);
        if (ep == NULL)
        {
            Debug("not in sorted or unsorted");
            return NULL;
        }
        if (sortedLog.isIn(ep->sortTimestamp, ep->myShardTag))
        {
            LogEntry *retval = FindUnsorted(shardTag);
            ASSERT(retval != NULL);
            return retval;
        }
        else
        {
            Debug("didn't find it in the sorted log!");
            return NULL;
        }
    }

    bool Log::InSorted(uint64_t shardTag)
    {
        Debug("looking inside sorted log for tag %d", shardTag);
        LogEntry *ep = FindUnsorted(shardTag);
        if (ep == NULL)
        {
            Debug("not in sorted or unsorted");
            return false;
        }
        return sortedLog.isIn(ep->sortTimestamp, ep->myShardTag);
    }

    void Log::ResortSorted(viewstamp_t vs, LogEntryState state, uint64_t shardTag, uint64_t finalSortedTs)
    {
        // Assert it's in UNsorted
        ASSERT(unorderedEntries.find(shardTag) != unorderedEntries.end());
        // Remove this tag from the sorted log
        LogEntry &entry = unorderedEntries[shardTag];
        entry.viewstamp = vs;
        entry.state = state;
        Debug("Old version is <sortedtimestamp = %d, shardTag = %d>", entry.sortTimestamp, shardTag);
        sortedLog.deleteElem(entry.sortTimestamp, shardTag);
        Debug("assigning new timestamp = %d", finalSortedTs);
        entry.sortTimestamp = finalSortedTs;
        Debug("Calling ResortSorted with <sortedTimestamp=%d, shardTag = %d>", finalSortedTs, shardTag);
        sortedLog.insert(finalSortedTs, shardTag); // TODO Anja: should this be insert or insertWithSameOrder??
        PrintSortedLog();
        Debug("and let's just see if we can find it!");
        sortedLog.isIn(finalSortedTs, shardTag);
        return;
    }

    bool Commute(LogEntry *a, LogEntry *b)
    {
        // TODO Anja
        return true;
    }

    // We know the sorted log has length >= 1 at this point
    // TODO Anja pass in commute function when replicas are instantiated
    int Log::MoveSortedToLog(uint64_t shardTag)
    {
        LogEntry *ep = FindUnsorted(shardTag);
        LogEntry *maybehead;
        ASSERT(ep != NULL);
        ASSERT(ep->state == LOG_STATE_READY || ep->state == LOG_STATE_FASTPATH);
        bool sawself = false;
        int found = 0;

        auto it = sortedLog.begin();
        while (it != sortedLog.end())
        {
            // save iterator!!
            auto nextIt = std::next(it);

            maybehead = FindUnsorted(std::get<1>(*it));
            if (!sawself && (maybehead->sortTimestamp == ep->sortTimestamp && maybehead->myShardTag == shardTag))
            {
                sawself = true; // i am the head of the log, there is a contiguous run of nonzero size
                found++;
                // delete self from sorted log and add to final log
                sortedLog.deleteElem(ep->sortTimestamp, ep->myShardTag);
                entries.push_back(*ep);
                ASSERT(found == 1);
            }
            else if (!sawself && !Commute(ep, maybehead))
            {
                // Found an entry earlier in the log that hasn't been made ready yet that I don't commute with... I must wait
                ASSERT(maybehead->state == LOG_STATE_ASSIGNED);
                ASSERT(found == 0);
                return 0;
            }
            else if (sawself && !Commute(ep, maybehead))
            {
                // maybehead == nothead
                ASSERT(maybehead->sortTimestamp >= ep->sortTimestamp);
                if (maybehead->state == LOG_STATE_READY || maybehead->state == LOG_STATE_FASTPATH)
                {
                    found++;
                    sortedLog.deleteElem(maybehead->sortTimestamp, maybehead->myShardTag);
                    entries.push_back(*maybehead);
                }
                else
                {
                    return found;
                }
            }

            // move to next iterator
            it = nextIt;
        }
        Panic("what happened???");
    }

    void Log::PrintSortedLog()
    {
        auto printLogEntry = [&](uint64_t shardtag, int i)
        {
            LogEntry *ep = FindUnsorted(shardtag);
            Debug("         SortedLog[%d] = LogEntry{tag=%d, arrivalts = %d, sortedts = %d, %s}", i, ep->myShardTag, ep->arrivalTimestamp, ep->sortTimestamp, PrintState(ep->state).c_str());
        };
        sortedLog.print(printLogEntry);
    }

    std::string Log::PrintState(LogEntryState logstate)
    {
        switch (logstate)
        {
        case LOG_STATE_SPECULATIVE:
            return "state = LOG_STATE_SPECULATIVE";
        case LOG_STATE_FASTPREPARED:
            return "state = LOG_STATE_FASTPREPARED";
        case LOG_STATE_ARRIVED:
            return "state = LOG_STATE_ARRIVED";
        case LOG_STATE_FASTPATH:
            return "state = LOG_STATE_FASTPATH";
        case LOG_STATE_PREPARED:
            return "state = LOG_STATE_PREPARED";
        case LOG_STATE_ASSIGNED:
            return "state = LOG_STATE_ASSIGNED";
        case LOG_STATE_READY:
            return "state = LOG_STATE_READY";
        case LOG_STATE_COMMITTED:
            return "state = LOG_STATE_COMMITTED";
        }
    }

    bool
    Log::SetStatus(opnum_t op, LogEntryState state)
    {
        LogEntry *entry = Find(op);
        if (entry == NULL)
        {
            return false;
        }

        entry->state = state;
        return true;
    }

    void Log::SetStatus(LogEntry &entry, LogEntryState state)
    {
        entry.state = state;
    }

    bool
    Log::SetRequest(opnum_t op, const Request &req)
    {
        if (useHash)
        {
            Panic("Log::SetRequest on hashed log not supported.");
        }

        LogEntry *entry = Find(op);
        if (entry == NULL)
        {
            return false;
        }

        entry->request = req;
        return true;
    }

    void
    Log::RemoveAfter(opnum_t op)
    {
#if PARANOID
        // We'd better not be removing any committed entries.
        for (opnum_t i = op; i <= LastOpnum(); i++)
        {
            ASSERT(Find(i)->state != LOG_STATE_COMMITTED);
        }
#endif

        if (op > LastOpnum())
        {
            return;
        }

        Debug("Removing log entries after " FMT_OPNUM, op);

        ASSERT(op - start < entries.size());
        entries.resize(op - start);

        ASSERT(LastOpnum() == op - 1);
    }

    LogEntry *
    Log::Last()
    {
        if (entries.empty())
        {
            return NULL;
        }

        return &entries.back();
    }

    viewstamp_t
    Log::LastViewstamp() const
    {
        if (entries.empty())
        {
            return viewstamp_t(0, start - 1);
        }
        else
        {
            return entries.back().viewstamp;
        }
    }

    opnum_t
    Log::LastOpnum() const
    {
        if (entries.empty())
        {
            return start - 1;
        }
        else
        {
            return entries.back().viewstamp.opnum;
        }
    }

    opnum_t
    Log::FirstOpnum() const
    {
        // XXX Not really sure what's appropriate to return here if the
        // log is empty
        return start;
    }

    bool
    Log::Empty() const
    {
        return entries.empty();
    }

    const string &
    Log::LastHash() const
    {
        if (entries.empty())
        {
            return initialHash;
        }
        else
        {
            return entries.back().hash;
        }
    }

    string
    Log::ComputeHash(string lastHash, const LogEntry &entry)
    {
        SHA_CTX ctx;
        unsigned char out[SHA_DIGEST_LENGTH];

        SHA1_Init(&ctx);

        SHA1_Update(&ctx, lastHash.c_str(), lastHash.size());
        SHA1_Update(&ctx, &entry.viewstamp, sizeof(entry.viewstamp));
        uint64_t x;
        x = entry.request.clientid();
        SHA1_Update(&ctx, &x, sizeof(x));
        x = entry.request.clientreqid();
        SHA1_Update(&ctx, &x, sizeof(x));
        SHA1_Update(&ctx, entry.request.op().c_str(),
                    entry.request.op().size());

        SHA1_Final(out, &ctx);

        return string((char *)out, SHA_DIGEST_LENGTH);
    }

    // IOCL specifics

} // namespace replication
