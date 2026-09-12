# todo prompts

Each file in this folder is a self-contained prompt for one autonomous agent. `AGENTS.md` always applies on top of it. Files are numbered in dependency order; this file is the order of record and says which prompts may overlap.

## Order and status

| Prompt                                   | Status | Depends on                                       | May run in parallel with     |
| ---------------------------------------- | ------ | ------------------------------------------------ | ---------------------------- |
| `1-device-tooling`                       | in progress |                                                  |                              |
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

## How to run

The owner starts one agent per prompt, sequentially, in goal mode, and checks in after each. These rules are the owner's standing instructions for that session; the prompt adds its own. Status cells read: blank = not started, `in progress`, `implemented (<hash>)`, `needs fixes`, `done (<hash>)`.

Implementation session:

1. **Read first.** `AGENTS.md`, this file, and the prompt. Confirm every dependency in the table is `done`; if one is not, stop and say so. Work only that prompt: do not start the next one, and do not edit other prompts except where the prompt says so (`3-lvgl` rewrites `4-ws-1.8-multiscreen`).
2. **Status.** Set the prompt's Status cell to `in progress` when you start and `implemented (<hash>)` in the same commit as the finished work; `done` belongs to the review session.
3. **Commits are authorized** by these rules. Use focused imperative subjects prefixed by the prompt number, for example `todo 2: preempt automatic reads for user input`. Commit once the host baseline is green (`node --run check`, `node --run check:full`, and `node --run stress` where the prompt lists it) and before any flashing, then again after device verification. Never commit a red baseline. Never push. **Commit when done:** a prompt is not finished until its work, its status cell, and its doc changes are committed; do not end the session with uncommitted work.
4. **Devices.** Both boards may be attached. Identify ports before flashing (`node --run ports -- --identify` once `1-device-tooling` exists; before that, `node --run monitor` and read the boot banner). Disable sleep for the session with a throwaway profile through `node --run configure` and restore `config/default.json` before finishing. Real Sonos mutations on the configured rooms are allowed; the owner mutes the amplifier. Put long captures in background commands. You may work for hours.
5. **Stop and report instead of guessing** when a product decision is genuinely ambiguous, an `OWNER DECISION` is unfilled, the baseline cannot be made green within the prompt's scope, or the prompt's own stop rule fires (physical acceptance, both tracks present, a missing dependency).
6. **Report.** Write the prompt's final report to `.local/reports/<prompt>-implementation.md` (ignored by git) and repeat it in your final message. Durable findings go to `docs/` under the durability rule; nothing else does.

Review session (a second agent, usually a stronger model):

1. Read `AGENTS.md`, this file, the prompt, `.local/reports/<prompt>-implementation.md`, and the diff of every commit whose subject starts with the prompt's `todo N:` prefix.
2. Check the prompt's completion list item by item against the code, not the report. Check that tests assert the firmware's own composition rather than a re-implementation of it, that device verification actually ran (logs under `.local`), that docs changed where the prompt required and nowhere else, and that `AGENTS.md` held (no migration code, no diary docs, no scope creep).
3. Run the green baseline yourself.
4. Fix clear defects and commit them with a `todo N review:` subject. Write anything larger as an ordered fix list in `.local/reports/<prompt>-review.md` and set Status to `needs fixes`; the owner then runs an implementation session on that list. When nothing remains, set Status to `done (<hash>)` and commit.

Kickoff messages (replace the number and name):

    Implement todo/2-user-input-priority.md following "How to run" in todo/README.md; commit when done.

    Review todo/2-user-input-priority.md following "How to run" in todo/README.md; commit when done.

    Apply .local/reports/2-user-input-priority-review.md, then finish per "How to run" in todo/README.md; commit when done.

## Conventions

- **Two-track prompt.** `4-ws-1.8-multiscreen` contains an LVGL track and a hand-rolled track. The last step of `3-lvgl` rewrites it to the chosen track and deletes the other. An agent that finds both tracks present stops and reports instead of guessing.
- **Phases.** `5-new-view-model` and `6-subscription-reconciliation` are split into Phase A (portable, host tests only, no UI) and Phase B (device UI). The default is sequential in the table's order; a Phase A may run beside `3-lvgl` in a separate git worktree only when the owner asks for it, in which case work there and never merge it yourself. A Phase B never starts before its Phase A is merged and `done`.
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
