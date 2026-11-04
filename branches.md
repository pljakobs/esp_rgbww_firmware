Cross-project sequencing and the concrete development-line migration checklist
are tracked in [MASTER_PLAN.md](../esp_rgb_webapp2/MASTER_PLAN.md).

## The Lightinator package consists of two repos that build on-device binaries:
- esp_rgbwwfirmware
    the firmware itself
- esp_rgb_weapp2
    the webapp

## Branch Naming Decision (2026-10-20)

Use version-first `<major>.<minor>/<stage>` branch names in both firmware and
webapp repositories. This allows both maintained product lines to have the same
promotion stages at the same time:

- `5.0/develop` -> `5.0/testing` -> `5.0/prod`
- `5.1/develop` -> `5.1/testing` -> `5.1/prod`

The version line is part of branch identity; `develop`, `testing` and `prod` are
stages, not globally unique branch names. Keep the lines independent. A 5.0 fix
stays on 5.0; a fix also needed on 5.1 is forward-ported deliberately. Promoting
5.1 does not require 5.0 to advance first.

Current branch mapping:

| Current branch | Versioned branch | Role |
| --- | --- | --- |
| `develop` | `5.0/develop` | 5.0 maintenance and patch development |
| `testing` | `5.0/testing` | 5.0 candidate qualification |
| `experimental` | `5.1/develop` | 5.1 integration and new feature work |
| `stable` workflow option | `5.0/prod` during migration | Legacy production name; confirm any external refs before retiring it |

Line constraints:

- 5.0 pins RGBWWLed `0.10.1` and builds ESP8266, ESP32 and ESP32-C3 for now.
- 5.1 pins RGBWWLed `0.11.0` and builds ESP32 and ESP32-C3 only; it does not target ESP8266.
- Each firmware branch pins its RGBWWLed submodule revision. Do not select a component version from an untracked header snapshot.
- When 5.1 is promoted, advance within `5.1/develop -> 5.1/testing -> 5.1/prod`. Keep 5.0 maintenance branches available for patches.

Rename existing branches only after CI, branch protections, webapp branches and
release consumers understand the versioned names. During migration, accept old
names as aliases where needed, but make versioned names canonical.

### CI Implementation Status

Versioned firmware/webapp CI preparation is on `ci/versioned-firmware-publishing`
and `ci/versioned-webapp-publishing`. The detailed operator procedure, current
workflow behavior, manual dispatch rules, publisher contract, reproducibility
status, and remaining gaps are documented in
[CI_RELEASE_PROCESS.md](CI_RELEASE_PROCESS.md).

The workflows accept canonical `5.0/*` and `5.1/*` refs while retaining legacy
aliases for migration. The canonical release branches have not yet been cut over;
do not retire `develop`, `testing`, `stable`, or `experimental` until branch
protections, dependency pins, webapp feeds, and consumers have been migrated.

## Proposed Architecture: Independent Releases, Shared API Contract

Status: proposal. The architecture below extends the branch policy above; it
does not describe already-implemented compatibility enforcement.

Lightinator has two independently releasable on-device components:

- `esp_rgbww_firmware`: native firmware, built per supported SoC and build type.
- `esp_rgb_webapp2`: webapp assets, usable on-device or from an external host.

`lightinator_server` distributes both and selects eligible updates. Firmware,
webapp, and device API each have their own version sequence. A fix in either
component does not require releasing the other, or changing the API version.
Newer webapps and older firmware, and vice versa, remain compatible when the
client supports the device's API contract and available capabilities.

### Independent Release Tracks

Use the version-first stage branches from the decision above independently in
each repository. Within a release line, promote `develop -> testing -> prod`.
A webapp candidate can advance without firmware moving, and firmware can advance
while keeping the current production webapp. Firmware and webapp channel
preferences should be separate; opting into 5.1 firmware must not implicitly
opt into 5.1 webapp updates. Allow a specific compatible release of either
component to be pinned.

Promotion within a line preserves its `x.y.z` version. A new release line starts
at its own `x.y` version; 5.1 promotion does not reset or merge into 5.0. Apply
fixes across lines as deliberate cherry-picks when the fix is relevant. Retain
5.0 maintenance branches while ESP8266 and its RGBWWLed `0.10.z` dependency are
supported.

Separate source branches from distribution channels. Build a candidate once,
record its immutable release ID, source commit, dependencies and checksums, then
promote those exact artifacts to testing and production. Do not rebuild merely
to change an embedded branch suffix. The source branch is provenance; channel
membership is mutable server metadata. Existing branch-based artifact paths can
remain immutable storage locations while discovery follows channel membership.

### Product-Aware OTA and Distribution

Status: requirement and design proposal; not implemented. The release service
must support the expanding Lightinator product line, not only controller
firmware. Expected products include the controller, wallpanel and color sensor.
Do not infer product identity from SoC, branch name or artifact filename.

Use stable product IDs such as `controller`, `wallpanel` and `color_sensor`.
Represent a release as immutable metadata that identifies the product, product
version, distribution channel, source repository and commit, and one or more
installable targets. A target should identify applicable hardware/model and
revision, SoC, build type, and its artifact roles. Products may have multiple
artifacts or target variants; do not assume every release is a single `app.bin`.

Illustrative manifest shape (field names are proposals):

```json
{
    "product": "controller",
    "version": "5.1.0-12",
    "channel": "5.1/develop",
    "source": {
        "repository": "esp_rgbww_firmware",
        "commit": "<source-commit>"
    },
    "targets": [
        {
            "hardware": "<board-model-or-revision>",
            "soc": "esp32",
            "buildType": "release",
            "artifacts": [
                {
                    "role": "firmware",
                    "filename": "app.bin",
                    "sha256": "<digest>",
                    "size": 12345
                }
            ]
        }
    ]
}
```

SQLite is the authoritative release catalogue. Its identity must include product
and target dimensions, for example `(product, version, hardware, soc, build_type,
artifact_role)`, with channel membership and source provenance recorded
separately. An upload must not overwrite another product's artifact merely
because the version, SoC and build type match. Keep existing controller routes
as compatibility adapters that map to `product=controller` during migration.

The upload API should accept a signed release manifest and its artifacts into a
staging release. The server validates the authenticated CI publisher, allowed
repository/workflow/ref, product policy, target matrix, names, sizes and
checksums; it then finalizes the complete release atomically. In particular,
5.1 controller policy rejects ESP8266, while 5.0 permits ESP8266, ESP32 and
ESP32-C3. A partially uploaded matrix must not appear as a complete release.
Use short-lived repository/workflow identity (for example GitHub OIDC) where
practical, plus signed artifact/manifest verification. Scope publisher
permissions by product and channel; never accept arbitrary branch/path values
as filesystem paths.

Discovery should select by product, channel, target hardware/SoC and relevant
compatibility requirements, returning an explicit no-match result rather than
another product's or line's latest build. Device clients remain responsible for
installing and verifying the artifact format they understand; the server is the
catalogue, authorization and distribution layer, not a universal device updater.

Repository responsibilities:

| Component | Product-aware requirement |
| --- | --- |
| `esp_rgbww_firmware` and its CI | Emit a release manifest per product/target; select SoCs by product and 5.0/5.1 line; pin the matching RGBWWLed revision; upload and finalize a complete signed release. |
| `esp_rgb_webapp2` | Declare supported API ranges/capabilities and product-specific UI capabilities; consume product-aware discovery without assuming every target is an ESP8266 controller. |
| `lightinator_server` | Make SQLite authoritative for products, releases, targets and artifacts; provide authenticated staging/finalize and product-aware discovery APIs. |
| `lightinator-log-service` | Resolve ELF/map files using product, release and target metadata so symbols cannot be selected from another product or line. |
| `Lightinator_HA_module` | Identify the supported product/target and request only compatible controller releases; retain a legacy controller path during migration. |

`version.json` is a compatibility projection for older clients, not a second
authority. Generate it from finalized SQLite records after release commit; a
projection failure must be retryable without invalidating the canonical catalog.
Keep the old projection and routes until their consumers have migrated, and test
that they represent the same release metadata.

Incremental migration:

1. Define stable product/target identifiers and the signed release manifest.
2. Extend the SQLite schema additively, mapping existing rows to
     `product=controller`; retain old API routes as adapters.
3. Add staged upload, validation, atomic finalization and compatibility
     projection. Shadow-compare API catalog resulChatGPT:

My overall take: this is a strong architecture, and the most important decisions are the right ones. You're moving from a branch-driven firmware release process toward a product-aware release system with immutable artifacts, independent component versions, and explicit compatibility contracts. That's the right direction for Lightinator as it grows beyond a single ESP8266/ESP32 controller.

My main concern isn't the architecture itself. It's the amount of release infrastructure being introduced at once. There are several distinct problems here, and I would solve them in a deliberate order rather than implementing the entire proposal as one large migration.

I'd keep the core design, tighten a few important details, and simplify the initial implementation.
1. The decisions I strongly agree with

Version-first branches: 5.0/develop, 5.1/develop

This is a good fit for parallel maintenance. A security or stability fix for 5.0 shouldn't depend on the progress of 5.1, and promoting 5.1 shouldn't force a 5.0 promotion. Explicit forward-ports make the relationship between lines visible.

Build once, promote the same artifact

This is one of the most important improvements in the proposal. A candidate that passed testing should be exactly the binary that reaches production, not a rebuild from a nominally equivalent branch. Immutable artifact identity and checksums make that verifiable.

Independent firmware, webapp and API versions

These have different reasons to change. A UI fix shouldn't require firmware release ceremony, and an internal firmware fix shouldn't create a fictional API change. Explicit compatibility metadata is much more robust than assuming versions move together.

A product-aware release catalogue

Moving away from filenames and SoC names as implicit product identity is essential. A controller, wallpanel and color sensor can have overlapping version numbers and target architectures without being the same product. Modeling products and installable targets explicitly avoids a lot of future ambiguity.

Compatibility checks in both update directions

Correctly recognizing that webapp updates can break firmware compatibility, and firmware updates can break the installed webapp, is crucial. Server-side filtering alone isn't enough when devices may be offline, clients may be old, or discovery metadata may be stale.

