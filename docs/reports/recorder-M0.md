# rec — recorder M0 report (layout, build, CLI skeleton, logging, doctor)

**Date:** 2026-10-01 · **Plan:** `Recorder_Implementation_Planv2.md` §16 M0 · **Decisions:** D-047 – D-053

## Acceptance

| Check (§16 M0) | Result | Status |
|---|---|---|
| Builds clean at `/W4` | `release`, `debug`, `x86-release`, `x86-debug`: 0 warnings, 0 errors; `rec_inject32.exe` is a 32-bit (machine 14C) binary | **met** |
| `rec doctor` runs | 14 checks on this laptop: 13 PASS, 1 WARN (disk speed, not measurable until `rec bench-disk` in M5) — output below | **met** |
| Log files created | `%LOCALAPPDATA%\rec\logs\rec.log` in the plan's format; session files tested (`rec_tests`) | **met** |

Tests: `rec_tests` 13/13 in release and debug (event registry, config round trip and validation,
size/hotkey parsing, log line format, session file contents under a 2,000-line burst, doctor);
codec `rcv_tests` still 72/72.

## What was built

- **Layout (§18):** `common/` (x86 + x64), `core/` (`rec_core`), `cli/` (`rec.exe`), `inject32/`
  (`rec_inject32.exe` skeleton), `tests/`, `third_party/` (CLI11 v2.7.2, toml++ v3.4.0, spdlog
  v1.17.0 used now; MinHook v1.3.4 and kiero2 `8f57dd9` vendored for M1).
- **Event registry:** all 45 codes of §10.4 and the text, plus 3 CLI codes (D-049).
- **Configuration:** `rec.toml` with the §17 defaults; `rec config show | set | reset`.
- **Logging:** asynchronous spdlog, per-subsystem loggers, rolling app log, console WARN+, session
  files (D-050).
- **CLI:** every §12.1 command and §12.2 option. Working: `doctor`, `config`; `launch`/`attach` parse
  and validate the recording settings; the others report the milestone that delivers them (exit 2).

## `rec doctor` on the development laptop

```
  PASS  Windows        Windows 11, build 22621
  PASS  CPU            Intel(R) Core(TM) i5-7200U CPU @ 2.50GHz, AVX2
  PASS  CPU threads    4 logical CPUs (encoder uses 2)
  PASS  Memory         15.4 GB
  PASS  GPU            Intel(R) HD Graphics 620, driver 31.0.101.2145
  PASS  Hotkey         F9 is free
  PASS  Output folder  C:\Users\Star2knb\Videos\rec (will be created)
  PASS  Filesystem     C:\ NTFS
  PASS  Free space     66 GB free (at least 35 min at 1280x720@60)
  WARN  Disk speed     not measured; 1280x720@60 needs ~33 MB/s (plan estimate; ~15 MB/s typical)
        -> measure it with rec bench-disk (arrives in recorder milestone M5)
  PASS  Power          AC power
  PASS  Game audio     per-process audio capture available
  PASS  HW encoder     Intel® Quick Sync Video H.264 Encoder MFT
  PASS  FFmpeg         C:\Users\Star2knb\AppData\Local\Microsoft\WinGet\Links\ffmpeg.exe

13 passed, 1 warning(s), 0 failed
```

## Application log sample

```
2026-10-01 18:24:48.633 [INFO ] [cli    ] I7004 command rec --config ...\rec_m0_test.toml config set hotkeys.toggle Ctrl+Shift+R (version 0.1.0 (recorder milestone M0))
2026-10-01 18:24:48.635 [INFO ] [cli    ] I7006 config_changed hotkeys.toggle = Ctrl+Shift+R
```

## Next: M1

`rec_testapp` (D3D11), injection (`launch`, `attach`, x64), kiero2 wrapper + vtable indices + pointer
validation, deferred install, MinHook `Present` hook that measures frame times, shared control block
and hook log ring, `rec list`, live status line.
