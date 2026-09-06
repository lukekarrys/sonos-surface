# Stick room/configuration checkpoint

## Current device setup

The owner chose `office`, `living-room`, and `bedroom`. Ellie's Room is intentionally
not configured. Kitchen's amp is broken; add `kitchen` explicitly after its
replacement appears in discovery. The persisted playlist rule remains
`{"living-room": true}`. Begin with `read_only=true`.

Open the monitor (close any previous monitor first):

```sh
.deps/venv/bin/python scripts/device.py monitor stick --port /dev/cu.usbmodem2101
```

Send `config-status` to inspect the persisted runtime room IDs, mode, and policy
without credentials. `rooms` logs topology plus the filtered list. `room-next`
uses exactly the same selection path as A double-click. Wait for each state
result before another gesture; busy input is rejected without later replay.

## Read-only physical test

Physical cycling and the omitted-shuffle album/playlist matrix in Office and
Living Room passed on 2026-09-06; see [recorded evidence](hardware.md#current-image-physical-nfc-policy-matrix).
Explicit-shuffle cards and intentional live playback remain pending.

1. Confirm `READ ONLY` on screen. Double-click A repeatedly: the cycle should be
   **Bedroom → Living Room → Office → Bedroom** (starting anywhere in that cycle).
   Confirm the room label and that room's playback/title/state update. Ellie's
   Room and Kitchen must never appear. Cycling must never change playback.
   Release both clicks and allow about half a second for M5 to decide the count.
   Serial should show `[button] A decided clicks=2`, then `input=room-next accepted`.
   `clicks=1` indicates a single refresh; `rejected: worker busy` means the gesture
   was recognized but not accepted. Completed notices return to Ready.
2. In Office, tap the playlist card, hold, remove, and retap. Check one accepted
   request per presentation: `roomDisplayId=office`, internal UUID, shuffle
   input absent → preserve. The static plan is logged, followed by
   `READ_ONLY_BLOCKED` at the first necessary write. There should be no music start.
3. Cycle to Living Room and tap the same playlist: omitted shuffle resolves true
   with `playlist-room-shuffle`. Tap an album in both rooms: omitted shuffle
   resolves false with `albums-in-order`. Explicit false/true cards keep explicit
   provenance. No blocked command is a playback success.
4. Select Office and reboot using USB `reboot`; confirm the preferred display ID
   restores Office and the mode still reads `READ ONLY`. Confirm physical A
   single-click still refreshes and B toggle cannot issue a write in this mode.

USB policy input can substitute for an explicit-shuffle card:

```json
{"format":"sonos-surface","version":1,"intent":{"source":{"service":"apple-music","url":"https://music.apple.com/us/playlist/example/pl.example"},"shuffle":false,"transport":"play"}}
```

The example URL is only for parsing/policy diagnostics. Use a known playable URL
for actual playback. Requests that require no write (e.g. pause when already paused)
can succeed read-only; the HTTP guard blocks effects, not observation success.

## Deliberate playback test

After the read-only checks, choose a comfortable output level and deliberately
send `read-only false` in the monitor. This saves and reboots; confirm `CONTROL`.
Update `.local/config.json` too before any later full config upload. No firmware
rebuild is needed. Only the three configured eligible rooms can receive requests.

1. Cycle and confirm room state; switching alone must stay silent. Tap album and
   playlist cards: verify the intended room plays, album shuffle is off, Living
   Room playlist shuffle is on, and non-policy rooms preserve playlist shuffle.
2. Check explicit shuffle false/true wins. Exercise comfortable absolute volume
   and small relative changes, repeat off/all/one, Play/Pause, and Next/Previous.
   Press B while playing to pause, wait for the observed result, then press B
   again to resume. Verify observed state and audible behavior agree; native
   skips never retry. B resolves against fresh state for the room selected at
   the press; unknown/transitioning state rejects rather than guessing.
3. Check source-only preserves playing versus non-playing, source+pause stays
   non-playing, and omitted volume/repeat remain preserved. Use the same known
   cards as previous checkpoints so a URL/account issue does not obscure results.
4. Send `read-only true` afterward and confirm it survives reboot. Update the
   private config file to match. Do not begin the next feature milestone.

Portable fixtures cover missing/grouped/reappearing rooms, name collisions,
renames, config changes, frozen accepted identity, and dispatch blocking. Actual
renames/grouping and subscription outage/renewal remain unmeasured physical cases;
this checkpoint does not require changing household topology to simulate them.