These are foundational decisions worth preserving.
2. The most important improvements I'd make
A. Separate the release line, release channel and software version

Your branch convention is fine, but there are three different concepts that should never become conflated:

    Release line: 5.0 or 5.1

    Channel: develop, testing or prod

    Artifact version: an immutable software release identifier, such as 5.1.0-12

Your example manifest currently has:
json

{
  "version": "5.1.0-12",
  "channel": "5.1/develop"
}

That works as a starting point, but I would store the line and channel separately in the canonical catalogue:
json

{
  "product": "controller",
  "releaseLine": "5.1",
  "version": "5.1.0-12",
  "channel": "develop"
}

The branch 5.1/develop then maps to a release line and a channel. It isn't itself the channel.

This separation matters because promotion must not change artifact identity. The same release ID can move from testing to production without changing its version, checksum or embedded metadata.

I'd also be explicit about the distinction between an artifact's immutable version and the mutable set of releases eligible for a channel.
B. Don't let branch names become permanent distribution identities

The proposal correctly says source branches are provenance and channel membership is mutable server metadata. I'd make that a hard architectural rule.

For example:

    A CI build originates from 5.1/testing.

    The resulting artifact has a stable release ID and source commit.

    The release is qualified and promoted to production.

    The server updates channel membership.

    The binary doesn't change, and no rebuild is needed.

The source branch is useful for auditability, but clients should request something like:

product=controller, channel=prod, target=esp32

rather than infer production status from an artifact residing under a /5.1/testing/ path.

You can retain existing branch-based storage paths during migration. Just don't let those paths define the long-term semantics of release discovery.
C. Define atomicity across the entire release, not just individual uploads

Your staged upload and finalization design is good. I'd make the invariant explicit:

A release is either complete and discoverable, or incomplete and undiscoverable.

That includes more than uploading all files successfully.

For a firmware release, finalization should verify:

    Every required SoC/build-type combination is present.

    Every artifact matches its declared checksum and size.

    The manifest matches the allowed product and release-line policy.

    All required auxiliary artifacts, such as symbol files or partition metadata, are present where applicable.

    The release metadata is immutable after finalization.

There is one subtle distinction worth preserving: a release doesn't necessarily need every theoretically supported target to be available. If a product's release policy permits a partial target set, that set must be explicitly declared and complete. The server must never silently interpret an interrupted upload as an intentional partial release.

I'd use an explicit release lifecycle:

staging → finalized → promoted

A failed staging operation remains retryable. A finalized release is immutable. Promotion changes channel membership, not release contents.
D. Tighten the definition of a release ID

You mention an immutable release ID, but don't define what makes it unique.

I'd generate a server-assigned release ID and separately retain:

    Product and release version.

    Source repository and commit.

    Release-manifest digest.

    Target-specific artifact digests.

    Build configuration and toolchain provenance.

    Contract identity.

    Creation and finalization metadata.

A Git commit is not a sufficient release identity: the same commit can produce different binaries under different toolchains, build configurations or dependency revisions.

Likewise, the version string isn't sufficient because different targets and build types can share a version.

For reproducibility and debugging, I'd also record the pinned RGBWWLed submodule commit, relevant build flags, and toolchain/container version. Reproducible builds are a valuable later goal, but verifiable provenance is a good initial requirement.
3. The API contract proposal is good, but needs one important distinction

I agree with the split between contractHash and apiVersion.

A normalized hash answers:

    Is this exactly the same public contract?

A SemVer API version answers:

    What compatibility guarantees have we reviewed and committed to?

Those are different questions.

However, I would avoid treating API SemVer as the only evidence that two particular implementations are compatible.
Compatibility is directional

Suppose firmware 5.0 provides API 1.1 and firmware 5.1 provides API 1.2.

A webapp may support both, but compatibility can still depend on what it sends and what it receives. The important dimensions are:

    Requests the client sends must be accepted by the firmware.

    Responses and events emitted by firmware must be understood by the client.

    Required capabilities must exist on the specific target.

    Semantics must remain compatible, not just field shapes.

Your proposal recognizes this already. I'd turn it into executable compatibility tests rather than leaving it as documentation.

For example, model the relationship as:
compatible⁡(W,F)=apiRangeMatches⁡(W,F)∧capabilitiesMatch⁡(W,F)∧behaviorTestsPass⁡(W,F)
compatible(W,F)=apiRangeMatches(W,F)∧capabilitiesMatch(W,F)∧behaviorTestsPass(W,F)

Here, W
W is a webapp release and F
F is a firmware target.

The range and capability checks are efficient runtime filters. The behavior tests are what give you confidence that the declared relationship is true.
One caveat about hashing ConfigDB schemas

I like using the four runtime .cfgdb files as the authoritative starting point, with operation metadata layered on top.

But don't make the normalized hash depend on an assumption that you can safely ignore all non-wire metadata. ConfigDB annotations may influence serialization, defaults, validation or other runtime behavior.

I would version the normalization algorithm itself, for example with a contractFormatVersion, and maintain golden fixtures that demonstrate how representative schema changes affect the projection.

Also, a schema-derived hash cannot detect every behavioral change. A firmware implementation could change the meaning of a method without changing its schema. That's why the API changelog, explicit version review and conformance tests are necessary.

Finally, don't automatically bump API SemVer just because the hash changes. Conversely, don't assume an unchanged hash proves compatibility.
4. The update and rollback design deserves the strongest safety guarantees

This is the part I'd be most careful about before enabling automatic updates.

Your proposal correctly checks compatibility in both directions. I'd add an explicit distinction between discovering an update and committing to install it.
Recommended update sequence

    Discover: Select a release for the correct product, hardware, SoC and channel.

    Validate: Verify its signature, digest, compatibility metadata and install constraints.

    Evaluate the resulting pair: Check the target firmware against the installed webapp, or the target webapp against the installed firmware.

    Plan: Determine whether the update can happen independently, requires a bridge release, or needs a coordinated upgrade.

    Install and verify: Use the device's supported OTA mechanism, then confirm that the new component starts and communicates correctly.

    Recover if needed: Use the device's actual rollback or recovery mechanism, accounting for persistent-data migrations.

A few details matter here:

    A signed manifest authenticates metadata only if the signature is verified against a trusted key and the signed content binds the artifact digests.

    TLS and authenticated CI publishing are valuable, but they don't replace device-side verification of the downloaded artifact.

    A checksum detects corruption; by itself, it doesn't establish authenticity.

    Rollback isn't automatically safe just because an older release remains in the catalogue. Data migrations, bootloader behavior and OTA partition layouts can make it impossible.

I'd also avoid promising automatic rollback unless each target has a tested rollback mechanism.

Your recommendation to retain a known-good working pair is excellent. Make that an explicit state that survives failed discovery, failed installation and a reboot.
5. SQLite is a reasonable starting point, with one caveat

I wouldn't introduce a more complex database just for this design. SQLite can serve a release catalogue well, especially for a single server instance or a modest distribution service.

However, I'd make the relational model a little more explicit than the illustrative composite key suggests.

I would separate the following entities:

Entity


Purpose

products


Stable identity and product policy

releases


Immutable release identity, version, provenance and manifest

release_targets


Hardware, revision, SoC and build-type applicability

artifacts


Immutable blobs, roles, sizes, digests and storage references

channel_memberships


Which releases are eligible for which channel

contract_versions


API SemVer, contract digest and immutable bundle reference

I would use surrogate IDs for relationships and unique constraints for natural identities, rather than making one large composite key the primary identifier for everything.

For example, artifact roles belong naturally to artifacts associated with a release target. A release may contain several files for one target, and a target may be shared across release policies.

Two implementation details are particularly important:

    Use database transactions for finalization and channel changes, with suitable locking or concurrency controls.

    Keep large artifact binaries in immutable blob storage or a filesystem/object store, with SQLite holding metadata and references.

If the current server already stores artifacts on disk, there's no need to move the bytes into the database. The important thing is that a database record never points to an artifact that is only partially written or has been overwritten by another release.
6. Your biggest practical risk is migration scope

The proposal covers several large changes at once:

    Versioned branch naming across two repositories.

    CI matrix changes and release tagging.

    A new product-aware release catalogue.

    Signed, authenticated uploads.

    API contract generation and SemVer governance.

    Bidirectional OTA compatibility.

    Home Assistant and log-service migration.

    New wallpanel and color-sensor product lines.

Each is defensible. Doing them all simultaneously would make failures difficult to diagnose.

I'd order the work around reducing risk as early as possible.
Phase 1
Release basics

Make the existing pipeline correct

    Implement versioned branch filters and independent SoC matrices.

    Fix the ESP8266-only one-time CI guards.

    Separate 5.0 and 5.1 version/tag counters.

    Pin dependencies and preserve immutable build outputs.

    Test the legacy branch aliases and deployed paths.

Phase 2
Compatibility

Establish a trustworthy API baseline

    Reconcile the four runtime schemas and operation metadata.

    Generate the contract digest.

    Establish reviewed API SemVer and a changelog.

    Add compatibility manifests and automated tests for existing firmware/webapp pairs.

    Pin unknown legacy builds until their compatibility has been verified.

Phase 3
Catalogue

Add product-aware releases alongside the current service

    Extend SQLite additively.

    Introduce immutable release manifests and staged uploads.

    Shadow-compare discovery results.

    Add atomic finalization, signatures and explicit no-match responses.

    Generate the legacy version.json projection from finalized records.

Phase 4
OTA safety

Enforce compatibility in real clients

    Add device-side artifact verification.

    Validate both directions of firmware/webapp updates.

    Test bridge upgrades, failed installs and rollback.

    Migrate Home Assistant and log-service consumers.

    Enable automatic cross-line updates only after hardware qualification.

Phase 5
Expansion

Add more products and retire legacy infrastructure

    Introduce wallpanel and color sensor as independent products.

    Define and test their own target policies and installers.

    Retire SSH publishing and legacy routes only when actual consumers have migrated.

