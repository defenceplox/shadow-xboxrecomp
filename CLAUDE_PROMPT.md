# Brief: Shadow the Hedgehog (Xbox, 2005) hangs on a looping loading screen

You are being asked for a diagnosis. Everything below is measured on a working
build, not recalled. Where a claim was later falsified I say so explicitly —
please do not build on a line marked **RETRACTED**.

---

## 1. What the software is

A native Windows C build of the title. The original 32-bit machine code has been
transcribed function-by-function into C: one C function per original routine, and
the original addresses are preserved as labels (`loc_XXXXXX`, `sub_XXXXXX`), so
addresses in this document are the title's own addresses.

Image base `0x00010000`, entry point `0x002AFB53` (`mainCRTStartup`), XDK build
5849, 8 linked libraries (XAPILIB, D3D8, DSOUND, XBOXKRNL, LIBCPMT, LIBCMT,
D3DX8, XGRAPHC). Data is at `game_files\default.xbe`; the disc tree is a
directory of **3,510 files** (2.53 GB) — corrected 2026-11-07; the earlier
"1,509 files" figure was a truncated extraction, see `CLAUDE_PROMPT_EXTRACTION.md`.

## 2. The symptom, exactly

The build boots, opens 31 disc files, renders a **loading screen with the word
"loading" legible**, then loops that screen forever. It is not a crash and not a
freeze: ~30 fps, 60 Hz vblank, `draws: 8` per frame, no faults, indefinitely
(observed to 110 s). Keyboard input produces no response.

Rendering is confirmed good — a person watching the window reads the word
"loading". So this is not a display or colour-space problem.

**Nobody knows what the loading screen is waiting for.**

> **Update 2026-11-07 (Pass 23–24 of `XBR_ATTEMPT.md`).** It is now known, and the
> screen is no longer the frontier. Two defects were stacked:
>
> 1. the disc tree was 43% extracted (a truncating XDVDFS directory walk) — fixed
>    and re-extracted, 3,510 files;
> 2. with the data complete, `DirectSoundCreate` returns `DSERR_NODRIVER`
>    (`0x88780078`) because the AC'97 codec poll never comes up, so the engine's
>    sound object stays NULL and the title calls `IDirectSound8` methods on it and
>    faults.
>
> With the tree fixed **and** `RECOMP_AC97_READY=1`, the title **sometimes renders
> the "PRESENTED BY SEGA" logo** — no loading screen, no crash — and sometimes
> drops back to the loading screen. Measured over six runs of one build: 35 disc
> opens → loading screen (3 of 6), 45 opens → past it (2 of 6), 40 → dark (1 of 6).
> The ten files between the two outcomes are the sound bank. So the loading screen
> is the **fallback drawn when the audio device does not come up in time**, and the
> remaining gate is the AC'97 bus-master submit (`sub_003EBED6` resets channel RR
> at NABM+0x0B and retries ~6,770 times in 20 s).

## 3. Boot / engine map (addresses are the title's)

| Thing | Address |
|---|---|
| Entry point | `0x002AFB53` |
| Engine main loop | `0x0011C4E`–`0x0011C6A` |
| Per-frame call site in main | `0x0011C5B` → `dispatch(cmd = 0x12)` |
| App command dispatch | `0x0011270` (cmd, arg) → pre-handler `0x00119A0` → op dispatcher `0x0011D7A0`; tables at `0x11388`/`0x11364` and `0x1D840`/`0x1D824` |
| Per-frame body ("tick") | `0x00057630` |
| Engine object pointer | `[0x00541FB4]` (heap; moves run to run: `0x014D1E90`, `0x014D0E90`) |
| Engine object's vtable | `0x00479C1C` |
| Quit flag | `0x0053550C` |

`0x00057630` decompiles to:

```c
esi = [0x00541FB4];                    /* engine object                  */
0xE53B0(esi != 0, 0x445938);           /* trace hook                     */
if ([0x00541FB8] == 0) {               /* byte gate, 0 = run             */
    ecx = [0x00541FB4]; edx = [ecx];   /* -> vtable 0x479C1C             */
    call [edx + 4];                    /* "pump"                         */
}
if ([0x0053948C] == 0) [0x0053948C] = 0x19450();
0x61FC0(this = [0x00541FA8]);
0x57270();
if ([0x00541CF4] == 0) [0x00541CF4] = 0x4F490();
0x4F670();
0x56FC0();
... 0x2BBC90(this = [0x00541FA8]) ...
```

