#!/usr/bin/env python3
import re
import sys
import os
import subprocess
import argparse
from collections import defaultdict

DEFAULT_ADDR2LINE = "/opt/esp-quick-toolchain/xtensa-lx106-elf/bin/xtensa-lx106-elf-addr2line"

# Regexes tailored precisely to your log outputs
MALLOC_PATTERN = re.compile(
    r"MC## malloc\((\d+)\)\s*=\s*([0-9a-fA-F]+)\s*\(cur\s*(\d+)\),\s*caller\s*([0-9a-fA-F]+)"
)
FREE_PATTERN = re.compile(
    r"MC## free\(([0-9a-fA-F]+)\)\s*->\s*(\d+)?\s*\(cur\s*(\d+)\),\s*caller\s*([0-9a-fA-F]+)"
)
REALLOC_PATTERN = re.compile(
    r"MC## realloc\((\d+)\s*->\s*(\d+)\)\s*=\s*([0-9a-fA-F]+)\s*->\s*([0-9a-fA-F]+)\s*\(cur\s*(\d+)\),\s*caller\s*([0-9a-fA-F]+)"
)

def resolve_address(addr2line_path, elf_path, address):
    if not os.path.exists(elf_path):
        return f"ELF file missing ({elf_path})"
    try:
        full_address = f"0x{address}" if not address.startswith("0x") else address
        result = subprocess.run(
            [addr2line_path, "-e", elf_path, "-f", "-C", full_address],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            check=True
        )
        lines = result.stdout.strip().split('\n')
        if len(lines) >= 2:
            return f"{lines[0].strip()} at {lines[1].strip()}"
        return result.stdout.strip()
    except Exception as e:
        return f"Resolution error ({str(e)})"

