# Progress table — Shadow the Hedgehog static recompilation

**Snapshot: 2026-11-07 (end of Pass 26).** Full narrative in `XBR_ATTEMPT.md`;
method warnings in `CLAUDE_PROMPT.md` §9. Everything marked ✅/🔴/🟡 is
**measured** — the evidence column *is* the measurement.

---

## 1. Assets and build

| Item | Value | State |
|---|---|---|
| Source ISO | 7,825,162,240 bytes; XDVDFS at `0x18300000` behind a 387 MB DVD-Video partition | ✅ |
| Disc tree | **3,510 files / 2,533,482,876 bytes / 126 directories** | ✅ two independent readers agree |
| Old truncated tree | 1,509 files / 1,625,596,064 bytes (`WORKSPACE/files_trunc`) | ✅ kept for A/B |
| XBE | 5,447,680 bytes, base `0x00010000`, entry `0x002AFB53`, 13/13 sections load | ✅ |
| Lifted code | **39,378 functions / 158 translation units / 104 MB** of generated C | ✅ compiles clean |
| Executable | `shadow_recomp.exe`, 27,993,470 bytes (x86-64, mingw-w64, run under wine 10) | ✅ matches build |
| Original toolchain | MSVC 7.1 / XDK 5849, Release, `/GS` off | ✅ |

## 2. Subsystem status

