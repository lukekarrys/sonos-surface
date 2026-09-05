# Repository Guidelines

## Project Structure & Module Organization

`sonos-surface` is a personal, single-household ESP32-S3 Sonos project. This repository currently contains specifications, not firmware. Read [product scope](docs/product.md), [intents and NFC format](docs/intent.md), [policies](docs/policy.md), and [planning and execution](docs/planner-executor.md) before changing behavior.

Future code must separate the portable core from device inputs/UI and Sonos transport. Both hardware targets share intent, policy, planner, executor, and AppState contracts. Do not import hardware, display, or network SDKs into the pure intent/policy/planning layers. Directory names and implementation language remain undecided.

## Build, Test, and Development Commands

No build, test, or development commands are configured. Add small macOS tests for portable logic, then integrate both available boards early; do not require a complete simulator first. When introducing tooling, document exact setup and verification commands in `README.md`; commit manifests and lockfiles together. Do not claim unconfigured commands have passed.

## Coding Style & Naming Conventions

No formatter or linter is selected. Configure indentation and naming with the chosen stack. Use descriptive Markdown headings, relative links, and valid JSON examples. Keep wire-format names consistent with the specifications.

Treat omitted intent fields as preservation, retain policy provenance, and express operation dependencies explicitly. Never encode Sonos ordering or sleeps in cards. Keep grouping, generic scripting, and unrelated Sonos management out of scope.

## Testing Guidelines

No framework or coverage threshold exists. Use the acceptance cases in the specifications as behavioral fixtures, including omission, explicit `false`, policy precedence, partial failure, uncertain retries, and stale state. Separate deterministic Mac tests from real-speaker tests; report which ran.

## Commit & Pull Request Guidelines

History currently contains only `init`; no established convention exists. Use focused commits with imperative subjects such as `Specify intent preservation semantics`.

PRs should explain behavior, validation, and unresolved risks; link issues when available and include screenshots for UI changes. Update affected specifications together. Keep household credentials and personal configuration out of commits.

## Agent Working Style

Resolve reversible engineering choices autonomously. Involve the human for meaningful hardware tests or choices that materially change user-visible semantics, persisted formats, or architecture. Label deliberate contracts, reference evidence, and experimental assumptions separately. Prefer small working slices on both boards; measure Sonos/NFC behavior before adding abstractions. Stay within the current task's authorized scope.
