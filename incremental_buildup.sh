    # !/bin/bash

    CONFIG="$1"

    if [[ -z "$CONFIG" ]]; then
        echo "Usage: $0 <path/to/config.json>"
        exit 1
    fi

    for fanout in 16 8 4 2 1; do
        echo "=== Running experiments for client_fanout = $fanout with config = $CONFIG ==="

        # Make sure issue_concurrent is set to true
        sed -i 's/"client_issue_concurrent": false,/"client_issue_concurrent": true,/g' "$CONFIG"
        # Make sure debug is disabled
        sed -i 's/"client_debug_output": true,/"client_debug_output": false,/g' "$CONFIG"
        sed -i 's/"server_debug_output": true,/"server_debug_output": false,/g' "$CONFIG"
        # Set client_fanout in JSON
        sed -i "s/\"client_fanout\": [0-9]\+,/\"client_fanout\": $fanout,/g" "$CONFIG"

        # ---- Run Experiment ----
        python3 experiments/run_multiple_experiments.py "$CONFIG"
        wait
        # ---- Move Results and Cleanup ----
        outdir="experiments/printdbg/micro_$(basename "$CONFIG" .json)_${fanout}"
        mv experiments/printdbg/2025* "$outdir"
        mv "$outdir/plots/tput-p50.png" "$outdir/tput-p50.png"
        rm -rf "$outdir/2*/2*/out/plots"
    done
