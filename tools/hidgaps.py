#!/usr/bin/env python3
"""Report gaps from a controller's hidraw node, as the host sees them.

Works for any pad - ours or the stock one - since it only watches input
reports arrive.  Every second it prints the reports received, the longest gap
between two of them and how many gaps exceeded the threshold; at the end, the
whole run's interval percentiles.  A gap is what the player feels as a stall
or dropout: the host had nothing new for that long.

    tools/hidgaps.py                     # the first hidraw node in /dev/bwrap
    tools/hidgaps.py /dev/bwrap/hidraw8 -t 60 --gap-ms 30

Needs no padbus and does not touch the pad.
"""

from __future__ import annotations

import argparse
import glob
import os
import select
import statistics
import sys
import time


def percentile(values, fraction):
    ordered = sorted(values)
    return ordered[min(len(ordered) - 1, int(fraction * len(ordered)))]


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("node", nargs="?", help="hidraw node (default: first in /dev/bwrap)")
    parser.add_argument("-t", "--seconds", type=float, default=60.0)
    parser.add_argument("--gap-ms", type=float, default=30.0,
                        help="count gaps longer than this (default 30 ms)")
    args = parser.parse_args()

    node = args.node
    if node is None:
        nodes = sorted(glob.glob("/dev/bwrap/hidraw*"))
        if not nodes:
            sys.exit("no hidraw node in /dev/bwrap")
        node = nodes[0]
    fd = os.open(node, os.O_RDONLY | os.O_NONBLOCK)
    print(f"{node}: {args.seconds:.0f} s, gaps over {args.gap_ms:.0f} ms counted", flush=True)

    start = time.monotonic()
    end = start + args.seconds
    second_end = start + 1.0
    last = None
    intervals = []
    second_count = 0
    second_max = 0.0
    second_gaps = 0
    total_gaps = 0
    while True:
        now = time.monotonic()
        if now >= second_end:
            # A report still missing at the end of the second is a gap too.
            open_gap = (now - last) * 1000.0 if last is not None else 0.0
            worst = max(second_max, open_gap)
            print(f"{second_end - start:5.0f} s  {second_count:4d} reports  "
                  f"longest gap {worst:6.1f} ms  gaps>{args.gap_ms:.0f}: {second_gaps}",
                  flush=True)
            second_end += 1.0
            second_count = 0
            second_max = 0.0
            second_gaps = 0
        if now >= end:
            break
        ready, _, _ = select.select([fd], [], [], max(0.0, min(second_end, end) - now))
        if not ready:
            continue
        try:
            os.read(fd, 1024)
        except BlockingIOError:
            continue
        except OSError as error:
            print(f"read failed: {error} (disconnected?)", flush=True)
            break
        stamp = time.monotonic()
        if last is not None:
            gap = (stamp - last) * 1000.0
            intervals.append(gap)
            second_max = max(second_max, gap)
            if gap > args.gap_ms:
                second_gaps += 1
                total_gaps += 1
        last = stamp
        second_count += 1
    os.close(fd)

    if intervals:
        print(f"total: {len(intervals) + 1} reports, "
              f"{(len(intervals) + 1) / args.seconds:.1f} per second; interval ms "
              f"p50 {statistics.median(intervals):.1f} p95 {percentile(intervals, 0.95):.1f} "
              f"p99 {percentile(intervals, 0.99):.1f} max {max(intervals):.1f}; "
              f"gaps over {args.gap_ms:.0f} ms: {total_gaps}")


if __name__ == "__main__":
    main()
