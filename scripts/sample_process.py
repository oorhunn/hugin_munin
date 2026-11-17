#!/usr/bin/env python3
"""Measure an existing app or service; no clipboard or process environment is read."""
import argparse
import json
import platform
import subprocess
import time
from datetime import datetime, timezone


def command(*args):
    return subprocess.check_output(args, text=True).strip()


def sample(pid):
    fields = command('/bin/ps', '-p', str(pid), '-o', 'time=', '-o', 'rss=').split()
    if len(fields) != 2:
        raise RuntimeError('process disappeared or ps returned an unexpected sample')
    # macOS ps reports cumulative process CPU as [[days-]hours:]minutes:seconds.
    clock = fields[0]
    days = 0
    if '-' in clock:
        day, clock = clock.split('-', 1)
        days = int(day)
    seconds = 0.0
    for part in clock.split(':'):
        seconds = seconds * 60 + float(part)
    return seconds + days * 86400, int(fields[1]) * 1024


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('pid', type=int)
    parser.add_argument('--seconds', type=float, default=600)
    parser.add_argument('--interval', type=float, default=5)
    parser.add_argument('--workload', required=True, help='Describe history, UI and scan state')
    args = parser.parse_args()
    if args.seconds <= 0 or args.interval <= 0:
        parser.error('duration and interval must be positive')
    started = datetime.now(timezone.utc).isoformat()
    cpu_start, rss_start = sample(args.pid)
    start = time.monotonic()
    samples = [{'elapsed_seconds': 0, 'rss_bytes': rss_start}]
    while time.monotonic() - start < args.seconds:
        time.sleep(min(args.interval, max(0, args.seconds - (time.monotonic() - start))))
        cpu_end, rss = sample(args.pid)
        samples.append({'elapsed_seconds': time.monotonic() - start, 'rss_bytes': rss})
    elapsed = time.monotonic() - start
    print(json.dumps({
        'started_utc': started, 'pid': args.pid, 'workload': args.workload,
        'os': platform.platform(), 'hardware': command('/usr/sbin/sysctl', '-n', 'hw.model'),
        'cpu': command('/usr/sbin/sysctl', '-n', 'machdep.cpu.brand_string'),
        'memory_bytes': int(command('/usr/sbin/sysctl', '-n', 'hw.memsize')),
        'elapsed_seconds': elapsed,
        'cpu_percent_one_core': (cpu_end - cpu_start) / elapsed * 100,
        'rss_start_bytes': rss_start, 'rss_end_bytes': samples[-1]['rss_bytes'],
        'rss_peak_sampled_bytes': max(s['rss_bytes'] for s in samples), 'samples': samples,
    }, indent=2))


if __name__ == '__main__':
    main()
