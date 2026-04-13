#!/usr/bin/env python3
"""
parse_stats.py  <exp_dir>

Parses instrumentation stderr log lines produced by the C++ Notice() calls
added to app_request.cc, craq/replica.cc, and iocl_craq/replica.cc, then
writes two summary files inside each per-protocol run directory found under
<exp_dir>:

  <proto_run_dir>/per_client_stats.txt  -- batch composition + clean/dirty read counts per client
  <proto_run_dir>/per_tail_stats.txt    -- tail batch depth + dirty-read depth per shard

Expected log patterns:

  Client stderr (app_request.cc):
    AppRequest batch composition: fanout=16 reads=9 writes=7

  Server stderr, TAIL replica (craq/replica.cc or iocl_craq/replica.cc):
    [2] Tail batch: size=3 pending_ooo=0 avg_batch_size=2.10 total_ops=847

  Server stderr, MIDDLE replica — dumped once at Close() (NOT per-read):
    [1] ReadStatsSummary clean=244 dirty=44 total=288
    [1] ReadStatsDepth depth=1 count=42
    [1] ReadStatsClient client=9 clean=200 dirty=44

Directory layout assumed:
  <exp_dir>/
    <proto_run_dir>/            <- e.g. iocl_craq-0/, craq-0/, vr-0/
      plots/
      *.json
      <timestamp>/
        out/
          client-{node}/
            client-{node}-{proc}-stderr-{run}.log
          server-{proto}-{shard}/
            server-{proto}-{shard}-{replica}-stderr-{run}.log
"""

import re
import os
import sys
import glob
from collections import defaultdict
from statistics import mean, median


BATCH_RE = re.compile(
    r'AppRequest batch composition: fanout=(\d+) reads=(\d+) writes=(\d+)'
)
TAIL_RE = re.compile(
    r'\[(\d+)\] Tail batch: size=(\d+) pending_ooo=(\d+) '
    r'avg_batch_size=([\d.]+) total_ops=(\d+)'
)
# Summary-dump format (one line per replica at Close())
SUMMARY_RE = re.compile(
    r'\[(\d+)\] ReadStatsSummary clean=(\d+) dirty=(\d+) total=(\d+)'
)
DEPTH_RE = re.compile(
    r'\[(\d+)\] ReadStatsDepth depth=(\d+) count=(\d+)'
)
CLIENT_RE = re.compile(
    r'\[(\d+)\] ReadStatsClient client=(\d+) clean=(\d+) dirty=(\d+)'
)
READ_TIMELINE_RE = re.compile(
    r'\[(\d+)\] ReadTimeline pos=(\d+) vr=(\d+) '
    r'coord_wait_ms=(\d+) vr_wait_ms=(\d+) ready_wait_ms=(\d+) total_ms=(\d+)'
)
APPREQ_SEND_RE = re.compile(
    r'SendOperation on AppRequest\[(\d+)\]:\s+([A-Za-z_]+)\('
)
APPREQ_REPLY_TIMELINE_RE = re.compile(
    r'AppRequest reply timeline: pos=(\d+) status=(\d+) issue_to_reply_ms=(\d+) batch_age_ms=(\d+)'
)
APPREQ_BATCH_DONE_RE = re.compile(
    r'AppRequest batch done: fanout=(\d+) total_reply_ms=(\d+)'
)
SHARD_SEND_RE = re.compile(
    r'\[shard (\d+)\] AppReqiest Sending Operation ([A-Za-z_]+)\('
)
BUFFER_RE = re.compile(
    r'Buffering read shardtag (\d+) waiting for CoordResponses \((\d+)/(\d+)\)'
)
UNBLOCK_RE = re.compile(
    r'Unblocking read shardtag (\d+) \((\d+)/(\d+) CoordResponses\)'
)
LOG_TS_RE = re.compile(r'^(\d{8})-(\d{6})-(\d{4})')