This ordering intentionally gets the current controller pipeline reliable before adding complexity for future products.
7. A few specific issues I'd resolve in the written design

These are small compared with the overall architecture, but worth making explicit.

Branch names versus SemVer. Decide how you derive the release line from 5.0 or 5.1 when the software's actual version is 5.0.z or 5.1.z. Don't allow a manually supplied version to contradict the selected branch.

Prerelease counters. Define whether 5.1.0-12 is an ordinary SemVer prerelease identifier and whether its counter is monotonic per line, per version, or per commit. Ensure the same version cannot be assigned to different artifacts accidentally.

Firmware and webapp channels. Separate preferences are correct. I'd also specify the default policy: a user selecting a development firmware channel should continue receiving the production webapp unless they explicitly choose otherwise, provided that pair is compatible.

Target identity. SoC isn't enough. Board revision, flash capacity, partition layout and bootloader/OTA format may determine whether an artifact is installable. Only include those dimensions where they affect eligibility, but define the target model so you can add them without redesigning the catalogue.

Capabilities. Give capabilities stable identifiers and semantics. A capability should mean the feature actually exists and behaves as promised on that build, not merely that its schema defines relevant messages.

Server-side compatibility. The server can select candidates, but clients must validate the returned metadata. Treat compatibility declarations as necessary evidence, not a substitute for target-side checks.

Production promotion. Define who or what is authorized to promote a release, and whether promotion is immediate or staged. A prod channel is a policy boundary, not just another branch name.

Webapp packaging. A webapp hosted remotely can change independently of a device's local update lifecycle. Pinning and rollback may require different mechanisms for on-device assets and externally hosted assets. I'd explicitly distinguish these deployment modes in the release metadata.
8. What I would avoid building prematurely

There are three areas where I'd resist unnecessary generalization.

- A separate API-contract repository. Your proposed immutable contract bundle can initially be generated and published by the existing schema pipeline. A separate repository is justified if ownership or release cadence demands it, not merely because the contract is independently versioned.

- Per-surface API versioning. Start with the aggregate API version, as you propose. Separate HTTP, JSON-RPC and MQTT version streams could make compatibility harder to reason about before you have a concrete need.

- A universal device updater. Your statement that the server is the catalogue and distribution layer, not a universal updater, is exactly right. The controller, wallpanel and sensor may have different bootloaders, installation procedures and recovery guarantees. Share metadata and policy where possible, not assumptions about how every device installs a binary.

The same principle applies to the manifest: make the data model extensible, but keep initial product policies explicit rather than building a highly generic rules engine.
Final verdict

I'd approve this architecture as the direction for Lightinator, subject to a few refinements.

The strongest parts are the separation of release identity from channel promotion, independent firmware/webapp/API versioning, product-aware artifact selection, and explicit compatibility enforcement. Those choices address real failure modes rather than merely making the pipeline more sophisticated.

My three highest-priority requirements would be:

    Immutable and correctly targeted releases. No artifact overwrite, cross-product selection, incomplete publication or ambiguous release identity.

    Provable compatibility and safe updates. Contract metadata, capability checks, actual conformance tests and tested recovery paths.

    Incremental migration with legacy compatibility. Keep existing devices and consumers working while the new catalogue and APIs are validated.

One final recommendation: before implementation, write a short set of release invariants that every CI workflow, server endpoint and device client must preserve. For example:

    A release ID always identifies the same manifest and artifact digests.

    A finalized release cannot be mutated.

    A channel never selects a release for the wrong product or target.

    An update is never offered as compatible based on version ordering alone.

    A failed discovery or installation does not discard the last known-good working configuration.

    A release can be promoted without rebuilding its artifacts.

If those invariants become automated tests, the architecture will be much easier to maintain than if correctness lives mainly in the design document.

The goal shouldn't be to build the most sophisticated release system. It should be to make it impossible for a normal release operation to publish the wrong binary or put a device into an incompatible state. Your proposal is already heading in that direction.ts with current SSH/`version.json`
     publishing before switching production writes.
4. Switch controller CI first, preserving immutable artifacts and verifying
     rollback/no-match behavior; then migrate webapp, log service and Home
     Assistant consumers.
5. Add wallpanel and color-sensor products only with their own target policies,
     signing authorization and update/install tests. Retire SSH publishing and
     legacy projections only after supported callers have migrated.

### Authoritative Contract Sources

The API-defining runtime ConfigDB schema consists of:

| File | Contract role |
| --- | --- |
| `jsonrpc.cfgdb` | Message body alternatives and references used by the RPC codec |
| `params.cfgdb` | Shared request, response, event and information payloads |
| `homeassistant.cfgdb` | Home Assistant command, state and discovery payloads |
| `value-types.cfgdb` | Shared scalar types, units and value constraints |

Treat these four files and their transitive public references as the primary
schema source, rather than maintaining a second hand-written wire schema.
`jsonrpc.cfgdb` selects body shapes, not a complete method registry: its comment
explicitly states that `Message::method` is free-form. Therefore also maintain
operation metadata binding HTTP routes, RPC methods, notifications and MQTT
topics to these bodies, including authentication and error behavior.

Configuration and data exposed through public endpoints are part of the contract
too: include the reachable public parts of `app-config.cfgdb`, `app-data.cfgdb`,
and their dependencies where necessary. Internal persistence layout is a separate
migration concern, not automatically a client API change.

There is already a `tools/generate-api-schemas.mjs` generator and an
`api-version.json` artifact, but they are not yet this pipeline. The inspected
generator uses `schema/json-rpc-api.cfgdb`, `defs.cfgdb`, configuration/data schemas,
and descriptive artifacts; it does not hash the four runtime schemas above.
Its API version is copied from `api.json` and currently marked
`1.0.0-inferred.1`. Reconcile or replace the older projection and verify operation
descriptions against runtime behavior before declaring a stable API baseline.

### Generated Identity Versus Compatibility Version

Use two identifiers with different purposes:

- `contractHash`: generated SHA-256 identifying an exact public contract.
- `apiVersion`: reviewed SemVer describing compatibility, independent of both
  firmware and webapp versions.

A hash cannot be ordered meaningfully for min/max checks. Generate the identity
automatically, but do not infer compatibility solely from a changed hash.

The proposed build pipeline is:

1. Parse the four runtime schemas, resolve their references and combine them with
    operation metadata and any other reachable public schemas.
2. Produce a normalized public-contract projection. Sort object keys, ignore
    whitespace/comments and code-generation-only metadata, but preserve constraints,
    meaningful defaults, union alternatives, and wire-visible names. Do not strip
    ConfigDB annotations unless their lack of wire impact has been verified.
3. Hash the projection, with a documented projection format/version. Retain source
    hashes separately for provenance. Add `contractHash` rather than silently
    changing the meaning of the existing `schemaHash` field.
4. Compare against a pinned released contract in CI. Require an explicit reviewed
    version decision for contract changes, supported by schema-diff checks and
    request/response/event conformance tests against actual firmware behavior.
5. Publish an immutable contract bundle, independently tagged, for example
    `api-v1.2.0`. Webapp and Home Assistant CI consume a pinned bundle, not a moving
    firmware branch. A separate contract repository is optional, not required.

Version policy:

| Change | API version action |
| --- | --- |
| Internal fix, no observable contract change | Keep API version |
| Compatible clarification or semantic correction | Patch |
| New optional operation or safely ignorable field/capability | Minor |
| Removed field, changed units/type/meaning, new required input | Major |

Compatibility is directional: firmware consumes requests and emits responses and
events; clients do the reverse. An enum addition or narrower constraint is not
automatically compatible. Clients should tolerate unknown optional response
fields, while defaults must preserve existing behavior. Semantic changes can
require a version bump even when the schema shape is unchanged; record them in
contract metadata and an API changelog.

Keep the API version distinct from JSON-RPC's `"2.0"` and existing `/info`
representation selectors. Start with one aggregate ChatGPT:

My overall take: this is a strong architecture, and the most important decisions are the right ones. You're moving from a branch-driven firmware release process toward a product-aware release system with immutable artifacts, independent component versions, and explicit compatibility contracts. That's the right direction for Lightinator as it grows beyond a single ESP8266/ESP32 controller.

My main concern isn't the architecture itself. It's the amount of release infrastructure being introduced at once. There are several distinct problems here, and I would solve them in a deliberate order rather than implementing the entire proposal as one large migration.

I'd keep the core design, tighten a few important details, and simplify the initial implementation.
1. The decisions I strongly agree with

Version-first branches: 5.0/develop, 5.1/develop

This is a good fit for parallel maintenance. A security or stability fix for 5.0 shouldn't depend on the progress of 5.1, and promoting 5.1 shouldn't force a 5.0 promotion. Explicit forward-ports make the relationship between lines visible.

Build once, promote the same artifact

This is one of the most important improvements in the proposal. A candidate that passed testing should be exactly the binary that reaches production, not a rebuild from a nominally equivalent branch. Immutable artifact identity and checksums make that verifiable.

Independent firmware, webapp and API versions

These have different reasons to change. A UI fix shouldn't require firmware release ceremony, and an internal firmware fix shouldn't create a fictional API change. Explicit compatibility metadata is much more robust than assuming versions move together.

A product-aware release catalogue

Moving away from filenames and SoC names as implicit product identity is essential. A controller, wallpanel and color sensor can have overlapping version numbers and target architectures without being the same product. Modeling products and installable targets explicitly avoids a lot of future ambiguity.

Compatibility checks in both update directions

Correctly recognizing that webapp updates can break firmware compatibility, and firmware updates can break the installed webapp, is crucial. Server-side filtering alone isn't enough when devices may be offline, clients may be old, or discovery metadata may be stale.

These are foundational decisions worth preserving.
2. The most important improvements I'd make
A. Separate the release line, release channel and software version

Your branch convention is fine, but there are three different concepts that should never become conflated:

    Release line: 5.0 or 5.1

    Channel: develop, testing or prod

    Artifact version: an immutable software release identifier, such as 5.1.0-12

Your example manifest currently has:
json

{
  "version": "5.1.0-12",
  "channel": "5.1/develop"
}

