#!/usr/bin/env python3
"""Aggregate GCC -fstack-usage (.su) files into a ranked per-function report.

Usage: su_report.py <root_dir> [top_n] [stack_bytes]

Each .su line is: <file>:<line>:<col>:<function>\t<bytes>\t<qualifier>
Qualifier is one of: static | dynamic | bounded (or combos).
On ESP8266 the user task runs on CONT_STACKSIZE = 4096 bytes, so any single
frame that is a large fraction of that is a red flag (deep call chains sum up).
The stack budget defaults to 4096 (ESP8266) but can be overridden with the
third argument or the STACK_BYTES environment variable for other targets.
"""
import os
import re
import sys

CONT_STACK = int(os.environ.get('STACK_BYTES', '4096'))

def _dedup_su_files(paths):
    """Collapse duplicate build trees into one .su per object.

    The ESP8266 build emits a separate object tree per config hash / rBoot ROM
    slot (e.g. .../build/App/App-<hash>/app/mqtt.su). Orphaned trees from an
    earlier config linger and would leak stale frame sizes into the report as
    duplicate rows. Key each file by its path with the volatile 'App-<hash>'
    build dir masked, then keep only the copy from the most recently modified
    tree.
    """
    best = {}
    for p in paths:
        key = re.sub(r'App-[0-9a-fA-F]+', 'App-#', p)
        try:
            mt = os.path.getmtime(p)
        except OSError:
            mt = 0
        cur = best.get(key)
        if cur is None or mt > cur[0]:
            best[key] = (mt, p)
    return [v[1] for v in best.values()]

def parse(root):
    rows = []
    su_paths = []
    for dirpath, _dirs, files in os.walk(root):
        for fn in files:
            if fn.endswith('.su'):
                su_paths.append(os.path.join(dirpath, fn))
    for path in _dedup_su_files(su_paths):
        fn = os.path.basename(path)
        with open(path, 'r', errors='replace') as fh:
            for raw in fh:
                line = raw.rstrip('\n')
                if not line.strip():
                    continue
                parts = line.split('\t')
                if len(parts) < 3:
                    continue
                loc, size_s, qual = parts[0], parts[1], parts[2]
                try:
                    size = int(size_s)
                except ValueError:
                    continue
                # loc = path:line:col:function  (C++ function contains '::')
                seg = loc.split(':')
                func = ':'.join(seg[3:]) if len(seg) >= 4 else loc
                rows.append((size, qual.strip(), func.strip(), fn[:-3]))
    return rows

# Risk bands as a fraction of the CONT stack. A single frame this large is a
# red flag because real call chains stack several frames on top of each other.
HIGH = CONT_STACK // 4    # >=25% of stack in ONE frame
MED = CONT_STACK // 8     # >=12.5%

def main():
    global CONT_STACK, HIGH, MED
    root = sys.argv[1] if len(sys.argv) > 1 else '.'
    top = int(sys.argv[2]) if len(sys.argv) > 2 else 40
    if len(sys.argv) > 3:
        CONT_STACK = int(sys.argv[3])
        HIGH, MED = CONT_STACK // 4, CONT_STACK // 8
    rows = parse(root)
    if not rows:
        print(f"# stack-usage report: no .su files found under {root}")
        print("# Build first with STACK_USAGE=1 (e.g. `make stackreport`).")
        return
    rows.sort(key=lambda r: r[0], reverse=True)
    n_high = sum(1 for r in rows if r[0] >= HIGH)
    n_med = sum(1 for r in rows if MED <= r[0] < HIGH)
    biggest = rows[0][0]

    print(f"# stack-usage risk report  ({len(rows)} functions, CONT_STACKSIZE={CONT_STACK} B)")
    print(f"#   risk bands: HIGH >= {HIGH} B (25%)   MED >= {MED} B (12.5%)")
    print(f"#   HIGH-risk frames: {n_high}    MED-risk frames: {n_med}    largest single frame: {biggest} B")
    if biggest >= HIGH:
        print(f"#   VERDICT: {n_high} frame(s) each consume >=25% of the stack alone; a call chain")
        print(f"#            through even two of these plus lwIP/TLS can overflow {CONT_STACK} B. INVESTIGATE.")
    elif biggest >= MED:
        print(f"#   VERDICT: no single frame exceeds 25%, but {n_med} MED frame(s) can still add up in deep chains.")
    else:
        print(f"#   VERDICT: no frame exceeds 12.5% of the stack; per-function risk is low.")
    print()

    print(f"{'bytes':>7}  {'%stk':>5}  {'risk':<4}  {'qual':<8}  {'unit':<14}  function")
    print('-' * 100)
    for size, qual, func, unit in rows[:top]:
        pct = 100.0 * size / CONT_STACK
        risk = 'HIGH' if size >= HIGH else ('MED' if size >= MED else '')
        f = func if len(func) <= 78 else func[:75] + '...'
        print(f"{size:>7}  {pct:>4.0f}%  {risk:<4}  {qual:<8}  {unit:<14.14}  {f}")
    print()
    dyn = [r for r in rows if 'dynamic' in r[1]]
    if dyn:
        print(f"# {len(dyn)} function(s) use DYNAMIC (VLA/alloca) stack — size is a floor, not a cap:")
        for size, qual, func, unit in dyn[:15]:
            f = func if len(func) <= 70 else func[:67] + '...'
            print(f"  {size:>6}  {unit:<16.16}  {f}")

if __name__ == '__main__':
    main()
