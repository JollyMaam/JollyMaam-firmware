#!/usr/bin/env python3
"""Report static stack usage of the linked image, per call chain.

The build prints "stack size limit exceeded (N) in foo" for every function
whose frame is bigger than the SDK's own `-warn-stack-size=256` lint.  That
lint is not a limit and says nothing about whether a chain fits; this tool
turns it into the answer.

It does three things:

  1. re-links with a much lower warning threshold, so the linker plugin reports
     a frame size for nearly every function;
  2. prints every object (this project's, the BTstack port's, the SDK's and the
     controller library's) back to LLVM IR with the vendor's own clang, and
     builds the direct call graph from that;
  3. walks the graph from the roots that matter and reports the deepest chain,
     in bytes, against the task stacks in src/main.c.

    tools/stack_report.py                 # the default roots
    tools/stack_report.py cyclone2_poll   # extra roots

Two caveats, both conservative:

  * calls through function pointers are not followed (the timer dispatcher
    posts the callback to a task queue, so its call is invisible here);
  * a function the report does not list is charged 32 bytes, the threshold the
    frame table was cut at, rather than 0.

The task stack sizes in src/main.c are counts of 32-bit words, not bytes: the
Jieli `os_task_create()` documents its `stksize` as "unit (u32)".  app_core is
640 words = 2560 bytes.
"""

import argparse
import glob
import os
import re
import shutil
import subprocess
import sys
import tempfile

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SDK = "/shared/github.com-Jieli-Tech/fw-AC63_BT_SDK"
TOOLCHAIN = "/shared/jieli-linux-toolchains-20250324.1/pi32v2/bin"
CLANG = os.path.join(TOOLCHAIN, "clang")
AR = os.path.join(TOOLCHAIN, "ar")

FRAME_THRESHOLD = 32

DEFAULT_ROOTS = [
    "cyclone2_poll",          # app_core: control loop + USB console
    "execute_command",        # app_core: the console command dispatcher
    "hci_send_event",         # btstack: HCI event construction
    "event_handler",          # btstack: HCI event dispatch
    "hid_device_send_interrupt_message",
    "bredr_rx_irq_handler",   # controller: runs on the interrupt stack
]

# Calls that hand work to another task: the callee runs with a different stack,
# so a chain must stop here or it will mix two task stacks together.  The
# timer dispatcher and the SDK's command queue both work this way.
TASK_BOUNDARY = {
    "user_send_cmd_prepare",
    "sys_timer_add",
    "sys_timeout_add",
    "os_taskq_post",
    "os_taskq_post_type",
    "os_taskq_post_msg",
    "task_create",
    "os_task_create",
    "os_task_create_affinity_core",
}


def run(command, **kwargs):
    return subprocess.run(command, shell=isinstance(command, str),
                          capture_output=True, text=True, **kwargs)


def frame_table(work, threshold):
    """Frame size per function, from the linker plugin itself."""
    # make only matches its own relative target names, so hand it $O/app.elf
    # rather than an absolute path.
    target = os.path.relpath(os.path.join(work, "app.elf"), REPO)
    link = run(f"make -n -B {target}", cwd=REPO)
    line = link.stdout.strip().splitlines()[-1]
    lowered = re.sub(r"warn-stack-size=\d+",
                     f"warn-stack-size={threshold}", line)
    result = run(lowered, cwd=REPO)
    text = result.stdout + result.stderr
    frames = {}
    for size, name in re.findall(
            r"stack size limit exceeded \((\d+)\) in (\S+)", text):
        frames[name] = int(size)
    return frames


def to_ir(source, destination):
    if os.path.exists(destination):
        return True
    result = run([CLANG, "-target", "pi32v2", "-x", "ir", "-S",
                  "-emit-llvm", source, "-o", destination])
    return result.returncode == 0 and os.path.exists(destination)


def extract_archive(archive, directory):
    os.makedirs(directory, exist_ok=True)
    run([AR, "x", archive], cwd=directory)


