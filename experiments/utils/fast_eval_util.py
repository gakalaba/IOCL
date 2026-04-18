import os
import json
import random
import numpy
import csv
import subprocess

STATS_FILE = 'stats.json'
LATENCY_STATS_TO_PLOT = ['p50', 'p90', 'p95', 'p99']


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


def ensure_plots_directory(base_directory, config):
    plots_directory = os.path.join(base_directory, config['plot_directory_name'])
    os.makedirs(plots_directory, exist_ok=True)
    return plots_directory


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
            for j in range(len(run_op_latencies[k])):
                if k in op_latencies:
                    if len(op_latencies[k]) <= j:
                        op_latencies[k].append(run_op_latencies[k][j])
                    else:
                        op_latencies[k][j] += run_op_latencies[k][j]
                else:
                    op_latencies[k] = [run_op_latencies[k][j]]
            if k in op_latency_counts:
                op_latency_counts[k] += vv
            else:
                op_latency_counts[k] = vv

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
            if k in run_op_tputs:
                if k in op_tputs:
                    op_tputs[k] += run_op_tputs[k]
                else:
                    op_tputs[k] = run_op_tputs[k]

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
    ignored = {'cdf': 1, 'cdf_log': 1, 'time': 1}

    for cat in runs[0]:
        if not ('region-' in cat) and type(runs[0][cat]) is dict:
            stats['run_stats'][cat] = {}
            for s in runs[0][cat]:
                if s in ignored:
                    continue
                data = []
                for run in runs:
                    data.append(run[cat][s])
                stats['run_stats'][cat][s] = calculate_statistics_for_data(data, cdf=False)

        if 'region-' in cat:
            stats['run_stats'][cat] = {}
            for cat2 in runs[0][cat]:
                if type(runs[0][cat][cat2]) is dict:
                    stats['run_stats'][cat][cat2] = {}
                    for s in runs[0][cat][cat2]:
                        if s in ignored:
                            continue
                        data = []
                        for run in runs:
                            data.append(run[cat][cat2][s])
                        stats['run_stats'][cat][cat2][s] = calculate_statistics_for_data(
                            data, cdf=False)

    stats_file = STATS_FILE if 'stats_file_name' not in config else config['stats_file_name']
    full_stats_path = os.path.join(local_out_directory, stats_file)
    print("WRITING STATS FILE TO:", full_stats_path)
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

        for client in config["clients"]:
            if get_region(config, client) != region:
                continue

            client_dir = client
            for k in range(config["client_processes_per_client_node"]):
                client_out_file = os.path.join(
                    local_out_directory,
                    client_dir,
                    '%s-%d-stdout-%d.log' % (client, k, run)
                )

                end_time_sec = {}
                end_time_usec = {}

                with open(client_out_file) as f:
                    ops = f.readlines()
                    foundEnd = False

                    for op in ops:
                        foundEnd = False
                        opCols = op.strip().split(',')

                        for x in range(0, len(opCols), 2):
                            if opCols[x].isdigit():
                                break

                            if len(opCols[x]) > 0 and opCols[x][0] == '#':
                                if opCols[x] == '#start':
                                    break
                                elif opCols[x] == '#end':
                                    cid = 0
                                    if x + 3 < len(opCols):
                                        cid = int(opCols[x+3])
                                    end_time_sec[cid] = float(opCols[x+1])
                                    end_time_usec[cid] = float(opCols[x+2])
                                    foundEnd = True
                                    break

                            if not opCols[x] in config['client_stats_blacklist']:
                                if 'input_latency_scale' in config:
                                    opLat = float(opCols[x+1]) / config['input_latency_scale']
                                else:
                                    opLat = float(opCols[x+1]) / 1e9

                                if 'output_latency_scale' in config:
                                    opLat = opLat * config['output_latency_scale']
                                else:
                                    opLat = opLat * 1e3

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

                                cid = 0
                                if x + 3 < len(opCols):
                                    cid = int(opCols[x + 3])

                                if cid not in op_latencies:
                                    op_latencies[cid] = {}
                                if cid not in op_times:
                                    op_times[cid] = {}
                                if cid not in op_latency_counts:
                                    op_latency_counts[cid] = {}

                                if opCols[x] in op_latencies[cid]:
                                    op_latencies[cid][opCols[x]].append(opLat)
                                else:
                                    op_latencies[cid][opCols[x]] = [opLat]

                                if opCols[x] in op_times[cid]:
                                    op_times[cid][opCols[x]].append(opTime)
                                else:
                                    op_times[cid][opCols[x]] = [opTime]

                                if not opCols[x] in config['client_combine_stats_blacklist']:
                                    if 'combined' in op_latencies[cid]:
                                        op_latencies[cid]['combined'].append(opLat)
                                    else:
                                        op_latencies[cid]['combined'] = [opLat]

                                    if 'combined' in op_times[cid]:
                                        op_times[cid]['combined'].append(opTime)
                                    else:
                                        op_times[cid]['combined'] = [opTime]

                                    if 'combined' in op_latency_counts[cid]:
                                        op_latency_counts[cid]['combined'] += 1
                                    else:
                                        op_latency_counts[cid]['combined'] = 1

                                if 'client_combine_ro_ops' in config:
                                    if opCols[x] in config['client_combine_ro_ops']:
                                        rorw = 'ro'
                                    else:
                                        rorw = 'rw'

                                    if rorw in op_latencies[cid]:
                                        op_latencies[cid][rorw].append(opLat)
                                    else:
                                        op_latencies[cid][rorw] = [opLat]

                                    if rorw in op_times[cid]:
                                        op_times[cid][rorw].append(opTime)
                                    else:
                                        op_times[cid][rorw] = [opTime]

                                    if rorw in op_latency_counts[cid]:
                                        op_latency_counts[cid][rorw] += 1
                                    else:
                                        op_latency_counts[cid][rorw] = 1

                                if opCols[x] in op_latency_counts[cid]:
                                    op_latency_counts[cid][opCols[x]] += 1
                                else:
                                    op_latency_counts[cid][opCols[x]] = 1

                        if foundEnd:
                            run_time_sec = end_time_sec[cid] + end_time_usec[cid] / 1e6
                            if cid in op_latency_counts:
                                for k1, v in op_latency_counts[cid].items():
                                    print('Client %s-%d %d tput %s is %f (%d / %f)' % (
                                        client, k, cid, k1, v / run_time_sec, v, run_time_sec))
                                    if k1 in op_tputs:
                                        op_tputs[k1] += v / run_time_sec
                                    else:
                                        op_tputs[k1] = v / run_time_sec

                client_stats_file = os.path.join(
                    local_out_directory,
                    client_dir,
                    '%s-%d-stats-%d.json' % (client, k, run)
                )
                try:
                    with open(client_stats_file) as f:
                        client_stats = json.load(f)
                        for k1, v in client_stats.items():
                            if (not 'stats_merge_lists' in config) or (not k1 in config['stats_merge_lists']):
                                if k1 not in stats:
                                    stats[k1] = v
                                else:
                                    stats[k1] += v
                            else:
                                if k1 not in stats:
                                    stats[k1] = v
                                else:
                                    if len(stats[k1]) < len(v):
                                        for uu in range(len(stats[k1]), len(v)):
                                            stats[k1].append(0)
                                    for uu in range(len(v)):
                                        stats[k1][uu] += v[uu]
                except FileNotFoundError:
                    print('No stats file %s.' % client_stats_file)
                except json.decoder.JSONDecodeError:
                    print('Invalid JSON file %s.' % client_stats_file)

        for instance_idx in range(config["num_instances"]):
            for shard_idx in range(len(config["shards"])):
                shard = config["shards"][shard_idx]
                for replica_idx in range(len(shard)):
                    replica = shard[replica_idx]
                    if get_region(config, replica) != region:
                        continue

                    server_stats_file = os.path.join(
                        local_out_directory,
                        'server-%d-%d' % (instance_idx, shard_idx),
                        'server-%d-%d-%d-stats-%d.json' % (instance_idx, shard_idx, replica_idx, run)
                    )
                    print(server_stats_file)
                    try:
                        with open(server_stats_file) as f:
                            server_stats = json.load(f)
                            for k, v in server_stats.items():
                                if not type(v) is dict:
                                    if (not 'stats_merge_lists' in config) or (not k in config['stats_merge_lists']):
                                        if k not in stats:
                                            stats[k] = v
                                        else:
                                            stats[k] += v
                                    else:
                                        if k not in stats:
                                            stats[k] = v
                                        else:
                                            if len(stats[k]) < len(v):
                                                for uu in range(len(stats[k]), len(v)):
                                                    stats[k].append(0)
                                            for uu in range(len(v)):
                                                stats[k][uu] += v
                    except FileNotFoundError:
                        print('No stats file %s.' % server_stats_file)
                    except json.decoder.JSONDecodeError:
                        print('Invalid JSON file %s.' % server_stats_file)

        for cid, opl in op_latencies.items():
            for k, v in opl.items():
                if k in r_op_latencies:
                    r_op_latencies[k].extend(v)
                else:
                    r_op_latencies[k] = v.copy()
            region_client_op_latencies[cid] = opl

        for cid, opt in op_times.items():
            for k, v in opt.items():
                if k in r_op_times:
                    r_op_times[k].extend(v)
                else:
                    r_op_times[k] = v.copy()
            region_client_op_times[cid] = opt

        for cid, oplc in op_latency_counts.items():
            for k, v in oplc.items():
                if k in r_op_latency_counts:
                    r_op_latency_counts[k] += v
                else:
                    r_op_latency_counts[k] = v

        for k, v in op_tputs.items():
            if k in r_op_tputs:
                r_op_tputs[k] += v
            else:
                r_op_tputs[k] = v

        for k, v in r_op_latencies.items():
            if k in region_op_latencies:
                region_op_latencies[k].append(v)
            else:
                region_op_latencies[k] = [v]

        for k, v in r_op_times.items():
            if k in region_op_times:
                region_op_times[k].append(v)
            else:
                region_op_times[k] = [v]

        for k, v in r_op_latency_counts.items():
            if k in region_op_latency_counts:
                region_op_latency_counts[k] = min(region_op_latency_counts[k], v)
            else:
                region_op_latency_counts[k] = v

        for k, v in r_op_tputs.items():
            if k in region_op_tputs:
                region_op_tputs[k].append(v)
            else:
                region_op_tputs[k] = [v]

    if 'fast_writes_0' in stats or 'slow_writes_0' in stats or 'fast_reads_0' in stats or 'slow_reads_0' in stats:
        fw0 = stats['fast_writes_0'] if 'fast_writes_0' in stats else 0
        sw0 = stats['slow_writes_0'] if 'slow_writes_0' in stats else 0
        if fw0 + sw0 > 0:
            stats['fast_write_ratio'] = fw0 / (fw0 + sw0)
            stats['slow_write_ratio'] = sw0 / (fw0 + sw0)
        fr0 = stats['fast_reads_0'] if 'fast_reads_0' in stats else 0
        sr0 = stats['slow_reads_0'] if 'slow_reads_0' in stats else 0
        if fr0 + sr0 > 0:
            stats['fast_read_ratio'] = fr0 / (fr0 + sr0)
            stats['slow_read_ratio'] = sr0 / (fr0 + sr0)

    total_committed = 0
    total_attempts = 0
    stats_new = {}
    for k, v in stats.items():
        if k.endswith('_committed'):
            k_prefix = k[:-len('_committed')]
            k_attempts = k_prefix + '_attempts'
            k_commit_rate = k_prefix + '_commit_rate'
            k_abort_rate = k_prefix + '_abort_rate'
            total_committed += stats[k]
            total_attempts += stats[k_attempts]
            stats_new[k_commit_rate] = stats[k] / stats[k_attempts]
            stats_new[k_abort_rate] = 1 - stats_new[k_commit_rate]

    for k, v in stats_new.items():
        stats[k] = v

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
        stats[op_type] = calculate_statistics_for_data(latencies, cdf=False)
        stats[op_type]['ops'] = len(latencies)
        if tput == -1:
            stats[op_type]['tput'] = len(latencies) / total_recorded_time
        else:
            stats[op_type]['old_tput'] = len(latencies) / total_recorded_time
            stats[op_type]['tput'] = tput
        if op_type == 'combined':
            stats['combined']['ops'] = len(latencies)
            stats['combined']['time'] = total_recorded_time


