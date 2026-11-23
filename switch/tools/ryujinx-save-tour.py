#!/usr/bin/env python3
"""Ryujinx save tour: load every save of a save collection in Ryujinx, one after another, and let the game
capture the shaders each spot needs.

  py -3 switch/tools/ryujinx-save-tour.py --saves "E:\\StarFoxAdventures\\Star Fox Adventures Save Collection (USA)"
      [--ryujinx E:\\ryujinx_deko3dauto] [--nro build-switch\\sdcard\\switch\\SFA-NX\\SFA-NX.nro]
      [--hops 2] [--timeout 900] [--restart]

The collection is folders "Section N. <name>", each holding either a .gci or numbered step folders
("1. <step>") with one .gci each. Saves run in order: section 0, 1, 2 ... 13, and inside a section step
1, 2, 3 ...

For each save the script
  1. copies the .gci over Ryujinx's card file (sdcard/switch/SFA-NX/user/USA/Card A/<the .gci's name>),
  2. starts Ryujinx with the NRO; switch_menu.ini carries shader_capture=<hops>, so the game taps A
     through the title and file select, waits until the save's player has stood for 5 s, dwells there
     turning while shaders compile, hops to up to <hops> far spots in the same map, dwells again, and quits
     (switch/src/shader_tour_switch.c capture mode, switch/src/switch_menu.cpp),
  3. waits for "[capture] quitting" in logs/log.txt (or --timeout seconds), then closes Ryujinx.

Everything compiled lands in Ryujinx's sdcard/switch/SFA-NX/cache/ (deko/dksh_local.bin,
pipeline_cache_deko.db). Merge it into the seed with
  py -3 switch/builder/seed-caches.py switch/seed switch/seed <ryujinx>/portable/sdcard/switch/SFA-NX/cache

Progress is kept in <ryujinx>/portable/sdcard/switch/SFA-NX/save-tour/progress.json: a stopped tour
resumes at the next unfinished save (--restart starts over). results.csv there has one line per save. The
card file that was there before the tour is kept as <card>.before-save-tour and put back at the end, and
switch_menu.ini gets its shader_capture line removed again.

The collection's saves are the user's own files; nothing here is copied into the repository.
"""
import argparse
import csv
import json
import re
import shutil
import subprocess
import sys
import time
from pathlib import Path

APP = 'SFA-NX'
CARD_SUBDIR = Path('user') / 'USA' / 'Card A'
HEADER = re.compile(r'^===== SFA-NX run .*$', re.M)


# Leading number of "Section 12. Name" / "3. Step", for numeric ordering (no number sorts last).
def order_key(path):
    m = re.match(r'\D*?(\d+)', path.name)
    return (int(m.group(1)) if m else 1 << 30, path.name)


# Every save of the collection in play order, as (label, .gci path).
def collect(saves_dir):
    out = []
    for section in sorted((p for p in saves_dir.iterdir() if p.is_dir()), key=order_key):
        direct = sorted(section.glob('*.gci'))
        if direct:
            out.append((section.name, direct[0]))
        for step in sorted((p for p in section.iterdir() if p.is_dir()), key=order_key):
            gcis = sorted(step.rglob('*.gci'))
            if gcis:
                out.append((f'{section.name} / {step.name}', gcis[0]))
    return out


# Sets (or with hops None removes) the shader_capture line in switch_menu.ini, keeping every other line.
# shader_tour= is removed too: the warp tour would start instead of the capture.
def set_capture(ini, hops):
    lines = ini.read_text(encoding='utf-8').splitlines() if ini.exists() else []
    lines = [l for l in lines if not l.startswith(('shader_capture=', 'shader_tour='))]
    if hops is not None:
        lines.append(f'shader_capture={hops}')
    ini.parent.mkdir(parents=True, exist_ok=True)
    ini.write_text('\n'.join(lines) + '\n', encoding='utf-8')


# The last run's text in log.txt (from its "===== SFA-NX run" header on), and that header line.
def last_run(log):
    try:
        text = log.read_text(encoding='utf-8', errors='replace')
    except OSError:
        return '', ''
    heads = list(HEADER.finditer(text))
    if not heads:
        return '', ''
    return heads[-1].group(0), text[heads[-1].start():]


# Ends Ryujinx and everything it started.
def close(proc):
    if proc.poll() is None:
        subprocess.call(['taskkill', '/PID', str(proc.pid), '/T', '/F'], stdout=subprocess.DEVNULL,
                        stderr=subprocess.DEVNULL)
        try:
            proc.wait(timeout=20)
        except subprocess.TimeoutExpired:
            pass


