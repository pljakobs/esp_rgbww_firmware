#!/usr/bin/env bash
#
# testswarm.sh — build the Sming Host emulator on the host, then run it INSIDE a
# podman container that is fully NAT'd to its container IP on a shared bridge
# network, so other containers (e.g. an nginx webserver) can reach the emulated
# firmware by this container's name/IP.
#
#   Host side (this script):  build (shared with testnet.sh via testlib.sh),
#                             image build, `podman run`.
#   Container side:           docker/testswarm.Containerfile + testswarm-run.sh
#                             (TAP interface, 1:1 NAT, launch under MODE).
#
# testnet.sh remains the single-instance harness (host TAP, no container).
#
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$REPO_ROOT/testlib.sh"

# ---------------------------------------------------------------------------
# Configuration
# ---------------------------------------------------------------------------
RUNTIME="${RUNTIME:-podman}"
IMAGE="${IMAGE:-rgbww-hostnet:latest}"
CONTAINER_NAME="${CONTAINER_NAME:-rgbww-app}"
NETWORK="${NETWORK:-containerized_rgbww_network}"

HOST_RUN_DIR="$REPO_ROOT/out/Host/debug"
DIAG_DIR="$REPO_ROOT/out/host-diag"
CONTAINERFILE="$REPO_ROOT/docker/testswarm.Containerfile"
ENTRYPOINT="$REPO_ROOT/testswarm-run.sh"
TESTLIB="$REPO_ROOT/testlib.sh"
VALGRIND_SUPP="$REPO_ROOT/.github/scripts/valgrind.supp"

# ---------------------------------------------------------------------------
# Diagnostic mode selection (same set as testnet.sh)
# ---------------------------------------------------------------------------
# Usage:  ./testswarm.sh [mode]     (or set TESTNET_MODE=<mode>)
MODE="${1:-${TESTNET_MODE:-memcheck}}"
tl_validate_mode "$MODE" || exit 2
ENABLE_SANITIZERS_VAL="$(tl_sanitizers_for_mode "$MODE")"

echo "[+] Diagnostic mode: $MODE (ENABLE_SANITIZERS=$ENABLE_SANITIZERS_VAL)"

mkdir -p "$DIAG_DIR"

cleanup() {
  echo -e "\n[+] Removing container $CONTAINER_NAME..."
  "$RUNTIME" rm -f "$CONTAINER_NAME" >/dev/null 2>&1 || true
}
trap cleanup EXIT

# ---------------------------------------------------------------------------
# 1. Build the Host binary (on the host — the runtime image has no toolchain)
# ---------------------------------------------------------------------------
echo "[+] Compiling target binaries for Host architecture..."
tl_source_sming
tl_build "$REPO_ROOT" "$ENABLE_SANITIZERS_VAL"

if [[ ! -d "$HOST_RUN_DIR" ]]; then
  echo "[-] Error: Host build directory not found at $HOST_RUN_DIR" >&2
  exit 1
fi

# ---------------------------------------------------------------------------
# 2. Ensure the runtime image exists
# ---------------------------------------------------------------------------
if [[ "${REBUILD_IMAGE:-0}" == "1" ]] || ! "$RUNTIME" image exists "$IMAGE"; then
  echo "[+] Building runtime image $IMAGE..."
  "$RUNTIME" build -t "$IMAGE" -f "$CONTAINERFILE" "$REPO_ROOT/docker"
fi

# ---------------------------------------------------------------------------
# 3. Ensure the shared bridge network exists
# ---------------------------------------------------------------------------
if ! "$RUNTIME" network exists "$NETWORK"; then
  echo "[+] Creating bridge network $NETWORK..."
  "$RUNTIME" network create "$NETWORK"
fi

# ---------------------------------------------------------------------------
# 4. Run the emulator inside the container
# ---------------------------------------------------------------------------
"$RUNTIME" rm -f "$CONTAINER_NAME" >/dev/null 2>&1 || true

SUPP_MOUNT=()
[[ -f "$VALGRIND_SUPP" ]] && SUPP_MOUNT=(-v "$VALGRIND_SUPP:/run/valgrind.supp:ro")

echo "[+] Starting container $CONTAINER_NAME on network $NETWORK (mode: $MODE)."
echo "[+] Other containers on '$NETWORK' can reach the firmware at http://$CONTAINER_NAME/"
echo "[+] Press Ctrl+C to stop and clean up."

exec "$RUNTIME" run --rm -it \
  --name "$CONTAINER_NAME" \
  --network "$NETWORK" \
  --cap-add NET_ADMIN \
  --cap-add SYS_PTRACE \
  --device /dev/net/tun \
  --security-opt label=disable \
  -e "TESTNET_MODE=$MODE" \
  -e "FLASH_SIZE=$TL_FLASH_SIZE" \
  -v "$HOST_RUN_DIR:/run/host" \
  -v "$DIAG_DIR:/run/diag" \
  -v "$ENTRYPOINT:/usr/local/bin/testswarm-run.sh:ro" \
  -v "$TESTLIB:/usr/local/bin/testlib.sh:ro" \
  "${SUPP_MOUNT[@]}" \
  "$IMAGE" "$MODE"
