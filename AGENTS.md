# Repository Guidelines

## Project Structure & Module Organization

`sonos-surface` is a personal, single-household ESP32-S3 Sonos project. Read [product scope](docs/product.md), [intents](docs/intent.md), [policies](docs/policy.md), [execution](docs/planner-executor.md), and [hardware evidence](docs/hardware.md) before changing behavior. The first two-board slice is demonstrated: M5 NFC playback and calibrated Waveshare center-button Pause/Play are owner-confirmed, with independent state reads on both. The V2 fit is stored per device in surface/touch NVS; diagnostics remain raw. Stick uses explicit target initialization. Native USB/early-boot power-cycle limitations remain; see the latest hardware-hardening evidence.

`libraries/SurfaceCore/src/` owns portable intent, policy, planning, execution, and AppState. `SurfaceSonos/src/` owns SOAP/Apple metadata behind `LocalHttp`. `SurfaceDevice/src/` contains the ESP32 runtime and separate board adapters. `firmware/sonos_surface/` is the shared entry point; `tests/` and `scripts/` contain validation/tooling. Keep hardware/network SDKs out of portable layers.

## Build, Test, and Development Commands

Run `python3 scripts/setup.py` for pinned Arduino-ESP32 dependencies, or add `--host-only` for portable tests. Run `python3 scripts/test.py`, then `python3 scripts/device.py build stick` or `build waveshare`. README documents exact flash/monitor commands. Keep generated `.deps/`, `.build/`, and private `.local/` files untracked. Update dependency pins and setup instructions together.

## Coding Style & Naming Conventions

C++17 uses two-space indentation, PascalCase types, and camelCase functions/fields. Python uses four spaces. No formatter is configured. Use descriptive Markdown headings, relative links, and valid JSON examples; keep wire names consistent with the specifications.

Treat omitted intent fields as preservation, retain policy provenance, and express operation dependencies explicitly. Never encode Sonos ordering or sleeps in cards. Keep grouping, generic scripting, and unrelated Sonos management out of scope.

## Testing Guidelines

Portable assertion tests run with address/undefined behavior sanitizers; no coverage threshold is imposed. Test omission, explicit false, policies, ordering, and failures as functionality grows. Keep the Mac probe read-only. Record real board/Sonos observations in `docs/hardware.md`; distinguish measured behavior from assumptions.

## Commit & Pull Request Guidelines

Use focused commits with imperative subjects. No broader commit convention has been established.

PRs should explain behavior, validation, and unresolved risks; link issues when available and include screenshots for UI changes. Update affected specifications together. Keep household credentials and personal configuration out of commits.

## Agent Working Style

Device runtime `read_only=true` blocks all Sonos mutations at HTTP dispatch;
`read_only=false` permits requested mutations only for configured, eligible rooms.
Room and playlist policy configuration use display IDs resolved from current names;
accepted requests freeze UUID and policy. M5/Waveshare flashing and non-Sonos tests
are authorized. Keep autonomous tests read-only; do not turn an existing true flag
false to complete testing. Stop for physical gestures or intentional live playback tests.

Resolve reversible engineering choices autonomously. Involve the human for meaningful hardware tests or choices that materially change user-visible semantics, persisted formats, or architecture. Label deliberate contracts, reference evidence, and experimental assumptions separately. Prefer small working slices on both boards; measure Sonos/NFC behavior before adding abstractions. Stay within the current task's authorized scope.
