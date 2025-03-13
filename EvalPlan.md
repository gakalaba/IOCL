# Evaluation Plan
## Latency in a single datacenter
- current
    - Figure 6
        - Normalized p50 latency in a single datacenter setting with
9 shards distributed evenly across 3 replicas. Latency for each setting
is normalized to the Multi-Paxos latency with the absolute values
for Ellis/Multi-Paxos shown in ms above each bar
- changes
    - Figure 6 (**specifying with uniform skew**)
        - additional bar for Ellis-CT
        - additional bar for 1-big-txn
    - Add new Figure
        - single-datacenter non-uniform skew, to demonstrate the wins Ellis-CT over 1-big-txn in skewed settings
        - 3 subfigures, showing 3 skew values
            - within each, we can show a plot similar to Figure 6
    - Questions
        - Should we keep the bar as a normalized figure or change it to absolute values?
            - this might not showcase constant time nature of Ellis-CT and 1BT

![image](https://hackmd.io/_uploads/B1puopxhkl.png)


## Latency in wide area
- current
    - Figure 7
        - Normalized p50 latency in the two wide-area settings
with three shards for fanouts of 1–32. Latency for each setting is
normalized to the Multi-Paxos latency with the absolute values for
Ellis/Multi-Paxos shown in ms above each bar
        - Setting 1 **Edge Clients**: clients are far from shard leaders,  colocated shard leaders
        - Setting 2 **Datacenter Clients and Geo-distributed Clusters**: colocated clients in same datacenter, shard leaders and replicas are far from each other.
- changes
    - Figure 7 (**specifying with uniform skew**)
        - additional bar for Ellis-CT
        - additional bar for 1-big-txn
## Real World Applications
- current
    - Retwis
    - Lamernewz
    - Grafana
- changes (**specifying with uniform skew**)
    - Retwis
        - add bar for 1-big-txn will be easy since it already exists in the codebase
        - add bar for Ellis-CT
    - Lamernewz
        - add bar for 1-big-transaction
        - add bar for Ellis-CT
    - Grafana
        - add bar for Ellis-CT, it has same API as Ellis (asynch/open requests)
        - add bar for 1-big-txn
            - implementation here would be something like client-size batching of logging events that then get issued as a large transaction
            - could add a bar for each batchsize
## Throughput
- current
    - Figure 12
        - 9 shard setup, single datacenter latencies, fanout 16, 10 client machines
        - (expected) tput of Ellis is around 60% that of MP
- changes
    - Figure 12 (**specifying with uniform skew**)
        - can add line for expected tput of Ellis-CT compared to MP
        - can add line for expected tput of 1-big-txn
    - Batching?

