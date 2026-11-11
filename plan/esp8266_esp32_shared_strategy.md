# ESP8266 Freeze, ESP32 Growth, Shared Codebase Strategy

## Goal

Keep the firmware codebase shared across ESP8266 and ESP32 for as long as it is
useful, while freezing new feature development on ESP8266. From this point on:

- ESP8266 receives bug fixes and selected Home Assistant work only.
- ESP32 continues to gain new features and platform-specific improvements.
- Shared business logic should remain as common as possible.

The end state is one repository that still builds for all supported architectures
from the same source tree, with architecture selection happening in the build
system and small target-specific modules, not by splitting the product into
separate repos.

## Problem Statement

The current firmware serves multiple hardware targets with a lot of overlap in
behavior, protocol handling, and UI contract. The risk is that feature growth for
ESP32 slowly forks the codebase into two unrelated products.

The goal is not to duplicate the product. The goal is to preserve one coherent
codebase while making the target-specific differences explicit and maintainable.

## Design Principle

Split by capability, not by device brand.

Shared code should ask:

- what capabilities are available?
- what protocol or hardware abstraction is active?
- what feature set is enabled for this build/target?

Shared code should not spread `if ESP8266` / `if ESP32` checks through core
logic.

## Proposed Structure

### Common core

Keep shared logic in a common layer for:

- API behavior
- persistence rules
- webapp serving contract
- state synchronization
- Home Assistant integration where the behavior is shared

### Platform-specific layers

Add target-specific modules for:

- ESP8266 compatibility and freeze-era bug fixes
- ESP32-only features
- hardware-specific transports or drivers
- memory/connection tuning that differs by target

### Capability layer

Expose a small runtime or compile-time capability description so shared code can
branch on behavior without knowing the exact board family.

Examples:

- supportsWebsocketPush
- supportsExtendedTelemetry
- supportsNewHomeAssistantFeature
- supportsHigherConnectionCount

## Sming-style option

Sming’s arch subtree approach is a useful model. The idea is valid if it is
implemented as target-specific subtrees or modules inside one repository:

- `arch/esp8266/` for frozen compatibility behavior
- `arch/esp32/` for growth and new features
- `common/` for the shared layer

That gives the same conceptual split without requiring separate products.

For this project, literal Git subtrees are probably not the right first move.
They are useful for vendored external code, but they add friction when the real
goal is a single maintainable product with shared logic and target-specific
build selection.

## Repository And Build Model

Use one repo, one source tree, and architecture-selectable build outputs.

That means:

- one repository remains the authoritative home for shared code
- builds for ESP8266, ESP32, and any future targets continue to come from the
  same repo
- platform-specific code lives behind clear module boundaries
- common behavior is implemented once and reused everywhere possible
- duplication is acceptable only when the hardware or platform contract truly
  differs

The repository should not become a forked pair of codebases with copied files
that drift independently. The build system should choose the correct
implementation for each target while keeping the shared code path intact.

### Class and interface extension rule

For classes that already exist in the common tree, keep the shared interface and
shared implementation there. If ESP32 needs extra API surface, add only the
ESP32-specific overloads, helpers, or derived extensions in a second target-
specific file set.

That means:

- do not copy the whole class into an ESP32 tree just because a few methods
diverge
- keep one definition of the common methods
- let the target-specific file set extend the class contract only where the
platform really needs it
- prefer a shared base interface plus a target extension layer if the divergence
starts to grow large

This keeps the common tree authoritative while still allowing ESP32 to evolve
faster without forcing ESP8266 to carry unrelated feature growth.

## Working Policy

### ESP8266

- No new feature work unless it is needed for bugfixes or Home Assistant
  compatibility.
- Keep behavior stable.
- Avoid risky changes to memory, networking, or boot flow.

### ESP32

- New features go here first.
- If a new feature can be shared safely, lift the shared parts into common code.
- Keep ESP32-specific extensions isolated from the common contract.

## Incremental Plan

1. Inventory current code by target sensitivity.
2. Identify common behavior that can be moved behind one shared abstraction.
3. Introduce a capability layer.
4. Split target-specific implementations into explicit modules.
5. Freeze ESP8266 feature growth policy in documentation and CI checks.
6. Extend ESP32 freely while preserving the shared contract.

## Decision

Use the Sming arch subtree idea as a structural reference, but implement the
split as a shared core plus target-specific modules in this repository.

That keeps the codebase unified, still buildable for all supported architectures
from one repo, while allowing ESP8266 to stabilize and ESP32 to keep growing.
