#!/usr/bin/env python3
r"""Turn an SFA-NX profiler report (sdmc:/switch/SFA-NX/logs/profile-NNN.txt) into per-function times.

    py -3 switch/tools/prof_report.py <profile-NNN.txt ...> [elf] [--top N] [--callers N]
    example: py -3 switch/tools/prof_report.py "logs/profile-*.txt" debug/SFA-NX-1556.elf
    
Reports are incremental (each file = the time since the previous one). Pass several files, or a glob
such as "logs/profile-*.txt", to add them up; frame-phase times are weighted by each file's frames.

Without an elf, picks the debug/SFA-NX-*.elf whose modification time is closest to the report's build
stamp (warns when more than 3 minutes apart: the profile then belongs to another build).

Each sample is 1 ms of the main thread's time (1000 Hz). "ms/frame" = samples / frames.
Needs devkitA64's aarch64-none-elf-nm and -addr2line.
"""
import bisect
import datetime
import glob
import os
import subprocess
import sys
from collections import Counter, defaultdict

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
# devkitA64 bin folders to search: SFA_NX_DEVKITPRO (Windows path of the devkitPro install) first, then the
# installer defaults on Windows and Linux. No local paths are hard-coded.
TOOLS = [os.path.join(d, "devkitA64", "bin")
         for d in (os.environ.get("SFA_NX_DEVKITPRO"), r"C:\devkitPro", "/opt/devkitpro") if d]


# Path of a devkitA64 binutils tool (nm, addr2line).
def tool(name):
    for d in TOOLS:
        for ext in (".exe", ""):
            p = os.path.join(d, "aarch64-none-elf-" + name + ext)
            if os.path.exists(p):
                return p
    sys.exit("aarch64-none-elf-%s not found (devkitA64)" % name)


# Parses one profile-NNN.txt: header values, the (role, count, pc, lr) sample rows, the "K" lock rows and
# the "S"/"s" slow-frame rows (format written by switch/src/prof_switch.cpp write_report).
def read_report(path):
    head, rows = {}, []
    head["locks"] = []
    head["slow"] = []
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("#") or line.startswith("SFA-NX profile"):
                continue
            parts = line.split()
            if parts[0] == "S" and len(parts) == 10:
                head["slow"].append({"frame": int(parts[1]), "t": float(parts[2]),
                                     "sec": [float(x) for x in parts[3:7]], "n": int(parts[7]),
                                     "lost": int(parts[8]), "flags": int(parts[9]), "samples": []})
            elif parts[0] == "s" and len(parts) == 7:
                if head["slow"]:
                    head["slow"][-1]["samples"].append(
                        (int(parts[1]), int(parts[2]), int(parts[3])) + tuple(int(x, 16) for x in parts[4:]))
            elif parts[0] == "K" and len(parts) == 8:
                head["locks"].append((int(parts[1]), int(parts[2]), tuple(int(x, 16) for x in parts[3:])))
            elif parts[0].isdigit() and len(parts) == 4:
                rows.append((int(parts[0]), int(parts[1]), int(parts[2], 16), int(parts[3], 16)))
            elif parts[0] == "section":
                head.setdefault("sections", []).append((parts[1], float(parts[2])))
            else:
                head[parts[0]] = " ".join(parts[1:])
    return head, rows


# The debug/SFA-NX-*.elf built closest to the report's build stamp.
def pick_elf(stamp):
    elfs = glob.glob(os.path.join(ROOT, "debug", "SFA-NX-*.elf"))
    if not elfs:
        sys.exit("no debug/SFA-NX-*.elf; pass the elf path")
    try:
        when = datetime.datetime.strptime(" ".join(stamp.split()), "%b %d %Y %H:%M:%S").timestamp()
    except ValueError:
        return max(elfs, key=os.path.getmtime), None
    best = min(elfs, key=lambda e: abs(os.path.getmtime(e) - when))
    return best, abs(os.path.getmtime(best) - when)


