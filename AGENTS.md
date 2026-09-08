# Repository Guidelines

## Project Structure & Module Organization

`sonos-surface` is a personal, single-household ESP32-S3 Sonos project. Read [product scope](docs/product.md), [intents](docs/intent.md), [policies](docs/policy.md), [execution](docs/planner-executor.md), and [hardware evidence](docs/hardware.md) before changing behavior. M5 NFC playback/room cycling and the Waveshare 1.8-inch frontend/artwork are owner-confirmed, with independent state reads on both. The V2 fit is stored per device in surface/touch NVS; diagnostics remain raw. Stick uses explicit target initialization. Native USB/early-boot limitations remain documented in hardware.

`libraries/SurfaceCore/src/` owns portable intent, policy, planning, execution, and AppState. `SurfaceSonos/src/` owns SOAP/Apple metadata behind `LocalHttp`. `SurfaceDevice/src/` contains the ESP32 runtime and separate board adapters. `firmware/sonos_surface/` is the shared entry point; `tests/` and `scripts/` contain validation/tooling. Keep hardware/network SDKs out of portable layers.

## Build, Test, and Development Commands

Run `python3 scripts/setup.py` for pinned Arduino-ESP32 dependencies, or add `--host-only` for portable tests. Run `python3 scripts/test.py`, then `python3 scripts/device.py build stick` or `build waveshare`. README documents exact flash/monitor commands. Keep generated `.deps/`, `.build/`, and private `.local/` files untracked. Update dependency pins and setup instructions together.

## Coding Style & Naming Conventions

C++17 uses two-space indentation, PascalCase types, and camelCase functions/fields. Python uses four spaces. No formatter is configured. Use descriptive Markdown headings, relative links, and valid JSON examples; keep wire names consistent with the specifications.

Omitted intent fields resolve through source/room policy, then preserve if still absent. Retain per-field provenance and express operation dependencies explicitly. Never encode Sonos ordering or sleeps in cards. Keep grouping, generic scripting, and unrelated Sonos management out of scope.

## Testing Guidelines

Portable assertion tests run with address/undefined behavior sanitizers; no coverage threshold is imposed. Test omission, explicit false, policies, ordering, and failures as functionality grows. Keep the Mac probe read-only. Record only durable board/Sonos findings in `docs/hardware.md`, following the [Documentation durability rule](#documentation-durability-rule); distinguish measured behavior from assumptions.

## Commit & Pull Request Guidelines

Use focused commits with imperative subjects. No broader commit convention has been established.

PRs should explain behavior, validation, and unresolved risks; link issues when available and include screenshots for UI changes. Update affected specifications together. Keep household credentials and personal configuration out of commits.

## Agent Working Style

Device runtime `read_only=true` blocks all Sonos mutations at HTTP dispatch;
`read_only=false` permits requested mutations only for configured, eligible rooms.
The room-keyed config object is the allowlist and contains source-specific exceptions.
Display IDs resolve from current names; accepted requests freeze UUID and policy. M5/Waveshare flashing and non-Sonos tests
are authorized. Keep autonomous tests read-only; do not turn an existing true flag
false to complete testing. Stop for physical gestures or intentional live playback tests.

Resolve reversible engineering choices autonomously. Involve the human for meaningful hardware tests or choices that materially change user-visible semantics, persisted formats, or architecture. Label deliberate contracts, reference evidence, and experimental assumptions separately. Prefer small working slices on both boards; measure Sonos/NFC behavior before adding abstractions. Stay within the current task's authorized scope.

## Documentation durability rule

Repository documentation describes CURRENT durable architecture, contracts,
hardware facts, setup, and known unresolved limitations.

Do NOT use repository docs as an engineering diary or test-run log.

In particular, DO NOT commit documentation merely to record:

- a device being flashed
- read_only being temporarily changed
- current config revisions
- the currently selected room
- a one-off owner test or confirmation
- a successful build/test/USB run
- temporary runtime state
- log filenames from a development session
- chronological "then we tested..." evidence
- completion of a milestone/checkpoint whose durable behavior is already documented

Those facts may remain in `.local/` logs or the agent conversation and normally
should NOT cause a repository change.

Only update durable docs when the work establishes or changes something a future
developer actually needs to know, such as:

- architecture or product semantics
- wire/config/schema contracts
- supported hardware or wiring
- reproducible setup/recovery procedure
- a persistent hardware quirk
- an unresolved limitation
- a measured constraint that materially affects implementation choices

When deciding whether to update docs, apply this test:

> If this exact development session had never happened, would a future agent still
> need this information to correctly understand, build, operate, or modify the system?

If no, do not commit it.

Examples:

GOOD:
- "read_only is a persisted runtime Sonos-mutation gate."
- "CST820 calibration is persisted per physical device."
- "Topology listener must not start before Wi-Fi because ESP32 networking can assert."
- "CO5300 rendering requires the full-frame PSRAM workaround."

DO NOT DOCUMENT:
- "Today the owner set read_only=false."
- "Stick is currently on revision 10."
- "The owner tested playback and it worked."
- "We flashed both boards and saved logs at .local/foo.log."
- "The device is currently left on Office."

Git history and ignored `.local/` logs are the development record. Durable docs are
not the development record.

Do not add or preserve historical checkpoint sections merely because previous
agents did so. If encountered during relevant work, remove stale session-specific
material when it is clearly no longer durable documentation.