def calculate_all_op_statistics(config, stats, region_op_latencies, region_op_times, region_op_latency_counts, region_op_tputs):
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

        for i in range(len(v)):
            sample_idxs = random.sample(range(len(v[i])), region_op_latency_counts[k])
            if k in norm_op_times:
                norm_op_latencies[k].extend([v[i][idx] for idx in sample_idxs])
                norm_op_times[k].extend([region_op_times[k][i][idx] for idx in sample_idxs])
            else:
                norm_op_latencies[k] = [v[i][idx] for idx in sample_idxs]
                norm_op_times[k] = [region_op_times[k][i][idx] for idx in sample_idxs]

        if not 'server_emulate_wan' in config or config['server_emulate_wan']:
            for i in range(len(v)):
                region_key = 'region-%d' % i
                if region_key not in stats:
                    stats[region_key] = {}
                op_tput = -1
                if k in region_op_tputs and len(region_op_tputs[k]) > i:
                    op_tput = region_op_tputs[k][i]
                calculate_op_statistics(config, stats[region_key], total_recorded_time, k, v[i], [], op_tput)

        calculate_op_statistics(config, stats, total_recorded_time, k, latencies, norm_op_latencies[k], tput)

    return norm_op_latencies, norm_op_times


def calculate_statistics_for_data(data, cdf=False, cdf_log_precision=4):
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
    return s


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

