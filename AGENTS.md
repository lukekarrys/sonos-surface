# Repository Guidelines

## Project Structure & Module Organization

`sonos-surface` is a personal, single-household ESP32-S3 Sonos project. Read [product scope](docs/product.md), [intents](docs/intent.md), [policies](docs/policy.md), [execution](docs/planner-executor.md), and [hardware evidence](docs/hardware.md) before changing behavior. M5 NFC playback/room cycling and the Waveshare 1.8-inch frontend/artwork are owner-confirmed, with independent state reads on both. The V2 fit is stored per device in surface/touch NVS; diagnostics remain raw. Stick uses explicit target initialization. Native USB/early-boot limitations remain documented in hardware.

`libraries/SurfaceCore/src/` owns portable intent, policy, planning, execution, and AppState. `SurfaceSonos/src/` owns SOAP/Apple metadata behind `LocalHttp`. `SurfaceDevice/src/` contains the ESP32 runtime and separate board adapters. `firmware/sonos_surface/` is the shared entry point; `tests/` and `scripts/` contain validation/tooling. Keep hardware/network SDKs out of portable layers.

## Build, Test, and Development Commands

Node 24 (24.12 or later) is the host runtime. Run `npm install`, then `node --run setup` for pinned Arduino dependencies (`-- --host-only` for portable dependencies). TypeScript 7 checks host code; VS Code uses the recommended TypeScript 7 extension and the workspace package. TypeScript executes natively with erasable syntax and explicit `.ts` imports; no transpilation step or runtime aliases. Package scripts through `node --run` are the canonical task interface.

Leave `node --run check` green: static TypeScript checking, Prettier, clang-format, and portable tests with sanitizers and compiler warnings treated as errors. Firmware/tooling changes should also pass `node --run check:full`, which builds `stick-s3` and `ws-1.8` with Arduino warnings set to `more`.

Run `node --run cpp:configure` (`stick-s3` default) or `node --run cpp:configure -- ws-1.8` to select one active embedded editor target; host-test clang++ context is always included from the real host target descriptions. The generated, ignored `.build/compile_commands.json` owns C++ editor/compiler context; do not add fake defines or disable diagnostics. Keep generated `.deps/`, `.build/`, `node_modules/`, and private `.local/` files untracked. Update dependency pins and setup instructions together.

## Environment profiles and local secrets

