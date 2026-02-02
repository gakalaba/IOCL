import os
import json
import random
import numpy
import csv
import traceback
import subprocess
import concurrent
import collections
import operator
import math

# -----------------------------
# Utilities
# -----------------------------

def convert_latency_nanos_to_millis(latencies):
    return list(map(lambda x: x / 1e6, latencies))


def get_region(config, server):
    for region, servers in config["server_regions"].items():
        if server in servers:
            return region
    raise ValueError("{} not in any region".format(server))


def get_regions(config):
    return set([get_region(config, s) for s in config["clients"] + config["server_names"]])


def get_num_regions(config):
    return len(set([get_region(config, s) for s in config["clients"] + config["server_names"]]))


# -----------------------------
# Stats calculation
# -----------------------------

STATS_FILE = 'stats.json'


def calculate_statistics(config, local_out_directory):
    runs = []
    op_latencies = {}
    op_times = {}
    op_latency_counts = {}
    op_tputs = {}
    client_op_latencies = []
    client_op_times = []

    for i in range(config['num_experiment_runs']):
        client_op_latencies.append({})
        client_op_times.append({})
        stats, run_op_latencies, run_op_times, run_op_latency_counts, run_op_tputs, run_client_op_latencies, run_client_op_times = calculate_statistics_for_run(
            config, local_out_directory, i)
        runs.append(stats)

        for k, vv in run_op_latency_counts.items():
            for j in range(len(run_op_latencies.get(k, []))):
                if k in op_latencies:
                    if len(op_latencies[k]) <= j:
                        op_latencies[k].append(run_op_latencies[k][j])
                    else:
                        op_latencies[k][j] += run_op_latencies[k][j]
                else:
                    op_latencies[k] = [run_op_latencies[k][j]]
            op_latency_counts[k] = op_latency_counts.get(k, 0) + vv

        for k, v in run_op_times.items():
            for j in range(len(v)):
                if k in op_times:
                    if len(op_times[k]) <= j:
                        op_times[k].append(v[j])
                    else:
                        op_times[k][j] += v[j]
                else:
                    op_times[k] = [v[j]]

        for k, v in run_op_tputs.items():
            # v is either:
            #  - a float (already aggregated), or
            #  - a list of per-region floats (common in your codepath)
            if isinstance(v, list):
                if k not in op_tputs:
                    op_tputs[k] = list(v)
                else:
                    # elementwise sum, allowing length mismatches
                    if len(op_tputs[k]) < len(v):
                        op_tputs[k].extend([0.0] * (len(v) - len(op_tputs[k])))
                    for ii in range(len(v)):
                        op_tputs[k][ii] += v[ii]
            else:
                # scalar
                op_tputs[k] = op_tputs.get(k, 0.0) + v


        for cid, opl in run_client_op_latencies.items():
            client_op_latencies[i][cid] = opl
        for cid, opt in run_client_op_times.items():
            client_op_times[i][cid] = opt

    stats = {}
    stats['aggregate'] = {}
    norm_op_latencies, norm_op_times = calculate_all_op_statistics(
        config, stats['aggregate'], op_latencies, op_times, op_latency_counts, op_tputs)

    for k, v in norm_op_latencies.items():
        op_latencies['%s_norm' % k] = v
    for k, v in norm_op_times.items():
        op_times['%s_norm' % k] = v

    stats['runs'] = runs
    stats['run_stats'] = {}

    if not runs:
        # No runs -> still write a stats file for callers, but nothing else to compute.
        stats_file = config.get('stats_file_name', STATS_FILE)
        with open(os.path.join(local_out_directory, stats_file), 'w') as f:
            json.dump(stats, f, indent=2, sort_keys=True)
        return stats, op_latencies, op_times, client_op_latencies, client_op_times

    ignored = {'cdf': 1, 'cdf_log': 1, 'time': 1}
    for cat in runs[0]:  # assume at least one run
        if (not ('region-' in cat)) and isinstance(runs[0][cat], dict):
            stats['run_stats'][cat] = {}
            for s in runs[0][cat]:
                if s in ignored:
                    continue
                data = [run[cat][s] for run in runs if cat in run and s in run[cat]]
                if data:
                    stats['run_stats'][cat][s] = calculate_statistics_for_data(data, cdf=False)

        if 'region-' in cat:
            stats['run_stats'][cat] = {}
            for cat2 in runs[0][cat]:
                if isinstance(runs[0][cat][cat2], dict):
                    stats['run_stats'][cat][cat2] = {}
                    for s in runs[0][cat][cat2]:
                        if s in ignored:
                            continue
                        data = [run[cat][cat2][s] for run in runs
                                if cat in run and cat2 in run[cat] and s in run[cat][cat2]]
                        if data:
                            stats['run_stats'][cat][cat2][s] = calculate_statistics_for_data(data, cdf=False)

    stats_file = config.get('stats_file_name', STATS_FILE)
    with open(os.path.join(local_out_directory, stats_file), 'w') as f:
        json.dump(stats, f, indent=2, sort_keys=True)

    return stats, op_latencies, op_times, client_op_latencies, client_op_times