## 4. What is confirmed working

- 60 Hz vblank, ~30 fps, `draws: 8`/frame, steady for 110 s; 5 title threads.
- 31 disc files opened and closed normally.
- All file **reads** complete: 30 reads, every one `got == want`, status `0`.
- Both one-shot inits above have run (`[0x0053948C]=0x539438`,
  `[0x00541CF4]=0x541CE8`); `[0x00541FB8]=0`, so the pump branch is taken.
- The tick is genuinely invoked every frame — a write-watch inside it shows the
  stack chain `0x11C60` (main loop) → `0x11294` (dispatch) → `0x1D80A`
  (app layer) → tick.
- A nested update `0x00021D10` is genuinely entered (observed at stack depth).

## 5. Findings, with status

| Finding | Status |
|---|---|
| `call [edx+4]` (the "pump") targets `0x254170`, which is a **bare `ret`** | **live fact** — but the vtable is `{0x2A1320, 0x254170, 0x2A1340, 0x254170, 0xE53B0}`, i.e. a base/abstract vtable. Empty slots are by design, not a bug. |
| Engine object's 64 fields never change over 8 s | **live fact**, but consistent with an object holding static tables. Not evidence of the gate. |
| `[0x00541FA8] == 0`, and `0x61FC0` is `ecx = [ecx]; if (ecx == 0) return;` — so that path short-circuits every frame | **dead end** — a full-run write-watch on `0x541FA8` produced **zero** events. Nothing in the image ever writes it. It is a never-activated subsystem, not a lost pointer. |
| `0x00541594` "stopped at 2, so the update stopped" | **RETRACTED** — watched live it oscillates `2 -> 3 -> 2 -> 3`. It is a **nesting depth**, not a progress counter. I sampled it at its resting value twice. |
| "37 frozen heap dwords" are the gate | **UNSOUND** — heap addresses are recycled, so snapshots at different times compare *different objects*. `0x01606C68` was a thread stack; `0x00FE8510` is a **freelist node** that writes its own address (`mov [0xFE8510], 0xFE8510`, chain `0x5406F8`/`0x4E5110`/`0x1B29E`/`0x535F38`). Address-based freezing over a heap cannot find state. |

## 6. Ruled out — each by measurement, not by argument