def ensure_plots_directory(base_directory, config):
    plots_directory = os.path.join(base_directory, config['plot_directory_name'])
    os.makedirs(plots_directory, exist_ok=True)
    return plots_directory

def get_stats_file_name(config):
    return config['stats_file_name'] if 'stats_file_name' in config else STATS_FILE

def generate_csv_for_tput_lat_plot(plot_csv_file, tputs, lats):
    if len(tputs) == 0 or len(lats) == 0:
        return

    pairs = sorted(zip(tputs, lats), key=lambda x: x[0])
    with open(plot_csv_file, 'w') as f:
        csvwriter = csv.writer(f)
        for tput, lat in pairs:
            csvwriter.writerow([tput, lat])



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


def generate_gnuplot_script_tput_lat_agg(config, plot_script_file):
    with open(plot_script_file, 'w') as f:
        write_gpi_header(f)
        f.write("set key top left\n")
        f.write("set xlabel '%s'\n" % config['plot_tput_lat_x_label'])
        f.write("set ylabel '%s'\n" % config['plot_tput_lat_y_label'])
        f.write("set terminal pngcairo size %d,%d enhanced dashed font '%s'\n" %
                (config['plot_tput_lat_png_width'],
                 config['plot_tput_lat_png_height'],
                 config['plot_tput_lat_png_font']))
        f.write('set output outfile\n')
        write_line_styles(f)
        f.write('plot ')
        for i in range(len(config['replication_protocol'])):
            # Pull series title from new config schema if present
            series_title = None
            if "plots" in config and len(config["plots"]) > 0:
                plot_cfg = config["plots"][0]
                if "series_titles" in plot_cfg and i < len(plot_cfg["series_titles"]):
                    series_title = plot_cfg["series_titles"][i]
            # fallback to legacy field
            if series_title is None:
                series_title = config['plot_tput_lat_series_title'][i]
            f.write("datafile%d title '%s' ls %d with linespoint" % (
                i, series_title.replace('_', '\\_'), i + 1))
            if i != len(config['replication_protocol']) - 1:
                f.write(', \\\n')