def calculate_statistics_for_run(config, local_out_directory, run):
    region_op_latencies = {}
    region_op_times = {}
    region_op_latency_counts = {}
    region_op_tputs = {}

    region_client_op_latencies = {}
    region_client_op_times = {}

    stats = {}

    regions = get_regions(config)
    for region in regions:
        r_op_latencies = {}
        r_op_latency_counts = {}
        r_op_times = {}
        r_op_tputs = {}

        op_latencies = {}
        op_latency_counts = {}
        op_tputs = {}
        op_times = {}

        # -------- Clients ----------
        for client in config["clients"]:
            if get_region(config, client) != region:
                continue

            client_dir = client
            for k in range(config["client_processes_per_client_node"]):
                client_out_file = os.path.join(
                    local_out_directory, client_dir, '%s-%d-stdout-%d.log' % (client, k, run)
                )

                if not os.path.exists(client_out_file):
                    # Don't crash if a client log is missing
                    continue

                end_time_sec = {}
                end_time_usec = {}

                with open(client_out_file) as f:
                    # NOTE: this is still potentially heavy; leaving logic as-is
                    ops = f.readlines()
                    foundEnd = False
                    cid = 0
                    for op in ops:
                        foundEnd = False
                        opCols = op.strip().split(',')
                        for x in range(0, len(opCols), 2):
                            if opCols[x].isdigit():
                                break
                            if len(opCols[x]) > 0 and opCols[x][0] == '#':
                                if opCols[x] == '#end':
                                    cid = 0
                                    if x + 3 < len(opCols):
                                        cid = int(opCols[x+3])
                                    end_time_sec[cid] = float(opCols[x+1])
                                    end_time_usec[cid] = float(opCols[x+2])
                                    foundEnd = True
                                    break
                                # ignore #start for now (unused downstream)
                                break

                            if opCols[x] in config['client_stats_blacklist']:
                                continue

                            # Latency
                            if 'input_latency_scale' in config:
                                opLat = float(opCols[x+1]) / config['input_latency_scale']
                            else:
                                opLat = float(opCols[x+1]) / 1e9
                            if 'output_latency_scale' in config:
                                opLat = opLat * config['output_latency_scale']
                            else:
                                opLat = opLat * 1e3

                            # Time
                            opTime = 0.0
                            if x + 2 < len(opCols):
                                if 'input_latency_scale' in config:
                                    opTime = float(opCols[x+2]) / config['input_latency_scale']
                                else:
                                    opTime = float(opCols[x+2]) / 1e9
                                if 'output_latency_scale' in config:
                                    opTime = opTime * config['output_latency_scale']
                                else:
                                    opTime = opTime * 1e3

                            cid = int(opCols[x+3]) if x + 3 < len(opCols) else 0

                            op_latencies.setdefault(cid, {}).setdefault(opCols[x], []).append(opLat)
                            op_times.setdefault(cid, {}).setdefault(opCols[x], []).append(opTime)
                            op_latency_counts.setdefault(cid, {}).setdefault(opCols[x], 0)
                            op_latency_counts[cid][opCols[x]] += 1

                            # combined bucket
                            if opCols[x] not in config['client_combine_stats_blacklist']:
                                op_latencies[cid].setdefault('combined', []).append(opLat)
                                op_times[cid].setdefault('combined', []).append(opTime)
                                op_latency_counts[cid].setdefault('combined', 0)
                                op_latency_counts[cid]['combined'] += 1

                            # optional ro/rw bucket
                            if 'client_combine_ro_ops' in config:
                                rorw = 'ro' if opCols[x] in config['client_combine_ro_ops'] else 'rw'
                                op_latencies[cid].setdefault(rorw, []).append(opLat)
                                op_times[cid].setdefault(rorw, []).append(opTime)
                                op_latency_counts[cid].setdefault(rorw, 0)
                                op_latency_counts[cid][rorw] += 1

                        if foundEnd and cid in end_time_sec:
                            run_time_sec = end_time_sec[cid] + end_time_usec[cid] / 1e6
                            if cid in op_latency_counts and run_time_sec > 0:
                                for k1, v in op_latency_counts[cid].items():
                                    op_tputs[k1] = op_tputs.get(k1, 0.0) + (v / run_time_sec)

                # Merge client stats json if exists
                client_stats_file = os.path.join(
                    local_out_directory, client_dir, '%s-%d-stats-%d.json' % (client, k, run)
                )
                if os.path.exists(client_stats_file):
                    try:
                        with open(client_stats_file) as f:
                            client_stats = json.load(f)
                        for k1, v in client_stats.items():
                            if ('stats_merge_lists' not in config) or (k1 not in config['stats_merge_lists']):
                                stats[k1] = stats.get(k1, 0) + v
                            else:
                                stats.setdefault(k1, [])
                                if len(stats[k1]) < len(v):
                                    stats[k1].extend([0] * (len(v) - len(stats[k1])))
                                for uu in range(len(v)):
                                    stats[k1][uu] += v[uu]
                    except Exception:
                        pass

        # -------- Servers ----------
        for instance_idx in range(config["num_instances"]):
            for shard_idx in range(len(config["shards"])):
                shard = config["shards"][shard_idx]
                for replica_idx in range(len(shard)):
                    replica = shard[replica_idx]
                    if get_region(config, replica) != region:
                        continue

                    server_stats_file = os.path.join(
                        local_out_directory, 'server-%d-%d' % (instance_idx, shard_idx),
                        'server-%d-%d-%d-stats-%d.json' % (instance_idx, shard_idx, replica_idx, run)
                    )
                    if not os.path.exists(server_stats_file):
                        continue
                    try:
                        with open(server_stats_file) as f:
                            server_stats = json.load(f)
                        for ksv, vsv in server_stats.items():
                            if isinstance(vsv, dict):
                                continue
                            if ('stats_merge_lists' not in config) or (ksv not in config['stats_merge_lists']):
                                stats[ksv] = stats.get(ksv, 0) + vsv
                            else:
                                stats.setdefault(ksv, [])
                                if len(stats[ksv]) < len(vsv):
                                    stats[ksv].extend([0] * (len(vsv) - len(stats[ksv])))
                                for uu in range(len(vsv)):
                                    stats[ksv][uu] += vsv[uu]
                    except Exception:
                        pass

        # -------- Merge per-region client arrays into region arrays ----------
        for cid, opl in op_latencies.items():
            for ktype, vlist in opl.items():
                r_op_latencies.setdefault(ktype, []).extend(vlist)
            region_client_op_latencies[cid] = opl

        for cid, opt in op_times.items():
            for ktype, vlist in opt.items():
                r_op_times.setdefault(ktype, []).extend(vlist)
            region_client_op_times[cid] = opt

        for cid, oplc in op_latency_counts.items():
            for ktype, count in oplc.items():
                r_op_latency_counts[ktype] = r_op_latency_counts.get(ktype, 0) + count

        for ktype, t in op_tputs.items():
            r_op_tputs[ktype] = r_op_tputs.get(ktype, 0.0) + t

        # normalize by server region to account for latency differences
        for ktype, vlist in r_op_latencies.items():
            region_op_latencies.setdefault(ktype, []).append(vlist)
        for ktype, vlist in r_op_times.items():
            region_op_times.setdefault(ktype, []).append(vlist)
        for ktype, count in r_op_latency_counts.items():
            # original uses min across regions; keep behavior
            region_op_latency_counts[ktype] = min(region_op_latency_counts.get(ktype, count), count)
        for ktype, t in r_op_tputs.items():
            region_op_tputs.setdefault(ktype, []).append(t)

    # Derived ratios (kept)
    if 'fast_writes_0' in stats or 'slow_writes_0' in stats or 'fast_reads_0' in stats or 'slow_reads_0' in stats:
        fw0 = stats.get('fast_writes_0', 0)
        sw0 = stats.get('slow_writes_0', 0)
        if fw0 + sw0 > 0:
            stats['fast_write_ratio'] = fw0 / (fw0 + sw0)
            stats['slow_write_ratio'] = sw0 / (fw0 + sw0)
        fr0 = stats.get('fast_reads_0', 0)
        sr0 = stats.get('slow_reads_0', 0)
        if fr0 + sr0 > 0:
            stats['fast_read_ratio'] = fr0 / (fr0 + sr0)
            stats['slow_read_ratio'] = sr0 / (fr0 + sr0)

    # Commit/abort rate derivation (guard missing *_attempts)
    total_committed = 0
    total_attempts = 0
    stats_new = {}
    for k, v in list(stats.items()):
        if k.endswith('_committed'):
            k_prefix = k[:-len('_committed')]
            k_attempts = k_prefix + '_attempts'
            if k_attempts not in stats or stats[k_attempts] == 0:
                continue
            k_commit_rate = k_prefix + '_commit_rate'
            k_abort_rate = k_prefix + '_abort_rate'
            total_committed += stats[k]
            total_attempts += stats[k_attempts]
            stats_new[k_commit_rate] = stats[k] / stats[k_attempts]
            stats_new[k_abort_rate] = 1 - stats_new[k_commit_rate]
    stats.update(stats_new)

    if total_attempts > 0:
        stats['committed'] = total_committed
        stats['attempts'] = total_attempts
        stats['commit_rate'] = total_committed / total_attempts
        stats['abort_rate'] = 1 - stats['commit_rate']

    norm_op_latencies, norm_op_times = calculate_all_op_statistics(
        config, stats, region_op_latencies, region_op_times, region_op_latency_counts, region_op_tputs)
    for k, v in norm_op_latencies.items():
        region_op_latencies['%s_norm' % k] = v
    for k, v in norm_op_times.items():
        region_op_times['%s_norm' % k] = v

    return stats, region_op_latencies, region_op_times, region_op_latency_counts, region_op_tputs, region_client_op_latencies, region_client_op_times


