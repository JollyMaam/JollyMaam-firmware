#!/usr/bin/env python3
"""Random connect / pair / disconnect against a console, hunting resets.

Leave the Switch in Change Grip/Order: `pair 0` then gets a fresh pairing and
`connect 0` is a pad-initiated reconnect.  Actions and delays are random,
including delays short enough to interrupt a pairing or a connection halfway.

    tools/fuzz_links.py --minutes 10

When padbus reports a reset, the marker ring is dumped straight away (before
the new boot can overwrite it) together with the actions that led up to it,
into /work/nslog/fuzz-resets.txt.
"""

import argparse
import random
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
LOG = Path("/work/nslog/padbus.log")
OUT = Path("/work/nslog/fuzz-resets.txt")
CLIENT = "fuzz"
ACTIONS = [("pair 0", 3), ("connect 0", 4), ("disconnect", 3)]


def padbus(*args: str) -> None:
    subprocess.run([str(ROOT / "tools/padbus.py"), *args], check=False,
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def resets() -> int:
    with LOG.open("rb") as f:
        return f.read().count(b"RESET: boot counter")


def delay() -> float:
    # Mostly human-scale gaps, sometimes a burst that lands mid-transition.
    r = random.random()
    if r < 0.25:
        return random.uniform(0.3, 2.0)
    if r < 0.7:
        return random.uniform(2.0, 8.0)
    return random.uniform(8.0, 20.0)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--minutes", type=float, default=10.0)
    parser.add_argument("--seed", type=int, default=None)
    args = parser.parse_args()
    seed = args.seed if args.seed is not None else int(time.time())
    random.seed(seed)
    print(f"seed {seed}", flush=True)
    names = [a for a, _ in ACTIONS]
    weights = [w for _, w in ACTIONS]
    history: list[str] = []
    baseline = resets()
    start_resets = baseline
    end = time.time() + args.minutes * 60
    count = 0
    padbus("request", CLIENT)
    padbus("send", CLIENT, "debug log on")
    while time.time() < end:
        action = random.choices(names, weights)[0]
        wait = delay()
        stamp = time.strftime("%H:%M:%S")
        padbus("request", CLIENT)
        padbus("send", CLIENT, action)
        history.append(f"{stamp} {action} (then {wait:.1f}s)")
        count += 1
        time.sleep(wait)
        now = resets()
        if now > baseline:
            baseline = now
            print(f"RESET after action {count}: {history[-1]}", flush=True)
            time.sleep(1.5)
            padbus("release", CLIENT)
            dump = subprocess.run([str(ROOT / "tools/ringdump.py"), "--boots", "2",
                                   "--tail", "250"], capture_output=True, text=True)
            with OUT.open("a") as f:
                f.write(f"==== reset at {time.strftime('%F %T')} seed {seed}\n")
                f.write("last actions:\n  " + "\n  ".join(history[-12:]) + "\n")
                f.write(dump.stdout + dump.stderr + "\n")
            padbus("request", CLIENT)
            padbus("send", CLIENT, "debug log on")
    padbus("send", CLIENT, "disconnect")
    padbus("release", CLIENT)
    print(f"{count} actions, {resets() - start_resets} resets (see {OUT})",
          flush=True)


if __name__ == "__main__":
    main()