def generate_tput_lat_plot(config, plots_directory, plot_name, tputs, lats):
    print("GENERATING TPUT-LAT PLOTS IN:", plots_directory)
    if len(tputs) == 0 or len(lats) == 0:
        print("Skipping plot %s because it has no data." % plot_name)
        return

    plot_csv_file = os.path.join(plots_directory, '%s.csv' % plot_name)
    generate_csv_for_tput_lat_plot(plot_csv_file, tputs, lats)

    if not os.path.exists(plot_csv_file) or os.path.getsize(plot_csv_file) == 0:
        print("Skipping plot %s because csv was empty." % plot_name)
        return

    plot_script_file = os.path.join(plots_directory, '%s.gpi' % plot_name)
    generate_gnuplot_script_tput_lat(config, plot_script_file)

    plot_out_file = os.path.join(plots_directory, '%s.png' % plot_name)
    run_gnuplot([plot_csv_file], plot_out_file, plot_script_file)

def generate_tput_lat_plots(config, base_out_directory, exp_out_directories):
    plots_directory = ensure_plots_directory(base_out_directory, config)
    stats_file_name = config['stats_file_name'] if 'stats_file_name' in config else STATS_FILE

    if isinstance(exp_out_directories, dict):
        exp_out_directories = [exp_out_directories[k] for k in sorted(exp_out_directories.keys())]

    tputs = []
    lats = {k: [] for k in LATENCY_STATS_TO_PLOT}

    for exp_out_directory in exp_out_directories:
        if not isinstance(exp_out_directory, str):
            print("Skipping non-string exp_out_directory:", exp_out_directory)
            continue

        stats_file = os.path.join(exp_out_directory, stats_file_name)
        print("Reading stats for tput-lat:", stats_file)

        if not os.path.exists(stats_file):
            print("Missing stats file:", stats_file)
            continue

        with open(stats_file) as f:
            stats = json.load(f)

        if 'run_stats' not in stats or 'combined' not in stats['run_stats']:
            print("Missing run_stats/combined in", stats_file)
            continue

        combined_run_stats = stats['run_stats']['combined']
        if 'tput' not in combined_run_stats or 'p50' not in combined_run_stats['tput']:
            print("Missing tput p50 in", stats_file)
            continue

        missing = False
        lat_vals = {}
        for lat_stat in LATENCY_STATS_TO_PLOT:
            if lat_stat not in combined_run_stats or 'p50' not in combined_run_stats[lat_stat]:
                print("Missing %s p50 in %s" % (lat_stat, stats_file))
                missing = True
                break
            lat_vals[lat_stat] = combined_run_stats[lat_stat]['p50']

        if missing:
            continue

        tputs.append(combined_run_stats['tput']['p50'])
        for lat_stat in LATENCY_STATS_TO_PLOT:
            lats[lat_stat].append(lat_vals[lat_stat])

    print("Collected %d throughput points for %s" % (len(tputs), plots_directory))

    for lat_stat in LATENCY_STATS_TO_PLOT:
        plot_name = 'tput-%s-lat' % lat_stat
        generate_tput_lat_plot(config, plots_directory, plot_name, tputs, lats[lat_stat])

