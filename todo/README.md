# todo prompts

Each file in this folder is a self-contained prompt for one autonomous agent. `AGENTS.md` always applies on top of it. Files are numbered in dependency order; this file is the order of record and says which prompts may overlap.

## Order and status

| Prompt                                   | Status | Depends on                                       | May run in parallel with     |
| ---------------------------------------- | ------ | ------------------------------------------------ | ---------------------------- |
| `1-device-tooling`                       | next   |                                                  |                              |
| `2-user-input-priority`                  |        | `1`                                              |                              |
| `3-lvgl`                                 |        | `1`, `2`                                         | `5` Phase A, `6` Phase A     |
| `5-new-view-model` Phase A               |        | `2`                                              | `3`, `6` Phase A             |
| `6-subscription-reconciliation` Phase A  |        | `2`                                              | `3`, `5` Phase A             |
| `4-ws-1.8-multiscreen`                   |        | `3` decision (one track left), `1`               |                              |
| `5-new-view-model` Phase B               |        | `4`, merged `5` Phase A                          |                              |
| `6-subscription-reconciliation` Phase B  |        | `5` Phase B, merged `6` Phase A                  |                              |
| `7-grouping`                             |        | `5` Phase B; topology subscription (done)        | `6` Phase B                  |

Dependency graph: `1` → `2` → { `3` ∥ `5A` ∥ `6A` } → `4` → `5B` → `6B` → `7`. `7-grouping` needs topology events and the pending/view model, not AVTransport/RenderingControl events, so `6B` and `7` are order-independent. `4` is numbered before `5` and `6` because their Phase B work builds on it, even though their Phase A work may already be running beside `3`.

Retired: `0-fsm-testing` (runtime lifecycle machines, fault harness, stress task) landed in `dbacbc2`, `9e15b0c`, `e45dc22`, and `93e48a0`; the prompt is in history at `git show 93e48a0:todo/0-fsm-testing.md`. Its one spec gap, background reads blocking user input, is `2-user-input-priority`.

## Conventions

- **Two-track prompt.** `4-ws-1.8-multiscreen` contains an LVGL track and a hand-rolled track. The last step of `3-lvgl` rewrites it to the chosen track and deletes the other. An agent that finds both tracks present stops and reports instead of guessing.
- **Phases.** `5-new-view-model` and `6-subscription-reconciliation` are split into Phase A (portable, host tests only, no UI) and Phase B (device UI). Phase A work runs in a separate git worktree and is merged before its Phase B starts.
- **Autonomous device verification.** Every prompt that touches the device carries a section with the exact USB steps an agent runs before asking the owner for anything. They rely on `1-device-tooling`: `node --run ports -- --identify`, `node --run usb`, `node --run ui` (`ui-touch`, `ui-button`, `ui-state`), and `monitor --until` / `--stats`. Injected input is never local activity and never bypasses admission, read_only, or policy.
- **Live mutations.** The committed default profile has `read_only: false`; device verification sends real transport, volume, seek, and queue mutations to the configured room because real-world behavior is what is being tested. The owner mutes the amplifier during long loops. `read_only: true` is the deliberate brake for tests where mutations would be disruptive; agents may flash such a profile for that purpose but never flip an existing true flag to false.
- **Owner decisions** are marked `OWNER DECISION` inside a prompt and must be filled in before the prompt runs. Defaults follow `AGENTS.md`.
- **Read-first lists** at the top of each prompt name the files whose current behavior the prompt builds on; the facts quoted there were true when the prompt was written and must be re-verified against the tree.
- **Reports, not diaries.** Prompts end with a short report; durable findings go into `docs/` under the documentation durability rule, and everything else stays in `.local/` or the conversation.

## Owner decisions

- Decided: autonomous Sonos mutations are allowed against any room in the `rooms` allowlist of the configuration flashed to the device under test (`config/default.json` by default). The rule lives in `AGENTS.md`; `6-subscription-reconciliation` §20 applies it.
- Recorded as default, `2-user-input-priority` §1: a user action during another user action is rejected visibly, never queued.
- Default off, `2-user-input-priority` §5: Stick B sends explicit Play/Pause from observed state instead of toggle's refresh-then-decide.
- By measurement, `2-user-input-priority` §6: Wi-Fi modem power save off while awake.
- Outstanding, `3-lvgl` §27: adopt, reject, or adopt with a limitation, after the physical test.
- `5-new-view-model` §7: pending stays visible until the job's terminal outcome (default) or reverts after a shorter horizon.