That works as a starting point, but I would store the line and channel separately in the canonical catalogue:
json

{
  "product": "controller",
  "releaseLine": "5.1",
  "version": "5.1.0-12",
  "channel": "develop"
}

The branch 5.1/develop then maps to a release line and a channel. It isn't itself the channel.

This separation matters because promotion must not change artifact identity. The same release ID can move from testing to production without changing its version, checksum or embedded metadata.

I'd also be explicit about the distinction between an artifact's immutable version and the mutable set of releases eligible for a channel.
B. Don't let branch names become permanent distribution identities

The proposal correctly says source branches are provenance and channel membership is mutable server metadata. I'd make that a hard architectural rule.

For example:

    A CI build originates from 5.1/testing.

    The resulting artifact has a stable release ID and source commit.

    The release is qualified and promoted to production.

    The server updates channel membership.

    The binary doesn't change, and no rebuild is needed.

The source branch is useful for auditability, but clients should request something like:

product=controller, channel=prod, target=esp32

rather than infer production status from an artifact residing under a /5.1/testing/ path.

You can retain existing branch-based storage paths during migration. Just don't let those paths define the long-term semantics of release discovery.
C. Define atomicity across the entire release, not just individual uploads

Your staged upload and finalization design is good. I'd make the invariant explicit:

A release is either complete and discoverable, or incomplete and undiscoverable.

That includes more than uploading all files successfully.

For a firmware release, finalization should verify:

    Every required SoC/build-type combination is present.

    Every artifact matches its declared checksum and size.

    The manifest matches the allowed product and release-line policy.

    All required auxiliary artifacts, such as symbol files or partition metadata, are present where applicable.

    The release metadata is immutable after finalization.

There is one subtle distinction worth preserving: a release doesn't necessarily need every theoretically supported target to be available. If a product's release policy permits a partial target set, that set must be explicitly declared and complete. The server must never silently interpret an interrupted upload as an intentional partial release.

I'd use an explicit release lifecycle:

staging → finalized → promoted

A failed staging operation remains retryable. A finalized release is immutable. Promotion changes channel membership, not release contents.
D. Tighten the definition of a release ID

You mention an immutable release ID, but don't define what makes it unique.

I'd generate a server-assigned release ID and separately retain:

    Product and release version.

    Source repository and commit.

    Release-manifest digest.

    Target-specific artifact digests.

    Build configuration and toolchain provenance.

    Contract identity.

    Creation and finalization metadata.

A Git commit is not a sufficient release identity: the same commit can produce different binaries under different toolchains, build configurations or dependency revisions.

Likewise, the version string isn't sufficient because different targets and build types can share a version.

For reproducibility and debugging, I'd also record the pinned RGBWWLed submodule commit, relevant build flags, and toolchain/container version. Reproducible builds are a valuable later goal, but verifiable provenance is a good initial requirement.
3. The API contract proposal is good, but needs one important distinction

I agree with the split between contractHash and apiVersion.

A normalized hash answers:

    Is this exactly the same public contract?

A SemVer API version answers:

    What compatibility guarantees have we reviewed and committed to?

Those are different questions.

However, I would avoid treating API SemVer as the only evidence that two particular implementations are compatible.
Compatibility is directional

Suppose firmware 5.0 provides API 1.1 and firmware 5.1 provides API 1.2.

A webapp may support both, but compatibility can still depend on what it sends and what it receives. The important dimensions are:

    Requests the client sends must be accepted by the firmware.

    Responses and events emitted by firmware must be understood by the client.

    Required capabilities must exist on the specific target.

    Semantics must remain compatible, not just field shapes.

Your proposal recognizes this already. I'd turn it into executable compatibility tests rather than leaving it as documentation.

For example, model the relationship as:
compatible⁡(W,F)=apiRangeMatches⁡(W,F)∧capabilitiesMatch⁡(W,F)∧behaviorTestsPass⁡(W,F)
compatible(W,F)=apiRangeMatches(W,F)∧capabilitiesMatch(W,F)∧behaviorTestsPass(W,F)

Here, W
W is a webapp release and F
F is a firmware target.

The range and capability checks are efficient runtime filters. The behavior tests are what give you confidence that the declared relationship is true.
One caveat about hashing ConfigDB schemas

I like using the four runtime .cfgdb files as the authoritative starting point, with operation metadata layered on top.

But don't make the normalized hash depend on an assumption that you can safely ignore all non-wire metadata. ConfigDB annotations may influence serialization, defaults, validation or other runtime behavior.

I would version the normalization algorithm itself, for example with a contractFormatVersion, and maintain golden fixtures that demonstrate how representative schema changes affect the projection.

Also, a schema-derived hash cannot detect every behavioral change. A firmware implementation could change the meaning of a method without changing its schema. That's why the API changelog, explicit version review and conformance tests are necessary.

Finally, don't automatically bump API SemVer just because the hash changes. Conversely, don't assume an unchanged hash proves compatibility.
4. The update and rollback design deserves the strongest safety guarantees

This is the part I'd be most careful about before enabling automatic updates.

Your proposal correctly checks compatibility in both directions. I'd add an explicit distinction between discovering an update and committing to install it.
Recommended update sequence

    Discover: Select a release for the correct product, hardware, SoC and channel.

    Validate: Verify its signature, digest, compatibility metadata and install constraints.

    Evaluate the resulting pair: Check the target firmware against the installed webapp, or the target webapp against the installed firmware.

    Plan: Determine whether the update can happen independently, requires a bridge release, or needs a coordinated upgrade.

    Install and verify: Use the device's supported OTA mechanism, then confirm that the new component starts and communicates correctly.

    Recover if needed: Use the device's actual rollback or recovery mechanism, accounting for persistent-data migrations.

A few details matter here:

    A signed manifest authenticates metadata only if the signature is verified against a trusted key and the signed content binds the artifact digests.

    TLS and authenticated CI publishing are valuable, but they don't replace device-side verification of the downloaded artifact.

    A checksum detects corruption; by itself, it doesn't establish authenticity.

    Rollback isn't automatically safe just because an older release remains in the catalogue. Data migrations, bootloader behavior and OTA partition layouts can make it impossible.

I'd also avoid promising automatic rollback unless each target has a tested rollback mechanism.

Your recommendation to retain a known-good working pair is excellent. Make that an explicit state that survives failed discovery, failed installation and a reboot.
5. SQLite is a reasonable starting point, with one caveat

I wouldn't introduce a more complex database just for this design. SQLite can serve a release catalogue well, especially for a single server instance or a modest distribution service.

However, I'd make the relational model a little more explicit than the illustrative composite key suggests.

I would separate the following entities:

Entity


Purpose

products


Stable identity and product policy

releases


Immutable release identity, version, provenance and manifest

release_targets


Hardware, revision, SoC and build-type applicability

artifacts


Immutable blobs, roles, sizes, digests and storage references

channel_memberships


Which releases are eligible for which channel

contract_versions


API SemVer, contract digest and immutable bundle reference

I would use surrogate IDs for relationships and unique constraints for natural identities, rather than making one large composite key the primary identifier for everything.

For example, artifact roles belong naturally to artifacts associated with a release target. A release may contain several files for one target, and a target may be shared across release policies.

Two implementation details are particularly important:

    Use database transactions for finalization and channel changes, with suitable locking or concurrency controls.

    Keep large artifact binaries in immutable blob storage or a filesystem/object store, with SQLite holding metadata and references.

If the current server already stores artifacts on disk, there's no need to move the bytes into the database. The important thing is that a database record never points to an artifact that is only partially written or has been overwritten by another release.
6. Your biggest practical risk is migration scope

The proposal covers several large changes at once:

    Versioned branch naming across two repositories.

    CI matrix changes and release tagging.

    A new product-aware release catalogue.

    Signed, authenticated uploads.

    API contract generation and SemVer governance.

    Bidirectional OTA compatibility.

    Home Assistant and log-service migration.

    New wallpanel and color-sensor product lines.

Each is defensible. Doing them all simultaneously would make failures difficult to diagnose.

I'd order the work around reducing risk as early as possible.
Phase 1
Release basics

Make the existing pipeline correct

    Implement versioned branch filters and independent SoC matrices.

    Fix the ESP8266-only one-time CI guards.

    Separate 5.0 and 5.1 version/tag counters.

    Pin dependencies and preserve immutable build outputs.

    Test the legacy branch aliases and deployed paths.

Phase 2
Compatibility

Establish a trustworthy API baseline

    Reconcile the four runtime schemas and operation metadata.

    Generate the contract digest.

    Establish reviewed API SemVer and a changelog.

    Add compatibility manifests and automated tests for existing firmware/webapp pairs.

    Pin unknown legacy builds until their compatibility has been verified.

Phase 3
Catalogue

Add product-aware releases alongside the current service

    Extend SQLite additively.

    Introduce immutable release manifests and staged uploads.

    Shadow-compare discovery results.

    Add atomic finalization, signatures and explicit no-match responses.

    Generate the legacy version.json projection from finalized records.

Phase 4
OTA safety

Enforce compatibility in real clients

    Add device-side artifact verification.

    Validate both directions of firmware/webapp updates.

    Test bridge upgrades, failed installs and rollback.

    Migrate Home Assistant and log-service consumers.

    Enable automatic cross-line updates only after hardware qualification.

Phase 5
Expansion

Add more products and retire legacy infrastructure

    Introduce wallpanel and color sensor as independent products.

    Define and test their own target policies and installers.

    Retire SSH publishing and legacy routes only when actual consumers have migrated.

This ordering intentionally gets the current controller pipeline reliable before adding complexity for future products.
7. A few specific issues I'd resolve in the written design

These are small compared with the overall architecture, but worth making explicit.

Branch names versus SemVer. Decide how you derive the release line from 5.0 or 5.1 when the software's actual version is 5.0.z or 5.1.z. Don't allow a manually supplied version to contradict the selected branch.

Prerelease counters. Define whether 5.1.0-12 is an ordinary SemVer prerelease identifier and whether its counter is monotonic per line, per version, or per commit. Ensure the same version cannot be assigned to different artifacts accidentally.