def generate_agg_tput_lat_plots(config, base_out_directory, out_directories):
    plots_directory = ensure_plots_directory(base_out_directory, config)

    csv_files = {}
    allowed = set(['tput-p50-lat', 'tput-p90-lat', 'tput-p95-lat', 'tput-p99-lat'])

    for out_directory in out_directories:
        sub_plot_directory = os.path.join(out_directory, config['plot_directory_name'])
        print("Scanning protocol plot dir:", sub_plot_directory)

        if not os.path.isdir(sub_plot_directory):
            print("Missing protocol plot dir:", sub_plot_directory)
            continue

        for f in os.listdir(sub_plot_directory):
            if not f.endswith('.csv'):
                continue

            csv_class = os.path.splitext(os.path.basename(f))[0]
            if csv_class not in allowed:
                continue

            if csv_class not in csv_files:
                csv_files[csv_class] = []
            csv_files[csv_class].append(os.path.join(sub_plot_directory, f))

    for csv_class, files in csv_files.items():
        if len(files) == 0:
            continue

        plot_script_file = os.path.join(plots_directory, '%s.gpi' % csv_class)
        generate_gnuplot_script_tput_lat_agg(config, plot_script_file)

        plot_out_file = os.path.join(plots_directory, '%s.png' % csv_class)
        run_gnuplot(files, plot_out_file, plot_script_file)

def _collect_leaf_out_dirs(x):
    """
    Recursively collect strings that look like leaf experiment out directories.
    """
    results = []
    if isinstance(x, str):
        results.append(x)
    elif isinstance(x, (list, tuple)):
        for item in x:
            results.extend(_collect_leaf_out_dirs(item))
    return results