# Runs one save: returns (status, shaders, pipelines, seconds, start position line).
def run_one(exe, nro, ryu_dir, log, timeout, console):
    before, _ = last_run(log)
    t0 = time.time()
    proc = subprocess.Popen([str(exe), str(nro)], cwd=str(ryu_dir), stdout=console, stderr=subprocess.STDOUT)
    status, run = 'timeout', ''
    try:
        while time.time() - t0 < timeout:
            time.sleep(5)
            head, run = last_run(log)
            if head and head != before:
                if '[capture] quitting' in run:
                    status = 'ok'
                    time.sleep(8)  # let the exit path flush the caches and the log
                    break
            if proc.poll() is not None:
                status = 'ok' if '[capture] quitting' in run else f'ended ({proc.returncode})'
                break
    finally:
        close(proc)
    _, run = last_run(log)
    m = re.search(r'\[capture\] done: (\d+) shaders and (\d+) pipelines', run)
    start = re.search(r'\[capture\] start at [^\n]*', run)
    return (status, int(m.group(1)) if m else 0, int(m.group(2)) if m else 0, round(time.time() - t0),
            start.group(0) if start else '')


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--saves', type=Path, required=True, help='the save collection folder')
    ap.add_argument('--ryujinx', type=Path, default=Path(r'E:\ryujinx_deko3dauto'),
                    help='Ryujinx folder with Ryujinx.exe and portable/')
    ap.add_argument('--nro', type=Path, help='NRO to copy in first (default: the one already there)')
    ap.add_argument('--hops', type=int, default=2, help='far spots to visit after the save\'s start (0-2)')
    ap.add_argument('--timeout', type=int, default=900, help='seconds per save before giving up')
    ap.add_argument('--restart', action='store_true', help='ignore progress.json and start at the first save')
    ap.add_argument('--only', type=int, help='run only save number N (1-based, as in results.csv)')
    args = ap.parse_args()

    exe = args.ryujinx / 'Ryujinx.exe'
    app_dir = args.ryujinx / 'portable' / 'sdcard' / 'switch' / APP
    if not exe.exists() or not app_dir.exists():
        sys.exit(f'need {exe} and {app_dir}')
    nro = app_dir / f'{APP}.nro'
    if args.nro:
        shutil.copy2(args.nro, nro)
    saves = collect(args.saves)
    if not saves:
        sys.exit(f'no .gci files under {args.saves}')

    work = app_dir / 'save-tour'
    work.mkdir(exist_ok=True)
    progress_path = work / 'progress.json'
    done = set()
    if progress_path.exists() and not args.restart:
        done = set(json.loads(progress_path.read_text(encoding='utf-8')).get('done', []))
    results = work / 'results.csv'
    new_csv = not results.exists() or args.restart
    log = app_dir / 'logs' / 'log.txt'
    ini = app_dir / 'config' / 'switch_menu.ini'

    card_dir = app_dir / CARD_SUBDIR
    card_dir.mkdir(parents=True, exist_ok=True)
    card = card_dir / saves[0][1].name
    backup = card.with_name(card.name + '.before-save-tour')
    if card.exists() and not backup.exists():
        shutil.copy2(card, backup)

    print(f'{len(saves)} saves, {len(done)} already done; hops {args.hops}, timeout {args.timeout} s')
    set_capture(ini, args.hops)
    try:
        with open(results, 'w' if new_csv else 'a', newline='', encoding='utf-8') as f, \
                open(work / 'ryujinx-console.txt', 'ab') as console:
            w = csv.writer(f)
            if new_csv:
                w.writerow(['n', 'save', 'status', 'shaders', 'pipelines', 'seconds', 'start'])
            for n, (label, gci) in enumerate(saves, 1):
                if (args.only and n != args.only) or (not args.only and label in done):
                    continue
                print(f'[{n}/{len(saves)}] {label}', flush=True)
                shutil.copy2(gci, card_dir / gci.name)
                status, shaders, pipes, secs, start = run_one(exe, nro, args.ryujinx, log, args.timeout, console)
                print(f'    {status}: {shaders} shaders, {pipes} pipelines, {secs} s  {start}', flush=True)
                w.writerow([n, label, status, shaders, pipes, secs, start])
                f.flush()
                done.add(label)
                progress_path.write_text(json.dumps({'done': sorted(done)}, indent=1), encoding='utf-8')
    finally:
        set_capture(ini, None)
        if backup.exists():
            shutil.copy2(backup, card)
    print(f'finished; results in {results}')


if __name__ == '__main__':
    main()
