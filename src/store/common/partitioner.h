/***********************************************************************
 *
 * store/common/partitioner.h:
 *
 * Copyright 2022 Jeffrey Helt, Matthew Burke, Amit Levy, Wyatt Lloyd
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
#ifndef PARTITIONER_H
#define PARTITIONER_H

#include <functional>
#include <random>
#include <unordered_set>
#include <string>
#include <vector>
#include <unordered_map>

enum partitioner_t
{
    DEFAULT = 0,
    WAREHOUSE_DIST_ITEMS,
    WAREHOUSE,
    LOAD_BALANCED,
};

class Partitioner
{
public:
    Partitioner() {}
    virtual ~Partitioner() {}
    virtual uint64_t operator()(const std::string &key, uint64_t numShards,
                                int group, const std::vector<int> &txnGroups) = 0;
    uint64_t operator()(const std::string &key, uint64_t numShards,
                        int group, const std::unordered_set<int> &txnGroups);
};

class DefaultPartitioner : public Partitioner
{
public:
    DefaultPartitioner() {}
    virtual ~DefaultPartitioner() {}

    virtual uint64_t operator()(const std::string &key, uint64_t numShards,
                                int group, const std::vector<int> &txnGroups);
};

class LoadBalancedPartitioner : public Partitioner
{
public:
    LoadBalancedPartitioner(double zipf_coef, int num_shards) :
        M(3000),
        num_shards_(num_shards)
    {
        // compute weights for top M keys
        std::vector<double> weights;
        weights.reserve(M);
        for (int k = 1; k <= M; k++) {
            double w = 1.0 / std::pow(static_cast<double>(k), zipf_coef);
            weights.push_back(w);
        }
        double total = std::accumulate(weights.begin(), weights.end(), 0.0);

        std::vector<double> freqs_;
        freqs_.reserve(M);
        int i = 0;
        for (double w : weights) {
            freqs_.push_back(w / total);
            i++;
        }

        // Calculate map of key to shard_idx,
        // balancing the load given the key frequencies
        std::vector<double> shard_load(num_shards, 0.0);
        for (int k = 0; k < M; k++) {
            double best_load = std::numeric_limits<double>::max();
            int best_shard = 0;
            for (int s = 0; s < num_shards; s++) {
                if (shard_load[s] < best_load) {
                    best_load = shard_load[s];
                    best_shard = s;
                }
            }
            key_to_shard_[k] = best_shard;
            shard_load[best_shard] += freqs_[k];
        }
    }
    virtual ~LoadBalancedPartitioner() {}

    virtual uint64_t operator()(const std::string &key, uint64_t numShards,
                                int group, const std::vector<int> &txnGroups);
private:
    // store some state
    int M; // cutoff point
    int num_shards_;
    std::unordered_map<int, int> key_to_shard_;
};

class WarehouseDistItemsPartitioner : public Partitioner
{
public:
    WarehouseDistItemsPartitioner(uint64_t numWarehouses) : numWarehouses(numWarehouses) {}
    virtual ~WarehouseDistItemsPartitioner() {}
    virtual uint64_t operator()(const std::string &key, uint64_t numShards,
                                int group, const std::vector<int> &txnGroups);

private:
    const uint64_t numWarehouses;
};

class WarehousePartitioner : public Partitioner
{
public:
    WarehousePartitioner(uint64_t numWarehouses, std::mt19937 &rd) : numWarehouses(numWarehouses), rd(rd) {}
    virtual ~WarehousePartitioner() {}

    virtual uint64_t operator()(const std::string &key, uint64_t numShards,
                                int group, const std::vector<int> &txnGroups);

private:
    const uint64_t numWarehouses;
    std::mt19937 &rd;
};

typedef std::function<uint64_t(const std::string &, uint64_t, int,
                               const std::vector<int> &)>
    partitioner;

extern partitioner default_partitioner;
extern partitioner warehouse_partitioner;

partitioner warehouse_district_partitioner_dist_items(uint64_t num_warehouses);
partitioner warehouse_district_partitioner(uint64_t num_warehouses, std::mt19937 &rd);

#endif /* PARTITIONER_H */
