#!/usr/bin/env python3
import sys
import os
import re
import signal
import subprocess
import argparse
import threading
from collections import defaultdict

from textual.app import App, ComposeResult
from textual.widgets import Header, Footer, DataTable, Static
from textual.containers import Horizontal, Vertical
from textual.reactive import reactive
import plotille

DEFAULT_ADDR2LINE = "/opt/esp-quick-toolchain/xtensa-lx106-elf/bin/xtensa-lx106-elf-addr2line"

# Regex patterns matching firmware output formats
MALLOC_PATTERN = re.compile(r"MC## malloc\((\d+)\)\s*=\s*(?:0x)?([0-9a-fA-F]+)\s*\(cur\s*(\d+)\),\s*callers?\s*([0-9a-fA-F,]+)")
FREE_PATTERN = re.compile(r"MC## free\((?:0x)?([0-9a-fA-F]+)\)\s*->\s*(\d+)?\s*\(cur\s*(\d+)\),\s*callers?\s*([0-9a-fA-F,]+)")
REALLOC_PATTERN = re.compile(r"MC## realloc\((\d+)\s*->\s*(\d+)\)\s*=\s*(?:0x)?([0-9a-fA-F]+)(?:\s*->\s*(?:0x)?([0-9a-fA-F]+))?\s*\(cur\s*(\d+)\),\s*callers?\s*([0-9a-fA-F,]+)")
RESET_PATTERN = "===starting cpu profiling==="

MODULE_RULES = [
    (re.compile(r"app_main|run_boot_appl"), "MODULE: Application Bootstrap"),
    (re.compile(r"vPort|pvPort|malloc|free|realloc"), "MODULE: Heap Allocator Internal"),
    (re.compile(r"vTask|xTask|lwip"), "MODULE: Operating System Kernel"),
    (re.compile(r"vsnprintf|sprintf|printf|Print|stream|String"), "MODULE: Core Print & Strings"),
    (re.compile(r"Web|Http|HttpClient|Network|Wlan|WiFi"), "MODULE: Network Stack Component"),
]

class HeapTrackerState:
    def __init__(self, elf_path, addr2line_path):
        self.elf_path = elf_path
        self.addr2line_path = addr2line_path

        self.allocated_bytes = defaultdict(int)
        self.peak_bytes = defaultdict(int)
        self.alloc_counts = defaultdict(int)
        self.free_counts = defaultdict(int)

        self.ptr_registry = {}

        self.min_seen_ptr = 0xFFFFFFFF
        self.max_seen_ptr = 0

        self.symbol_cache = {}
        self.history = []
        self.total_heap_usage = 0
        self.lock = threading.Lock()

    def classify_module(self, symbol_text):
        for pattern, classification in MODULE_RULES:
            if pattern.search(symbol_text):
                return classification
        return None

    def resolve_address(self, addr_hex):
        primary_addr = addr_hex.strip().lower()
        if primary_addr in self.symbol_cache:
            return self.symbol_cache[primary_addr]

        try:
            res = subprocess.run(
                [self.addr2line_path, "-f", "-e", self.elf_path, f"0x{primary_addr}"],
                capture_output=True, text=True, check=True
            )
            lines = res.stdout.strip().split("\n")
            if len(lines) >= 2:
                func = lines[0].strip()
                loc = lines[1].strip()
                if func == "??":
                    identity = f"0x{primary_addr}"
                else:
                    classification = self.classify_module(func)
                    if classification:
                        identity = classification
                    else:
                        identity = f"{func} ({loc.split('/')[-1]})"
            else:
                identity = f"0x{primary_addr}"
        except Exception:
            identity = f"0x{primary_addr}"

        self.symbol_cache[primary_addr] = identity
        return identity

    def process_malloc(self, size_str, addr_str, cur_heap_str, caller_str):
        size = int(size_str)
        addr = int(addr_str, 16)

        with self.lock:
            self.total_heap_usage = int(cur_heap_str)
            resolved_identity = self.resolve_address(caller_str)

            self.ptr_registry[addr] = (resolved_identity, size)

            self.allocated_bytes[resolved_identity] += size
            if self.allocated_bytes[resolved_identity] > self.peak_bytes[resolved_identity]:
                self.peak_bytes[resolved_identity] = self.allocated_bytes[resolved_identity]

            self.alloc_counts[resolved_identity] += 1

            if addr > 0:
                self.min_seen_ptr = min(self.min_seen_ptr, addr)
                self.max_seen_ptr = max(self.max_seen_ptr, addr + size)

            self.history.append(self.total_heap_usage)
            if len(self.history) > 100:
                self.history.pop(0)

    def process_free(self, addr_str, reported_size_str, cur_heap_str, current_caller_str):
        addr = int(addr_str, 16)

        with self.lock:
            self.total_heap_usage = int(cur_heap_str)
            if addr in self.ptr_registry:
                origin_identity, size = self.ptr_registry.pop(addr)
                if size > 0:
                    self.allocated_bytes[origin_identity] = max(0, self.allocated_bytes[origin_identity] - size)
                self.free_counts[origin_identity] += 1

            self.history.append(self.total_heap_usage)
            if len(self.history) > 100:
                self.history.pop(0)

    def process_realloc(self, old_size_str, new_size_str, first_addr_str, second_addr_str, cur_heap_str, caller_str):
        old_size = int(old_size_str)
        new_size = int(new_size_str)

        if not second_addr_str:
            old_addr = int(first_addr_str, 16)
            new_addr = old_addr
        else:
            old_addr = int(first_addr_str, 16)
            new_addr = int(second_addr_str, 16)

        with self.lock:
            self.total_heap_usage = int(cur_heap_str)

            if old_addr in self.ptr_registry:
                origin_identity, tracked_old_size = self.ptr_registry.pop(old_addr)
                old_size = tracked_old_size
            else:
                origin_identity = self.resolve_address(caller_str)

            self.allocated_bytes[origin_identity] = max(0, self.allocated_bytes[origin_identity] - old_size)

            self.ptr_registry[new_addr] = (origin_identity, new_size)
            self.allocated_bytes[origin_identity] += new_size

            if self.allocated_bytes[origin_identity] > self.peak_bytes[origin_identity]:
                self.peak_bytes[origin_identity] = self.allocated_bytes[origin_identity]

            self.alloc_counts[origin_identity] += 1

            if new_addr > 0:
                self.min_seen_ptr = min(self.min_seen_ptr, new_addr)
                self.max_seen_ptr = max(self.max_seen_ptr, new_addr + new_size)

            self.history.append(self.total_heap_usage)
            if len(self.history) > 100:
                self.history.pop(0)

    def reset_metrics(self):
        with self.lock:
            self.allocated_bytes.clear()
            self.peak_bytes.clear()
            self.alloc_counts.clear()
            self.free_counts.clear()
            self.ptr_registry.clear()
            self.min_seen_ptr = 0xFFFFFFFF
            self.max_seen_ptr = 0
            self.history.clear()
            self.total_heap_usage = 0


