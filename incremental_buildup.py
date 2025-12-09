#!/usr/bin/env python3
import json
import sys
import shutil
import glob
import subprocess
from pathlib import Path

if len(sys.argv) != 2:
    print("Usage: run_experiments.py <path/to/config.json>")
    sys.exit(1)

CONFIG_PATH = Path(sys.argv[1])

# === Base load values ===
BASE_TOTAL = 320
BASE_PPN = 64

print(f"Base client_total = {BASE_TOTAL}")
print(f"Base client_process_per_client_node = {BASE_PPN}")

# === Load config ===
with open(CONFIG_PATH) as f:
    config = json.load(f)

# === Determine which client slots to scale ===
protocols = config.get("replication_protocol", [])
TARGET_IDXS = [
    i for i, p in enumerate(protocols)
    if p in ("iocl_ct", "strong")
]

print("Scaling indices:", TARGET_IDXS)


def update_load(config, fanout):
    """Update load-dependent fields."""
    config["client_issue_concurrent"] = True
    config["client_debug_output"] = False
    config["server_debug_output"] = False
    config["client_fanout"] = fanout

    new_total = BASE_TOTAL // fanout
    new_ppn = BASE_PPN // fanout

    print(f"Setting client_total={new_total}, client_ppn={new_ppn}")

    for idx in TARGET_IDXS:
        if idx < len(config["client_total"]):
            config["client_total"][idx][0] = new_total
        if idx < len(config["client_processes_per_client_node"]):
            config["client_processes_per_client_node"][idx][0] = new_ppn


def save_config(cfg):
    with open(CONFIG_PATH, "w") as f:
        json.dump(cfg, f, indent=2)


# -------------------------------
# NEW: SKEW MODES
# -------------------------------

SKEWS = [
    {"type": "uniform"},           # uniform mode — no partitioner, uniform keys
    {"type": "zipf", "zipf": 0.8},
    {"type": "zipf", "zipf": 0.99},
    {"type": "zipf", "zipf": 1.2},
]

FANOUT_VALUES = [1, 2, 4, 8, 16]


for skew in SKEWS:

    print(f"\n\n========== SKEW MODE: {skew} ==========\n")

    if skew["type"] == "uniform":
        # Uniform mode
        config["client_key_selector"] = "uniform"

        # Remove partitioner
        if "partitioner" in config:
            del config["partitioner"]

        # Ensure zipf fields don't stay around incorrectly
        if "client_zipf_coefficient" in config:
            del config["client_zipf_coefficient"]

        zipf_label = "uniform"

    else:
        # Zipf mode
        z = skew["zipf"]
        config["client_key_selector"] = "zipf"
        config["partitioner"] = "load_balanced"
        config["client_zipf_coefficient"] = z
        zipf_label = f"zipf{z}"

    for fanout in FANOUT_VALUES:
        print(f"--- Running skew={zipf_label}, fanout={fanout} ---")

        update_load(config, fanout)
        save_config(config)

        # -------- Run experiment --------
        subprocess.run(["python3", "experiments/run_multiple_experiments.py", str(CONFIG_PATH)], check=True)

        # -------- Move results --------
        outdir = f"{CONFIG_PATH.stem}_{zipf_label}_fanout{fanout}"
        dest_dir = Path(f"/proj/praxis-PG0/exp/icon/KEEP_DATA/wan/{outdir}")
        dest_dir.mkdir(parents=True, exist_ok=True)

        for path in glob.glob("experiments/printdbg/2025*"):
            print(f"Moving {path} → {dest_dir}")
            shutil.move(path, dest_dir)