def generate_plots(config, base_out_directory, out_dirs):
    """
    Preserve old interface, but only generate tput-lat plots.

    Expected old caller shape:
        out_dirs = [sub_out_dirs, out_dirs]

    However sub_out_dirs can be nested recursively, so flatten it.
    """
    ensure_plots_directory(base_out_directory, config)

    print("GENERATE_PLOTS base_out_directory =", base_out_directory)
    print("GENERATE_PLOTS out_dirs =", out_dirs)

    if not isinstance(out_dirs, list) or len(out_dirs) < 2:
        print("generate_plots: unexpected out_dirs shape, skipping")
        return

    nested_subdirs = out_dirs[0]
    protocol_dirs = out_dirs[1]

    if not protocol_dirs:
        print("generate_plots: no protocol dirs")
        return

    # Populate each protocol/plots/ from its leaf out/ stats files.
    for i, protocol_dir in enumerate(protocol_dirs):
        if i >= len(nested_subdirs):
            print("generate_plots: missing leaf dirs for protocol", protocol_dir)
            continue

        leaf_out_dirs = _collect_leaf_out_dirs(nested_subdirs[i])

        print("generate_plots: protocol_dir =", protocol_dir)
        print("generate_plots: raw leaf structure =", nested_subdirs[i])
        print("generate_plots: flattened leaf_out_dirs =", leaf_out_dirs)

        if not leaf_out_dirs:
            print("generate_plots: no leaf out dirs for protocol", protocol_dir)
            continue

        config_new = config.copy()
        config_new['base_local_exp_directory'] = base_out_directory
        config_new['replication_protocol'] = config['replication_protocol'][i]
        # Pull series title from new config schema if present
        series_title = None
        if "plots" in config and len(config["plots"]) > 0:
            plot_cfg = config["plots"][0]
            if "series_titles" in plot_cfg and i < len(plot_cfg["series_titles"]):
                series_title = plot_cfg["series_titles"][i]

        # fallback to legacy field
        if series_title is None:
            series_title = config['plot_tput_lat_series_title'][i]
        config_new['plot_tput_lat_series_title'] = series_title

        generate_tput_lat_plots(config_new, protocol_dir, leaf_out_dirs)

    # Populate top-level experiment/plots/ from protocol/plots/*.csv
    generate_agg_tput_lat_plots(config, base_out_directory, protocol_dirs)


def generate_cdf_plots(config, local_out_directory, stats, executor):
    ensure_plots_directory(local_out_directory, config)
    return


def generate_ot_plots(config, local_out_directory, stats, op_latencies, op_times, client_op_latencies, client_op_times, executor):
    ensure_plots_directory(local_out_directory, config)
    return


def generate_agg_cdf_plots(config, base_out_directory, sub_out_directories):
    ensure_plots_directory(base_out_directory, config)
    return


def generate_tail_at_scale_plots(config, base_out_directory, sub_out_directories):
    ensure_plots_directory(base_out_directory, config)
    return


def regenerate_plots(config_file, exp_dir, executor, calc_stats=True):
    with open(config_file) as f:
        config = json.load(f)

        if not 'client_stats_blacklist' in config:
            config['client_stats_blacklist'] = []
        if not 'client_combine_stats_blacklist' in config:
            config['client_combine_stats_blacklist'] = []
        if not 'client_cdf_plot_blacklist' in config:
            config['client_cdf_plot_blacklist'] = []

        out_directories = sorted(next(os.walk(exp_dir))[1])
        if 'plots' in out_directories:
            out_directories.remove('plots')
        out_directories = [os.path.join(exp_dir, d) for d in out_directories]
        out_directories = out_directories[:len(config['replication_protocol'])]

        for i in range(len(out_directories)):
            out_dir = out_directories[i]
            dirs = sorted(next(os.walk(out_dir))[1])
            if 'plots' in dirs:
                dirs.remove('plots')
            dirs = [os.path.join(out_dir, d, config['out_directory_name']) for d in dirs]

            config_new = config.copy()
            config_new['base_local_exp_directory'] = exp_dir
            config_new['replication_protocol'] = config['replication_protocol'][i]
            # Pull series title from new config schema if present
            series_title = None
            if "plots" in config and len(config["plots"]) > 0:
                plot_cfg = config["plots"][0]
                if "series_titles" in plot_cfg and i < len(plot_cfg["series_titles"]):
                    series_title = plot_cfg["series_titles"][i]

            # fallback to legacy field
            if series_title is None:
                series_title = config['plot_tput_lat_series_title'][i]
            config_new['plot_tput_lat_series_title'] = series_title

            if calc_stats:
                for j in range(len(dirs)):
                    sub_out_dir = dirs[j]
                    config_new_new = config_new.copy()
                    config_new_new['base_local_exp_directory'] = exp_dir
                    n = config_new['client_nodes_per_server'][j]
                    m = config_new['client_processes_per_client_node'][j]
                    config_new_new['client_nodes_per_server'] = n
                    config_new_new['client_processes_per_client_node'] = m
                    calculate_statistics(config_new_new, sub_out_dir)

            generate_tput_lat_plots(config_new, out_dir, dirs)

        generate_agg_tput_lat_plots(config, exp_dir, out_directories)


def generate_varying_write_csvs(config_file, exp_dir, calc_stats=True):
    return


def regenerate_tail_at_scale_plots(config_file, exp_dir):
    return