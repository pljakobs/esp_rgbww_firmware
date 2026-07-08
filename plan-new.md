# Hardware-in-the-Loop CI Plan

## Goal

Add reliable on-hardware testing to firmware CI so each run can validate:

- built artifacts are correct
- deploy and OTA update paths work end-to-end
- post-deploy API behavior is verified on real boards

Target boards:

- ESP8266
- ESP32
- ESP32-C3

## Proposed Lab Architecture

1. Host model

- Use a dedicated VM with USB passthrough for all three boards.
- Install one GitHub self-hosted runner in the VM.
- Let runner connect outbound to GitHub Actions (preferred over inbound SSH control).

2. Device connectivity

- Attach all boards via a stable USB hub.
- Create persistent udev symlinks per board (for example `/dev/esp8266`, `/dev/esp32`, `/dev/esp32c3`).
- Add per-board metadata (chip type, serial, expected baud, reset method).

3. Security

- Keep direct public SSH disabled if possible.
- If remote shell access is needed, use WireGuard or Tailscale and key-only auth.
- Restrict runner user permissions to required flashing and serial access only.

4. Isolation and scheduling

- Add workflow-level concurrency so only one hardware run uses the lab at a time.
- Add a simple board lock mechanism for future parallel expansion.

## CI Workflow Design

Use a two-stage pipeline:

1. Build stage (cloud runner)

- Build firmware and web artifacts.
- Publish artifacts and checksums.
- Publish metadata (version, commit, build timestamp, artifact URLs).

2. Hardware stage (self-hosted runner)

- Download artifacts and verify checksums.
- Flash baseline image per board.
- Run smoke checks (`/ping`, `/info`, websocket handshake).
- Execute OTA update flow using produced artifacts.
- Re-run smoke checks and targeted regression tests.
- Persist serial logs, flash logs, and HTTP/websocket probe logs as artifacts.

## Test Matrix

Minimum matrix dimensions:

- board: `esp8266`, `esp32`, `esp32c3`
- branch profile: `develop`, `experimental` (as needed)

Initial gate policy:

- Required on `develop` and release branches
- Optional or nightly on `experimental` until stable

## Hardware Test Cases (Initial)

1. Flash and boot

- Device flashes successfully.
- Device boots and API responds within timeout.

2. Baseline API checks

- `/ping` returns expected payload.
- `/info` includes expected version/build fields.

3. Malformed JSON resilience (current regression focus)

- POST malformed JSON to `/color` and `/on`.
- Expect HTTP 400 with `Invalid JSON` body.
- Verify device remains reachable after each malformed request.

4. Websocket checks

- Handshake succeeds.
- Missing method request returns JSON-RPC error.
- Unknown method request returns JSON-RPC error.

5. OTA flow

- Trigger update from current artifacts.
- Verify update completes and reported version changes.
- Verify API/websocket behavior still passes after reboot.

## Observability and Artifacts

Per run, store:

- serial logs per board
- flasher command output
- OTA transaction logs
- HTTP probe traces
- websocket request and response traces
- runner environment metadata (board map, artifact checksums)

Add a compact machine-readable summary file (`json`) for trend analysis.

## Reliability Safeguards

- Add hard timeout for each test phase.
- Add automatic board reset between phases.
- Add one controlled retry for known flaky transport steps.
- Fail fast on artifact checksum mismatch.
- Mark known flaky tests separately from hard failures during rollout.

## Implementation Steps

Phase 1: Lab bring-up

- Provision VM and install self-hosted runner.
- Configure USB passthrough and stable device naming.
- Validate manual flash and serial capture for all three boards.

Phase 2: Workflow integration

- Add hardware workflow file in `.github/workflows`.
- Add artifact download and checksum validation.
- Add board matrix and concurrency controls.

Phase 3: Core tests

- Add baseline API and websocket smoke tests to hardware job.
- Add malformed JSON resilience tests.
- Add OTA update + post-update validation.

Phase 4: Hardening

- Add structured logs and summary report.
- Add retry and reset logic.
- Tune timeouts and quarantine flaky cases.

Phase 5: Enforcement

- Move from informational to required checks on target branches.
- Document operations and recovery runbook.

## Operational Runbook (Minimum)

Document:

- how to recover a stuck board
- how to rebind missing USB devices
- how to rotate runner tokens and SSH keys
- how to triage failed OTA runs
- who owns hardware maintenance

## Success Criteria

- Hardware workflow is stable for at least 20 consecutive runs.
- OTA and malformed JSON regressions are caught in CI before merge.
- Artifact mismatch and deployment issues are detected automatically.
- Mean time to diagnose hardware CI failures is reduced via logs and summaries.