def calculate_op_statistics(config, stats, total_recorded_time, op_type, latencies, norm_latencies, tput):
    if len(latencies) > 0:
        stats[op_type] = calculate_statistics_for_data(latencies)
        stats[op_type]['ops'] = len(latencies)
        if tput == -1:
            stats[op_type]['tput'] = len(latencies) / total_recorded_time
        else:
            stats[op_type]['old_tput'] = len(latencies) / total_recorded_time
            stats[op_type]['tput'] = tput
        if op_type == 'combined':
            stats['combined']['ops'] = len(latencies)
            stats['combined']['time'] = total_recorded_time


def calculate_all_op_statistics(config, stats, region_op_latencies, region_op_times,
                               region_op_latency_counts, region_op_tputs):
    total_recorded_time = float(
        config['client_experiment_length'] - config['client_ramp_up'] - config['client_ramp_down'])

    norm_op_latencies = {}
    norm_op_times = {}

    for k, v in region_op_latencies.items():
        latencies = [lat for region_lats in v for lat in region_lats]

        tput = -1 if len(v) == 0 else 0
        if k in region_op_tputs:
            for region_tput in region_op_tputs[k]:
                tput += region_tput

        # safe sampling
        for i in range(len(v)):
            m = min(region_op_latency_counts.get(k, 0), len(v[i]))
            if m <= 0:
                continue
            sample_idxs = random.sample(range(len(v[i])), m)

            if k in norm_op_latencies:
                norm_op_latencies[k].extend([v[i][idx] for idx in sample_idxs])
                norm_op_times[k].extend([region_op_times[k][i][idx] for idx in sample_idxs])
            else:
                norm_op_latencies[k] = [v[i][idx] for idx in sample_idxs]
                norm_op_times[k] = [region_op_times[k][i][idx] for idx in sample_idxs]

        if (not 'server_emulate_wan' in config) or config['server_emulate_wan']:
            for i in range(len(v)):
                region_key = 'region-%d' % i
                if region_key not in stats:
                    stats[region_key] = {}
                op_tput = -1
                if k in region_op_tputs and len(region_op_tputs[k]) > i:
                    op_tput = region_op_tputs[k][i]
                calculate_op_statistics(config, stats[region_key], total_recorded_time, k, v[i], [], op_tput)

        calculate_op_statistics(config, stats, total_recorded_time, k, latencies, norm_op_latencies.get(k, []), tput)

    return norm_op_latencies, norm_op_times


