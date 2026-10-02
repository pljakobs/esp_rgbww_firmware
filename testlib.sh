#!/usr/bin/env bash
#
# testlib.sh — shared helpers for the Host-emulator test harnesses.
#

TL_TAP_IF="${TL_TAP_IF:-tap0}"
TL_HOST_CIDR="${TL_HOST_CIDR:-192.168.13.1/24}"
TL_HOST_IP="${TL_HOST_IP:-192.168.13.1}"
TL_APP_IP="${TL_APP_IP:-192.168.13.2}"
TL_NETMASK="${TL_NETMASK:-255.255.255.0}"
TL_APP_SUBNET="${TL_APP_SUBNET:-192.168.13.0/24}"
TL_FLASH_SIZE="${FLASH_SIZE:-4M}"

TL_APP_BIN="firmware/app"
TL_FLASH_BIN="firmware/flash.bin"

TL_MODE_HELP="Valid modes:
  memcheck  (default) Valgrind memcheck, hardened flags — best for HEAP CORRUPTION.
  dhat      Valgrind DHAT heap profiler — best proxy for HEAP FRAGMENTATION.
  massif    Valgrind Massif — heap + stack time-series.
  asan      Build with ASan+UBSan, run WITHOUT valgrind — only tool for STACK corruption.
  gdb       Run under gdbserver (no instrumentation); attach gdb from another terminal.
  none      Run the binary directly, no instrumentation."

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

tl_sanitizers_for_mode() {
  [[ "$1" == "asan" ]] && echo 1 || echo 0
}

tl_source_sming() {
  if [[ -f /opt/Sming/Tools/export.sh ]]; then
    set +u; source /opt/Sming/Tools/export.sh; set -u
  elif [[ -f /opt/sming/Tools/export.sh ]]; then
    set +u; source /opt/sming/Tools/export.sh; set -u
  else
    true
  fi
}

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
      ENABLE_GDB=0 \
      ENABLE_GDB_CONSOLE=0 \
       ENABLE_SANITIZERS="$san" \
       COM_SPEED=115200
  mkdir -p "$(dirname "$marker")"
  echo "$san" > "$marker"
}

# Hilfsfunktion auf Top-Level Ebene
tl_run_cmd() {
  if [[ -n "${LOG_FILE:-}" ]]; then
    # Relativen Log-Pfad in absoluten Pfad umwandeln
    local log_path="$LOG_FILE"
    [[ "$log_path" != /* ]] && log_path="$REPO_ROOT/$log_path"
    
    mkdir -p "$(dirname "$log_path")"
    echo "[+] Teeing application output to: $log_path"
    "$@" 2>&1 | tee -a "$log_path"
  else
    "$@"
  fi
}

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
      tl_run_cmd valgrind --tool=memcheck \
               --leak-check=full \
               --show-leak-kinds=all \
               --track-origins=yes \
               --read-var-info=yes \
               --num-callers=40 \
               --freelist-vol=50000000 \
               --malloc-fill=0xAA \
               --free-fill=0xDD \
               --vgdb=yes \
               --log-file="$diag_dir/valgrind-memcheck.log" \
               "${supp_arg[@]}" \
               "$app_bin" "${app_args[@]}"
      ;;

    dhat)
      echo "[+] DHAT output: $diag_dir/dhat.out.<pid>  (view with dh_view.html)"
      tl_run_cmd valgrind --tool=dhat \
               --dhat-out-file="$diag_dir/dhat.out.%p" \
               --num-callers=40 \
               --log-file="$diag_dir/valgrind-dhat.log" \
               "${supp_arg[@]}" \
               "$app_bin" "${app_args[@]}"
      ;;

    massif)
      echo "[+] Massif output: $diag_dir/massif.out.<pid>  (view with ms_print)"
      tl_run_cmd valgrind --tool=massif \
               --threshold=0.05 \
               --ignore-fn=allocateHeapHog \
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
      tl_run_cmd "$app_bin" "${app_args[@]}"
      ;;

    none)
      echo "[+] Running uninstrumented."
      tl_run_cmd "$app_bin" "${app_args[@]}"
      ;;

    gdb)
      local port="${GDB_PORT:-1234}"
      local abs_bin; abs_bin="$(pwd)/$app_bin"
      if ! command -v gdbserver >/dev/null 2>&1; then
        echo "[-] gdbserver not found. Install it." >&2
        return 3
      fi
      echo "[+] gdbserver listening on :$port"
      tl_run_cmd gdbserver ":$port" "$app_bin" "${app_args[@]}"
      ;;

    *)
      echo "[-] tl_launch: unknown mode '$mode'." >&2
      return 2
      ;;
  esac
}