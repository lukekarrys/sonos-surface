# todo prompts

Each file in this folder is a self-contained prompt for one autonomous agent. `AGENTS.md` always applies on top of it. Prompts are ordered by intent, not strictly by filename prefix; this file is the order of record.

## Order and status

| Prompt                                   | Status                                           | Depends on                                          | May run in parallel with                    |
| ---------------------------------------- | ------------------------------------------------ | --------------------------------------------------- | ------------------------------------------- |
| `0-fsm-testing`                          | done (`dbacbc2`, `9e15b0c`, `e45dc22`, `93e48a0`) |                                                     |                                             |
| `0-device-tooling`                       | next                                             | fsm                                                 |                                             |
| `0-lvgl`                                 |                                                  | `0-device-tooling`                                  | `2` Phase A, `3` Phase A                    |
| `2-new-view-model` Phase A               |                                                  | fsm                                                 | `0-lvgl`, `3` Phase A                       |
| `3-subscription-reconcilliation` Phase A |                                                  | fsm                                                 | `0-lvgl`, `2` Phase A                       |
| `1-ws-1.8-multiscreen`                   |                                                  | `0-lvgl` decision (one track left), `0-device-tooling` |                                          |
| `2-new-view-model` Phase B               |                                                  | `1`, merged `2` Phase A                             |                                             |
| `3-subscription-reconcilliation` Phase B |                                                  | `2` Phase B, merged `3` Phase A                     |                                             |
| `4-grouping`                             |                                                  | `2` Phase B; topology subscription (done)           | `3` Phase B                                 |

Dependency graph: fsm → `0-device-tooling` → { `0-lvgl` ∥ `2A` ∥ `3A` } → `1` → `2B` → `3B` → `4`. `4-grouping` needs topology events and the pending/view model, not AVTransport/RenderingControl events, so `3B` and `4` are order-independent.

## Conventions

- **Two-track prompt.** `1-ws-1.8-multiscreen` contains an LVGL track and a hand-rolled track. The last step of `0-lvgl` rewrites it to the chosen track and deletes the other. An agent that finds both tracks present stops and reports instead of guessing.
- **Phases.** `2-new-view-model` and `3-subscription-reconcilliation` are split into Phase A (portable, host tests only, no UI) and Phase B (device UI). Phase A work runs in a separate git worktree and is merged before its Phase B starts.
- **Autonomous device verification.** Every prompt that touches the device carries a section with the exact USB steps an agent runs before asking the owner for anything. They rely on `0-device-tooling`: `node --run ports -- --identify`, `node --run usb`, `node --run ui` (`ui-touch`, `ui-button`, `ui-state`), and `monitor --until`. Injected input is never local activity and never bypasses admission, read_only, or policy.
- **Owner decisions** are marked `OWNER DECISION` inside a prompt and must be filled in before the prompt runs. Defaults follow `AGENTS.md`.
- **Read-first lists** at the top of each prompt name the files whose current behavior the prompt builds on; the facts quoted there were true when the prompt was written and must be re-verified against the tree.
- **Reports, not diaries.** Prompts end with a short report; durable findings go into `docs/` under the documentation durability rule, and everything else stays in `.local/` or the conversation.

## Owner decisions

- Decided: autonomous Sonos mutations are allowed against any room in the `rooms` allowlist of the configuration flashed to the device under test (`config/default.json` by default). The rule lives in `AGENTS.md`; `3-subscription-reconcilliation` §20 applies it.
- Outstanding, `0-lvgl` §27: adopt, reject, or adopt with a limitation, after the physical test.
- `2-new-view-model` §7: pending stays visible until the job's terminal outcome (default) or reverts after a shorter horizon.
