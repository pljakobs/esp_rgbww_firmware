#!/usr/bin/env bash
#
# testlib.sh — shared helpers for the Host-emulator test harnesses.
#
#   testnet.sh    single-instance harness  (host TAP + NAT, runs the app directly)
#   testswarm.sh  container harness         (runs the app inside a podman container
#                                            NAT'd to its container IP on a shared net)
#
# This file only defines functions/constants; source it, do not execute it.
#
# The diagnostic MODE system (memcheck/dhat/massif/asan/none) and the Host build
# rules live here so both harnesses stay in lock-step.

# ---------------------------------------------------------------------------
# Network layout (shared by both harnesses)
# ---------------------------------------------------------------------------
TL_TAP_IF="${TL_TAP_IF:-tap0}"
TL_HOST_CIDR="${TL_HOST_CIDR:-192.168.13.1/24}"
TL_HOST_IP="${TL_HOST_IP:-192.168.13.1}"
TL_APP_IP="${TL_APP_IP:-192.168.13.2}"
TL_NETMASK="${TL_NETMASK:-255.255.255.0}"
TL_APP_SUBNET="${TL_APP_SUBNET:-192.168.13.0/24}"
TL_FLASH_SIZE="${FLASH_SIZE:-4M}"

# App binary/flash paths relative to the Host run dir (out/Host/debug).
TL_APP_BIN="firmware/app"
TL_FLASH_BIN="firmware/flash.bin"

# ---------------------------------------------------------------------------
# Diagnostic mode helpers
# ---------------------------------------------------------------------------
# Usage text describing every supported mode (printed by callers on error).
TL_MODE_HELP="Valid modes:
  memcheck  (default) Valgrind memcheck, hardened flags — best for HEAP CORRUPTION.
  dhat      Valgrind DHAT heap profiler — best proxy for HEAP FRAGMENTATION.
  massif    Valgrind Massif — heap + stack time-series.
  asan      Build with ASan+UBSan, run WITHOUT valgrind — only tool for STACK corruption.
  gdb       Run under gdbserver (no instrumentation); attach gdb from another terminal.
  none      Run the binary directly, no instrumentation."

# tl_validate_mode <mode> -> 0 if valid, 2 otherwise
tl_validate_mode() {
  case "$1" in
    memcheck|dhat|massif|asan|gdb|none) return 0 ;;
    *)
      echo "[-] Unknown mode '$1'." >&2
      echo "$TL_MODE_HELP" >&2
      return 2
      ;;
  esac
}

# tl_sanitizers_for_mode <mode> -> prints "1" for asan, "0" otherwise.
# ASan builds are the only ones with sanitizers; every valgrind mode builds
# WITHOUT them so the instrumented binary is never run under valgrind.
tl_sanitizers_for_mode() {
  [[ "$1" == "asan" ]] && echo 1 || echo 0
}

# ---------------------------------------------------------------------------
# Build helpers (used by testnet.sh and testswarm.sh on the host)
# ---------------------------------------------------------------------------

# tl_source_sming — source the Sming environment if present.
tl_source_sming() {
  if [[ -f /opt/Sming/Tools/export.sh ]]; then
    set +u; source /opt/Sming/Tools/export.sh; set -u
  elif [[ -f /opt/sming/Tools/export.sh ]]; then
    set +u; source /opt/sming/Tools/export.sh; set -u
  fi
}

# tl_build <repo_root> <enable_sanitizers 0|1>
# Builds the Host binary. Toggling ENABLE_SANITIZERS does not fully invalidate
# Sming's incremental build (stale .o keep __asan_*/__ubsan_* references while
# the relink omits the sanitizer runtime -> "undefined reference to __asan_*"
# link failures), so we force a clean whenever the sanitizer state changes.
tl_build() {
  local repo_root="$1" san="$2"
  local marker="$repo_root/out/.testnet_sanitizers" last=""
  [[ -f "$marker" ]] && last="$(cat "$marker" 2>/dev/null || true)"
  if [[ "$last" != "$san" ]]; then
    echo "[+] Sanitizer state changed ('$last' -> '$san'); cleaning Host build..."
    make SMING_ARCH=Host clean >/dev/null
  fi
  make SMING_ARCH=Host flash \
       DISABLE_WERROR=1 \
       ENABLE_HOSTFS=0 \
       ENABLE_SANITIZERS="$san" \
       COM_SPEED=115200
  mkdir -p "$(dirname "$marker")"
  echo "$san" > "$marker"
}