def summarize(values, unit=''):
    if not values:
        return 'n/a'
    s = sorted(values)
    n = len(s)
    p95_idx = max(0, int(n * 0.95) - 1)
    return (
        f'n={n}  avg={mean(values):.2f}{unit}  '
        f'min={s[0]}{unit}  p50={s[n//2]}{unit}  '
        f'p95={s[p95_idx]}{unit}  max={s[-1]}{unit}'
    )


def histogram(values, buckets=8):
    """Return a compact ASCII histogram string."""
    if not values:
        return ''
    mn, mx = min(values), max(values)
    if mn == mx:
        return f'  all={mn} (n={len(values)})'
    step = max(1, (mx - mn + buckets - 1) // buckets)
    counts = defaultdict(int)
    for v in values:
        counts[(v - mn) // step] += 1
    lines = []
    for b in sorted(counts):
        lo = mn + b * step
        hi = lo + step - 1
        bar = '#' * min(40, counts[b])
        lines.append(f'  {lo:>6}-{hi:<6}  {bar} ({counts[b]})')
    return '\n'.join(lines)


def histogram_from_dict(depth_dict):
    """Return ASCII histogram string from {depth: count} dict."""
    if not depth_dict:
        return ''
    mn, mx = min(depth_dict), max(depth_dict)
    if mn == mx:
        total = sum(depth_dict.values())
        return f'  all={mn} (n={total})'
    lines = []
    for depth in sorted(depth_dict):
        bar = '#' * min(40, depth_dict[depth])
        lines.append(f'  {depth:>6}-{depth:<6}  {bar} ({depth_dict[depth]})')
    return '\n'.join(lines)


def parse_log_ts_ms(line):
    m = LOG_TS_RE.match(line)
    if not m:
        return None
    hhmmss = m.group(2)
    sub_ms = int(m.group(3))
    hour = int(hhmmss[0:2])
    minute = int(hhmmss[2:4])
    second = int(hhmmss[4:6])
    return ((hour * 60 + minute) * 60 + second) * 1000 + sub_ms


def open_output_file(out_path):
    try:
        return open(out_path, 'w'), out_path
    except PermissionError:
        root, ext = os.path.splitext(out_path)
        fallback_path = f'{root}.regen{ext}'
        return open(fallback_path, 'w'), fallback_path


def find_run_dirs(exp_dir):
    """
    Find all per-protocol run directories under exp_dir.

    A run dir is a timestamped directory (e.g. 2026-04-12-12-34-44) that
    contains an 'out/' subdirectory.  We walk up to 3 levels deep.

    Returns a list of absolute paths to run dirs.
    """
    run_dirs = []
    # Pattern: exp_dir/<anything>/<timestamp>/out  (2 levels deep)
    # or:      exp_dir/<timestamp>/out              (1 level deep)
    for pattern in [
        os.path.join(exp_dir, '*/[0-9][0-9][0-9][0-9]-*', 'out'),
        os.path.join(exp_dir, '[0-9][0-9][0-9][0-9]-*', 'out'),
        os.path.join(exp_dir, '**/[0-9][0-9][0-9][0-9]-*', 'out'),
    ]:
        for out_path in glob.glob(pattern, recursive=True):
            run_dir = os.path.dirname(out_path)
            if run_dir not in run_dirs:
                run_dirs.append(run_dir)
    return sorted(set(run_dirs))


def parse_client_logs(run_dir):
    """
    Returns:
      {client_label: {
          'reads': [], 'writes': [],
          'shard_counts': {shard: count},
          'batch_sequences': [[shard, ...], ...],
          'adjacent_repeat_counts': [],
          'unique_shards_per_batch': [],
          'reply_timeline': [sample, ...],
          'batch_total_reply_ms': [],
      }}
    """
    stats = defaultdict(lambda: {
        'reads': [],
        'writes': [],
        'shard_counts': defaultdict(int),
        'batch_sequences': [],
        'adjacent_repeat_counts': [],
        'unique_shards_per_batch': [],
        'reply_timeline': [],
        'batch_total_reply_ms': [],
    })
    pattern = os.path.join(run_dir, 'out/client-*/*-stderr-*.log')
    for path in sorted(glob.glob(pattern)):
        m = re.match(r'(client-\d+-\d+)-\d+-stderr', os.path.basename(path))
        if not m:
            continue
        label = m.group(1)
        pending_appreq_ops = []
        current_batch_reads = None
        current_batch_writes = None
        current_batch_total = 0
        current_batch_seq = []

        def flush_batch():
            nonlocal current_batch_reads, current_batch_writes, current_batch_total, current_batch_seq
            if current_batch_total > 0 and len(current_batch_seq) == current_batch_total:
                stats[label]['batch_sequences'].append(list(current_batch_seq))
                repeats = 0
                for idx in range(1, len(current_batch_seq)):
                    if current_batch_seq[idx] == current_batch_seq[idx - 1]:
                        repeats += 1
                stats[label]['adjacent_repeat_counts'].append(repeats)
                stats[label]['unique_shards_per_batch'].append(len(set(current_batch_seq)))
            current_batch_reads = None
            current_batch_writes = None
            current_batch_total = 0
            current_batch_seq = []

        try:
            with open(path, encoding='utf-8', errors='replace') as f:
                for line in f:
                    bm = BATCH_RE.search(line)
                    if bm:
                        flush_batch()
                        stats[label]['reads'].append(int(bm.group(2)))
                        stats[label]['writes'].append(int(bm.group(3)))
                        current_batch_reads = int(bm.group(2))
                        current_batch_writes = int(bm.group(3))
                        current_batch_total = current_batch_reads + current_batch_writes
                        pending_appreq_ops.clear()
                        continue

                    am = APPREQ_SEND_RE.search(line)
                    if am:
                        pending_appreq_ops.append((int(am.group(1)), am.group(2).lower()))
                        continue

                    sm = SHARD_SEND_RE.search(line)
                    if sm:
                        shard = int(sm.group(1))
                        op = sm.group(2).lower()
                        stats[label]['shard_counts'][shard] += 1
                        if current_batch_total > 0:
                            matched_idx = None
                            for idx, (_, pending_op) in enumerate(pending_appreq_ops):
                                if pending_op == op:
                                    matched_idx = idx
                                    break
                            if matched_idx is None and pending_appreq_ops:
                                matched_idx = 0
                            if matched_idx is not None:
                                del pending_appreq_ops[matched_idx]
                            if len(current_batch_seq) < current_batch_total:
                                current_batch_seq.append(shard)
                            if len(current_batch_seq) == current_batch_total:
                                flush_batch()
                    rtm = APPREQ_REPLY_TIMELINE_RE.search(line)
                    if rtm:
                        stats[label]['reply_timeline'].append({
                            'pos': int(rtm.group(1)),
                            'status': int(rtm.group(2)),
                            'issue_to_reply_ms': int(rtm.group(3)),
                            'batch_age_ms': int(rtm.group(4)),
                        })
                    bdm = APPREQ_BATCH_DONE_RE.search(line)
                    if bdm:
                        stats[label]['batch_total_reply_ms'].append(int(bdm.group(2)))
                flush_batch()
        except OSError:
            pass
    return stats


def parse_server_logs(run_dir):
    """
    Returns:
      tail_stats:   {shard_label: {'batch_sizes': [], 'pending_ooo': [],
                                   'coord_wait_ms': [], 'queue_depth_samples': [],
                                   'max_queue_depth': int, 'buffered_reads': int,
                                   'unblocked_reads': int}}
      middle_stats: {shard_label: {'clean': int, 'dirty': int,
                                   'depth_hist': {depth: count},
                                   'by_client': {client_id: {'clean': int, 'dirty': int}},
                                   'read_timeline': [sample, ...]}}
    """
    tail_stats   = defaultdict(lambda: {
        'batch_sizes': [],
        'pending_ooo': [],
        'coord_wait_ms': [],
        'queue_depth_samples': [],
        'max_queue_depth': 0,
        'buffered_reads': 0,
        'unblocked_reads': 0,
    })
    middle_stats = defaultdict(lambda: {
        'clean': 0, 'dirty': 0,
        'depth_hist': defaultdict(int),
        'by_client': defaultdict(lambda: {'clean': 0, 'dirty': 0}),
        'read_timeline': [],
    })

    pattern = os.path.join(run_dir, 'out/server-*/*-stderr-*.log')
    for path in sorted(glob.glob(pattern)):
        fname = os.path.basename(path)
        m = re.match(r'server-(\d+)-(\d+)-(\d+)-stderr', fname)
        if not m:
            continue
        proto, shard, replica = int(m.group(1)), int(m.group(2)), int(m.group(3))
        shard_label = f'shard-{shard}'
        buffered_by_shardtag = {}
        queue_depth = 0
        try:
            with open(path, encoding='utf-8', errors='replace') as f:
                for line in f:
                    tm = TAIL_RE.search(line)
                    if tm and int(tm.group(1)) == replica:
                        tail_stats[shard_label]['batch_sizes'].append(int(tm.group(2)))
                        tail_stats[shard_label]['pending_ooo'].append(int(tm.group(3)))

                    sm = SUMMARY_RE.search(line)
                    if sm and int(sm.group(1)) == replica:
                        middle_stats[shard_label]['clean']  += int(sm.group(2))
                        middle_stats[shard_label]['dirty']  += int(sm.group(3))

                    dm = DEPTH_RE.search(line)
                    if dm and int(dm.group(1)) == replica:
                        depth = int(dm.group(2))
                        count = int(dm.group(3))
                        middle_stats[shard_label]['depth_hist'][depth] += count

                    cm = CLIENT_RE.search(line)
                    if cm and int(cm.group(1)) == replica:
                        cid   = int(cm.group(2))
                        clean = int(cm.group(3))
                        dirty = int(cm.group(4))
                        middle_stats[shard_label]['by_client'][cid]['clean'] += clean
                        middle_stats[shard_label]['by_client'][cid]['dirty'] += dirty

                    rtm = READ_TIMELINE_RE.search(line)
                    if rtm and int(rtm.group(1)) == replica:
                        middle_stats[shard_label]['read_timeline'].append({
                            'pos': int(rtm.group(2)),
                            'vr': int(rtm.group(3)),
                            'coord_wait_ms': int(rtm.group(4)),
                            'vr_wait_ms': int(rtm.group(5)),
                            'ready_wait_ms': int(rtm.group(6)),
                            'total_ms': int(rtm.group(7)),
                        })

                    qm = BUFFER_RE.search(line)
                    if qm and replica == 1:
                        shardtag = int(qm.group(1))
                        ts_ms = parse_log_ts_ms(line)
                        buffered_by_shardtag[shardtag] = ts_ms
                        queue_depth += 1
                        tail_stats[shard_label]['buffered_reads'] += 1
                        tail_stats[shard_label]['queue_depth_samples'].append(queue_depth)
                        tail_stats[shard_label]['max_queue_depth'] = max(
                            tail_stats[shard_label]['max_queue_depth'], queue_depth
                        )

                    um = UNBLOCK_RE.search(line)
                    if um and replica == 1:
                        shardtag = int(um.group(1))
                        ts_ms = parse_log_ts_ms(line)
                        start_ms = buffered_by_shardtag.pop(shardtag, None)
                        if start_ms is not None and ts_ms is not None and ts_ms >= start_ms:
                            tail_stats[shard_label]['coord_wait_ms'].append(ts_ms - start_ms)
                        tail_stats[shard_label]['unblocked_reads'] += 1
                        queue_depth = max(0, queue_depth - 1)
                        tail_stats[shard_label]['queue_depth_samples'].append(queue_depth)
        except OSError:
            pass

    return tail_stats, middle_stats


def write_client_stats(run_dir, client_stats, middle_stats):
    out_path = os.path.join(run_dir, 'per_client_stats.txt')

    # Build per-client_id clean/dirty totals from all shards.
    cid_reads = defaultdict(lambda: {'clean': 0, 'dirty': 0})
    for shard_data in middle_stats.values():
        for cid, counts in shard_data['by_client'].items():
            cid_reads[cid]['clean'] += counts['clean']
            cid_reads[cid]['dirty'] += counts['dirty']

    f, final_path = open_output_file(out_path)
    with f:
        if not client_stats and not cid_reads:
            f.write('No AppRequest batch composition or clean/dirty read lines found.\n')
            f.write('Make sure the binary was built with the instrumentation.\n')
            print(f'Wrote {final_path} (no data)')
            return

        f.write('=== Per-Client Batch Composition ===\n')
        f.write('(reads/writes: ops decided per fanout batch at issue time)\n')
        f.write('(clean/dirty: read outcome at MIDDLE, aggregated across all shards)\n\n')

        all_reads  = []
        all_writes = []
        for label in sorted(client_stats):
            d = client_stats[label]
            reads, writes = d['reads'], d['writes']
            all_reads.extend(reads)
            all_writes.extend(writes)
            f.write(f'[{label}]\n')
            f.write(f'  batches : {len(reads)}\n')
            f.write(f'  reads   : {summarize(reads)}\n')
            f.write(f'  writes  : {summarize(writes)}\n')
            f.write(f'  reads histogram:\n{histogram(reads)}\n')
            shard_counts = d['shard_counts']
            if shard_counts:
                f.write('  shard issue counts:\n')
                for shard in sorted(shard_counts):
                    f.write(f'    shard={shard:<3} count={shard_counts[shard]}\n')
            if d['batch_sequences']:
                f.write(f'  unique shards / batch : {summarize(d["unique_shards_per_batch"])}\n')
                f.write(f'  adjacent repeats      : {summarize(d["adjacent_repeat_counts"])}\n')
                repeat_hist = defaultdict(int)
                for repeats in d['adjacent_repeat_counts']:
                    repeat_hist[repeats] += 1
                f.write(f'  adjacent-repeat histogram:\n{histogram_from_dict(repeat_hist)}\n')
                combo_counts = defaultdict(int)
                for seq in d['batch_sequences']:
                    combo_counts[tuple(seq)] += 1
                top_combos = sorted(combo_counts.items(), key=lambda kv: (-kv[1], kv[0]))[:10]
                if top_combos:
                    f.write('  top shard combinations:\n')
                    for seq, count in top_combos:
                        seq_str = ','.join(str(s) for s in seq)
                        f.write(f'    [{seq_str}]  count={count}\n')
            if d['reply_timeline']:
                f.write(f'  batch total reply ms : {summarize(d["batch_total_reply_ms"], unit="ms")}\n')
                by_pos = defaultdict(list)
                for sample in d['reply_timeline']:
                    by_pos[sample['pos']].append(sample)
                f.write('  reply timeline by position:\n')
                for pos in sorted(by_pos):
                    vals = by_pos[pos]
                    f.write(f'    pos={pos:<2} issue_to_reply_ms={summarize([t["issue_to_reply_ms"] for t in vals], unit="ms")} '
                            f'batch_age_ms={summarize([t["batch_age_ms"] for t in vals], unit="ms")}\n')
            f.write('\n')

        if all_reads:
            f.write('[AGGREGATE batch composition across all clients]\n')
            f.write(f'  total batches : {len(all_reads)}\n')
            f.write(f'  reads         : {summarize(all_reads)}\n')
            f.write(f'  writes        : {summarize(all_writes)}\n')
            f.write(f'  reads histogram:\n{histogram(all_reads)}\n')
            agg_adjacent_repeats = []
            agg_unique_shards = []
            agg_combo_counts = defaultdict(int)
            agg_shard_counts = defaultdict(int)
            for d in client_stats.values():
                agg_adjacent_repeats.extend(d['adjacent_repeat_counts'])
                agg_unique_shards.extend(d['unique_shards_per_batch'])
                for shard, count in d['shard_counts'].items():
                    agg_shard_counts[shard] += count
                for seq in d['batch_sequences']:
                    agg_combo_counts[tuple(seq)] += 1
            if agg_shard_counts:
                f.write('  shard issue counts:\n')
                for shard in sorted(agg_shard_counts):
                    f.write(f'    shard={shard:<3} count={agg_shard_counts[shard]}\n')
            if agg_unique_shards:
                f.write(f'  unique shards / batch : {summarize(agg_unique_shards)}\n')
                f.write(f'  adjacent repeats      : {summarize(agg_adjacent_repeats)}\n')
                repeat_hist = defaultdict(int)
                for repeats in agg_adjacent_repeats:
                    repeat_hist[repeats] += 1
                f.write(f'  adjacent-repeat histogram:\n{histogram_from_dict(repeat_hist)}\n')
            top_combos = sorted(agg_combo_counts.items(), key=lambda kv: (-kv[1], kv[0]))[:10]
            if top_combos:
                f.write('  top shard combinations:\n')
                for seq, count in top_combos:
                    seq_str = ','.join(str(s) for s in seq)
                    f.write(f'    [{seq_str}]  count={count}\n')
            agg_reply_timeline = []
            agg_batch_total_reply_ms = []
            for d in client_stats.values():
                agg_reply_timeline.extend(d['reply_timeline'])
                agg_batch_total_reply_ms.extend(d['batch_total_reply_ms'])
            if agg_reply_timeline:
                f.write(f'  batch total reply ms : {summarize(agg_batch_total_reply_ms, unit="ms")}\n')
                by_pos = defaultdict(list)
                for sample in agg_reply_timeline:
                    by_pos[sample['pos']].append(sample)
                f.write('  reply timeline by position:\n')
                for pos in sorted(by_pos):
                    vals = by_pos[pos]
                    f.write(f'    pos={pos:<2} issue_to_reply_ms={summarize([t["issue_to_reply_ms"] for t in vals], unit="ms")} '
                            f'batch_age_ms={summarize([t["batch_age_ms"] for t in vals], unit="ms")}\n')
            f.write('\n')

        if cid_reads:
            f.write('\n=== Per-Client Clean/Dirty Read Counts (from MIDDLE server logs) ===\n')
            f.write('(client_id is the raw C++ uint64; matches rid().client_id() in server logs)\n\n')

            total_clean = total_dirty = 0
            for cid in sorted(cid_reads):
                c = cid_reads[cid]['clean']
                d = cid_reads[cid]['dirty']
                total = c + d
                pct = f'{100*c/total:.1f}% clean' if total > 0 else 'n/a'
                f.write(f'  client_id={cid:<6}  clean={c:<6} dirty={d:<6} total={total:<6}  ({pct})\n')
                total_clean += c
                total_dirty += d

            total_all = total_clean + total_dirty
            pct_all = f'{100*total_clean/total_all:.1f}% clean' if total_all > 0 else 'n/a'
            f.write(f'\n  TOTAL            clean={total_clean:<6} dirty={total_dirty:<6} '
                    f'total={total_all:<6}  ({pct_all})\n')

    print(f'Wrote {final_path}')


def write_tail_stats(run_dir, tail_stats, middle_stats):
    out_path = os.path.join(run_dir, 'per_tail_stats.txt')
    f, final_path = open_output_file(out_path)
    with f:
        if not tail_stats and not middle_stats:
            f.write('No Tail batch or ReadStats lines found.\n')
            f.write('Make sure the binary was built with the instrumentation.\n')
            print(f'Wrote {final_path} (no data)')
            return

        f.write('=== Per-Shard Coordination Queueing ===\n')
        f.write('(coord_wait_ms: time a read spent buffered at the middle waiting for CoordResponses)\n')
        f.write('(queue_depth: number of buffered coord-waiting reads outstanding on that shard)\n')
        f.write('(tail batch_size/pending_ooo kept below as secondary transport/tail signals)\n\n')

        all_sizes = []
        all_ooo   = []
        all_waits = []
        all_depth_samples = []
        for label in sorted(tail_stats):
            d = tail_stats[label]
            sizes, ooo = d['batch_sizes'], d['pending_ooo']
            all_sizes.extend(sizes)
            all_ooo.extend(ooo)
            waits = d['coord_wait_ms']
            depths = d['queue_depth_samples']
            all_waits.extend(waits)
            all_depth_samples.extend(depths)
            f.write(f'[{label}]\n')
            f.write(f'  buffered reads   : {d["buffered_reads"]}\n')
            f.write(f'  unblocked reads  : {d["unblocked_reads"]}\n')
            f.write(f'  coord_wait_ms    : {summarize(waits, unit="ms")}\n')
            if waits:
                f.write(f'  coord_wait histogram:\n{histogram(waits)}\n')
            f.write(f'  queue_depth      : {summarize(depths)}\n')
            f.write(f'  max_queue_depth  : {d["max_queue_depth"]}\n')
            f.write(f'  tail batch_size  : {summarize(sizes)}\n')
            f.write(f'  tail pending_ooo : {summarize(ooo)}\n')
            f.write('\n')

        if tail_stats:
            f.write('[AGGREGATE across all shards]\n')
            f.write(f'  coord_wait_ms    : {summarize(all_waits, unit="ms")}\n')
            if all_waits:
                f.write(f'  coord_wait histogram:\n{histogram(all_waits)}\n')
            f.write(f'  queue_depth      : {summarize(all_depth_samples)}\n')
            f.write(f'  tail batch_size  : {summarize(all_sizes)}\n')
            f.write(f'  tail pending_ooo : {summarize(all_ooo)}\n')
            f.write('\n')

        f.write('\n=== Per-Shard Middle Read Outcomes ===\n')
        f.write('(clean: served directly; dirty: required VersionRequest to TAIL (+200ms))\n')
        f.write('(dirty_depth = lastOp - lastCommitted when read arrived dirty)\n\n')

        total_clean = total_dirty = 0
        all_depth_hist = defaultdict(int)
        all_timeline = []
        for label in sorted(middle_stats):
            d = middle_stats[label]
            clean  = d['clean']
            dirty  = d['dirty']
            dh     = d['depth_hist']
            timeline = d['read_timeline']
            total  = clean + dirty
            pct    = f'{100*clean/total:.1f}% clean' if total > 0 else 'n/a'
            total_clean += clean
            total_dirty += dirty
            all_timeline.extend(timeline)
            for depth, cnt in dh.items():
                all_depth_hist[depth] += cnt

            f.write(f'[{label} MIDDLE]\n')
            f.write(f'  clean reads  : {clean}\n')
            f.write(f'  dirty reads  : {dirty}\n')
            f.write(f'  total reads  : {total}  ({pct})\n')
            if dh:
                f.write(f'  depth histogram:\n{histogram_from_dict(dh)}\n')
            if timeline:
                f.write(f'  total latency   : {summarize([t["total_ms"] for t in timeline], unit="ms")}\n')
                f.write(f'  coord wait      : {summarize([t["coord_wait_ms"] for t in timeline], unit="ms")}\n')
                f.write(f'  vr wait         : {summarize([t["vr_wait_ms"] for t in timeline], unit="ms")}\n')
                f.write(f'  ready->exec wait: {summarize([t["ready_wait_ms"] for t in timeline], unit="ms")}\n')
            f.write('\n')

        total_all = total_clean + total_dirty
        pct_all = f'{100*total_clean/total_all:.1f}% clean' if total_all > 0 else 'n/a'
        f.write('[AGGREGATE across all shards]\n')
        f.write(f'  clean reads       : {total_clean}\n')
        f.write(f'  dirty reads       : {total_dirty}\n')
        f.write(f'  total reads       : {total_all}  ({pct_all})\n')
        if all_depth_hist:
            f.write(f'  depth histogram:\n{histogram_from_dict(all_depth_hist)}\n')
        if all_timeline:
            f.write(f'  total latency     : {summarize([t["total_ms"] for t in all_timeline], unit="ms")}\n')
            f.write(f'  coord wait        : {summarize([t["coord_wait_ms"] for t in all_timeline], unit="ms")}\n')
            f.write(f'  vr wait           : {summarize([t["vr_wait_ms"] for t in all_timeline], unit="ms")}\n')
            f.write(f'  ready->exec wait  : {summarize([t["ready_wait_ms"] for t in all_timeline], unit="ms")}\n')

            by_pos = defaultdict(list)
            by_path = defaultdict(list)
            for sample in all_timeline:
                by_pos[sample['pos']].append(sample)
                by_path['vr' if sample['vr'] else 'clean'].append(sample)

            f.write('\n=== Aggregate Read Timeline By Position ===\n')
            for pos in sorted(by_pos):
                vals = by_pos[pos]
                f.write(f'  pos={pos:<2} total={len(vals):<4} '
                        f'total_ms={summarize([t["total_ms"] for t in vals], unit="ms")} '
                        f'coord_wait_ms={summarize([t["coord_wait_ms"] for t in vals], unit="ms")} '
                        f'vr_wait_ms={summarize([t["vr_wait_ms"] for t in vals], unit="ms")} '
                        f'ready_wait_ms={summarize([t["ready_wait_ms"] for t in vals], unit="ms")}\n')

            f.write('\n=== Aggregate Read Timeline By Path ===\n')
            for path_label in ['clean', 'vr']:
                vals = by_path.get(path_label, [])
                if not vals:
                    continue
                f.write(f'  path={path_label:<5} total={len(vals):<4} '
                        f'total_ms={summarize([t["total_ms"] for t in vals], unit="ms")} '
                        f'coord_wait_ms={summarize([t["coord_wait_ms"] for t in vals], unit="ms")} '
                        f'vr_wait_ms={summarize([t["vr_wait_ms"] for t in vals], unit="ms")} '
                        f'ready_wait_ms={summarize([t["ready_wait_ms"] for t in vals], unit="ms")}\n')

    print(f'Wrote {final_path}')


def process_run_dir(run_dir):
    client_stats = parse_client_logs(run_dir)
    tail_stats, middle_stats = parse_server_logs(run_dir)
    write_client_stats(run_dir, client_stats, middle_stats)
    write_tail_stats(run_dir, tail_stats, middle_stats)


def main():
    if len(sys.argv) != 2:
        print(f'Usage: {sys.argv[0]} <exp_dir>', file=sys.stderr)
        print('  exp_dir: experiment directory containing per-protocol run dirs, e.g.', file=sys.stderr)
        print('    experiments/printdbg/10_shards/50%_reads', file=sys.stderr)
        print('  Or a single timestamped run dir:', file=sys.stderr)
        print('    experiments/printdbg/10_shards/50%_reads/iocl_craq-0/2026-04-11-23-26-43', file=sys.stderr)
        sys.exit(1)

    exp_dir = sys.argv[1].rstrip('/')
    if not os.path.isdir(exp_dir):
        print(f'Not a directory: {exp_dir}', file=sys.stderr)
        sys.exit(1)

    # If exp_dir itself is a run dir (has out/ directly), process it alone.
    if os.path.isdir(os.path.join(exp_dir, 'out')):
        process_run_dir(exp_dir)
        return

    run_dirs = find_run_dirs(exp_dir)
    if not run_dirs:
        print(f'No run dirs found under {exp_dir}', file=sys.stderr)
        sys.exit(1)

    for run_dir in run_dirs:
        print(f'--- {run_dir} ---')
        process_run_dir(run_dir)


if __name__ == '__main__':
    main()