def main():
    parser = argparse.ArgumentParser(description="Isolate code locations causing high memory churn and track metrics/lifetime graphs.")
    parser.add_argument("-l", "--log", required=True, help="Path to log file (e.g. output.txt)")
    parser.add_argument("-e", "--elf", required=True, help="Path to your app_0.out ELF image")
    parser.add_argument("-a", "--addr2line", default=DEFAULT_ADDR2LINE, help="Path to xtensa-lx106-elf-addr2line")
    parser.add_argument("-t", "--top", type=int, default=10, help="Number of locations to display")
    parser.add_argument("-g", "--target-graph", required=False, help="Target caller address hex structure (e.g. 402233b0) for lifetime analysis graph")

    args = parser.parse_args()

    if not os.path.exists(args.log):
        print(f"[-] Error: Log file '{args.log}' not found.")
        sys.exit(1)

    # Convert graph target parameter to match clean hex string notation if provided
    target_addr = args.target_graph.lower().replace("0x", "") if args.target_graph else None

    # Structural Metrics Tracking per caller address
    caller_stats = defaultdict(lambda: {"malloc": 0, "free": 0, "realloc": 0, "total": 0, "max_bytes": 0, "current_bytes": 0})
    ptr_registry = {}  # Tracking live allocations: ptr -> (caller, size)

    timeline = []      # Chronological ledger of (tick, current_footprint)
    current_target_footprint = 0
    tick = 0
    total_all_events = 0

    with open(args.log, 'r', encoding='utf-8', errors='ignore') as f:
        for line in f:
            # Check for realloc
            match_realloc = REALLOC_PATTERN.search(line)
            if match_realloc:
                old_size = int(match_realloc.group(1))
                new_size = int(match_realloc.group(2))
                old_ptr = match_realloc.group(3).lower()
                new_ptr = match_realloc.group(4).lower()
                caller = match_realloc.group(6).lower()

                # Clear out previous size footprint if tracking live pointer metrics
                if old_ptr in ptr_registry:
                    _, recorded_old_size = ptr_registry.pop(old_ptr)
                    caller_stats[caller]["current_bytes"] -= recorded_old_size
                    if caller == target_addr:
                        current_target_footprint -= recorded_old_size

                caller_stats[caller]["realloc"] += 1
                caller_stats[caller]["total"] += 1
                caller_stats[caller]["current_bytes"] += new_size
                ptr_registry[new_ptr] = (caller, new_size)

                if caller_stats[caller]["current_bytes"] > caller_stats[caller]["max_bytes"]:
                    caller_stats[caller]["max_bytes"] = caller_stats[caller]["current_bytes"]

                if caller == target_addr:
                    current_target_footprint += new_size

                total_all_events += 1
                if target_addr:
                    timeline.append((tick, current_target_footprint))
                    tick += 1
                continue

            # Check for malloc
            match_malloc = MALLOC_PATTERN.search(line)
            if match_malloc:
                size = int(match_malloc.group(1))
                ptr = match_malloc.group(2).lower()
                caller = match_malloc.group(4).lower()

                caller_stats[caller]["malloc"] += 1
                caller_stats[caller]["total"] += 1
                caller_stats[caller]["current_bytes"] += size
                ptr_registry[ptr] = (caller, size)

                if caller_stats[caller]["current_bytes"] > caller_stats[caller]["max_bytes"]:
                    caller_stats[caller]["max_bytes"] = caller_stats[caller]["current_bytes"]

                if caller == target_addr:
                    current_target_footprint += size

                total_all_events += 1
                if target_addr:
                    timeline.append((tick, current_target_footprint))
                    tick += 1
                continue

            # Check for free
            match_free = FREE_PATTERN.search(line)
            if match_free:
                ptr = match_free.group(1).lower()
                caller = match_free.group(4).lower()

                caller_stats[caller]["free"] += 1
                caller_stats[caller]["total"] += 1

                if ptr in ptr_registry:
                    orig_caller, orig_size = ptr_registry.pop(ptr)
                    caller_stats[orig_caller]["current_bytes"] -= orig_size
                    if orig_caller == target_addr:
                        current_target_footprint -= orig_size

                total_all_events += 1
                if target_addr:
                    timeline.append((tick, current_target_footprint))
                    tick += 1
                continue

    if total_all_events == 0:
        print("[-] Error: Found no lines matching the 'MC##' instrumented layouts.")
        sys.exit(1)

    print(f"[+] Processed {total_all_events} total allocation/deallocation lines. Aggregating hot-spots...")

    # Sort callers by global allocation activity count
    sorted_offenders = sorted(caller_stats.items(), key=lambda x: x[1]["total"], reverse=True)[:args.top]

    print("\n" + "="*110)
    print(f" TOP {args.top} HIGH-CHURN MEMORY TRACKING CODE LOCATIONS WITH RETENTION TARGETS")
    print("="*110)
    print(f"{'RANK':<6} | {'CALLER ADDR':<13} | {'MAX BYTES HELD':<16} | {'CHURN ACTIONS':<15} | {'RESOLVED SOURCE LOCATION'}")
    print("-" * 110)

    for rank, (addr, stats) in enumerate(sorted_offenders, 1):
        percentage = (stats["total"] / total_all_events) * 100
        resolved_source = resolve_address(args.addr2line, args.elf, addr)
        addr_hex = f"0x{addr}"
        churn_str = f"{stats['total']} ({percentage:.1f}%)"

        print(f"[{rank:02d}]  | {addr_hex:<13} | {stats['max_bytes']:<16} | {churn_str:<15} | {resolved_source}")
        print(f"       └── Breakdown -> Mallocs: {stats['malloc']} | Frees: {stats['free']} | Reallocs: {stats['realloc']}")

    print("="*110)

    # Output Timeline Lifetime Graph if target configuration passed
    if target_addr:
        print(f"\n" + "="*80)
        print(f" LIFETIME MEMORY FOOTPRINT TIMELINE FOR: 0x{target_addr}")
        print(f" Source: {resolve_address(args.addr2line, args.elf, target_addr)}")
        print(f"" + "="*80)

        if not timeline:
            print("No action records updated target metric data structures.")
            return

        max_seen = max([pt[1] for pt in timeline]) if timeline else 0
        if max_seen == 0:
            print("Target address allocated 0 net bytes across the execution tracking timeline.")
            return

        # Downsample timeline data vectors to fit normal terminal columns cleanly
        cols = 60
        step = max(1, len(timeline) // cols)
        downsampled = timeline[::step][:cols]

        for tick_idx, bytes_held in downsampled:
            bar_length = int((bytes_held / max_seen) * 40) if max_seen > 0 else 0
            bar = "█" * bar_length
            print(f"Tick {tick_idx:04d} [{bytes_held:6} Bytes] | {bar}")
        print("="*80)

if __name__ == "__main__":
    main()