| Subsystem | State | Evidence | Next |
|---|---|---|---|
| CPU / instruction lift | ✅ | 39,378 functions run; one unhandled instruction form in a whole run (`wbinvd`, no-op) | — |
| Guest memory | ✅ | 64 MB @ `0x10000`, 48 MB heap @ `0xF80000`, MMIO traps, aliases at `0x80000000`/`0xF0000000` | — |
| Threads | ✅ | main + **6** `PsCreateSystemThreadEx` workers (`0x002ADE73`), TLS each | — |
| File I/O | ✅ | 45 opens max; only 2 genuine misses (`d:\dvd\`, `t:\$u\contentmeta.xbx`); all reads `got == want`, st 0 | — |
| Rendering | ✅ | software **pushbuffer executor** → D3D11; 6–16 draws/frame; 1–30 FPS; text legible | — |
| Boot sequence | 🟡 | reaches **loading screen** or **"PRESENTED BY SEGA"** — a race (35 opens→loading ×3, 45→logo ×2, 40→dark ×1 of 6 runs) | §3 gate 2 |
| Audio: device create | 🔴 | `DirectSoundCreate` → **`0x88780078 DSERR_NODRIVER`**; engine ignores it → `[0x550310]` stays 0 (watch: 0 writes) → `IDirectSound8` on NULL → fault at `0x758E5618` | codec stub or device model |
| Audio: submit loop | 🟢 not the wall | `sub_003EBED6` completes **every call, ~46×/s** (marks m0…m3 equal); `sub_003E6D68` **never runs** when stalled | — |
| Main stall | 🔴 unnamed | lock-poll on guest lock **`0x5A0320`**, ~**33k** lock/unlock pairs/s (`sub_00328DB0`/`sub_00328DD0`); both threads acquire | find the poll's owner + condition |
| Input (USB/OHCI) | 🔴 | enumeration dies on the first control transfer; TDs decode impossible (`cbp=640039A7`, `head=006AE875` unaligned) | needed to **play**, not to boot |
| PFIFO / PGRAPH | 🟡 stubs, bypassed | `USER PUT == GET` at every stall seen | later |
| Kernel imports | 🟡 | 378 listed: **145 bridged, 233 stubs**; hot path uses only bridged ordinals | log & rank only |
| Ghidra naming | 🟡 blocked | Ghidra 12.2 + JDK 25 installed; analysis runs; **vendored export script is Jython (Py2), unsupported in 12.2** | port to Java/PyGhidra |

## 3. Gates, ranked

| # | Gate | Blocks | Cost | Cheapest next measurement |
|---|---|---|---|---|
| 1 | Audio device create (`DSERR_NODRIVER`) | anything past first render | small (switch) / medium (stub) | done — `RECOMP_AC97_READY=1` |
| 2 | **Loading state machine never completes** | boot sequence completing | unknown | **named**: `NowLoadingTask` (ctor `0x598D0`, vtable `0x445B68`, live at `[0x544F40]`) → `SetFinished` `sub_00059890` → app-state change `sub_00061FF0(5)`; driver `sub_00025810` polls `sub_002ADB31` (worker block `0x2ADxxx`) and **sees only states 0 and 2, forever**; progress `[0x541814]` stays **0** (should reach `0x64`) |
| 3 | Gamepad (OHCI) | playability | medium | fix TD/ED decode against a known-good OHCI |
| 4 | Audio *output* | nothing that matters yet | days | skip until 1–3 |

## 4. Ruled out — do not re-suggest

| Hypothesis | Killed by |
|---|---|
| Missing/corrupt disc files | 3,510 re-extracted; 1,509 old rows sha256-identical; 2 genuine misses |
| An unimplemented instruction | only `wbinvd` reached, a no-op |
| GPU FIFO / completion interrupt | `PUT == GET` at every stall |
| The emulated APU (MCPX) | crash byte-identical with `RECOMP_NO_APU=1` |
| Async I/O never completing | every read synchronous, `got == want`, st 0 |
| "Waiting for input" | input doesn't exist; enabling USB changes nothing |
| Dead job queue (`0x5923E0`) | 16 slots, empty dispatcher, **no enqueue site in the image** |
| A frozen heap dword | heap addresses are recycled — unsound method, retracted |
| **A `KeWaitForSingleObject` timeout** | **0 timeouts** in 7 runs, both outcomes (wrapper does return `STATUS_TIMEOUT`) |
| **Lock starvation / deadlock on `0x5A0320`** | both threads acquire it; 24 contention events, none stuck |
| **"Parked in the DSOUND submit"** | submit completes 46×/s; `sub_003E6D68` never runs — that chain was **stale stack slots** |
| Stack-scan chains generally | only `sp[0]` is real; the filtered scan named a function that never ran |

## 5. Recent passes (what each settled)

| Pass | Settled |
|---|---|
| 21 | disc has no `sound/`, no `dvd/` — **retracted in 23** |
| 22 | engine object inert; heap-freeze method unsound |
| 23 | **extractor bug**: 43% of the disc never extracted; fixed; 1,509 → 3,510 files; A/B |
| 24 | crash named: `DSERR_NODRIVER` → NULL object → `0x758E5618`; codec-ready flips create to `S_OK` |
| 25 | outcome is a **race** (35 vs 45 opens); RR / the trap are not the gate |
| 26 | submit **not stuck** (46×/s, all segments); real spinner is the **`0x5A0320` lock poll**; audit alias bug fixed |

## 6. Tooling available

| Tool | What it gives |
|---|---|
| `RECOMP_WATCH=<va>` | hardware watch; names the guest writer chain |
| `RECOMP_WATCHDOG_SECS=<n>` | registers + counters at a stall (**counters yes, scanned chain no**) |
| `RECOMP_FILE_TRACE=1` | guest frames behind each file open |
| `RECOMP_AC97_LOG=1` | every trapped AC'97 bus-master write (offset, word, pc) |
| `RECOMP_XBR_AUDIT=1\|all` | call operands, object chains, `xbr_mark` segment counters |
| `RECOMP_WAIT_TRACE=1` | waits **and outcomes** (requested timeout in ms, elapsed, result) |
| `RECOMP_DATA_DIR=<path>` | pick the disc tree at run time (A/B without renaming) |
| `run_logo.bat` / `run_loading.bat` | one click: codec-ready run / old loading-screen run |

## 7. Known debt and risks

- **Generated-code edits are not durable** — 12 `XBR-AUDIT`/`XBR-MARK` sites live in
  `recomp_0152.c`/`recomp_0153.c`; `tools/xlate.py` regeneration loses them.
- **Three copies of the exe** must stay in sync (`xboxrecomp/`, `shadow_game/`, `build_win/`).
- **Two disc trees = 4 GB.**
- **Runs are non-deterministic** — repeat runs before believing anything; the open count
  (35/45) is the cheapest correlate.
- **Instruments can lie** — the audit misclassified the `0x80000000` alias and the
  watchdog's stack scan named a function that never ran. Check the instrument first.

## 8. One-line summary

> A complete, verified disc extraction and a 39,378-function lift that boots, loads,
> renders and reaches the SEGA logo. It stops because the title calls DirectSound and
> does not check the result, and because the TaskManager's queue poll never sees its
> condition become true. Audio output and gamepad input are open, secondary.

## 9. Engine architecture, recovered from strings (2026-11-07)

The Ghidra RTTI/string pass surfaced the title's own class-name and assert strings,
which name the architecture the stall lives in:

```
NowLoadingTask                     TaskManager::GetTaskRoot : Null Pointer Exception
ResourceManagerTask                FadeTask / FadeDrawTask
TStageInitalizeTask                DeltaMorphAnimManagerTask
Task::exist_child
```

So the engine is a **Task / TaskManager** design: the loading screen is a
`NowLoadingTask`, tasks are owned by a `TaskManager`, and `FadeTask`/`FadeDrawTask`
handle the transitions. The anonymous `0x32xxxx` "queue framework" that burns 11k
lock operations a second is therefore almost certainly the TaskManager's queue, not
a game-specific loader — and the thing that must complete to leave the loading
screen is a **task**, not a file.

Caveat: the strings are not referenced by literal address in the lifted code, so the
referencing code has to be found another way (computed table base, or by locating the
TaskManager object and walking its vtable).


