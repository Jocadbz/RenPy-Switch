# Switch hardware test plan

Everything below runs on a real Nintendo Switch. Nothing in this repository has
been verified on hardware by this development session until a test log is
recorded here. Host-side builds only prove that the binary was produced, not
that it runs.

## Deployment

1. Copy the packaged directory (the one containing `<id>.nro`, `package.json`,
   `licenses/` and `HARDWARE_TEST.md`) to the SD card, e.g.
   `sdmc:/switch/renpy-switch-smoke.nro` (or a folder under `sdmc:/switch/`).
2. Launch it from the homebrew menu with the **application-mode** entry
   (title override / forwarder) if available. Applet mode (album) reuses the
   host process and is not a reliable test target.
3. After the run, collect:
   - `sdmc:/renpy_switch.log` (boot, Python and engine log)
   - `sdmc:/switch/renpy/data/renpy-switch-smoke/logs/` (if it was reached)
   - `sdmc:/atmosphere/crash_reports/` — on a native crash, Atmosphere writes a
     report containing the exact module base, ESR, FAR and register dump. Send
     the newest `.log` from there along with the boot log.
   - any system error code shown on screen

Report both the log tail and the last `[boot:N]` / `[main]` line seen.

## Smoke test script (tests/fixtures/dialogue)

| # | Step | Expected |
|---|------|----------|
| 1 | Launch | Splash/progress appears within ~30 s; no error applet |
| 2 | Log | `[boot:1]` through `[boot:8]` in `renpy_switch.log` |
| 3 | Main menu | Menu renders; controller and touch both highlight buttons |
| 4 | Start | First dialogue line appears with readable text (incl. the non-ASCII test line) |
| 5 | Choice | Both branches reachable; choice text correct afterwards |
| 6 | Save | "Save" button; no crash, no error |
| 7 | Quit | "Quit to hbmenu" returns to the homebrew menu without a system error dialog |
| 8 | Relaunch | Launch again; "Load test save" restores the branch choice |
| 9 | Rollback | Rollback restores the pre-choice line; text is consistent |
| 10 | HOME | HOME button returns to the game; saves committed |

## Secondary tests

- **Sleep/resume**: sleep mid-dialogue, resume; audio (if any) and input recover.
- **Controller reconnect**: disconnect/reconnect Joy-Con during dialogue.
- **On-screen keyboard**: open `renpy.input` (not in the fixture; use a real game).
- **Audio**: any game with BGM/SFX; check pitch, no popping on loop.
- **Movies**: any game using `movie` or WebM playback; check video/audio sync.
- **Large archives**: game with >1 GiB `.rpa`; watch for memory pressure.
- **Multipersistent**: two games sharing MultiPersistent keys.

## Recording results

Append a section to this file (or the packaging report) with:

- Date, console model, firmware version, launch method (applet/application)
- The packaged `package.json` (`nro_sha256`) — identifies the exact build
- Result per step above, plus the last log line and any error code

Do not mark a compatibility claim as verified from emulator output or from a
successful host build alone.