# ---------------------------------------------------------------------------
# Launch helper (used on the host by testnet.sh and inside the container by
# testswarm-run.sh — identical instrumentation either way)
# ---------------------------------------------------------------------------

# tl_launch <mode> <app_bin> <diag_dir> <supp_file> -- <app_args...>
#   app_bin   path to the app binary (relative to the current dir)
#   diag_dir  absolute dir for valgrind/asan artifacts
#   supp_file valgrind suppressions file ("" to skip)
tl_launch() {
  local mode="$1" app_bin="$2" diag_dir="$3" supp="$4"
  shift 4
  [[ "${1:-}" == "--" ]] && shift
  local app_args=("$@")

  local supp_arg=()
  [[ -n "$supp" && -f "$supp" ]] && supp_arg=(--suppressions="$supp")
  mkdir -p "$diag_dir"

  case "$mode" in
    memcheck)
      echo "[+] memcheck log: $diag_dir/valgrind-memcheck.log"
      echo "[+] on-demand snapshots: run 'vgdb --pid=<pid> monitor leak_check' in another terminal"
      valgrind --tool=memcheck \
               --leak-check=full \
               --show-leak-kinds=all \
               --track-origins=yes \
               --read-var-info=yes \
               --num-callers=40 \
               --freelist-vol=50000000 \
               --malloc-fill=0xAA \
               --free-fill=0xDD \
               --vgdb=yes \
               --vgdb-error=0 \
               --log-file="$diag_dir/valgrind-memcheck.log" \
               "${supp_arg[@]}" \
               "$app_bin" "${app_args[@]}"
      ;;

    dhat)
      echo "[+] DHAT output: $diag_dir/dhat.out.<pid>  (view with dh_view.html)"
      valgrind --tool=dhat \
               --dhat-out-file="$diag_dir/dhat.out.%p" \
               --num-callers=40 \
               --log-file="$diag_dir/valgrind-dhat.log" \
               "${supp_arg[@]}" \
               "$app_bin" "${app_args[@]}"
      ;;

    massif)
      echo "[+] Massif output: $diag_dir/massif.out.<pid>  (view with ms_print)"
      valgrind --tool=massif \
               --stacks=yes \
               --massif-out-file="$diag_dir/massif.out.%p" \
               --detailed-freq=1 \
               --max-snapshots=200 \
               --log-file="$diag_dir/valgrind-massif.log" \
               "${supp_arg[@]}" \
               "$app_bin" "${app_args[@]}"
      ;;

    asan)
      echo "[+] ASan log: $diag_dir/asan.log  UBSan log: $diag_dir/ubsan.log"
      export ASAN_OPTIONS="detect_leaks=1:detect_stack_use_after_return=1:strict_string_checks=1:check_initialization_order=1:abort_on_error=1:log_path=$diag_dir/asan.log"
      export UBSAN_OPTIONS="print_stacktrace=1:halt_on_error=1:log_path=$diag_dir/ubsan.log"
      "$app_bin" "${app_args[@]}"
      ;;

    none)
      echo "[+] Running uninstrumented."
      "$app_bin" "${app_args[@]}"
      ;;

    gdb)
      # Run under gdbserver so a debugger can attach from OUTSIDE this process
      # while the app keeps the full testnet environment (TAP interface + args).
      # gdbserver waits for the debugger to connect before starting the app, and
      # stops it in gdb on SIGSEGV/SIGABRT so 'bt' shows the real crash site.
      local port="${GDB_PORT:-1234}"
      local abs_bin; abs_bin="$(pwd)/$app_bin"
      if ! command -v gdbserver >/dev/null 2>&1; then
        echo "[-] gdbserver not found. Install it (e.g. 'sudo dnf install gdb-gdbserver' or 'sudo apt install gdbserver')." >&2
        return 3
      fi
      echo "[+] gdbserver listening on :$port (waits for the debugger before running)."
      echo "[+] Attach from another terminal with:"
      echo "      gdb '$abs_bin' -ex 'target remote :$port' -ex 'handle SIG34 SIG35 SIG36 SIG33 nostop noprint pass' -ex continue"
      echo "[+] On SIGSEGV/SIGABRT the app stops in gdb; use 'bt' / 'bt full' to inspect."
      gdbserver ":$port" "$app_bin" "${app_args[@]}"
      ;;

    *)
      echo "[-] tl_launch: unknown mode '$mode'." >&2
      return 2
      ;;
  esac
}