# -----------------------------
# Percentiles / CDF
# -----------------------------

def calculate_cdf_for_npdata(npdata):
    return [[i, numpy.percentile(npdata, i, interpolation='higher')] for i in range(1, 100)]


def calculate_cdf_log_for_npdata(npdata, precision):
    ptiles = []
    base = 0
    scale = 1
    for i in range(0, precision):
        for j in range(0, 90):
            if i == 0 and j == 0:
                continue
            pct = base + j / scale
            ptiles.append([pct, numpy.percentile(npdata, pct, interpolation='higher')])
        base += 90 / scale
        scale *= 10
    return ptiles


def calculate_statistics_for_data(data, cdf=True, cdf_log_precision=4):
    npdata = numpy.asarray(data)
    s = {
        'p50': numpy.percentile(npdata, 50).item(),
        'p75': numpy.percentile(npdata, 75).item(),
        'p90': numpy.percentile(npdata, 90).item(),
        'p95': numpy.percentile(npdata, 95).item(),
        'p99': numpy.percentile(npdata, 99).item(),
        'p99.9': numpy.percentile(npdata, 99.9).item(),
        'max': numpy.amax(npdata).item(),
        'min': numpy.amin(npdata).item(),
        'mean': numpy.mean(npdata).item(),
        'stddev': numpy.std(npdata).item(),
        'var': numpy.var(npdata).item(),
    }
    if cdf:
        s['cdf'] = calculate_cdf_for_npdata(npdata)
        s['cdf_log'] = calculate_cdf_log_for_npdata(npdata, cdf_log_precision)
    return s


# -----------------------------
# Plotting (aggregate-only)
# -----------------------------

def generate_gnuplot_script_cdf_agg_new(script_file, out_file, x_label,
                                       y_label, width, height, font,
                                       series, title):
    """
    series: list of tuples (label, csv_path)
            csv format is what your per-exp cdf writer makes:
              col1 = latency_value, col2 = cdf_prob
    """
    with open(script_file, 'w') as f:
        write_gpi_header(f)
        f.write("set title \"%s\"\n" % title)
        f.write("set key bottom right\n")
        f.write("set xlabel '%s'\n" % x_label)
        f.write("set ylabel '%s'\n" % y_label)
        f.write("set terminal pngcairo size %d,%d enhanced dashed font '%s'\n" %
                (width, height, font))
        f.write("set output '%s'\n" % out_file)
        write_line_styles(f)

        f.write("plot ")
        for i, (lbl, path) in enumerate(series):
            # Use col1 (x) and col2 (y)
            f.write("'%s' using 1:2 title \"%s\" ls %d with lines" %
                    (path, lbl.replace('_', '\\\\\\_'), i + 1))
            if i != len(series) - 1:
                f.write(", \\\n")


