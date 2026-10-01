# rec — recorder M1 report (test app, injection, Present hook, rec list)

**Date:** 2026-10-01 · **Plan:** `Recorder_Implementation_Planv2.md` §16 M1 · **Decisions:** D-054 – D-064

M1 puts the hook into a game and measures; nothing is captured yet.

## Acceptance

| Check (§16 M1) | Result | Status |
|---|---|---|
| Attach/detach 100× without a crash | `tests\m1_attach_detach.ps1`: 100 cycles of `rec attach --duration 1` against `rec_testapp` (D3D11, vsync). Game alive after every cycle, hook DLL unloaded after every cycle, log has 100 "hooks installed" and 100 "detached". Run twice (before and after the takeover fix below), both pass | **met** |
| `attach` hooks successfully | above, and `rec attach` on a game that uses `Present1` (`--present1`) | **met** |
| `launch` hooks successfully, `I1103` seen | `rec launch rec_testapp.exe -- --late-load-ms 1500`: hook injected into the suspended process, `I1103 hook_install_deferred`, hooks installed 2 s later when the game loaded D3D11, `I1101 backend_selected D3D11 1280x720` | **met** |
| Game fps visible | live line, e.g. `○ IDLE rec_testapp.exe D3D11 1280x720 \| game 60.1 fps \| hook 1.1 us (max 2.0)` | **met** |
| Hook log records arrive in the host log | `[hook   ] I1101 backend_selected …`, `E1401 host_lost …`, `I1103 …` in `%LOCALAPPDATA%\rec\logs\rec.log` | **met** |

Tests: `rec_tests` 21/21 in release and debug (new: protocol names, log ring order/wrap/overflow and
4 producers × 20,000 records with nothing lost or duplicated, host link, graphics module
classification, process lookup, anti-cheat matching); `tests\m1_robustness.ps1` 21/21 checks
(below); codec `rcv_tests` unchanged. All four configurations build at `/W4` with 0 warnings
(`release`, `debug`, `x86-release`; `rec_hook32.dll` compiles and the shared-memory layout checks
hold on x86, but the 32-bit hook has not been run).

## Other checks (`tests\m1_robustness.ps1`)

1. A process that never loads Direct3D (`ping.exe`): attach succeeds, `I1103` logged, clean detach, DLL unloaded.
2. Host killed (hard) while attached: the game keeps running, the hook stays loaded and logs `E1401 host_lost`
   after 3 s; a new `rec attach` takes over the existing hook and measures frames; `rec detach`
   (no arguments) finds the hook and removes it.
3. `Present1` path measured.
4. Anti-cheat: a game folder containing `EasyAntiCheat_x64.dll`: `rec launch` refuses (E1004, exit 1), names the file,
   starts nothing; `--force` goes ahead with a warning.
5. Launch of a game that loads Direct3D late (as above).

## Measurements (i5-7200U laptop, AC power, no game running)

`rec_testapp` unthrottled (no vsync, only `ClearView` rectangles), 1280×720, attached for 15 s:

| Metric | Result | Plan target |
|---|---|---|
| Hook cost per Present (entry to just before the original call, includes two QPC reads) | **0.2 – 0.4 µs average**; worst single call in a second 2 – 4 µs, once 28 µs | P1: < 5 µs (met on average; the occasional 28 µs outlier is a descheduled thread, not the code) |
| Game frame rate, hook not attached / attached | ~1,750 fps / ~1,750 fps (per-second samples 1,460 – 2,115 in both; no difference visible) | – |
| Address lookup (kiero2 `locate`) | 18 – 70 ms warm, 143 – 693 ms the first time in a process (driver load) | "typically tens of ms" |

These are one machine, one synthetic scene; nothing here says how a real game behaves.

## Bugs found and fixed while testing

- **Takeover failed after the host died.** The hook closed its mapping handles after mapping; a named
  object loses its name when the last handle closes, so a new `rec` created fresh, unrelated objects
  and never saw the hook ("state: not loaded"). The hook now keeps its handles (Globals::ctl_map /
  log_map). The takeover and `rec detach` checks cover it.
- **Anti-cheat false positive.** Substring matching flagged `WindscribeService.exe` for "BEService".
  Matching now requires the entry at the start of a name or after a non-alphanumeric character (D-061);
  covered by a test.

## Known limits

- **Not tried on a real game.** Everything above is the synthetic test app, a console process and
  ping. Minecraft Bedrock (the planned test target) has not been hooked. Games that start through a
  launcher which spawns the real game need `rec attach` on the game, not `rec launch`.
- **Paths written but not exercised:** `W1106` (vtable pointer outside `dxgi.dll`), `E1105` (kiero2
  lookup failure) and kiero2's debug output going into the ring need a way to force a failure (the plan's
  `--debug-kiero-fail`, M5); `E1003` (elevated target) needs an elevated game; the 3-second anti-cheat
  rescan after launch has only run against clean games; `E1107` (exception in the guard) has no trigger yet
  (`--debug-hook-throw`, M5).
- **Detach has a theoretical window.** After the in-hook counter reaches zero a thread still has to run
  the detour's last instructions, so the DLL waits 250 ms twice before unloading (D-059). The 100 cycles
  don't show a problem, but this is a timing argument, not a proof. A game with a thread stuck in
  Present keeps the DLL loaded.
- **D3D10-only games** (no `d3d11.dll`) are not hooked (D-056). 32-bit games are refused until M7.
- **The anti-cheat scan is system-wide, as the plan says:** a PC with an anti-cheat driver or service
  running (e.g. Vanguard) needs `--force` for every game. A signed/protected game that blocks unsigned DLLs
  gets "LoadLibrary failed inside the target"; `rec` does not try to get around that.

## Next: M2

D3D11 capture: copy + scale + NV12 shader + staging ring + shared frame ring, hotkey thread (F9) and
audio cue, session state machine, host receives frames and checksums them. Acceptance: P1 and P2
measured, F9 start/stop in fullscreen, `W1201` rare.