# Function symbols of the ELF, sorted by address, for mapping sampled PCs to functions.
def load_symbols(elf):
    out = subprocess.run([tool("nm"), "-n", "-C", "--defined-only", elf], capture_output=True, text=True,
                         check=True).stdout
    addrs, names = [], []
    data_addrs, data_names = [], []
    anchor = None
    for line in out.splitlines():
        parts = line.split(" ", 2)
        if len(parts) < 3:
            continue
        if parts[1] in "dDbBvV":
            data_addrs.append(int(parts[0], 16))
            data_names.append(parts[2])
            continue
        if parts[1] not in "tTwW":
            continue
        addr = int(parts[0], 16)
        if parts[2] == "sfa_nx_prof_anchor":
            anchor = addr
        addrs.append(addr)
        names.append(parts[2])
    if anchor is None:
        sys.exit("sfa_nx_prof_anchor not in %s: not a profiler build" % elf)
    load_symbols.data = (data_addrs, data_names)
    return addrs, names, anchor


# Adds several incremental reports together (sample counts summed, frame phases weighted by frames).
def merge(paths):
    """Sum incremental reports of one build into one header + row list."""
    heads, rows, locks = [], Counter(), Counter()
    for p in paths:
        h, r = read_report(p)
        heads.append(h)
        for role, count, pc, lr in r:
            rows[(role, pc, lr)] += count
        for role, count, key in h["locks"]:
            locks[(role, key)] += count
    builds = {h.get("build") for h in heads}
    anchors = {h.get("anchor") for h in heads}
    if len(builds) > 1 or len(anchors) > 1:
        sys.exit("reports come from different builds/runs (build %s, anchor %s): pass one run's files" %
                 (sorted(map(str, builds)), sorted(map(str, anchors))))
    head = dict(heads[0])
    frames = sum(int(h.get("frames", "0")) for h in heads)
    for key in ("seconds",):
        head[key] = "%.1f" % sum(float(h.get(key, "0")) for h in heads)
    for key in ("frames", "slow_frames", "dropped", "failed"):
        head[key] = str(sum(int(h.get(key, "0")) for h in heads))
    for key in ("audio_underruns", "audio_low"):
        head[key] = str(sum(int(h.get(key, "0")) for h in heads))
    head["audio_lock_wait_max_ms"] = "%.2f" % max(float(h.get("audio_lock_wait_max_ms", "0")) for h in heads)
    sec = defaultdict(float)
    order = []
    for h in heads:
        for name, ms in h.get("sections", []):
            if name not in sec:
                order.append(name)
            sec[name] += ms * int(h.get("frames", "0"))
    head["sections"] = [(n, sec[n] / max(frames, 1)) for n in order]
    head["locks"] = [(role, c, key) for (role, key), c in locks.items()]
    head["slow"] = [s for h in heads for s in h["slow"]]
    return head, [(role, c, pc, lr) for (role, pc, lr), c in rows.items()]


