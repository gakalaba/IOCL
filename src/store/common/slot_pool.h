// -*- mode: c++; c-file-style: "k&r"; c-basic-offset: 4 -*-
/***********************************************************************
 *
 * store/common/backend/slot_pool.h:
 *   slot-pool allocator
 *
 * Copyright 2026 Anja Kalaba
 * Copyright 2022 Jeffrey Helt, Matthew Burke, Amit Levy, Wyatt Lloyd
 * Copyright 2015 Irene Zhang <iyzhang@cs.washington.edu>
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

#ifndef _SLOT_POOL_H_
#define _SLOT_POOL_H_

#include <unordered_map>
#include <vector>

#include "lib/assert.h"

template <typename Slot, typename Key = uint64_t>
class SlotPool {
public:
    explicit SlotPool(size_t n) : slots_(n) {
        free_.reserve(n);
        for (uint32_t i = 0; i < n; ++i) {
            free_.push_back(n - 1 - i);
        }
    }

    uint32_t Alloc(Key key) {
        ASSERT(!free_.empty());
        uint32_t idx = free_.back();
        free_.pop_back();
        ASSERT(key_to_idx_.find(key) == key_to_idx_.end());
        key_to_idx_.emplace(key, idx);
        slots_[idx].in_use = true;
        return idx;
    }

    uint32_t Alloc() {
        ASSERT(!free_.empty());
        uint32_t idx = free_.back();
        free_.pop_back();
        ASSERT(!slots_[idx].in_use);
        slots_[idx].in_use = true;
        return idx;
    }

    uint32_t AllocIfNotPresent(Key key) {
        auto it = key_to_idx_.find(key);
        if (it != key_to_idx_.end()) {
            return it->second;
        } else {
            return Alloc(key);
        }
    }

    Slot &GetByKey(Key key) {
        auto it = key_to_idx_.find(key);
        ASSERT(it != key_to_idx_.end());
        return slots_[it->second];
    }

    Slot *GetByKeyIfPresent(Key key) {
        auto it = key_to_idx_.find(key);
        if (it != key_to_idx_.end()) {
            return &slots_[it->second];
        } else {
            return nullptr;
        }
    }

    bool ContainsKey(Key key) const {
        return key_to_idx_.find(key) != key_to_idx_.end();
    }

    Slot &GetByIdx(uint32_t idx) {
        ASSERT(idx < slots_.size());
        return slots_[idx];
    }

    uint32_t GetIdx(Key key) const {
        auto it = key_to_idx_.find(key);
        ASSERT(it != key_to_idx_.end());
        return it->second;
    }

    void FreeByKey(Key key) {
        auto it = key_to_idx_.find(key);
        ASSERT(it != key_to_idx_.end());
        uint32_t idx = it->second;
        key_to_idx_.erase(it);
        slots_[idx].in_use = false;
        free_.push_back(idx);
    }

    void FreeByIdx(uint32_t idx) {
        ASSERT(idx < slots_.size());
        slots_[idx].in_use = false;
        free_.push_back(idx);
    }

    size_t Size() const {
        return slots_.size();
    }

private:
    std::vector<Slot> slots_;
    std::vector<uint32_t> free_;
    std::unordered_map<Key, uint32_t> key_to_idx_;
};

#endif /* _SLOT_POOL_H_ */