Firmware and webapp channels. Separate preferences are correct. I'd also specify the default policy: a user selecting a development firmware channel should continue receiving the production webapp unless they explicitly choose otherwise, provided that pair is compatible.

Target identity. SoC isn't enough. Board revision, flash capacity, partition layout and bootloader/OTA format may determine whether an artifact is installable. Only include those dimensions where they affect eligibility, but define the target model so you can add them without redesigning the catalogue.

Capabilities. Give capabilities stable identifiers and semantics. A capability should mean the feature actually exists and behaves as promised on that build, not merely that its schema defines relevant messages.

Server-side compatibility. The server can select candidates, but clients must validate the returned metadata. Treat compatibility declarations as necessary evidence, not a substitute for target-side checks.

Production promotion. Define who or what is authorized to promote a release, and whether promotion is immediate or staged. A prod channel is a policy boundary, not just another branch name.

Webapp packaging. A webapp hosted remotely can change independently of a device's local update lifecycle. Pinning and rollback may require different mechanisms for on-device assets and externally hosted assets. I'd explicitly distinguish these deployment modes in the release metadata.
8. What I would avoid building prematurely

There are three areas where I'd resist unnecessary generalization.

- A separate API-contract repository. Your proposed immutable contract bundle can initially be generated and published by the existing schema pipeline. A separate repository is justified if ownership or release cadence demands it, not merely because the contract is independently versioned.

- Per-surface API versioning. Start with the aggregate API version, as you propose. Separate HTTP, JSON-RPC and MQTT version streams could make compatibility harder to reason about before you have a concrete need.

- A universal device updater. Your statement that the server is the catalogue and distribution layer, not a universal updater, is exactly right. The controller, wallpanel and sensor may have different bootloaders, installation procedures and recovery guarantees. Share metadata and policy where possible, not assumptions about how every device installs a binary.

The same principle applies to the manifest: make the data model extensible, but keep initial product policies explicit rather than building a highly generic rules engine.
Final verdict

I'd approve this architecture as the direction for Lightinator, subject to a few refinements.

The strongest parts are the separation of release identity from channel promotion, independent firmware/webapp/API versioning, product-aware artifact selection, and explicit compatibility enforcement. Those choices address real failure modes rather than merely making the pipeline more sophisticated.

My three highest-priority requirements would be:

    Immutable and correctly targeted releases. No artifact overwrite, cross-product selection, incomplete publication or ambiguous release identity.

    Provable compatibility and safe updates. Contract metadata, capability checks, actual conformance tests and tested recovery paths.

    Incremental migration with legacy compatibility. Keep existing devices and consumers working while the new catalogue and APIs are validated.

One final recommendation: before implementation, write a short set of release invariants that every CI workflow, server endpoint and device client must preserve. For example:

    A release ID always identifies the same manifest and artifact digests.

    A finalized release cannot be mutated.

    A channel never selects a release for the wrong product or target.

    An update is never offered as compatible based on version ordering alone.

    A failed discovery or installation does not discard the last known-good working configuration.

    A release can be promoted without rebuilding its artifacts.

If those invariants become automated tests, the architecture will be much easier to maintain than if correctness lives mainly in the design document.

The goal shouldn't be to build the most sophisticated release system. It should be to make it impossible for a normal release operation to publish the wrong binary or put a device into an incompatible state. Your proposal is already heading in that direction.API version for simplicity;
per-surface fingerprints can help review changes without creating independent
HTTP, RPC and MQTT compatibility systems immediately.

### Discovery And Release Manifests

Firmware should expose a small discovery block in device info and embed the same
metadata in its release manifest. Full schemas remain server-side, not in device
RAM. Illustrative proposed fields:

```json
{
  "api": {
     "version": "1.2.0",
     "contractHash": "sha256:<normalized-contract-digest>",
     "capabilities": ["lights.single", "presets"]
  }
}
```

Each webapp release declares supported API ranges and mandatory capabilities,
independently of its own component version:

```json
{
  "version": "3.4.1",
  "apiCompatibility": [
     { "minApi": "1.1.0", "maxApiExclusive": "2.0.0" }
  ],
  "requiredCapabilities": ["presets"]
}
```

For device API `v`, a range matches when `minApi <= v < maxApiExclusive`, using
a SemVer parser, never lexical string comparison. Multiple ranges are ORed,
allowing a bridge webapp to support two API majors. All required capabilities
must be present. Pre-release APIs need explicit opt-in and qualification; do not
silently admit them into stable ranges. Avoid unboChatGPT:

My overall take: this is a strong architecture, and the most important decisions are the right ones. You're moving from a branch-driven firmware release process toward a product-aware release system with immutable artifacts, independent component versions, and explicit compatibility contracts. That's the right direction for Lightinator as it grows beyond a single ESP8266/ESP32 controller.

My main concern isn't the architecture itself. It's the amount of release infrastructure being introduced at once. There are several distinct problems here, and I would solve them in a deliberate order rather than implementing the entire proposal as one large migration.

I'd keep the core design, tighten a few important details, and simplify the initial implementation.
1. The decisions I strongly agree with

Version-first branches: 5.0/develop, 5.1/develop

This is a good fit for parallel maintenance. A security or stability fix for 5.0 shouldn't depend on the progress of 5.1, and promoting 5.1 shouldn't force a 5.0 promotion. Explicit forward-ports make the relationship between lines visible.

Build once, promote the same artifact

This is one of the most important improvements in the proposal. A candidate that passed testing should be exactly the binary that reaches production, not a rebuild from a nominally equivalent branch. Immutable artifact identity and checksums make that verifiable.

Independent firmware, webapp and API versions

These have different reasons to change. A UI fix shouldn't require firmware release ceremony, and an internal firmware fix shouldn't create a fictional API change. Explicit compatibility metadata is much more robust than assuming versions move together.

A product-aware release catalogue

Moving away from filenames and SoC names as implicit product identity is essential. A controller, wallpanel and color sensor can have overlapping version numbers and target architectures without being the same product. Modeling products and installable targets explicitly avoids a lot of future ambiguity.

Compatibility checks in both update directions

Correctly recognizing that webapp updates can break firmware compatibility, and firmware updates can break the installed webapp, is crucial. Server-side filtering alone isn't enough when devices may be offline, clients may be old, or discovery metadata may be stale.

These are foundational decisions worth preserving.
2. The most important improvements I'd make
A. Separate the release line, release channel and software version

Your branch convention is fine, but there are three different concepts that should never become conflated:

    Release line: 5.0 or 5.1

    Channel: develop, testing or prod

    Artifact version: an immutable software release identifier, such as 5.1.0-12

Your example manifest currently has:
json

{
  "version": "5.1.0-12",
  "channel": "5.1/develop"
}

That works as a starting point, but I would store the line and channel separately in the canonical catalogue:
json

{
  "product": "controller",
  "releaseLine": "5.1",
  "version": "5.1.0-12",
  "channel": "develop"
}

The branch 5.1/develop then maps to a release line and a channel. It isn't itself the channel.

This separation matters because promotion must not change artifact identity. The same release ID can move from testing to production without changing its version, checksum or embedded metadata.

I'd also be explicit about the distinction between an artifact's immutable version and the mutable set of releases eligible for a channel.
B. Don't let branch names become permanent distribution identities

The proposal correctly says source branches are provenance and channel membership is mutable server metadata. I'd make that a hard architectural rule.

For example:

    A CI build originates from 5.1/testing.

    The resulting artifact has a stable release ID and source commit.

    The release is qualified and promoted to production.

    The server updates channel membership.

    The binary doesn't change, and no rebuild is needed.

The source branch is useful for auditability, but clients should request something like:

product=controller, channel=prod, target=esp32

rather than infer production status from an artifact residing under a /5.1/testing/ path.

You can retain existing branch-based storage paths during migration. Just don't let those paths define the long-term semantics of release discovery.
C. Define atomicity across the entire release, not just individual uploads

Your staged upload and finalization design is good. I'd make the invariant explicit:

A release is either complete and discoverable, or incomplete and undiscoverable.

That includes more than uploading all files successfully.

For a firmware release, finalization should verify:

    Every required SoC/build-type combination is present.

    Every artifact matches its declared checksum and size.

    The manifest matches the allowed product and release-line policy.

    All required auxiliary artifacts, such as symbol files or partition metadata, are present where applicable.

    The release metadata is immutable after finalization.

There is one subtle distinction worth preserving: a release doesn't necessarily need every theoretically supported target to be available. If a product's release policy permits a partial target set, that set must be explicitly declared and complete. The server must never silently interpret an interrupted upload as an intentional partial release.

I'd use an explicit release lifecycle:

staging → finalized → promoted

A failed staging operation remains retryable. A finalized release is immutable. Promotion changes channel membership, not release contents.
D. Tighten the definition of a release ID

You mention an immutable release ID, but don't define what makes it unique.

I'd generate a server-assigned release ID and separately retain:

    Product and release version.

    Source repository and commit.

    Release-manifest digest.

    Target-specific artifact digests.

    Build configuration and toolchain provenance.

    Contract identity.

    Creation and finalization metadata.

A Git commit is not a sufficient release identity: the same commit can produce different binaries under different toolchains, build configurations or dependency revisions.

Likewise, the version string isn't sufficient because different targets and build types can share a version.

For reproducibility and debugging, I'd also record the pinned RGBWWLed submodule commit, relevant build flags, and toolchain/container version. Reproducible builds are a valuable later goal, but verifiable provenance is a good initial requirement.
3. The API contract proposal is good, but needs one important distinction

I agree with the split between contractHash and apiVersion.

A normalized hash answers:

    Is this exactly the same public contract?

A SemVer API version answers:

    What compatibility guarantees have we reviewed and committed to?

Those are different questions.

However, I would avoid treating API SemVer as the only evidence that two particular implementations are compatible.
Compatibility is directional

Suppose firmware 5.0 provides API 1.1 and firmware 5.1 provides API 1.2.

A webapp may support both, but compatibility can still depend on what it sends and what it receives. The important dimensions are:

    Requests the client sends must be accepted by the firmware.

    Responses and events emitted by firmware must be understood by the client.

    Required capabilities must exist on the specific target.

    Semantics must remain compatible, not just field shapes.