def generate_gnuplot_script_cdf_log_agg_new(script_file, out_file, x_label,
                                           y_label, width, height, font,
                                           series, title):
    """
    Same as above, but turns y into tail scale: -log10(1-y)
    (matches your old plot style)
    """
    with open(script_file, 'w') as f:
        write_gpi_header(f)
        f.write("set title \"%s\"\n" % title)
        f.write("set key bottom right\n")
        f.write("set ytics (0,0.9,0.99,0.999,0.9999,1.0)\n")
        f.write("set xlabel '%s'\n" % x_label)
        f.write("set ylabel '%s'\n" % y_label)
        f.write("set terminal pngcairo size %d,%d enhanced dashed font '%s'\n" %
                (width, height, font))
        f.write("set output '%s'\n" % out_file)
        write_line_styles(f)

        f.write("plot ")
        for i, (lbl, path) in enumerate(series):
            labels = ":yticlabels(3)" if i == 0 else ""
            # y = -log10(1 - $2)
            f.write("'%s' using 1:(-log10(1-$2))%s title \"%s\" ls %d with lines" %
                    (path, labels, lbl.replace('_', '\\\\\\_'), i + 1))
            if i != len(series) - 1:
                f.write(", \\\n")


def _flatten_leaf_dirs(out_dirs):
    """
    out_dirs is often a nested structure. Leaf nodes are strings (paths).
    Returns all leaf string paths in BFS order.
    """
    leaf = []
    q = collections.deque([out_dirs])
    while q:
        node = q.popleft()
        if isinstance(node, str):
            leaf.append(node)
        elif isinstance(node, list) or isinstance(node, tuple):
            for x in node:
                q.append(x)
    return leaf


def generate_plots(config, base_out_directory, out_dirs):
    """
    Runnable aggregate-only generate_plots that you can safely call again.

    What it does:
      1) Creates base_out_directory/<plot_directory_name>
      2) Scans ALL leaf experiment dirs for plots/aggregate-*.csv
      3) Produces multi-series aggregate plots (cdf + log) into base plots dir
      4) Produces tput-*-lat plots into base plots dir by reading stats.json from leaf dirs

    What it does NOT do:
      - lot/tot/run-* plots
      - config['plots'] "specific plots"
    """
    plots_directory = os.path.join(base_out_directory, config['plot_directory_name'])
    os.makedirs(plots_directory, exist_ok=True)

    leaf_dirs = _flatten_leaf_dirs(out_dirs)

    # --- (A) Generate tput-*-lat plots at the base directory ---
    # This creates: plots/tput-p50-lat.png, etc.
    # It reads STATS_FILE from each leaf dir.
    try:
        generate_tput_lat_plots(config, base_out_directory, leaf_dirs)
    except Exception as e:
        print("WARNING: generate_tput_lat_plots failed:", e)

    # --- (B) Collect aggregate-*.csv files from each leaf dir's plots/ directory ---
    # We want to build multi-series plots across leaf dirs.
    # Group by csv_class (e.g., 'aggregate-combined', 'aggregate-combined-log', etc.)
    csv_by_class = collections.defaultdict(list)  # csv_by_class[csv_class] -> [(label, path), ...]

    for d in leaf_dirs:
        sub_plot_dir = os.path.join(d, config['plot_directory_name'])
        if not os.path.isdir(sub_plot_dir):
            continue

        # Try to derive a decent label from directory name
        # (keeps things readable without needing the full old title logic)
        label = os.path.basename(os.path.normpath(d))

        for fname in os.listdir(sub_plot_dir):
            if not (fname.endswith('.csv') and fname.startswith('aggregate-')):
                continue
            csv_class = os.path.splitext(fname)[0]
            csv_by_class[csv_class].append((label, os.path.join(sub_plot_dir, fname)))

    if not csv_by_class:
        print("No aggregate-*.csv files found under leaf plots/ directories.")
        return

    # --- (C) Emit one multi-series plot per csv_class into the base plots dir ---
    # Use your config['cdf_plots'] settings if present; else fall back to plot_cdf_*.
    cdf_cfg = config.get('cdf_plots', None)
    if cdf_cfg is None:
        # fallback to older keys
        cdf_cfg = {
            'x_label': config.get('plot_cdf_x_label', 'Latency (ms)'),
            'y_label': config.get('plot_cdf_y_label', 'CDF'),
            'width': config.get('plot_cdf_png_width', 1200),
            'height': config.get('plot_cdf_png_height', 800),
            'font': config.get('plot_cdf_png_font', 'Helvetica,14'),
        }

    for csv_class, series in csv_by_class.items():
        # Keep a stable order so colors/lines don't reshuffle run-to-run
        series = sorted(series, key=lambda t: t[0])

        title = csv_class.replace('_', '\\\\\\_')

        plot_script_file = os.path.join(plots_directory, '%s.gpi' % csv_class)
        plot_out_file = os.path.join(plots_directory, '%s.png' % csv_class)

        if 'log' in csv_class:
            generate_gnuplot_script_cdf_log_agg_new(
                plot_script_file, plot_out_file,
                cdf_cfg['x_label'], cdf_cfg['y_label'],
                cdf_cfg['width'], cdf_cfg['height'], cdf_cfg['font'],
                series, title
            )
        else:
            generate_gnuplot_script_cdf_agg_new(
                plot_script_file, plot_out_file,
                cdf_cfg['x_label'], cdf_cfg['y_label'],
                cdf_cfg['width'], cdf_cfg['height'], cdf_cfg['font'],
                series, title
            )

        subprocess.call(['gnuplot', plot_script_file])


