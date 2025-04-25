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

#ifndef _COMMON_IOCL_UTILS_H_
#define _COMMON_IOCL_UTILS_H_

#include "lib/message.h"

namespace replication
{
    uint64_t IntToPid(uint64_t tag)
    {
        uint64_t pid = (tag >> 32) & 0xFFFFFFFF;
        return pid;
    }

    uint64_t IntToSeqno(uint64_t tag)
    {
        uint64_t seqno = (tag & 0xFFFFFFFF);
        return seqno;
    }

    uint64_t CreateTag(uint64_t pid, uint64_t seqno)
    {
        return (pid << 32) | (seqno & 0xFFFFFFFF);
    }

    // Custom comparator to compare tuples by the first element and break ties by insertion order
    struct CompareByFirstAndInsertionOrder
    {
        bool operator()(const std::tuple<int, int, size_t> &a, const std::tuple<int, int, size_t> &b) const
        {
            // First compare by shardTag (first element of tuple)
            if (std::get<0>(a) != std::get<0>(b))
            {
                return std::get<0>(a) < std::get<0>(b); // Compare by first element
            }
            // If shardTag is the same, compare by the insertion order (third element)
            return std::get<2>(a) < std::get<2>(b); // Compare by insertion order
        }
    };

    class IOCLog
    {
    public:
        IOCLog() : insertion_counter(0) {}

        // Insert a tuple with a unique second element
        void insert(uint64_t sortedTimestamp, uint64_t shardTag)
        {
            std::tuple<uint64_t, uint64_t, size_t> entry = std::make_tuple(sortedTimestamp, shardTag, insertion_counter++);
            sortedLog.insert(entry);
        }

        // Find method: first search by first element, then by second element
        bool isIn(uint64_t sortedTimestamp, uint64_t shardTag)
        {
            auto it = sortedLog.lower_bound(std::make_tuple(sortedTimestamp, 0, 0)); // Find range for the first element
            while (it != sortedLog.end() && std::get<0>(*it) == sortedTimestamp)
            {
                if (std::get<1>(*it) == shardTag)
                {
                    Debug("found it!");
                    return true; // Found the second element in the same shardTag range
                }
                ++it;
            }
            Debug("didn't find it!");
            return false; // Not found
        }

        // Custom delete method: delete by first and second element (O(log n + k))
        void deleteElem(uint64_t sortedTimestamp, uint64_t shardTag)
        {
            auto it = sortedLog.lower_bound(std::make_tuple(sortedTimestamp, 0, 0)); // Find range for the first element
            while (it != sortedLog.end() && std::get<0>(*it) == sortedTimestamp)
            {
                if (std::get<1>(*it) == shardTag)
                {
                    // Delete the matching element
                    sortedLog.erase(it);
                    Debug("Deleted elem <sortedTimestamp = %d, shardTag = %d>", std::get<0>(*it), std::get<1>(*it));
                    return; // Element deleted, exit function
                }
                ++it;
            }
            Debug("Element <sortedTimestamp = %d, shardTag = %d> not found", sortedTimestamp, shardTag);
        }

        // Print the set for debugging
        void print(std::function<void(uint64_t, int)> logPrinter) const
        {
            int i = 0;
            for (const auto &entry : sortedLog)
            {
                Debug("SortedLog[%d]: <sortedTimestamp = %d, shardTag = %d, insertionOrder = %d>", i, std::get<0>(entry), std::get<1>(entry), std::get<2>(entry));
                logPrinter(std::get<1>(entry), i);
                i++;
            }
        }

        auto begin() const
        {
            return sortedLog.begin();
        }

        // Method to get an iterator to the end of the sorted log
        auto end() const
        {
            return sortedLog.end();
        }

    private:
        std::set<std::tuple<uint64_t, uint64_t, size_t>, CompareByFirstAndInsertionOrder> sortedLog;
        size_t insertion_counter; // Counter to track insertion order
    };

} // namespace replication

#endif /* _COMMON_IOCL_UTILS_H_ */
