#!/bin/bash

CONFIG="$1"

if [[ -z "$CONFIG" ]]; then
    echo "Usage: $0 <path/to/config.json>"
    exit 1
fi

# Outer loop: Zipf coefficients
for zipf in 0.8 0.99 1.2; do
    echo "=== Setting Zipf coefficient to $zipf ==="

    # Update client_zipf_coefficient
    sed -i "s/\"client_zipf_coefficient\": [0-9.]\+,/\"client_zipf_coefficient\": $zipf,/g" "$CONFIG"

    # Inner loop: fanout values
    for fanout in 1 2 4 8 16; do
        echo "=== Running experiments for zipf = $zipf, client_fanout = $fanout ==="

        # Ensure client_issue_concurrent is true
        sed -i 's/"client_issue_concurrent": false,/"client_issue_concurrent": true,/g' "$CONFIG"

        # Disable debug flags
        sed -i 's/"client_debug_output": true,/"client_debug_output": false,/g' "$CONFIG"
        sed -i 's/"server_debug_output": true,/"server_debug_output": false,/g' "$CONFIG"

        # Set client_fanout
        sed -i "s/\"client_fanout\": [0-9]\+,/\"client_fanout\": $fanout,/g" "$CONFIG"

        # ---- Run Experiment ----
        python3 experiments/run_multiple_experiments.py "$CONFIG"
        wait

        # ---- Move Results and Cleanup ----
        outdir="experiments/printdbg/$(basename "$CONFIG" .json)_zipf${zipf}_fanout${fanout}"
        mv experiments/printdbg/2025* "$outdir"
        mv "$outdir/plots/tput-p50.png" "$outdir/tput-p50.png"
        rm -rf "$outdir"/2*/2*/out/plots
    done

done

