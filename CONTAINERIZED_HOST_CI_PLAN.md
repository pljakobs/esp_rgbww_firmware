# Containerized Host-Build for Webapp CI — Plan

Status: **Design / feasibility (not yet implemented)**
Date: 2026-07-19
Branch context: `ci/swarm-test`

## Goal

Package the Sming **Host emulator** build of the firmware as a pre-built container
image that transparently exposes the firmware at its own reachable IP address
(HTTP + WebSocket), so the webapp (`esp_rgb_webapp2`) CI can target it as if it
were a real device — no local swarm/bridge plumbing required on the CI runner.

## Key finding: how Host networking actually works

The Sming Host emulator on Linux does **not** bind plain host BSD sockets. It runs
a **full lwIP stack over a TAP device** (`tapif`,
`/opt/sming/Sming/Components/lwip/src/Arch/Host/Linux/lwip_arch.cpp`).

The "external IP" is therefore a first-class, supported concept — it is passed on
the command line. From `testswarm-local.sh` (instance launch):

```sh
LI_CHIP_ID="$chip" "$APP_BIN" \
  --flashfile="$flash" \
  --flashsize="$FLASH_SIZE" \
  --ifname="$tap" \
  --ipaddr="$ip" \
  --gateway="$GATEWAY_IP" \
  --netmask="$NETMASK"
```

Consequence: to get packets in/out of the emulator you **must** have a TAP device.
There is no socket-only mode where a simple `-p 80:80` would suffice. This shapes
the whole container design (needs `NET_ADMIN` + `/dev/net/tun`).

## Container design

### Single node = single container

Entrypoint responsibilities (runs as the container's PID 1 / supervisor):

1. Create TAP: `ip tuntap add tap0 mode tap`.
2. Assign a host-side gateway address on the TAP, e.g. `10.0.0.1/24`, bring it up.
3. Launch the emulator bound to the TAP:
   ```sh
   app --ifname=tap0 --ipaddr=10.0.0.2 --gateway=10.0.0.1 \
       --netmask=255.255.255.0 --flashfile=/data/flash.bin --flashsize=...
   ```
4. Enable forwarding + NAT so the container's own IP transparently proxies to the
   emulator:
   ```sh
   sysctl -w net.ipv4.ip_forward=1
   iptables -t nat -A PREROUTING -p tcp --dport 80 -j DNAT --to-destination 10.0.0.2:80
   iptables -t nat -A POSTROUTING -j MASQUERADE
   ```

Result: from the Docker network, `http://<container-ip>/` (or the container DNS
name) reaches the emulator's HTTP **and** `/ws` WebSocket. The webapp cannot tell
it is talking to an emulator.

### Run flags (unavoidable)

```sh
docker run --cap-add=NET_ADMIN --device=/dev/net/tun ...
```

- Prefer running via `docker run` inside a **CI step** (full flag control) over a
  GitHub Actions `services:` container. `services:` can take
  `options: --cap-add=NET_ADMIN --device=/dev/net/tun`, but only works if the
  runner exposes `/dev/net/tun` (hosted runners do; the step approach is less
  finicky).

### Multi-node swarm as containers

Instead of the host bridge (`br-swarm`) + per-instance TAP (`swtapN`) plumbing in
`testswarm-local.sh`, run **N containers on one user-defined Docker bridge
network**. Each gets its own IP + DNS name automatically.

- mDNS / multicast works **within a single user-defined bridge network**, so the
  swarm convergence test (`/hosts?all=true`, neighbour discovery) runs
  container-to-container.
- Keep all swarm nodes on the **same** Docker network — multicast will not cross
  networks.
- Each node still needs a unique `LI_CHIP_ID` (see `app/host_identity.cpp`) and its
  own `flash.bin` — set via env + per-container volume/baked image.

## Image build

Multi-stage Dockerfile:

- **Builder stage:** full Sming toolchain; runs `make SMING_ARCH=Host` to produce
  the `app` binary and a deterministic `flash.bin` (config + LittleFS).
- **Runtime stage:** slim (glibc) + `app` + `flash.bin` + `iproute2` + `iptables`
  + the entrypoint script. Heavy toolchain layers are discarded.

Publish to GHCR, e.g. `ghcr.io/<org>/rgbww-firmware-host:<fw-version>`, and consume
from the webapp CI workflow.

## Blockers / caveats

1. **Host build currently fails to link.** `make SMING_ARCH=Host` dies with
   ASan/UBSan `undefined reference` errors (`__asan_*`, `__ubsan_*`) in
   `jsonprocessor.o` — sanitizer instrumentation compiled in but the sanitizer
   runtime is not linked (flag mismatch between compile and link). The existing
   `out/Host/debug/firmware/app` binary predates this and the swarm reuses it via
   `SWARM_SKIP_BUILD=1`. **A clean image build must fix this first** — either link
   the sanitizer runtime consistently, or disable ASan/UBSan for the Host CI build.

2. **`System.restart()` kills the process.** On `ARCH_HOST`,
   `WebappOta::activateStaging()` already **skips** the reboot ("skipping reboot on
   host"), so the OTA test path keeps serving in the same process. But any other
   restart path would terminate the container — wrap the emulator launch in a
   supervisor loop in the entrypoint so the container self-heals if needed.

3. **Deterministic start state.** Bake `flash.bin` (config + LittleFS) into the
   image so every `docker run` starts identically — this is what makes
   WebappOTA / serve / sync tests stable and reproducible.

4. **Privileges.** `NET_ADMIN` + `/dev/net/tun` are mandatory (see networking
   finding). Plan CI around that; do not expect an unprivileged image to work.

## How the webapp consumes it

- Point the webapp's dev/CI API host at the container IP/DNS name. The webapp reads
  its target from `src/config/localOverrides.js`
  (`localhost.ip_address`) via `src/stores/storeConstants.js`; REST goes to
  `http://<ip>/<endpoint>` (`src/services/api.js`) and the WebSocket to
  `ws://<ip>/ws` (`src/services/initializeStores.js`).
- Highest-value reuse: drive the **real** Pinia `appDataStore` distributed
  sync-lock protocol against a booted node (or a multi-container swarm) from the
  webapp's existing Vitest suite (`vitest run`), injecting a `WebSocket` global
  (e.g. the `ws` package) since jsdom lacks one.

## Open decisions / next steps

- [ ] Fix (or disable) the ASan/UBSan Host link failure so `make SMING_ARCH=Host`
      produces `app` cleanly in the builder stage.
- [ ] Write the multi-stage `Dockerfile` + `entrypoint.sh` (TAP + DNAT + supervisor).
- [ ] Decide single-node vs swarm compose layout for CI.
- [ ] Add a webapp CI workflow that pulls the GHCR image and runs Vitest/Playwright
      against it.
- [ ] Decide whether `flash.bin` is baked into the image or mounted per run.