class BlockInspector(Static):
    """Displays localized heap blocks and their corresponding source code owners on hover."""
    current_inspection = reactive(None)

    def render(self) -> str:
        if not self.current_inspection:
            return (
                "[bold green]BLOCK SECTOR INSPECTOR[/bold green]\n\n"
                "Move mouse over the Fragmentation Grid to inspect block details."
            )

        sector_start, sector_end, bytes_per_char, blocks = self.current_inspection

        output = [
            "[bold green]BLOCK SECTOR INSPECTOR[/bold green]",
            f" Sector Range: [cyan]0x{sector_start:08x} - 0x{sector_end:08x}[/cyan]",
            f" Sector Width: [cyan]{bytes_per_char / 1024:.2f} KiB[/cyan]",
            f"\nAllocations Found ({len(blocks)}):",
            "---------------------------------------"
        ]

        if not blocks:
            output.append(" [dim]No active allocations reside in this slice.[/dim]")
        else:
            for addr, size, identity in blocks:
                output.append(f" 📦 [bold green]0x{addr:08x}[/bold green] ({size:,} B)")
                output.append(f"   └── [dim]{identity}[/dim]")

        return "\n".join(output)


class HeapBitmapView(Static):
    def __init__(self, tracker_state, **kwargs):
        super().__init__(**kwargs)
        self.tracker = tracker_state
        self.display_width = 40

    def render(self) -> str:
        with self.tracker.lock:
            min_ptr = self.tracker.min_seen_ptr
            max_ptr = self.tracker.max_seen_ptr
            allocs = sorted([(p, sz, ident) for p, (ident, sz) in self.tracker.ptr_registry.items()], key=lambda x: x[0])

        if not allocs or min_ptr >= max_ptr:
            return "No Active Allocations Tracked."

        total_bytes = max_ptr - min_ptr
        bytes_per_char = max(1.0, total_bytes / self.display_width)

        grid = ["."] * self.display_width

        for start_p, size, ident in allocs:
            start_idx = int((start_p - min_ptr) / bytes_per_char)
            end_idx = int((start_p + size - min_ptr) / bytes_per_char)

            start_idx = max(0, min(start_idx, self.display_width - 1))
            end_idx = max(start_idx, min(end_idx, self.display_width - 1))

            char_marker = "█"
            if "Network" in ident:
                char_marker = "[bold cyan]N[/bold cyan]"
            elif "Json" in ident:
                char_marker = "[bold yellow]J[/bold yellow]"
            elif "Allocator" in ident:
                char_marker = "[bold red]A[/bold red]"
            elif "String" in ident:
                char_marker = "[bold magenta]S[/bold magenta]"

            for i in range(start_idx, end_idx + 1):
                grid[i] = char_marker

        meta_info = (
            f"[bold cyan]Segment Map Analytics:[/bold cyan]\n"
            f"  ├── Heap Base : 0x{min_ptr:08x}\n"
            f"  ├── Heap End  : 0x{max_ptr:08x}\n"
            f"  ├── Total Size: {total_bytes / 1024:.2f} KiB\n"
            f"  └── Resolution: {bytes_per_char:.1f} B/char"
        )
        return "Fragmentation Grid (Hover to inspect):\n" + "".join(grid) + "\n\n" + meta_info

    def on_mouse_move(self, event) -> None:
        with self.tracker.lock:
            min_ptr = self.tracker.min_seen_ptr
            max_ptr = self.tracker.max_seen_ptr
            if min_ptr >= max_ptr or not self.tracker.ptr_registry:
                return

            total_bytes = max_ptr - min_ptr
            bytes_per_char = max(1.0, total_bytes / self.display_width)

            # Match exact row offset for the terminal grid rendering line
            if event.y != 1:
                return

            cursor_char_x = event.x
            if cursor_char_x < 0 or cursor_char_x >= self.display_width:
                return

            sector_start = int(min_ptr + (cursor_char_x * bytes_per_char))
            sector_end = int(sector_start + bytes_per_char)

            intersecting_blocks = []
            for p_addr, (identity, size) in self.tracker.ptr_registry.items():
                if max(sector_start, p_addr) < min(sector_end, p_addr + size):
                    intersecting_blocks.append((p_addr, size, identity))

            try:
                inspector = self.app.query_one("#block-inspector", BlockInspector)
                inspector.current_inspection = (sector_start, sector_end, bytes_per_char, intersecting_blocks)
            except Exception:
                pass