# Prints frame phases, the top functions per thread role (self time and with callers), lock waits, and
# optionally the slowest frames.
def main():
    args, opts, argv = [], {}, sys.argv[1:]
    i = 0
    while i < len(argv):
        a = argv[i]
        if a.startswith("--"):
            opts[a] = argv[i + 1] if i + 1 < len(argv) else ""
            i += 2
            continue
        args.extend(sorted(glob.glob(a)) if any(ch in a for ch in "*?") else [a])
        i += 1
    top = int(opts.get("--top", 40))
    nslow = int(opts.get("--slow", 20))
    ncallers = int(opts.get("--callers", 12))
    reports = [a for a in args if not a.lower().endswith(".elf")]
    elfs = [a for a in args if a.lower().endswith(".elf")]
    if not reports:
        sys.exit(__doc__)
    head, rows = merge(reports)
    if elfs:
        elf, gap = elfs[0], None
    else:
        elf, gap = pick_elf(head.get("build", ""))
    addrs, names, elf_anchor = load_symbols(elf)
    bias = int(head["anchor"], 16) - elf_anchor
    frames = max(int(head.get("frames", "0")), 1)
    total = sum(r[1] for r in rows)

    def func(addr):
        a = addr - bias
        i = bisect.bisect_right(addrs, a) - 1
        return names[i] if i >= 0 else "?%x" % a

    print("report  %s" % (reports[0] if len(reports) == 1 else "%d files: %s .. %s" %
                                (len(reports), reports[0], reports[-1])))
    print("build   %s" % head.get("build"))
    print("elf     %s%s" % (elf, "" if gap is None else "  (%.0f s from the build stamp%s)" %
                            (gap, ", PROBABLY THE WRONG BUILD" if gap > 180 else "")))
    print("time    %s s, %s frames, %s slow (>20 ms), %d samples (dropped %s, failed %s)" %
          (head.get("seconds"), frames, head.get("slow_frames"), total, head.get("dropped"), head.get("failed")))
    for name, ms in head.get("sections", []):
        print("  %-8s %6.2f ms/frame" % (name, ms))
    if "renderer" in head:
        print("render  %s renderer, frame overlap %s" % (head["renderer"], head.get("frame_overlap", "?")))
    if any("audio_underruns" in h for h in [head]):
        print("audio   %s underruns, %s pumps under 10 ms queued, longest lock wait %s ms" %
              (head["audio_underruns"], head["audio_low"], head["audio_lock_wait_max_ms"]))

    self_counts = Counter()
    callers = defaultdict(Counter)
    for role, count, pc, lr in rows:
        if role != 0:
            continue
        f = func(pc)
        self_counts[f] += count
        callers[f][func(lr)] += count

    print("\nSelf time, main thread (samples = ms):")
    print("%7s %6s %8s  %s" % ("samples", "%", "ms/frame", "function"))
    for f, c in self_counts.most_common(top):
        print("%7d %5.1f%% %8.3f  %s" % (c, 100.0 * c / total, c / frames, f))

    print("\nCallers (by LR) of the top %d:" % ncallers)
    for f, c in self_counts.most_common(ncallers):
        print("  %s  (%d)" % (f, c))
        for caller, cc in callers[f].most_common(4):
            print("      %5.1f%%  %s" % (100.0 * cc / c, caller))

    # Other sampled threads (perf_switch.cpp roles): 1 FIFO processor, 2 render worker, 3 shader compiler.
    role_names = {1: "FIFO processor", 2: "render worker", 3: "shader compiler", 4: "audio"}
    other = defaultdict(Counter)
    for role, count, pc, lr in rows:
        if role != 0:
            other[role][func(pc)] += count
    for role in sorted(other):
        n = sum(other[role].values())
        print("\nRole %d (%s): %d samples, %.2f ms/frame" % (role, role_names.get(role, "?"), n, n / frames))
        for f, c in other[role].most_common(12):
            print("%7d %5.1f%% %8.3f  %s" % (c, 100.0 * c / n, c / frames, f))

    # Lock waits (threads blocked in libnx mutexLock): which mutex, and the call chain.
    data_addrs, data_names = load_symbols.data

    def data_sym(addr):
        a = addr - bias
        i = bisect.bisect_right(data_addrs, a) - 1
        if i < 0 or a - data_addrs[i] > 0x10000:
            return "heap 0x%x" % addr
        off = a - data_addrs[i]
        return data_names[i] + ("+0x%x" % off if off else "")

    lock_rows = head.get("locks", [])
    if lock_rows:
        by_role = defaultdict(Counter)
        chains = defaultdict(Counter)
        for role, c, key in lock_rows:
            m = data_sym(key[0])
            by_role[role][m] += c
            chain = " <- ".join(func(r) for r in key[1:] if r)
            chains[(role, m)][chain] += c
        print("\nLock waits (ms/frame by mutex, top call chains):")
        for role in sorted(by_role):
            print("  %s:" % ("main thread" if role == 0 else role_names.get(role, "role %d" % role)))
            for m, c in by_role[role].most_common(6):
                print("    %8.3f  %s" % (c / frames, m))
                for chain, cc in chains[(role, m)].most_common(4):
                    print("        %5.1f%%  %s" % (100.0 * cc / c, chain))

    slow = head.get("slow", [])
    if slow:
        report_slow(slow, rows, frames, func, data_sym, role_names, nslow)