def write_gpi_header(f):
    f.write("set datafile separator ','\n")


def write_line_styles(f):
    f.write('set style line 1 linetype 1 linewidth 2\n')
    f.write('set style line 2 linetype 1 linecolor "green" linewidth 2\n')
    f.write('set style line 3 linetype 1 linecolor "blue" linewidth 2\n')
    f.write('set style line 4 linetype 4 linewidth 2\n')
    f.write('set style line 5 linetype 5 linewidth 2\n')
    f.write('set style line 6 linetype 8 linewidth 2\n')


def run_gnuplot(data_files, out_file, script_file):
    args = ['gnuplot', '-e', "outfile='%s'" % out_file]
    for i in range(len(data_files)):
        args += ['-e', "datafile%d='%s'" % (i, data_files[i])]
    args.append(script_file)
    subprocess.call(args)


def generate_csv_for_cdf_plot(csv_file, cdf_data, log=False):
    with open(csv_file, 'w') as f:
        csvwriter = csv.writer(f)
        k = 1
        for i in range(len(cdf_data)):
            data = [cdf_data[i][1], cdf_data[i][0] / 100]
            if log and abs(cdf_data[i][0] / 100 - (1 - 10**-k)) < 0.000001:
                data.append(1 - 10**-k)
                k += 1
            csvwriter.writerow(data)


def generate_gnuplot_script_cdf(config, script_file):
    with open(script_file, 'w') as f:
        f.write("set datafile separator ','\n")
        f.write("set key bottom right\n")
        f.write("set xlabel '%s'\n" % config['plot_cdf_x_label'])
        f.write("set ylabel '%s'\n" % config['plot_cdf_y_label'])
        f.write("set terminal pngcairo size %d,%d enhanced font '%s'\n" %
                (config['plot_cdf_png_width'], config['plot_cdf_png_height'],
                 config['plot_cdf_png_font']))
        f.write('set output outfile\n')
        f.write("plot datafile0 title '%s' with lines\n" %
                config['plot_cdf_series_title'].replace('_', '\\_'))


def generate_gnuplot_script_cdf_log(config, script_file):
    with open(script_file, 'w') as f:
        f.write("set datafile separator ','\n")
        f.write("set key bottom right\n")
        f.write("set ytics (0,0.9,0.99,0.999,0.9999,1.0)\n")
        f.write("set xlabel '%s'\n" % config['plot_cdf_x_label'])
        f.write("set ylabel '%s'\n" % config['plot_cdf_y_label'])
        f.write("set terminal pngcairo size %d,%d enhanced font '%s'\n" %
                (config['plot_cdf_png_width'], config['plot_cdf_png_height'],
                 config['plot_cdf_png_font']))
        f.write('set output outfile\n')
        f.write("plot datafile0 using 1:(-log10(1-$2)):yticlabels(3) title '%s' with lines\n" %
                config['plot_cdf_series_title'].replace('_', '\\_'))


def generate_cdf_plot(config, plots_directory, plot_name, cdf_data):
    plot_name = plot_name.replace('_', '-')
    plot_csv_file = os.path.join(plots_directory, '%s.csv' % plot_name)
    generate_csv_for_cdf_plot(plot_csv_file, cdf_data)
    plot_script_file = os.path.join(plots_directory, '%s.gpi' % plot_name)
    generate_gnuplot_script_cdf(config, plot_script_file)
    run_gnuplot([plot_csv_file], os.path.join(plots_directory, '%s.png' % plot_name), plot_script_file)


def generate_cdf_log_plot(config, plots_directory, plot_name, cdf_data):
    plot_name = plot_name.replace('_', '-')
    plot_csv_file = os.path.join(plots_directory, '%s.csv' % plot_name)
    generate_csv_for_cdf_plot(plot_csv_file, cdf_data, log=True)
    plot_script_file = os.path.join(plots_directory, '%s.gpi' % plot_name)
    generate_gnuplot_script_cdf_log(config, plot_script_file)
    run_gnuplot([plot_csv_file], os.path.join(plots_directory, '%s.png' % plot_name), plot_script_file)


