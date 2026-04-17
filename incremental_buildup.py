#!/usr/bin/env python3
import copy
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

# === Load config ===
with open(CONFIG_PATH) as f:
    base_config = json.load(f)

# === Determine which protocol slots to scale ===
protocols = base_config.get("replication_protocol", [])
TARGET_IDXS = [
    i for i, p in enumerate(protocols)
    if p in ("iocl_ct", "strong")
]

print("Scaling indices:", TARGET_IDXS)

# === Save original load values ===
BASE_CLIENTS_USED = copy.deepcopy(base_config["clients_used"])
BASE_CLIENT_TOTAL = copy.deepcopy(base_config["client_total"])
BASE_PPN = copy.deepcopy(base_config["client_processes_per_client_node"])

print("Base clients_used =", BASE_CLIENTS_USED)
print("Base client_total =", BASE_CLIENT_TOTAL)
print("Base client_processes_per_client_node =", BASE_PPN)


def update_load(config, fanout):
    """Update load-dependent fields."""
    config["client_issue_concurrent"] = True
    config["client_debug_output"] = False
    config["server_debug_output"] = False
    config["client_fanout"] = fanout

    # Reset to original values first
    config["clients_used"] = copy.deepcopy(BASE_CLIENTS_USED)
    config["client_total"] = copy.deepcopy(BASE_CLIENT_TOTAL)
    config["client_processes_per_client_node"] = copy.deepcopy(BASE_PPN)

    # Scale only IOCL_CT and Spanner
    for idx in TARGET_IDXS:
        if idx < len(config["clients_used"]):
            config["clients_used"][idx][0] = BASE_CLIENTS_USED[idx][0] // fanout
        if idx < len(config["client_total"]):
            config["client_total"][idx] = [
                x // fanout for x in BASE_CLIENT_TOTAL[idx]
            ]

    print(f"Setting fanout={fanout}")
    for idx in range(len(protocols)):
        print(
            f"  idx={idx}, protocol={protocols[idx]}, "
            f"clients_used={config['clients_used'][idx]}, "
            f"client_total={config['client_total'][idx]}, "
            f"client_ppn={config['client_processes_per_client_node'][idx]}"
        )


def save_config(cfg):
    with open(CONFIG_PATH, "w") as f:
        json.dump(cfg, f, indent=2)


# -------------------------------
# NEW: SKEW MODES
# -------------------------------

SKEWS = [
    {"type": "zipf", "zipf": 0.8},
    {"type": "zipf", "zipf": 0.9},
    {"type": "zipf", "zipf": 0.99},
    {"type": "uniform"}           # uniform mode — no partitioner, uniform keys
]

FANOUT_VALUES = [1, 4, 16]


for skew in SKEWS:

    print(f"\n\n========== SKEW MODE: {skew} ==========\n")

    config = copy.deepcopy(base_config)

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
        dest_root = Path("/proj/praxis-PG0/exp/new/KEEP_DATA/lan")

        for path in glob.glob("experiments/printdbg/2026*"):
            src = Path(path)

            # Step A: rename the directory in place to experiments/printdbg/<outdir>
            renamed = src.with_name(outdir)
            print(f"Renaming {src} → {renamed}")
            src.rename(renamed)

            # Step B: move renamed directory AS-IS into /proj/.../lan/
            final_dst = dest_root / outdir
            print(f"Moving {renamed} → {final_dst}")

            # final destination must NOT exist, to avoid shutil merging semantics
            if final_dst.exists():
                print(f"[WARN] {final_dst} already exists — deleting it first")
                shutil.rmtree(final_dst)

            shutil.move(str(renamed), str(final_dst))