Your proposal recognizes this already. I'd turn it into executable compatibility tests rather than leaving it as documentation.

For example, model the relationship as:
compatible⁡(W,F)=apiRangeMatches⁡(W,F)∧capabilitiesMatch⁡(W,F)∧behaviorTestsPass⁡(W,F)
compatible(W,F)=apiRangeMatches(W,F)∧capabilitiesMatch(W,F)∧behaviorTestsPass(W,F)

Here, W
W is a webapp release and F
F is a firmware target.

The range and capability checks are efficient runtime filters. The behavior tests are what give you confidence that the declared relationship is true.
One caveat about hashing ConfigDB schemas

I like using the four runtime .cfgdb files as the authoritative starting point, with operation metadata layered on top.

But don't make the normalized hash depend on an assumption that you can safely ignore all non-wire metadata. ConfigDB annotations may influence serialization, defaults, validation or other runtime behavior.

I would version the normalization algorithm itself, for example with a contractFormatVersion, and maintain golden fixtures that demonstrate how representative schema changes affect the projection.

Also, a schema-derived hash cannot detect every behavioral change. A firmware implementation could change the meaning of a method without changing its schema. That's why the API changelog, explicit version review and conformance tests are necessary.

Finally, don't automatically bump API SemVer just because the hash changes. Conversely, don't assume an unchanged hash proves compatibility.
4. The update and rollback design deserves the strongest safety guarantees

This is the part I'd be most careful about before enabling automatic updates.

Your proposal correctly checks compatibility in both directions. I'd add an explicit distinction between discovering an update and committing to install it.
Recommended update sequence

    Discover: Select a release for the correct product, hardware, SoC and channel.

    Validate: Verify its signature, digest, compatibility metadata and install constraints.

    Evaluate the resulting pair: Check the target firmware against the installed webapp, or the target webapp against the installed firmware.

    Plan: Determine whether the update can happen independently, requires a bridge release, or needs a coordinated upgrade.

    Install and verify: Use the device's supported OTA mechanism, then confirm that the new component starts and communicates correctly.

    Recover if needed: Use the device's actual rollback or recovery mechanism, accounting for persistent-data migrations.

A few details matter here:

    A signed manifest authenticates metadata only if the signature is verified against a trusted key and the signed content binds the artifact digests.

    TLS and authenticated CI publishing are valuable, but they don't replace device-side verification of the downloaded artifact.

    A checksum detects corruption; by itself, it doesn't establish authenticity.

    Rollback isn't automatically safe just because an older release remains in the catalogue. Data migrations, bootloader behavior and OTA partition layouts can make it impossible.

I'd also avoid promising automatic rollback unless each target has a tested rollback mechanism.

Your recommendation to retain a known-good working pair is excellent. Make that an explicit state that survives failed discovery, failed installation and a reboot.
5. SQLite is a reasonable starting point, with one caveat

I wouldn't introduce a more complex database just for this design. SQLite can serve a release catalogue well, especially for a single server instance or a modest distribution service.

However, I'd make the relational model a little more explicit than the illustrative composite key suggests.

I would separate the following entities:

Entity


Purpose

products


Stable identity and product policy

releases


Immutable release identity, version, provenance and manifest

release_targets


Hardware, revision, SoC and build-type applicability

artifacts


Immutable blobs, roles, sizes, digests and storage references

channel_memberships


Which releases are eligible for which channel

contract_versions


API SemVer, contract digest and immutable bundle reference

I would use surrogate IDs for relationships and unique constraints for natural identities, rather than making one large composite key the primary identifier for everything.

For example, artifact roles belong naturally to artifacts associated with a release target. A release may contain several files for one target, and a target may be shared across release policies.

Two implementation details are particularly important:

    Use database transactions for finalization and channel changes, with suitable locking or concurrency controls.

    Keep large artifact binaries in immutable blob storage or a filesystem/object store, with SQLite holding metadata and references.

If the current server already stores artifacts on disk, there's no need to move the bytes into the database. The important thing is that a database record never points to an artifact that is only partially written or has been overwritten by another release.
6. Your biggest practical risk is migration scope

The proposal covers several large changes at once:

    Versioned branch naming across two repositories.

    CI matrix changes and release tagging.

    A new product-aware release catalogue.

    Signed, authenticated uploads.

    API contract generation and SemVer governance.

    Bidirectional OTA compatibility.

    Home Assistant and log-service migration.

    New wallpanel and color-sensor product lines.

Each is defensible. Doing them all simultaneously would make failures difficult to diagnose.

I'd order the work around reducing risk as early as possible.
Phase 1
Release basics

Make the existing pipeline correct

    Implement versioned branch filters and independent SoC matrices.

    Fix the ESP8266-only one-time CI guards.

    Separate 5.0 and 5.1 version/tag counters.

    Pin dependencies and preserve immutable build outputs.

    Test the legacy branch aliases and deployed paths.

Phase 2
Compatibility

Establish a trustworthy API baseline

    Reconcile the four runtime schemas and operation metadata.

    Generate the contract digest.

    Establish reviewed API SemVer and a changelog.

    Add compatibility manifests and automated tests for existing firmware/webapp pairs.

    Pin unknown legacy builds until their compatibility has been verified.

Phase 3
Catalogue

Add product-aware releases alongside the current service

    Extend SQLite additively.

    Introduce immutable release manifests and staged uploads.

    Shadow-compare discovery results.

    Add atomic finalization, signatures and explicit no-match responses.

    Generate the legacy version.json projection from finalized records.

Phase 4
OTA safety

Enforce compatibility in real clients

    Add device-side artifact verification.

    Validate both directions of firmware/webapp updates.

    Test bridge upgrades, failed installs and rollback.

    Migrate Home Assistant and log-service consumers.

    Enable automatic cross-line updates only after hardware qualification.

Phase 5
Expansion

Add more products and retire legacy infrastructure

    Introduce wallpanel and color sensor as independent products.

    Define and test their own target policies and installers.

    Retire SSH publishing and legacy routes only when actual consumers have migrated.

This ordering intentionally gets the current controller pipeline reliable before adding complexity for future products.
7. A few specific issues I'd resolve in the written design

These are small compared with the overall architecture, but worth making explicit.

Branch names versus SemVer. Decide how you derive the release line from 5.0 or 5.1 when the software's actual version is 5.0.z or 5.1.z. Don't allow a manually supplied version to contradict the selected branch.

Prerelease counters. Define whether 5.1.0-12 is an ordinary SemVer prerelease identifier and whether its counter is monotonic per line, per version, or per commit. Ensure the same version cannot be assigned to different artifacts accidentally.

Firmware and webapp channels. Separate preferences are correct. I'd also specify the default policy: a user selecting a development firmware channel should continue receiving the production webapp unless they explicitly choose otherwise, provided that pair is compatible.

Target identity. SoC isn't enough. Board revision, flash capacity, partition layout and bootloader/OTA format may determine whether an artifact is installable. Only include those dimensions where they affect eligibility, but define the target model so you can add them without redesigning the catalogue.

Capabilities. Give capabilities stable identifiers and semantics. A capability should mean the feature actually exists and behaves as promised on that build, not merely that its schema defines relevant messages.

Server-side compatibility. The server can select candidates, but clients must validate the returned metadata. Treat compatibility declarations as necessary evidence, not a substitute for target-side checks.

Production promotion. Define who or what is authorized to promote a release, and whether promotion is immediate or staged. A prod channel is a policy boundary, not just another branch name.

Webapp packaging. A webapp hosted remotely can change independently of a device's local update lifecycle. Pinning and rollback may require different mechanisms for on-device assets and externally hosted assets. I'd explicitly distinguish these deployment modes in the release metadata.
8. What I would avoid building prematurely

There are three areas where I'd resist unnecessary generalization.

- A separate API-contract repository. Your proposed immutable contract bundle can initially be generated and published by the existing schema pipeline. A separate repository is justified if ownership or release cadence demands it, not merely because the contract is independently versioned.

- Per-surface API versioning. Start with the aggregate API version, as you propose. Separate HTTP, JSON-RPC and MQTT version streams could make compatibility harder to reason about before you have a concrete need.

- A universal device updater. Your statement that the server is the catalogue and distribution layer, not a universal updater, is exactly right. The controller, wallpanel and sensor may have different bootloaders, installation procedures and recovery guarantees. Share metadata and policy where possible, not assumptions about how every device installs a binary.

The same principle applies to the manifest: make the data model extensible, but keep initial product policies explicit rather than building a highly generic rules engine.
Final verdict

I'd approve this architecture as the direction for Lightinator, subject to a few refinements.

The strongest parts are the separation of release identity from channel promotion, independent firmware/webapp/API versioning, product-aware artifact selection, and explicit compatibility enforcement. Those choices address real failure modes rather than merely making the pipeline more sophisticated.

My three highest-priority requirements would be:

    Immutable and correctly targeted releases. No artifact overwrite, cross-product selection, incomplete publication or ambiguous release identity.

    Provable compatibility and safe updates. Contract metadata, capability checks, actual conformance tests and tested recovery paths.

    Incremental migration with legacy compatibility. Keep existing devices and consumers working while the new catalogue and APIs are validated.

One final recommendation: before implementation, write a short set of release invariants that every CI workflow, server endpoint and device client must preserve. For example:

    A release ID always identifies the same manifest and artifact digests.

    A finalized release cannot be mutated.

    A channel never selects a release for the wrong product or target.

    An update is never offered as compatible based on version ordering alone.

    A failed discovery or installation does not discard the last known-good working configuration.

    A release can be promoted without rebuilding its artifacts.

If those invariants become automated tests, the architecture will be much easier to maintain than if correctness lives mainly in the design document.

The goal shouldn't be to build the most sophisticated release system. It should be to make it impossible for a normal release operation to publish the wrong binary or put a device into an incompatible state. Your proposal is already heading in that direction.unded support for future majors.