class HeapTrackerApp(App):
    CSS = """
    Screen { background: #1e1e1e; }
    #top-bar { height: 3; background: #2d2d2d; color: #ffffff; padding: 0 2; }
    #main-layout { layout: vertical; height: 1fr; }
    #table-panel { height: 50%; border: solid #3c3c3c; }
    #lower-layout { layout: horizontal; height: 50%; }
    #chart-panel { width: 35%; border: solid #3c3c3c; padding: 1; }
    #bitmap-panel { width: 35%; border: solid #3c3c3c; padding: 1; }
    #inspector-panel { width: 30%; border: solid #3c3c3c; padding: 1; background: #1a1a1a; }
    DataTable { background: #1e1e1e; color: #e0e0e0; height: 100%; }
    Static { color: #e0e0e0; }
    #app-title { width: 1fr; content-align: left middle; height: 100%; }
    #heap-size-display { width: auto; content-align: right middle; height: 100%; }
    """
    BINDINGS = [
        ("q", "quit", "Quit Dashboard"),
        ("c", "clear_stats", "Reset Data Counters")
    ]

    def __init__(self, state_tracker):
        super().__init__()
        self.state_tracker = state_tracker

    def compose(self) -> ComposeResult:
        yield Header()
        with Horizontal(id="top-bar"):
            yield Static("Sming Live Heap Analytics Monitor", id="app-title")
            yield Static("Active Allocation Footprint: 0 Bytes", id="heap-size-display")
        with Vertical(id="main-layout"):
            yield Vertical(DataTable(id="metrics-table"), id="table-panel")
            yield Horizontal(
                Vertical(Static("CONTEXT LIFETIME TIMELINE (TRENDS IN KiB)\n", id="chart-header"), Static("", id="live-chart"), id="chart-panel"),
                Vertical(HeapBitmapView(self.state_tracker, id="live-bitmap"), id="bitmap-panel"),
                Vertical(BlockInspector(id="block-inspector"), id="inspector-panel"),
                id="lower-layout"
            )
        yield Footer()

