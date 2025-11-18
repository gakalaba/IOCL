    # !/bin/bash

    for fanout in 1 2 4 8; do
        echo "=== Running experiments for client_fanout = $fanout ==="

        # Make sure issue_concurrent is set to true
        sed -i 's/"client_issue_concurrent": false,/"client_issue_concurrent": true,/g' experiments/configs/micro_iocl_2shards_diff_nodes.json
        # Make sure debug is disabled
        sed -i 's/"client_debug_output": true,/"client_debug_output": false,/g' experiments/configs/micro_iocl_2shards_diff_nodes.json
        sed -i 's/"server_debug_output": true,/"server_debug_output": false,/g' experiments/configs/micro_iocl_2shards_diff_nodes.json
        # Set client_fanout in JSON
        sed -i "s/\"client_fanout\": [0-9]\+,/\"client_fanout\": $fanout,/g" experiments/configs/micro_iocl_2shards_diff_nodes.json

        # ---- Run Experiment ----
        python3 experiments/run_multiple_experiments.py experiments/configs/micro_iocl_2shards_diff_nodes.json
        wait
        # rm -rf experiments/printdbg/2*/2*/2*/out/plots
        mv experiments/printdbg/2* experiments/printdbg/micro_iocl_2shards_diff_nodes_${fanout}
        mv experiments/printdbg/micro_iocl_2shards_diff_nodes_${fanout}/plots/tput-p50.png experiments/printdbg/micro_iocl_2shards_diff_nodes_${fanout}/tput-p50.png

        echo "=== Running experiments for client_fanout = $fanout ==="

        # Make sure issue_concurrent is set to true
        sed -i 's/"client_issue_concurrent": false,/"client_issue_concurrent": true,/g' experiments/configs/micro_iocl_2shards_same_nodes.json
        # Make sure debug is disabled
        sed -i 's/"client_debug_output": true,/"client_debug_output": false,/g' experiments/configs/micro_iocl_2shards_same_nodes.json
        sed -i 's/"server_debug_output": true,/"server_debug_output": false,/g' experiments/configs/micro_iocl_2shards_same_nodes.json
        # Set client_fanout in JSON
        sed -i "s/\"client_fanout\": [0-9]\+,/\"client_fanout\": $fanout,/g" experiments/configs/micro_iocl_2shards_same_nodes.json

        # ---- Run Experiment ----
        python3 experiments/run_multiple_experiments.py experiments/configs/micro_iocl_2shards_same_nodes.json
        wait
        # rm -rf experiments/printdbg/2*/2*/2*/out/plots
        mv experiments/printdbg/2* experiments/printdbg/micro_iocl_2shards_same_nodes_${fanout}
        mv experiments/printdbg/micro_iocl_2shards_same_nodes_${fanout}/plots/tput-p50.png experiments/printdbg/micro_iocl_2shards_same_nodes_${fanout}/tput-p50.png
    done