Capabilities describe actual device/build support, not merely what the schema
allows. A webapp can retain single-light behavior on ESP8266 and expose multi-light
controls only when ESP32 firmware advertises `lights.multiple`. Optional features
must not become baseline requirements just because the UI knows about them.

### Server Compatibility Selection

Reuse the min/max selection concept, but introduce API-specific bounds instead of
storing API versions in fields whose names mean firmware versions.

The local `lightinator_server` implementation inspected here stores
`min_firmware` and filters `/api/webapp/versions` by `firmware_version`; no maximum
bound was found there. Its README describes `/api/webapp/latest`, which firmware
OTA requests, but that route is absent from the inspected local implementation.
Confirm the deployed revision before migration rather than assuming those
capabilities are already available.

Proposed additive changes:

- Firmware records expose provided API version/hash and capabilities alongside
  SoC, build type, immutable artifact identity and release channels.
- Webapp records store API compatibility ranges, required capabilities, checksums
  and OTA package requirements.
- Webapp discovery accepts device API identity and capabilities, filters by the
  selected webapp channel and compatibility, then returns the newest eligible
  release. For example, add `api_version=1.2.0` to version/latest queries and
  define a capability parameter or structured request.
- Firmware discovery returns target API metadata so clients can check the
  installed webapp before offering or installing a firmware update.
- A no-match result is explicit. Never fall back to an incompatible latest build.

Keep `firmware_version` and `min_firmware` working for legacy clients during
migration. If API and legacy constraints both apply, enforce both; the legacy
path must not bypass an API mismatch. Firmware-specific exclusions can still
handle known implementation bugs, but should be exceptions. Flash capacity,
filesystem layout and OTA package-format support remain separate install checks.

### Safe Updates In Both Directions

1. Before updating the webapp, compare its manifest with the running firmware's
    API and capabilities.
2. Before updating firmware, compare the installed webapp's manifest with the
    target firmware's API and capabilities, as well as hardware/install constraints.
3. If the pair would be incompatible, prefer installing a bridge webapp supporting
    both APIs first. Otherwise block ordinary automatic updates and require a
    documented coordinated-upgrade or recovery procedure.
4. The webapp also negotiates on connection, including when hosted remotely.
    On mismatch, do not issue normal control/configuration writes; retain a
    recovery path rather than presenting partially functional controls.
5. Apply the same checks to rollback. Persistent-data migrations can independently
    prevent rollback, so record migration support, backups and recovery procedures.

Server selection and device-side checks must agree. Keep the current working pair
when discovery fails or no compatible candidate exists. Retain compatible
known-good artifacts for every maintained API/device line.

### Release Gates And Examples

Firmware candidates must build across supported SoCs/build types, regenerate
contracts without drift, pass schema-diff and Host/API conformance checks, and
exercise OTA on representative hardware. ESP32-only additions must preserve
ESP8266 compilation.

Webapp candidates must pass packaging checks, contract tests against the oldest
supported API of each maintained range and current candidates, and browser tests
against maintained fixtures/emulators. Cover missing capabilities, unknown optional
fields, mismatches and legacy discovery. Qualify production firmware/webapp pairs,
including update order and rollback, before promoting to `prod`.

- A webapp UI fix releases alone with unchanged API requirements.
- A firmware memory fix releases alone with unchanged API identity when the
  observable contract is unchanged.
- An additive ESP32 multi-light API can introduce a minor API version while a
  newer webapp retains its older single-light path for ESP8266.
- Removing the single-light interface requires a new API major and a qualified
  bridge client or explicit coordinated upgrade. Keep the old compatible webapp
  available for maintained older firmware.

### Incremental Adoption

1. Verify the four runtime schemas and operation mappings against real traffic;
    establish a stable API baseline rather than trusting the older inferred marker.
2. Extend the generator and CI to publish normalized identities, reviewed versions
    and immutable contract bundles from the runtime schema graph.
3. Add firmware discovery and webapp compatibility manifests. Map legacy firmware
    versions to API baselines only when checked; unknown builds remain pinned or
    require manual qualification, not a guessed compatible API.
4. Extend server storage, uploads and discovery additively, testing range boundaries,
    pre-releases, capabilities, no-match results and legacy callers.
5. Enforce metadata in firmware OTA, webapp startup and other update clients such
    as Home Assistant before enabling automatic cross-line upgrades.
6. Introduce immutable channel promotion and publish a tested compatibility matrix.
    Retire legacy filtering only after supported callers migrate or have an
    explicitly maintained legacy endpoint.

The result is two independent release tracks connected by a versioned, tested
contract. Release channels express confidence; API metadata expresses compatibility.

suggestions:

ChatGPT:

My overall take: this is a strong architecture, and the most important decisions are the right ones. You're moving from a branch-driven firmware release process toward a product-aware release system with immutable artifacts, independent component versions, and explicit compatibility contracts. That's the right direction for Lightinator as it grows beyond a single ESP8266/ESP32 controller.

My main concern isn't the architecture itself. It's the amount of release infrastructure being introduced at once. There are several distinct problems here, and I would solve them in a deliberate order rather than implementing the entire proposal as one large migration.

I'd keep the core design, tighten a few important details, and simplify the initial implementation.
1. The decisions I strongly agree with

Version-first branches: 5.0/develop, 5.1/develop

This is a good fit for parallel maintenance. A security or stability fix for 5.0 shouldn't depend on the progress of 5.1, and promoting 5.1 shouldn't force a 5.0 promotion. Explicit forward-ports make the relationship between lines visible.

Build once, promote the same artifact

This is one of the most important improvements in the proposal. A candidate that passed testing should be exactly the binary that reaches production, not a rebuild from a nominally equivalent branch. Immutable artifact identity and checksums make that verifiable.

Independent firmware, webapp and API versions

These have different reasons to change. A UI fix shouldn't require firmware release ceremony, and an internal firmware fix shouldn't create a fictional API change. Explicit compatibility metadata is much more robust than assuming versions move together.

A product-aware release catalogue

Moving away from filenames and SoC names as implicit product identity is essential. A controller, wallpanel and color sensor can have overlapping version numbers and target architectures without being the same product. Modeling products and installable targets explicitly avoids a lot of future ambiguity.

Compatibility checks in both update directions

Correctly recognizing that webapp updates can break firmware compatibility, and firmware updates can break the installed webapp, is crucial. Server-side filtering alone isn't enough when devices may be offline, clients may be old, or discovery metadata may be stale.

These are foundational decisions worth preserving.
2. The most important improvements I'd make
A. Separate the release line, release channel and software version

Your branch convention is fine, but there are three different concepts that should never become conflated:

    Release line: 5.0 or 5.1

    Channel: develop, testing or prod

    Artifact version: an immutable software release identifier, such as 5.1.0-12

Your example manifest currently has:
json

{
  "version": "5.1.0-12",
  "channel": "5.1/develop"
}

That works as a starting point, but I would store the line and channel separately in the canonical catalogue:
json

{
  "product": "controller",
  "releaseLine": "5.1",
  "version": "5.1.0-12",
  "channel": "develop"
}

The branch 5.1/develop then maps to a release line and a channel. It isn't itself the channel.

This separation matters because promotion must not change artifact identity. The same release ID can move from testing to production without changing its version, checksum or embedded metadata.

I'd also be explicit about the distinction between an artifact's immutable version and the mutable set of releases eligible for a channel.
B. Don't let branch names become permanent distribution identities

The proposal correctly says source branches are provenance and channel membership is mutable server metadata. I'd make that a hard architectural rule.

For example:

    A CI build originates from 5.1/testing.

    The resulting artifact has a stable release ID and source commit.

    The release is qualified and promoted to production.

    The server updates channel membership.

    The binary doesn't change, and no rebuild is needed.

The source branch is useful for auditability, but clients should request something like:

product=controller, channel=prod, target=esp32

rather than infer production status from an artifact residing under a /5.1/testing/ path.

You can retain existing branch-based storage paths during migration. Just don't let those paths define the long-term semantics of release discovery.
C. Define atomicity across the entire release, not just individual uploads

Your staged upload and finalization design is good. I'd make the invariant explicit:

A release is either complete and discoverable, or incomplete and undiscoverable.

That includes more than uploading all files successfully.

For a firmware release, finalization should verify:

    Every required SoC/build-type combination is present.

    Every artifact matches its declared checksum and size.

    The manifest matches the allowed product and release-line policy.

    All required auxiliary artifacts, such as symbol files or partition metadata, are present where applicable.

    The release metadata is immutable after finalization.

There is one subtle distinction worth preserving: a release doesn't necessarily need every theoretically supported target to be available. If a product's release policy permits a partial target set, that set must be explicitly declared and complete. The server must never silently interpret an interrupted upload as an intentional partial release.

I'd use an explicit release lifecycle:

staging → finalized → promoted

A failed staging operation remains retryable. A finalized release is immutable. Promotion changes channel membership, not release contents.
D. Tighten the definition of a release ID

You mention an immutable release ID, but don't define what makes it unique.

I'd generate a server-assigned release ID and separately retain:

    Product and release version.

    Source repository and commit.

    Release-manifest digest.

    Target-specific artifact digests.

    Build configuration and toolchain provenance.

    Contract identity.

    Creation and finalization metadata.

A Git commit is not a sufficient release identity: the same commit can produce different binaries under different toolchains, build configurations or dependency revisions.

Likewise, the version string isn't sufficient because different targets and build types can share a version.

For reproducibility and debugging, I'd also record the pinned RGBWWLed submodule commit, relevant build flags, and toolchain/container version. Reproducible builds are a valuable later goal, but verifiable provenance is a good initial requirement.
3. The API contract proposal is good, but needs one important distinction

I agree with the split between contractHash and apiVersion.

A normalized hash answers:

    Is this exactly the same public contract?

A SemVer API version answers:

    What compatibility guarantees have we reviewed and committed to?

Those are different questions.

However, I would avoid treating API SemVer as the only evidence that two particular implementations are compatible.
Compatibility is directional

Suppose firmware 5.0 provides API 1.1 and firmware 5.1 provides API 1.2.