SECTIONS = ("game", "present", "wait", "acquire")


# Slow-frame section: how many frames went over 20 ms, which phase was long, and what each thread was doing
# in the worst `nslow` of them.
def report_slow(slow, rows, frames, func, data_sym, role_names, nslow):
    """Slow frames (over 20 ms) recorded one by one: their phases and what each thread was doing."""
    n = len(slow)
    avg = [sum(s["sec"][i] for s in slow) / n for i in range(4)]
    total = [sum(s["sec"]) for s in slow]
    print("\nSlow frames recorded: %d (mean %.1f ms; game %.2f, present %.2f, wait %.2f, acquire %.2f)" %
          ((n, sum(total) / n) + tuple(avg)))
    print("  after a profile write: %d" % sum(1 for s in slow if s["flags"] & 1))
    buckets = Counter()
    for t in total:
        if t > 100:
            buckets[">100 ms"] += 1
        elif t > 34:
            buckets["34-100 ms"] += 1
        else:
            buckets["20-34 ms"] += 1
    print("  by length: %s" % ", ".join("%s %d" % kv for kv in sorted(buckets.items())))
    longest = Counter(SECTIONS[max(range(4), key=lambda i: s["sec"][i])] for s in slow)
    print("  longest phase: %s" % ", ".join("%s %d" % kv for kv in longest.most_common()))

    def label(smp):
        role, sec, kind, a, b, c = smp
        if kind == 1:
            return "LOCK %s <- %s <- %s" % (data_sym(a), func(b), func(c))
        return func(a)

    normal = defaultdict(Counter)
    for role, count, pc, lr in rows:
        normal[role][func(pc)] += count
    agg = defaultdict(Counter)
    for s in slow:
        for smp in s["samples"]:
            agg[(smp[0], smp[1])][label(smp)] += 1
    print("\nSlow frames, ms per slow frame by thread and main-thread phase (avg = ms in an average frame):")
    for key in sorted(agg, key=lambda k: -sum(agg[k].values())):
        role, sec = key
        tot = sum(agg[key].values())
        if tot / n < 0.5:
            continue
        print("  %s during %s: %.1f ms" % ("main" if role == 0 else role_names.get(role, "role %d" % role),
                                          SECTIONS[sec] if sec < 4 else sec, tot / n))
        for f, c in agg[key].most_common(8):
            print("    %7.2f  (avg %6.3f)  %s" % (c / n, normal[role][f] / frames, f[:140]))

    print("\nWorst %d slow frames:" % min(nslow, n))
    for s in sorted(slow, key=lambda s: -sum(s["sec"]))[:nslow]:
        print("  frame %d at %.1f s: %.1f ms (game %.1f, present %.1f, wait %.1f, acquire %.1f)%s%s" %
              ((s["frame"], s["t"] / 1000.0, sum(s["sec"])) + tuple(s["sec"]) +
               (" [after profile write]" if s["flags"] & 1 else "",
                " [%d samples lost]" % s["lost"] if s["lost"] else "")))
        per = defaultdict(Counter)
        for smp in s["samples"]:
            per[smp[0]][label(smp)] += 1
        for role in sorted(per):
            name = "main" if role == 0 else role_names.get(role, "role %d" % role)
            tops = ", ".join("%s %d" % (f[:70], c) for f, c in per[role].most_common(3))
            print("      %-15s %s" % (name, tops))


if __name__ == "__main__":
    main()