`config/*.json` are the durable environment profiles, shared by any devices in that environment. Hardware targets are `stick-s3` and `ws-1.8`; their build facts live in `scripts/hardware-targets.ts`. Follow the [target, profile, and port distinction](README.md#flash-and-configure). Filenames are arbitrary human labels and never imply Sonos room identity; only JSON `rooms` keys select targets. The host tools expand `${NAME}` in JSON string values using process environment values over repo-root `.env` (or `--env-file`). Keep `.env` ignored and use `.env.example` for variable names. Never log or save resolved credential-bearing JSON. `.local/` is for logs, captures, and temporary/generated data.

`node --run configure -- --port PORT` uses `config/default.json`; `--config PATH` selects any profile path. `node --run flash:TARGET -- --port PORT --config PATH` preflights the profile before building/flashing, then configures and verifies status. Without `--config`, flash preserves device configuration. Firmware receives current JSON only; environment expansion belongs solely to the host.

## Coding Style & Naming Conventions

C++17 uses two-space indentation, PascalCase types, and camelCase functions/fields. Prettier owns all Markdown (`proseWrap: "never"`; do not hard wrap prose), TypeScript, JSON, and YAML formatting (`node --run format`); clang-format owns C/C++ and the sketch (`node --run format:cpp`). Use only erasable TypeScript syntax: no enums, parameter properties, or runtime namespaces. Use descriptive Markdown headings, relative links, and valid JSON examples; keep wire names consistent with the specifications.

Omitted intent fields resolve through source/room policy, then preserve if still absent. Retain per-field provenance and express operation dependencies explicitly. Never encode Sonos ordering or sleeps in cards. Keep grouping, generic scripting, and unrelated Sonos management out of scope.

## Testing Guidelines

Portable assertion tests run with address/undefined behavior sanitizers; no coverage threshold is imposed. Test omission, explicit false, policies, ordering, and failures as functionality grows. Keep the Mac probe read-only. Record only durable board/Sonos findings in `docs/hardware.md`, following the [Documentation durability rule](#documentation-durability-rule); distinguish measured behavior from assumptions.

## Commit & Pull Request Guidelines

Use focused commits with imperative subjects. Agents working a `todo/` prompt commit their finished work themselves when the prompt's completion list is met, following the commit rules in `todo/README.md` (`todo N:` subjects, green baseline first, never push); do not end such a session with uncommitted work. No broader commit convention has been established.

PRs should explain behavior, validation, and unresolved risks; link issues when available and include screenshots for UI changes. Update affected specifications together. Commit environment profiles with environment placeholders; keep household credentials out of Git.

## Agent Working Style

Background Sonos/network activity does not count as user activity for device sleep; wake is physical-button-only unless the owner explicitly changes that product behavior. Changes to physical button semantics or wake behavior must update the corresponding button/state diagram in `docs/hardware.md`.

Device runtime `read_only=true` blocks all Sonos mutations at HTTP dispatch; `read_only=false` permits requested mutations only for configured, eligible rooms. `rooms` is always the authoritative room allowlist and contains source-specific exceptions. Top-level `policy` applies only to configured rooms and cannot enroll rooms. Display IDs resolve from current names; accepted requests freeze UUID and policy. M5/Waveshare flashing and non-Sonos tests are authorized. Autonomous Sonos mutations are authorized against any room listed in the `rooms` allowlist of the configuration currently flashed to the device under test (`config/default.json` unless another profile was flashed; read the allowlist from that device's `config-status` reply, never from a guessed file), whether issued from host tooling or through that device. Prefer bounded, reversible changes (volume steps, pause/resume of a room already playing, seek within the current track, next/previous) and restore the previous state when practical; never touch a room outside that allowlist. The committed default profile permits mutations; `read_only=true` is the deliberate brake for tests where mutations would be disruptive. Do not turn an existing `read_only=true` flag false to complete testing; device-side mutation tests use a profile that already permits them. Stop for physical gestures.

Resolve reversible engineering choices autonomously. Involve the human for meaningful hardware tests or choices that materially change user-visible semantics, persisted formats, or architecture. Label deliberate contracts, reference evidence, and experimental assumptions separately. Prefer small working slices on both boards; measure Sonos/NFC behavior before adding abstractions. Stay within the current task's authorized scope.

## Breaking changes and migrations

This is an owner-controlled personal project. All deployed devices are expected to run the current firmware and current configuration format.

Unless the owner explicitly requests otherwise for a specific change:

- prefer clean breaking changes over backward compatibility
- support exactly one current persisted config/NVS format
- do not add migration code, compatibility readers, deprecated aliases, fallback parsers, schema upgraders, or dual-format support
- do not write migration guides or upgrade procedures
- do not document obsolete configuration shapes
- when a persisted format changes, update the current implementation, examples, tests, committed environment profiles, and local environment values as needed, and remove the superseded path
- old private configuration may simply be replaced
- all controlled devices may be flashed/configured together

Do not preserve a legacy behavior merely because an earlier commit/device used it. If backward compatibility or a staged migration is required, the owner will explicitly state that requirement. Git history is sufficient documentation of superseded formats.

A format intentionally part of the current product contract is not migration compatibility. Plain Apple Music URL NFC cards are a supported input format; keep that support unless separately asked to remove it. Describe supported inputs directly, without legacy rollout or migration rationale.

## Documentation durability rule

Repository documentation describes CURRENT durable architecture, contracts, hardware facts, setup, and known unresolved limitations.

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

Those facts may remain in `.local/` logs or the agent conversation and normally should NOT cause a repository change.

Only update durable docs when the work establishes or changes something a future developer actually needs to know, such as:

- architecture or product semantics
- wire/config/schema contracts
- supported hardware or wiring
- reproducible setup/recovery procedure
- a persistent hardware quirk
- an unresolved limitation
- a measured constraint that materially affects implementation choices

When deciding whether to update docs, apply this test:

> If this exact development session had never happened, would a future agent still need this information to correctly understand, build, operate, or modify the system?

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

Git history and ignored `.local/` logs are the development record. Durable docs are not the development record.

Do not add or preserve historical checkpoint sections merely because previous agents did so. If encountered during relevant work, remove stale session-specific material when it is clearly no longer durable documentation.