| Hypothesis | What ruled it out |
|---|---|
| A frozen hardware timer (PTIMER) stalls the fade | instrumented PTIMER reads: **0 reads** in a whole run — never touched |
| A frozen kernel clock | `KeQueryPerformanceCounter`/`InterruptTime` are monotonic host clocks; the clock ordinals never appear in the call profile |
| Pending/uncompleted async I/O | every read completed synchronously with status 0; no pending completions |
| An unimplemented system call returning 0 | only one unhandled instruction form was reached in a whole run (`wbinvd`, a no-op) |
| A missing asset | **RETRACTED 2026-11-07** — the claim was that the 12 failed opens (`d:\sound\bank0.pac`, `d:\sound\AudioData*.bin`, `d:\dvd\`) "do not exist on the master disc either (1,576 entries; no `sound/`)". They **do** exist: the disc has 3,510 files including a 50-file `sound/` directory, and the "no `sound/`" reading came from a directory walk that stopped at the first sector of every multi-sector directory. Re-extracted and A/B'd: see `CLAUDE_PROMPT_EXTRACTION.md`. Only `d:\dvd\` (×2) and the `t:\$u\contentmeta.xbx` temp-drive probe are genuine misses. |
| Audio subsystem gating the load | **NOT ruled out — it is the gate (2026-11-07).** On the truncated tree `RECOMP_NO_APU=1` changed nothing (22 files, `draws: 8`). On the **full** tree the DirectSound create returns `DSERR_NODRIVER` (`0x88780078`) and the title calls `IDirectSound8` methods on the resulting NULL object, which faults (reproducible byte-for-byte, with and without `RECOMP_NO_APU`). With `RECOMP_AC97_READY=1` the create returns `S_OK` and the sound bank loads — and over six runs of one build the outcome splits on the open count: **35 disc opens → loading screen** (3 of 6), **45 → "PRESENTED BY SEGA" logo** (2 of 6), 40 → dark (1 of 6). So the loading screen is the **fallback drawn when the audio device does not come up in time**; the remaining gate is the AC'97 bus-master submit (`sub_003EBED6` resets channel RR at NABM+0x0B, two channels, ~6,770 retries in 20 s). See Pass 23–25 of `XBR_ATTEMPT.md` and `CLAUDE_PROMPT_EXTRACTION.md`. |
| Waiting for keyboard input | input focus verified (`XSetInputFocus` confirmed on the game window) then Return/space/arrows: no change |
| Waiting for a gamepad | the pad path is incomplete (see §8) but enabling it does not change the visible behaviour |
| A vestigial async job queue is the gate | a 16-slot job table at `0x5923E0` exists with an empty dispatcher loop, but a byte-scan finds **no enqueue site anywhere** — it is dead code the title never uses |

## 7. Instrumentation available in the build

Compiled-in debug switches (names omitted deliberately; ask if you need them):
pick a memory dword and print it periodically; **poke** a value at boot;
**watch** a dword and, on every change, capture the call chain from the stack;
dump every title thread's stack; take timed memory snapshots and diff them;
trace file opens with the requesting path and result; trace kernel waits; trace
USB transactions with counters; sample host PCs.

Practical notes: run headless under Xvfb at `1024x768x24`; capture the game
window by title, never the root window (a stale crash dialog once sat on top and
produced a completely misleading OCR reading).

## 8. One incomplete subsystem, for completeness

A USB host-controller model exists but enumeration is incomplete: the title's own
driver resets the controller, sets `HcControl=0xBE`, programs a periodic list and
takes interrupts, then dies on the **first control transfer**. The transfer
descriptors decode as impossible values (`cbp=640039A7 be=89645000`, and an
endpoint descriptor whose `head=006AE875` is not 16-byte aligned), which looks
like a field-decode error on our side. Until that is fixed the title sees no
controller, so it also cannot be given input.
This is *a* gap; it is not proven to be *the* gate, because enabling it changes
nothing visible.

## 9. Method warnings — mistakes already made in this investigation

1. **A counter sampled twice is not a rate.** Two equal samples say nothing if
   the value returns to a baseline. Watch it, or sample many times in flight.
2. **A heap address is not an object.** Recycling breaks any before/after diff.
   Anchor to a pointer in static data, not to an address.
3. **Watch/trace call chains are leads, not proof.** Stale stack slots look like
   return addresses. Prefer chains with a plausible outer frame (main loop →
   dispatch → handler).
4. **Searching for literal addresses misses computed stores.** Absence of a
   `mov [addr], imm` does not prove nothing writes it; a store through a
   register or a struct field will not appear.
5. **A live renderer proves nothing about game logic.** `draws: 8` per frame and
   a visible animation coexist happily with a stalled state machine.

## 10. The question

> What is the loading screen waiting for, and what is the cheapest experiment
> that would distinguish the candidate causes?

Directions not yet taken, in the order I would try them:

1. **Watch something that changes every frame while the screen loops** — the
   opposite of what failed in §5. Its writer is live, so the captured chain lands
   in the code that is actually running; then walk down into the tick's work
   (`0x57270`, `0x4F670`, `0x56FC0`, `0x2BBC90`).
2. **The app-command layer.** In this engine a screen change is a command
   (`0x11270` → `0x1D7A0`, tables `0x11388`/`0x11364`, `0x1D840`/`0x1D824`). The
   title dispatches `cmd=0x12` every frame. If the loading screen should end by
   issuing a different command, enumerate which commands *are* dispatched and
   which never arrive — the missing one names the subsystem that should have
   sent it.
3. **Name the loading screen itself.** Its text comes from a typed animation
   asset (`csdFiles\Loading_EN.xncp`); whichever code drives that animation also
   decides when it stops.

Please answer with: (a) the most likely cause, (b) the single cheapest
experiment that would confirm or kill it, (c) what evidence would prove you
wrong. Prefer measurements over plausibility, and say plainly when something is
a guess.