def collect_ir(work, directory):
    """Print every linked object, and the SDK archives, back to IR."""
    os.makedirs(directory, exist_ok=True)
    sources = []
    for pattern in ("src/*.o", "btstack/src/*.o", "btstack/src/classic/*.o",
                    "btstack/port/jieli-br23/*.o"):
        sources += sorted(glob.glob(os.path.join(work, pattern)))
    for source in sources:
        name = source.replace("/", "_")
        to_ir(source, os.path.join(directory, name + ".ll"))

    for archive in sorted(glob.glob(os.path.join(SDK, "cpu/br23/liba/*.a"))):
        members = os.path.join(directory, os.path.basename(archive))
        extract_archive(archive, members)
        for member in sorted(glob.glob(os.path.join(members, "*.o"))):
            to_ir(member, member + ".ll")


CALL = re.compile(r"call\b[^\n]*?@([A-Za-z_.$][\w.$]*)")
DEFINE = re.compile(r"^define\b[^\n]*?@([A-Za-z_.$][\w.$]*)")


def call_graph(directory):
    callees = {}
    defined = set()
    for path in glob.glob(os.path.join(directory, "**", "*.ll"),
                          recursive=True):
        current = None
        with open(path, errors="ignore") as handle:
            for line in handle:
                match = DEFINE.match(line)
                if match:
                    current = match.group(1)
                    defined.add(current)
                    callees.setdefault(current, set())
                    continue
                if line.startswith("}"):
                    current = None
                    continue
                if current is None:
                    continue
                for target in CALL.findall(line):
                    if not target.startswith("llvm."):
                        callees[current].add(target)
    return callees, defined


def depths(callees, frames, threshold=FRAME_THRESHOLD):
    memo = {}

    def frame(name):
        return frames.get(name, threshold)

    def walk(name, stack=()):
        if name in memo:
            return memo[name]
        best = 0
        if name not in TASK_BOUNDARY:
            for callee in callees.get(name, ()):
                if callee in stack:
                    continue
                best = max(best, walk(callee, stack + (name,)))
        memo[name] = frame(name) + best
        return memo[name]

    return memo, walk, frame


def heaviest_chain(root, callees, memo, walk, frame):
    chain = []
    seen = set()
    name = root
    while name is not None and name not in seen:
        chain.append(name)
        seen.add(name)
        best, nxt = -1, None
        for callee in callees.get(name, ()):
            if callee in seen:
                continue
            value = walk(callee)
            if value > best:
                best, nxt = value, callee
        name = nxt
    return chain


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("roots", nargs="*", default=None)
    parser.add_argument("--build", default=os.path.join(REPO, "build"))
    parser.add_argument("--threshold", type=int, default=FRAME_THRESHOLD,
                        help="frames above this are reported exactly; smaller "
                             "ones are charged this much")
    parser.add_argument("--keep", action="store_true",
                        help="keep the IR directory for inspection")
    args = parser.parse_args()

    roots = args.roots or DEFAULT_ROOTS
    work = tempfile.mkdtemp(prefix="stackreport-")
    try:
        frames = frame_table(args.build, args.threshold)
        print(f"# {len(frames)} functions with a frame above "
              f"{args.threshold} bytes, from the linker plugin")
        collect_ir(args.build, os.path.join(work, "ir"))
        callees, defined = call_graph(os.path.join(work, "ir"))
        print(f"# {len(defined)} functions in the call graph")
        print()
        memo, walk, frame = depths(callees, frames, args.threshold)
        for root in roots:
            if root not in defined:
                print(f"{root}: not in the call graph")
                continue
            print(f"{root}: ~{walk(root)} bytes")
            chain = heaviest_chain(root, callees, memo, walk, frame)
            print("   " + " -> ".join(f"{n}({frame(n)})" for n in chain[:12]))
    finally:
        if args.keep:
            print(f"# IR kept in {work}")
        else:
            shutil.rmtree(work, ignore_errors=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
