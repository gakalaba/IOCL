# !/bin/bash

for fanout in 1 2 4 8; do
    echo "=== Running experiments for client_fanout = $fanout ==="

    # Set client_fanout in JSON
    sed -i "s/\"client_fanout\": [0-9]\+,/\"client_fanout\": $fanout,/g" experiments/configs/micro_iocl_quick.json

    # ---- Step 1 ----
    python3 experiments/run_multiple_experiments.py experiments/configs/micro_iocl_quick.json
    wait
    rm -rf experiments/printdbg/2*/2*/2*/out/plots
    mv experiments/printdbg/2* experiments/printdbg/step1_fanout_${fanout}
    mv experiments/printdbg/step1_fanout_${fanout}/plots/tput-p50.png experiments/printdbg/step1_fanout_${fanout}/tput-p50_step1.png

    # Enable debug
    sed -i 's/"client_debug_output": false,/"client_debug_output": true,/g' experiments/configs/micro_iocl_quick.json
    sed -i 's/"server_debug_output": false,/"server_debug_output": true,/g' experiments/configs/micro_iocl_quick.json
    python3 experiments/run_multiple_experiments.py experiments/configs/micro_iocl_quick.json
    wait
    rm -rf experiments/printdbg/2*/2*/2*/out/plots
    mv experiments/printdbg/2* experiments/printdbg/step1_debug_fanout_${fanout}
    mv experiments/printdbg/step1_debug_fanout_${fanout}/plots/tput-p50.png experiments/printdbg/step1_debug_fanout_${fanout}/tput-p50_step1_debug.png

    # Disable debug
    sed -i 's/"client_debug_output": true,/"client_debug_output": false,/g' experiments/configs/micro_iocl_quick.json
    sed -i 's/"server_debug_output": true,/"server_debug_output": false,/g' experiments/configs/micro_iocl_quick.json

    # ---- Step 2 (client_issue_concurrent=true) ----
    sed -i 's/"client_issue_concurrent": false,/"client_issue_concurrent": true,/g' experiments/configs/micro_iocl_quick.json

    python3 experiments/run_multiple_experiments.py experiments/configs/micro_iocl_quick.json
    wait
    rm -rf experiments/printdbg/2*/2*/2*/out/plots
    mv experiments/printdbg/2* experiments/printdbg/step2_fanout_${fanout}
    mv experiments/printdbg/step2_fanout_${fanout}/plots/tput-p50.png experiments/printdbg/step2_fanout_${fanout}/tput-p50_step2.png

    # Enable debug again
    sed -i 's/"client_debug_output": false,/"client_debug_output": true,/g' experiments/configs/micro_iocl_quick.json
    sed -i 's/"server_debug_output": false,/"server_debug_output": true,/g' experiments/configs/micro_iocl_quick.json
    python3 experiments/run_multiple_experiments.py experiments/configs/micro_iocl_quick.json
    wait
    rm -rf experiments/printdbg/2*/2*/2*/out/plots
    mv experiments/printdbg/2* experiments/printdbg/step2_debug_fanout_${fanout}
    mv experiments/printdbg/step2_debug_fanout_${fanout}/plots/tput-p50.png experiments/printdbg/step2_debug_fanout_${fanout}/tput-p50_step2_debug.png

    # Disable debug and reset concurrency for next iteration
    sed -i 's/"client_debug_output": true,/"client_debug_output": false,/g' experiments/configs/micro_iocl_quick.json
    sed -i 's/"server_debug_output": true,/"server_debug_output": false,/g' experiments/configs/micro_iocl_quick.json
    sed -i 's/"client_issue_concurrent": true,/"client_issue_concurrent": false,/g' experiments/configs/micro_iocl_quick.json

done