def on_mount(self) -> None:
    table = self.query_one("#metrics-table", DataTable)
    table.add_column("Context Identity", key="identity")
    table.add_column("Max Bytes", key="max_bytes")
    table.add_column("Live Bytes", key="live_bytes")
    table.add_column("Allocs", key="allocs")
    table.add_column("Reallocs", key="reallocs")
    table.add_column("Frees", key="frees")

    def refresh_dashboard_view(self) -> None:
        table = self.query_one("#metrics-table", DataTable)
        heap_display = self.query_one("#heap-size-display", Static)
        chart_display = self.query_one("#live-chart", Static)
        bitmap_display = self.query_one("#live-bitmap", HeapBitmapView)

        with self.state_tracker.lock:
            heap_display.update(f"Active Allocation Footprint: {self.state_tracker.total_heap_usage:,} Bytes")

            snapshot_rows = []
            for identity in self.state_tracker.allocated_bytes.keys():
                snapshot_rows.append((
                    identity,
                    self.state_tracker.allocated_bytes[identity],
                    self.state_tracker.peak_bytes[identity],
                    self.state_tracker.alloc_counts[identity],
                    self.state_tracker.free_counts[identity]
                ))
            snapshot_rows.sort(key=lambda x: x[1], reverse=True)

            current_keys = set()
            for row in snapshot_rows:
                identity = row[0]
                row_key = f"row_{identity}" # Create a stable internal key
                current_keys.add(row_key)

                if row_key in table.rows:
                    table.update_cell(row_key, "max_bytes", f"{row[2]:,}")
                    table.update_cell(row_key, "live_bytes", f"{row[1]:,}")
                    table.update_cell(row_key, "allocs", str(row[3]))
                    table.update_cell(row_key, "frees", str(row[4]))
                else:
                    table.add_row(
                        identity, f"{row[2]:,}", f"{row[1]:,}", str(row[3]), "0", str(row[4]),
                        key=row_key
                    )

            # Clean up stale rows
            for existing_key in list(table.rows.keys()):
                if existing_key not in current_keys:
                    table.remove_row(existing_key)

            # Update waveform chart
            if len(self.state_tracker.history) > 1:
                plot = plotille.plot(range(len(self.state_tracker.history)), self.state_tracker.history, width=36, height=10, interp="linear")
                chart_display.update(f"[cyan]{plot}[/cyan]")
            else:
                chart_display.update("\n\n   Waiting for data...")

        bitmap_display.refresh()

    def action_clear_stats(self) -> None:
        self.state_tracker.reset_metrics()
        self.query_one("#metrics-table", DataTable).clear()

def stream_reader_worker(state_tracker, log_source):
    last_line_signature = None

    while True:
        try:
            raw_line = log_source.readline()
            if not raw_line:
                break
            line = raw_line.decode("utf-8", errors="ignore").strip()

            if RESET_PATTERN in line:
                state_tracker.reset_metrics()
                last_line_signature = None
                continue

            if line == last_line_signature:
                continue
            last_line_signature = line

            if "MC##" in line:
                idx = line.rfind("MC##")
                if idx > 0:
                    line = line[idx:]

            malloc_match = MALLOC_PATTERN.search(line)
            if malloc_match:
                size_str, addr_str, cur_heap_str, callers_str = malloc_match.groups()
                primary_caller = callers_str.split(',')[0]
                state_tracker.process_malloc(size_str, addr_str, cur_heap_str, primary_caller)
                continue

            free_match = FREE_PATTERN.search(line)
            if free_match:
                addr_str, reported_size_str, cur_heap_str, callers_str = free_match.groups()
                primary_caller = callers_str.split(',')[0]
                state_tracker.process_free(addr_str, reported_size_str, cur_heap_str, primary_caller)
                continue

            realloc_match = REALLOC_PATTERN.search(line)
            if realloc_match:
                old_size_str, new_size_str, first_addr_str, second_addr_str, cur_heap_str, callers_str = realloc_match.groups()
                primary_caller = callers_str.split(',')[0]
                state_tracker.process_realloc(old_size_str, new_size_str, first_addr_str, second_addr_str, cur_heap_str, primary_caller)
                continue
        except Exception:
            continue

if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="Live btop-style Sming heap tracker interface.")
    parser.add_argument("-e", "--elf", required=True, help="Path to your app_0.out ELF image")
    parser.add_argument("-a", "--addr2line", default=DEFAULT_ADDR2LINE, help="Path to toolchain addr2line")
    args = parser.parse_args()

    if not sys.stdin.isatty():
        piped_fd = os.dup(sys.stdin.fileno())
        log_source = os.fdopen(piped_fd, "rb")

        tty_fd = os.open("/dev/tty", os.O_RDONLY)
        os.dup2(tty_fd, sys.stdin.fileno())
        os.close(tty_fd)

        sys.stdin = open(sys.stdin.fileno(), "r", encoding="utf-8")
    else:
        print("[-] Error: Script must be executed via an incoming data pipeline.")
        sys.exit(1)

    state = HeapTrackerState(args.elf, args.addr2line)

    worker = threading.Thread(target=stream_reader_worker, args=(state, log_source), daemon=True)
    worker.start()

    app = HeapTrackerApp(state)
    app.run()