A webapp may support both, but compatibility can still depend on what it sends and what it receives. The important dimensions are:

    Requests the client sends must be accepted by the firmware.

    Responses and events emitted by firmware must be understood by the client.

    Required capabilities must exist on the specific target.

    Semantics must remain compatible, not just field shapes.

Your proposal recognizes this already. I'd turn it into executable compatibility tests rather than leaving it as documentation.

For example, model the relationship as:
compatible⁡(W,F)=apiRangeMatches⁡(W,F)∧capabilitiesMatch⁡(W,F)∧behaviorTestsPass⁡(W,F)
compatible(W,F)=apiRangeMatches(W,F)∧capabilitiesMatch(W,F)∧behaviorTestsPass(W,F)

Here, W
W is a webapp release and F
F is a firmware target.

The range and capability checks are efficient runtime filters. The behavior tests are what give you confidence that the declared relationship is true.
One caveat about hashing ConfigDB schemas

I like using the four runtime .cfgdb files as the authoritative starting point, with operation metadata layered on top.

But don't make the normalized hash depend on an assumption that you can safely ignore all non-wire metadata. ConfigDB annotations may influence serialization, defaults, validation or other runtime behavior.

I would version the normalization algorithm itself, for example with a contractFormatVersion, and maintain golden fixtures that demonstrate how representative schema changes affect the projection.

Also, a schema-derived hash cannot detect every behavioral change. A firmware implementation could change the meaning of a method without changing its schema. That's why the API changelog, explicit version review and conformance tests are necessary.

Finally, don't automatically bump API SemVer just because the hash changes. Conversely, don't assume an unchanged hash proves compatibility.
4. The update and rollback design deserves the strongest safety guarantees

This is the part I'd be most careful about before enabling automatic updates.

Your proposal correctly checks compatibility in both directions. I'd add an explicit distinction between discovering an update and committing to install it.
Recommended update sequence

    Discover: Select a release for the correct product, hardware, SoC and channel.

    Validate: Verify its signature, digest, compatibility metadata and install constraints.

    Evaluate the resulting pair: Check the target firmware against the installed webapp, or the target webapp against the installed firmware.

    Plan: Determine whether the update can happen independently, requires a bridge release, or needs a coordinated upgrade.

    Install and verify: Use the device's supported OTA mechanism, then confirm that the new component starts and communicates correctly.

    Recover if needed: Use the device's actual rollback or recovery mechanism, accounting for persistent-data migrations.

A few details matter here:

    A signed manifest authenticates metadata only if the signature is verified against a trusted key and the signed content binds the artifact digests.

    TLS and authenticated CI publishing are valuable, but they don't replace device-side verification of the downloaded artifact.

    A checksum detects corruption; by itself, it doesn't establish authenticity.

    Rollback isn't automatically safe just because an older release remains in the catalogue. Data migrations, bootloader behavior and OTA partition layouts can make it impossible.

I'd also avoid promising automatic rollback unless each target has a tested rollback mechanism.

Your recommendation to retain a known-good working pair is excellent. Make that an explicit state that survives failed discovery, failed installation and a reboot.
5. SQLite is a reasonable starting point, with one caveat

I wouldn't introduce a more complex database just for this design. SQLite can serve a release catalogue well, especially for a single server instance or a modest distribution service.

However, I'd make the relational model a little more explicit than the illustrative composite key suggests.

I would separate the following entities:

Entity


Purpose

products


Stable identity and product policy

releases


Immutable release identity, version, provenance and manifest

release_targets


Hardware, revision, SoC and build-type applicability

artifacts


Immutable blobs, roles, sizes, digests and storage references

channel_memberships


Which releases are eligible for which channel

contract_versions


API SemVer, contract digest and immutable bundle reference

I would use surrogate IDs for relationships and unique constraints for natural identities, rather than making one large composite key the primary identifier for everything.

For example, artifact roles belong naturally to artifacts associated with a release target. A release may contain several files for one target, and a target may be shared across release policies.

Two implementation details are particularly important:

    Use database transactions for finalization and channel changes, with suitable locking or concurrency controls.

    Keep large artifact binaries in immutable blob storage or a filesystem/object store, with SQLite holding metadata and references.

If the current server already stores artifacts on disk, there's no need to move the bytes into the database. The important thing is that a database record never points to an artifact that is only partially written or has been overwritten by another release.
6. Your biggest practical risk is migration scope

The proposal covers several large changes at once:

    Versioned branch naming across two repositories.

    CI matrix changes and release tagging.

    A new product-aware release catalogue.

    Signed, authenticated uploads.

    API contract generation and SemVer governance.

    Bidirectional OTA compatibility.

    Home Assistant and log-service migration.

    New wallpanel and color-sensor product lines.

Each is defensible. Doing them all simultaneously would make failures difficult to diagnose.

I'd order the work around reducing risk as early as possible.
Phase 1
Release basics

Make the existing pipeline correct

    Implement versioned branch filters and independent SoC matrices.

    Fix the ESP8266-only one-time CI guards.

    Separate 5.0 and 5.1 version/tag counters.

    Pin dependencies and preserve immutable build outputs.

    Test the legacy branch aliases and deployed paths.

Phase 2
Compatibility

Establish a trustworthy API baseline

    Reconcile the four runtime schemas and operation metadata.

    Generate the contract digest.

    Establish reviewed API SemVer and a changelog.

    Add compatibility manifests and automated tests for existing firmware/webapp pairs.

    Pin unknown legacy builds until their compatibility has been verified.

Phase 3
Catalogue

Add product-aware releases alongside the current service

    Extend SQLite additively.

    Introduce immutable release manifests and staged uploads.

    Shadow-compare discovery results.

    Add atomic finalization, signatures and explicit no-match responses.

    Generate the legacy version.json projection from finalized records.

Phase 4
OTA safety

Enforce compatibility in real clients

    Add device-side artifact verification.

    Validate both directions of firmware/webapp updates.

    Test bridge upgrades, failed installs and rollback.

    Migrate Home Assistant and log-service consumers.

    Enable automatic cross-line updates only after hardware qualification.

Phase 5
Expansion

Add more products and retire legacy infrastructure

    Introduce wallpanel and color sensor as independent products.

    Define and test their own target policies and installers.

    Retire SSH publishing and legacy routes only when actual consumers have migrated.

This ordering intentionally gets the current controller pipeline reliable before adding complexity for future products.
7. A few specific issues I'd resolve in the written design

These are small compared with the overall architecture, but worth making explicit.

Branch names versus SemVer. Decide how you derive the release line from 5.0 or 5.1 when the software's actual version is 5.0.z or 5.1.z. Don't allow a manually supplied version to contradict the selected branch.

Prerelease counters. Define whether 5.1.0-12 is an ordinary SemVer prerelease identifier and whether its counter is monotonic per line, per version, or per commit. Ensure the same version cannot be assigned to different artifacts accidentally.

Firmware and webapp channels. Separate preferences are correct. I'd also specify the default policy: a user selecting a development firmware channel should continue receiving the production webapp unless they explicitly choose otherwise, provided that pair is compatible.

Target identity. SoC isn't enough. Board revision, flash capacity, partition layout and bootloader/OTA format may determine whether an artifact is installable. Only include those dimensions where they affect eligibility, but define the target model so you can add them without redesigning the catalogue.

Capabilities. Give capabilities stable identifiers and semantics. A capability should mean the feature actually exists and behaves as promised on that build, not merely that its schema defines relevant messages.

Server-side compatibility. The server can select candidates, but clients must validate the returned metadata. Treat compatibility declarations as necessary evidence, not a substitute for target-side checks.

Production promotion. Define who or what is authorized to promote a release, and whether promotion is immediate or staged. A prod channel is a policy boundary, not just another branch name.

Webapp packaging. A webapp hosted remotely can change independently of a device's local update lifecycle. Pinning and rollback may require different mechanisms for on-device assets and externally hosted assets. I'd explicitly distinguish these deployment modes in the release metadata.
8. What I would avoid building prematurely

There are three areas where I'd resist unnecessary generalization.

- A separate API-contract repository. Your proposed immutable contract bundle can initially be generated and published by the existing schema pipeline. A separate repository is justified if ownership or release cadence demands it, not merely because the contract is independently versioned.

- Per-surface API versioning. Start with the aggregate API version, as you propose. Separate HTTP, JSON-RPC and MQTT version streams could make compatibility harder to reason about before you have a concrete need.

- A universal device updater. Your statement that the server is the catalogue and distribution layer, not a universal updater, is exactly right. The controller, wallpanel and sensor may have different bootloaders, installation procedures and recovery guarantees. Share metadata and policy where possible, not assumptions about how every device installs a binary.

The same principle applies to the manifest: make the data model extensible, but keep initial product policies explicit rather than building a highly generic rules engine.
Final verdict

I'd approve this architecture as the direction for Lightinator, subject to a few refinements.

The strongest parts are the separation of release identity from channel promotion, independent firmware/webapp/API versioning, product-aware artifact selection, and explicit compatibility enforcement. Those choices address real failure modes rather than merely making the pipeline more sophisticated.

My three highest-priority requirements would be:

    Immutable and correctly targeted releases. No artifact overwrite, cross-product selection, incomplete publication or ambiguous release identity.

    Provable compatibility and safe updates. Contract metadata, capability checks, actual conformance tests and tested recovery paths.

    Incremental migration with legacy compatibility. Keep existing devices and consumers working while the new catalogue and APIs are validated.

One final recommendation: before implementation, write a short set of release invariants that every CI workflow, server endpoint and device client must preserve. For example:

    A release ID always identifies the same manifest and artifact digests.

    A finalized release cannot be mutated.

    A channel never selects a release for the wrong product or target.

    An update is never offered as compatible based on version ordering alone.

    A failed discovery or installation does not discard the last known-good working configuration.

    A release can be promoted without rebuilding its artifacts.

If those invariants become automated tests, the architecture will be much easier to maintain than if correctness lives mainly in the design document.

The goal shouldn't be to build the most sophisticated release system. It should be to make it impossible for a normal release operation to publish the wrong binary or put a device into an incompatible state. Your proposal is already heading in that direction.