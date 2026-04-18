// -*- mode: c++; c-file-style: "k&r"; c-basic-offset: 4 -*-
/***********************************************************************
 *
 * quorumset.h:
 *   utility type for tracking sets of messages received from other
 *   replicas and determining whether a quorum of responses has been met
 *
 * Copyright 2013-2015 Irene Zhang <iyzhang@cs.washington.edu>
 *                     Naveen Kr. Sharma <naveenks@cs.washington.edu>
 *                     Dan R. K. Ports  <drkp@cs.washington.edu>
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

#ifndef _COMMON_QUORUMSET_H_
#define _COMMON_QUORUMSET_H_

namespace replication {

// Hash for viewstamp_t
struct ViewstampHash
{
    std::size_t operator()(const viewstamp_t &vs) const noexcept
    {
        // Simple 64-bit mix of view and opnum.
        // Good enough for this use.
        uint64_t x = static_cast<uint64_t>(vs.view);
        uint64_t y = static_cast<uint64_t>(vs.opnum);

        uint64_t h = x;
        h ^= y + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
        return static_cast<std::size_t>(h);
    }
};

struct ViewstampEq
{
    bool operator()(const viewstamp_t &a, const viewstamp_t &b) const noexcept
    {
        return a.view == b.view && a.opnum == b.opnum;
    }
};

template <class IDTYPE, class HASH = std::hash<IDTYPE>, class EQ = std::equal_to<IDTYPE>>
class QuorumSet
{
public:
    QuorumSet(int numRequired, int numReplicas)
        : numRequired(numRequired),
          numReplicas(numReplicas)
    {
        ASSERT(numReplicas > 0);
        ASSERT(numRequired > 0);
        ASSERT(numReplicas <= 64);
    }

    void
    Clear()
    {
        states.clear();
    }

    void
    Clear(const IDTYPE &id)
    {
       states.erase(id);
    }

    int
    NumRequired() const
    {
        return numRequired;
    }

    bool
    CheckForQuorum(const IDTYPE &id) const
    {
        auto it = states.find(id);
        if (it == states.end()) {
            return false;
        }
        return it->second.count >= numRequired;
    }

    bool
    AddAndCheckForQuorum(IDTYPE id, int replicaIdx)
    {
        ASSERT(replicaIdx >= 0);
        ASSERT(replicaIdx < numReplicas);

        State &s = states[id];
        const uint64_t bit = 1ULL << replicaIdx;

        if ((s.seen_mask & bit) == 0) {
            s.seen_mask |= bit;
            s.count++;
        }

        return s.count >= numRequired;
    }

    void
    Add(const IDTYPE &id, int replicaIdx)
    {
        (void)AddAndCheckForQuorum(id, replicaIdx);
    }

    int Count(const IDTYPE &id) const
    {
        auto it = states.find(id);
        if (it == states.end()) {
            return 0;
        }
        return it->second.count;
    }

public:
    int numRequired;
private:
    struct State {
        uint8_t count = 0;
        uint64_t seen_mask = 0;
    };
    int numReplicas;
    std::unordered_map<IDTYPE, State, HASH, EQ> states;
};

}      // namespace replication

#endif  // _COMMON_QUORUMSET_H_