def generate_cdf_plots(config, local_out_directory, stats, executor):
    """
    Only generate aggregate-* (and aggregate-*-log) plots.
    No run-* plots at all.
    """
    futures = []
    plots_directory = os.path.join(local_out_directory, config['plot_directory_name'])
    os.makedirs(plots_directory, exist_ok=True)

    for op_type in stats.get('aggregate', {}):
        if (op_type not in config['client_cdf_plot_blacklist']) and ('region-' not in op_type):
            cdf_plot_name = 'aggregate-%s' % op_type
            futures.append(executor.submit(generate_cdf_plot, config, plots_directory, cdf_plot_name,
                                           stats['aggregate'][op_type]['cdf']))
            cdf_log_plot_name = 'aggregate-%s-log' % op_type
            futures.append(executor.submit(generate_cdf_log_plot, config, plots_directory, cdf_log_plot_name,
                                           stats['aggregate'][op_type]['cdf_log']))
        elif 'region-' in op_type:
            for op_type2 in stats['aggregate'][op_type]:
                if op_type2 in config['client_cdf_plot_blacklist']:
                    continue
                cdf_plot_name = 'aggregate-%s-%s' % (op_type, op_type2)
                futures.append(executor.submit(generate_cdf_plot, config, plots_directory, cdf_plot_name,
                                               stats['aggregate'][op_type][op_type2]['cdf']))
                cdf_log_plot_name = 'aggregate-%s-%s-log' % (op_type, op_type2)
                futures.append(executor.submit(generate_cdf_log_plot, config, plots_directory, cdf_log_plot_name,
                                               stats['aggregate'][op_type][op_type2]['cdf_log']))

    concurrent.futures.wait(futures)


def generate_ot_plots(*args, **kwargs):
    # stripped intentionally
    return


# -----------------------------
# Throughput-latency plots (tput-p*)
# -----------------------------

def generate_gnuplot_script_tput_lat(config, plot_script_file):
    with open(plot_script_file, 'w') as f:
        f.write("set datafile separator ','\n")
        f.write("set key top left\n")
        f.write("set xlabel '%s'\n" % config['plot_tput_lat_x_label'])
        f.write("set ylabel '%s'\n" % config['plot_tput_lat_y_label'])
        f.write("set terminal pngcairo size %d,%d enhanced font '%s'\n" %
                (config['plot_tput_lat_png_width'],
                 config['plot_tput_lat_png_height'],
                 config['plot_tput_lat_png_font']))
        f.write('set output outfile\n')
        f.write("plot datafile0 title '%s' with linespoint\n" %
                config['plot_tput_lat_series_title'].replace('_', '\\_'))


def generate_csv_for_tput_lat_plot(plot_csv_file, tputs, lats):
    with open(plot_csv_file, 'w') as f:
        csvwriter = csv.writer(f)
        for i in range(len(tputs)):
            csvwriter.writerow([tputs[i], lats[i]])


def generate_tput_lat_plot(config, plots_directory, plot_name, tputs, lats):
    plot_csv_file = os.path.join(plots_directory, '%s.csv' % plot_name)
    generate_csv_for_tput_lat_plot(plot_csv_file, tputs, lats)
    plot_script_file = os.path.join(plots_directory, '%s.gpi' % plot_name)
    generate_gnuplot_script_tput_lat(config, plot_script_file)
    run_gnuplot([plot_csv_file], os.path.join(plots_directory, '%s.png' % plot_name), plot_script_file)


def generate_tput_lat_plots(config, base_out_directory, exp_out_directories):
    plots_directory = os.path.join(base_out_directory, config['plot_directory_name'])
    os.makedirs(plots_directory, exist_ok=True)

    tputs = []
    lats = {}

    for i in range(len(exp_out_directories)):
        stats_file = os.path.join(exp_out_directories[i], STATS_FILE)
        if not os.path.exists(stats_file):
            continue
        with open(stats_file) as f:
            stats = json.load(f)

        combined_run_stats = stats.get('run_stats', {}).get('combined', None)
        if not combined_run_stats:
            continue

        if 'tput' in combined_run_stats and 'p50' in combined_run_stats['tput']:
            tputs.append(combined_run_stats['tput']['p50'])
        else:
            continue

        ignore = {'stddev': 1, 'var': 1, 'tput': 1, 'ops': 1}
        for lat_stat, lat in combined_run_stats.items():
            if lat_stat in ignore:
                continue
            lats.setdefault(lat_stat, []).append(lat['p50'])

    for lat_stat, lat in lats.items():
        plot_name = 'tput-%s-lat' % lat_stat
        generate_tput_lat_plot(config, plots_directory, plot_name, tputs, lat)


# -----------------------------
# Aggregation across subdirs (aggregate-* only)
# -----------------------------

def generate_gnuplot_script_cdf_log_agg(config, script_file):
    with open(script_file, 'w') as f:
        write_gpi_header(f)
        f.write("set key bottom right\n")
        f.write("set ytics (0,0.9,0.99,0.999,0.9999,1.0)\n")
        f.write("set xlabel '%s'\n" % config['plot_cdf_x_label'])
        f.write("set ylabel '%s'\n" % config['plot_cdf_y_label'])
        f.write("set terminal pngcairo size %d,%d enhanced dashed font '%s'\n" %
                (config['plot_cdf_png_width'], config['plot_cdf_png_height'], config['plot_cdf_png_font']))
        f.write('set output outfile\n')
        write_line_styles(f)
        f.write('plot ')
        for i in range(len(config['replication_protocol'])):
            labels = ':yticlabels(3)' if i == 0 else ''
            f.write("datafile%d using 1:(-log10(1-$2))%s title '%s' ls %d with lines" %
                    (i, labels, config['plot_cdf_series_title'][i].replace('_', '\\_'), i + 1))
            if i != len(config['replication_protocol']) - 1:
                f.write(', \\\n')


