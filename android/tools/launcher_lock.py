#!/usr/bin/env python3
"""Hold the launcher lock without leaking it, and wait for signal cleanup."""
import fcntl
import os
from pathlib import Path
import signal
import subprocess
import sys
import time


def main():
    lock_path, *command = sys.argv[1:]
    child = None

    def forward(signum, _frame):
        if child is None:
            raise SystemExit(128 + signum)
        try:
            os.killpg(child.pid, signum)
        except ProcessLookupError:
            pass

    for sig in (signal.SIGINT, signal.SIGTERM, signal.SIGHUP):
        signal.signal(sig, forward)
    with open(lock_path, 'a') as lock:
        # Python descriptors are close-on-exec. Child sessions never own the lock.
        os.set_inheritable(lock.fileno(), False)
        deadline = time.monotonic() + 5
        while True:
            try:
                fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
                break
            except BlockingIOError:
                if time.monotonic() >= deadline:
                    print(f'padtool emulator: launcher lock is held: {lock_path}', file=sys.stderr)
                    subprocess.run(['fuser', '-v', lock_path], check=False)
                    print('Stop the earlier launcher with Ctrl-C, then retry.', file=sys.stderr)
                    return 75
                time.sleep(.1)
        env = dict(os.environ, PADTOOL_EMULATOR_LOCK_HELD=str(Path(lock_path).parent))
        child = subprocess.Popen(command, env=env, start_new_session=True)
        try:
            result = child.wait()
        finally:
            if child.poll() is None:
                os.killpg(child.pid, signal.SIGTERM)
                child.wait(timeout=20)
        return result if result >= 0 else 128 - result


if __name__ == '__main__':
    sys.exit(main())