def generate_gnuplot_script_cdf_agg(config, script_file):
    with open(script_file, 'w') as f:
        write_gpi_header(f)
        f.write("set key bottom right\n")
        f.write("set xlabel '%s'\n" % config['plot_cdf_x_label'])
        f.write("set ylabel '%s'\n" % config['plot_cdf_y_label'])
        f.write("set terminal pngcairo size %d,%d enhanced dashed font '%s'\n" %
                (config['plot_cdf_png_width'], config['plot_cdf_png_height'], config['plot_cdf_png_font']))
        f.write('set output outfile\n')
        write_line_styles(f)
        f.write('plot ')
        for i in range(len(config['replication_protocol'])):
            f.write("datafile%d title '%s' ls %d with lines" %
                    (i, config['plot_cdf_series_title'][i].replace('_', '\\_'), i + 1))
            if i != len(config['replication_protocol']) - 1:
                f.write(', \\\n')


def generate_agg_cdf_plots(config, base_out_directory, sub_out_directories):
    plots_directory = os.path.join(base_out_directory, config['plot_directory_name'])
    os.makedirs(plots_directory, exist_ok=True)

    csv_files = {}
    for i in range(len(sub_out_directories)):
        for j in range(len(sub_out_directories[i])):
            sub_plot_directory = os.path.join(sub_out_directories[i][j], config['plot_directory_name'])
            if not os.path.isdir(sub_plot_directory):
                continue
            for f in os.listdir(sub_plot_directory):
                if f.endswith('.csv') and f.startswith('aggregate'):
                    csv_class = os.path.splitext(os.path.basename(f))[0]
                    csv_files.setdefault(csv_class, [])
                    while len(csv_files[csv_class]) <= j:
                        csv_files[csv_class].append([])
                    csv_files[csv_class][j].append(os.path.join(sub_plot_directory, f))

    for csv_class, file_lists in csv_files.items():
        for j in range(len(file_lists)):
            plot_script_file = os.path.join(plots_directory, '%s-%d.gpi' % (csv_class, j))
            if 'log' in csv_class:
                generate_gnuplot_script_cdf_log_agg(config, plot_script_file)
            else:
                generate_gnuplot_script_cdf_agg(config, plot_script_file)
            run_gnuplot(file_lists[j], os.path.join(plots_directory, '%s-%d.png' % (csv_class, j)), plot_script_file)


# -----------------------------
# regenerate_plots() (kept)
# -----------------------------

def regenerate_plots(config_file, exp_dir, executor, calc_stats=True):
    with open(config_file) as f:
        config = json.load(f)

    config.setdefault('client_stats_blacklist', [])
    config.setdefault('client_combine_stats_blacklist', [])
    config.setdefault('client_cdf_plot_blacklist', [])

    out_directories = sorted(next(os.walk(exp_dir))[1])
    if 'plots' in out_directories:
        out_directories.remove('plots')
    out_directories = [os.path.join(exp_dir, d) for d in out_directories]
    out_directories = out_directories[:len(config['replication_protocol'])]

    sub_out_directories = []
    for i in range(len(out_directories)):
        out_dir = out_directories[i]
        dirs = sorted(next(os.walk(out_dir))[1])
        if 'plots' in dirs:
            dirs.remove('plots')
        dirs = [os.path.join(out_dir, d, config['out_directory_name']) for d in dirs]

        config_new = config.copy()
        config_new['base_local_exp_directory'] = exp_dir
        server_replication_protocol = config['replication_protocol'][i]
        config_new['replication_protocol'] = server_replication_protocol
        config_new['plot_cdf_series_title'] = config['plot_cdf_series_title'][i]
        config_new['plot_tput_lat_series_title'] = config['plot_tput_lat_series_title'][i]
        config_new['replication_protocol_settings'] = config['replication_protocol_settings'][i]

        sub_out_directories.append(dirs)

        for j in range(len(dirs)):
            sub_out_dir = dirs[j]
            config_new_new = config_new.copy()
            config_new_new['base_local_exp_directory'] = exp_dir
            n = config_new['client_nodes_per_server'][j]
            m = config_new['client_processes_per_client_node'][j]
            config_new_new['client_nodes_per_server'] = n
            config_new_new['client_processes_per_client_node'] = m

            if calc_stats:
                stats, op_latencies, op_times, client_op_latencies, client_op_times = calculate_statistics(
                    config_new_new, sub_out_dir
                )
            else:
                with open(os.path.join(sub_out_dir, STATS_FILE)) as sf:
                    stats = json.load(sf)

            generate_cdf_plots(config_new_new, sub_out_dir, stats, executor)

        generate_tput_lat_plots(config_new, out_dir, dirs)

    generate_agg_cdf_plots(config, exp_dir, sub_out_directories)
