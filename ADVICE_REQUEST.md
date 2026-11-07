# State of play — static recompilation of *Shadow the Hedgehog* (Xbox, 2005)

**Written 2026-11-07 for an outside reviewer.** Everything marked *measured* came
from a run on this machine; everything marked *guess* is reasoning. If you only
read one section, read **§4 (the gate)** and **§7 (the questions)**.

---

## 1. What the project is

A native PC build of a 2005 Xbox game, produced by **static recompilation**: the
original 32-bit x86 machine code is transcribed into C, one C function per
original routine, with the original addresses preserved as symbols
(`sub_003E5A27`). There is **no emulator and no interpreter loop** — the guest's
code runs as host code, and the Xbox's kernel/HAL/GPU/audio devices are supplied
by a hand-written replacement layer that the recompiled code calls into.

The pipeline is the open-source toolkit **xboxrecomp v0.16.0** (vendored in this
repo) plus this project's own additions.

| Thing | Value |
|---|---|
| Source medium | `Shadow the Hedgehog (USA) (En,Ja,Fr,De,Es,It).iso`, 7,825,162,240 bytes |
| Filesystem | **not** ISO9660 — an XDVDFS volume at file offset `0x18300000`, behind a 387 MB DVD-Video partition |
| Disc contents | 3,510 files / 2,533,482,876 bytes / 126 directories (measured; an earlier 1,509 count was a truncated directory walk, now fixed) |
| Executable | `default.xbe`, 5,447,680 bytes, base `0x00010000`, entry `0x002AFB53` |
| Toolchain | MSVC 7.1 / XDK build 5849, Release, `/GS` off |
| Sections | 13: `.text` (at `0x00011000`, 3,927,084 bytes) + `D3D`, `DSOUND`, `D3DX`, `XGRPH`, `XPP`, `PSFD00`, `DOLBY`, `.rdata`, `.data`, `$$XTIMAGE`, `$$XSIMAGE`, `.XTLID` |
| Lifted code | **39,378 functions in 158 translation units**, 104 MB of generated C (`.text` is 3.9 MB of x86) |

### Build and run (so your advice can be concrete)

```bash
# build (cross-compile to Windows from Linux)
cd shadow_game/build_win && cmake --build . --target shadow_recomp -j4
# install beside the data
cp shadow_recomp.exe ../../shadow_recomp.exe

# run headless
Xvfb :77 -screen 0 1024x768x24 & export DISPLAY=:77
cd .. && wine shadow_recomp.exe
# useful switches (all read from the environment):
#   RECOMP_AC97_READY=1, RECOMP_AC97_TRAP=1, RECOMP_AC97_LOG=1, RECOMP_NO_APU=1,
#   RECOMP_VBLANK=paced (default), RECOMP_PB_EXEC=1 (default), RECOMP_FB_WINDOW=1 (default),
#   RECOMP_FILE_TRACE=1, RECOMP_MISSING_FILES=all, RECOMP_WATCH=<va>,
#   RECOMP_WATCHDOG_SECS=<n>, RECOMP_PC_SAMPLE=<secs>, RECOMP_PEEK=<va>,
#   RECOMP_XBR_AUDIT=1|all (ours), RECOMP_STATE_SNAP, RECOMP_STACK_DUMP
```

Paths are resolved from the exe's own folder: `game_files/default.xbe` for the
XBE and `..\WORKSPACE\files` for the extracted disc tree.

---

## 2. Where we are — measured

The title **boots, loads data, renders frames, and gets as far as the boot logo
sequence.** It has never reached the title screen or gameplay.

| Subsystem | State |
|---|---|
| CPU / instruction lift | 39,378 functions translate and run. One unhandled instruction form reached in a whole run (`wbinvd`, no-op). |
| Guest memory | 64 MB window at `0x00010000`, 48 MB heap at `0x00F80000`, MMIO apertures with fault-based traps. |
| Threads | main + **6** `PsCreateSystemThreadEx` workers (routine `0x002ADE73`), one TLS block each. |
| Kernel imports | XBE lists 378; **145 bridged, 233 unresolved** (stubs). Thunk table at `0x429AE0` (87 entries + extras); slots are synthetic VAs `0xFE000000 + i*4`. |
| File I/O | Complete. 45 successful opens in a good run; only two genuine misses (`d:\dvd\`, `t:\$u\contentmeta.xbx`). Every read completes `got == want`, status 0. |
| Rendering | Real: a **pushbuffer executor** (`RECOMP_PB_EXEC`) interprets the title's GPU command stream into a software framebuffer, presented through D3D11/SDL. 6–16 draws/frame, 1–30 FPS. `NV2A PFIFO`/`PGRAPH` proper are still stubs, and that is currently fine because the executor bypasses them. |
| Audio | **The gate.** See §4. |
| USB / gamepad | Not working: the title's OHCI driver programmes the controller then dies on the first control transfer; its transfer descriptors decode as impossible values (`cbp=640039A7 be=89645000`, an endpoint descriptor whose `head=006AE875` is not 16-byte aligned). So there is no input at all. This is *a* gap, not proven to be *the* gap. |

Two screenshots are reproduced under one build (same binary, same env):
`loading` = dark screen, red ring, the word "loading"; `logo` = **"PRESENTED BY
SEGA"**. Which one you get is a **race** (§4.2).

---

## 3. How we measure (use these; don't re-derive them)

* `RECOMP_WATCH=<va>` — hardware watch on a guest dword; prints the *guest* call
  chain that wrote it. Works on Windows/wine (VEH).
* `RECOMP_WATCHDOG_SECS=<n>` — if the title makes no progress, dump that thread's
  registers, a guest-stack walk of return addresses, recent indirect-call
  targets, and the GPU PUT/GET.
* `RECOMP_PC_SAMPLE=<secs>` — sample every guest thread's RIP and exit.
* `RECOMP_FILE_TRACE=1` — name the guest frames behind every file open.
* `RECOMP_AC97_LOG=1` (**added this session**) — log every trapped AC'97
  bus-master write: offset, 32-bit word, whether it is a channel-control byte, pc.
* `RECOMP_XBR_AUDIT=1|all` (**added this session**) — a hand-written audit module
  (`shadow_game/src/xbr_audit.c`) with call sites in the generated DSOUND code.
  It prints a call's operands, a `this -> [0x14] -> [0x8] -> [0x10]` object chain,
  and the computed `dest` of the `rep movsd` helper, with every read range-checked
  so the audit cannot fault on the pointer it is reporting. Silent unless enabled.

**Caveat we learned the hard way:** the filtered stack scan (in-range words only)
lies — it named a function as absent from a chain it was definitely in. Only
`sp[0]` (raw, the return address the caller pushed) is authoritative.

---

## 4. The gate — what we can measure, and what we cannot get past

### 4.1 Fact one: the DirectSound object is NULL because the codec says "no driver"

On the complete disc tree, the title calls the DirectSound create
(`sub_003E5A27` → `sub_003E59BA`) and it returns **`0x88780078` = `DSERR_NODRIVER`**.
An object is produced but only stored on success, so the engine's sound pointer
`[0x00550310]` stays **0** — verified with `RECOMP_WATCH=0x550310`: *zero writes in
a whole run*.

The engine does not check the HRESULT. It then calls `IDirectSound8` methods on
NULL. Traced with `RECOMP_XBR_AUDIT=all` (measured, one run):

```
E5A27.result   a0=0x88780078 (DSERR_NODRIVER)  a1=0x00FFE000 (object produced, discarded)
E35B5.entry    p1=0x00000000
E3473.entry    ecx (this) = 0x00000001     <- MEM32(0x0C), the low-memory word
E6853.entry    ecx (ctx)  = 0x00000000
EAD45.memcpy   ctx=0 size=0x818 src=0x00FBE828 off=0x6400 dest=0x00006400
CRASH          fault addr=0x758E5618 (read), eax=0x758D5604
```

So: NULL object → `MEM32(0+0xC)` (which happens to hold 1) becomes `this` → a
`rep movsd` helper computes `dest = *(*(ctx+0x10)) + off` = `0x6400` and silently
writes `0x818` bytes over guest VA `0x6400–0x6C18` (below the image base) → the
next call faults reading `0x758E5618`, ~1.8 GB outside RAM.

The runtime *already documents* this failure in `src/kernel/xbox_memory_layout.c`:
the title resets the AC'97 codec at `0xFEC0012C` and polls `0xFEC00130` bit 8 a
thousand times; on zeroed registers the bit never appears and the create fails.

**Setting that bit (`RECOMP_AC97_READY=1`) makes the create return `S_OK`** and the
whole cascade disappears: no crash.

### 4.2 Fact two: with the codec up, the title loads the sound bank — sometimes

Six runs, one build, one environment (full tree + `RECOMP_AC97_READY=1`):

| disc opens | screen | runs | FPS / draws |
|---|---|---|---|
| **35** | loading screen | 3 of 6 | 26.5 / 27.6 / 20.8, `draws: 8` |
| **45** | **"PRESENTED BY SEGA" logo** | 2 of 6 | 1.0–2.0, `draws: 16→6` |
| 40 | dark, static | 1 of 6 | 0.0, `draws: 16` |

The ten files that separate 35 from 45 are exactly the sound bank:
`d:\sound\bank0.pac`, the seven `d:\sound\AudioData*.bin`, and two
`d:\fonts\Advertise\*`. **So the loading screen is the fallback taken when the
sound-bank load does not complete in time — a race, and the open count predicts
which way it goes.**

### 4.3 Fact three: what the title waits on inside the submit

The parked thread, named by the watchdog (`+10` is innermost):

```
[WATCHDOG] guest esp=0x00F7F20C
  regs: eax=0x70 ecx=0x00F7F202 ebx=0x01282124 esi=0x806F00F4
  icalls so far: 750,482
  recent ICALL targets: FE000000 00328DB0 FE000004 00328DD0 FE00011C ...
  guest stack: +10:003E6D7B +20:003E704C +40:003E3637 +48:003E42BC +54:0025FD20
  NV2A USER PUT=GET (GPU FIFO idle)
```

All five frames are DSOUND (`sub_003E6D68` ← `sub_003E6FA8` ← `sub_003E3619` ←
`sub_003E42A4` ← engine `sub_0025FC80`). The innermost spin is in
`sub_003EBED6`, six lines of guest code:

```c
eax = MEM32(MEM32(esi) * 4 + 0x3FF128);   // per-channel base
MEM8(eax + 0xFEC0010B) = 2;               // NABM + 0x0B, bit 1 = RR (channel reset)
...
ecx = MEM8(eax + 0xFEC0010B) & 2;         // MSVC hoisted this load out of the loop
if (ecx != 0) goto spin;                  // wait for the controller to clear RR
```

`RECOMP_AC97_LOG=1` over 20 s produced 13,765 trapped writes: channels **`0x11B`**
and **`0x17B`** are re-reset ~6,770 times each, from two distinct pcs — two
threads, both in the submit path. So it is a **submit retry loop**, not one wait.

### 4.4 What we tried, and what each attempt did

| Attempt | Result |
|---|---|
| Trap the AC'97 page so RR never lands (`RECOMP_AC97_TRAP=1`) | Works exactly as designed (this code predates us and quotes the same instruction) — but **regresses**: stalls *earlier*, 15 disc opens, landing on `ebx=0x8007000E` (`E_OUTOFMEMORY`) at `+10:003EC081`. The code's own comment says "measured worse as a default", and it is. |
| Patch that one store to write `0` ("controller accepted instantly") | **Negative.** Still 45 opens, but **0 draws and no window**, and the *same* stall address `+10:003E6D7B`. With RR already clear the spin sits in the neighbouring TLS lock helper `sub_003E3250` (`[FS:0x24]`, indirect call through `0x429C3C`). Reverted. |
| `RECOMP_NO_APU=1`, `RECOMP_KICK_ACK`, clock knobs | No change, or worse. The crash is byte-identical with the emulated APU disabled, so it is not the APU model. |

### 4.5 The two things I cannot do

1. **I cannot find the timeout that decides the race.** Something gives up on the
   sound-bank load and falls back to the loading screen. If it is a guest timer we
   are starving; if it is an `NtWaitForSingleObject` with a timeout, the *waker* is
   missing. I have not located either — and every attempt to "help" the audio path
   changes the timing, which changes which outcome you get.
2. **I cannot make the submit complete** without modelling the AC'97 bus-master
   device properly: run bit (RPBM), CIV/LVI, PICB (current buffer address/length),
   a real DMA ring feeding the emulated APU sink, and the DSP command interface the
   title kicks afterwards (`0xFEC00100+`, `0xFE802008`, `0xFE802020`, doorbells).
   Answering one bit at a time does not work — measured twice (§4.4).

**Bluntly:** the title's audio init appears to be a *hard* dependency on the boot
sequence — the engine does not check the create's HRESULT and the fallback is the
loading screen — so "make audio optional" is not available to us the way it would
be for a title that tested the return code.

---

## 5. Already ruled out — please don't re-suggest these

Each of these was eliminated by measurement, not argument:

| Hypothesis | Why it is dead |
|---|---|
| Missing/corrupt game files | Disc tree re-extracted and cross-checked against an independent XDVDFS reader: 3,510 files, byte-identical to the master for all 1,509 previously-extracted files. Only 2 genuine misses (`d:\dvd\` doesn't exist; `t:\$u\contentmeta.xbx` is a temp-drive probe). |
| An unimplemented instruction | One form reached in a whole run (`wbinvd`, a no-op). |
| GPU FIFO / completion interrupts | `NV2A USER PUT == GET` at every stall observed. PFIFO/PGRAPH are stubs but the pushbuffer executor bypasses them and drawing works. |
| The emulated APU (MCPX) | Crash is byte-identical with `RECOMP_NO_APU=1`, and the submit spin is unchanged. |
| Pending/unfinished async I/O | Every read completes synchronously, `got == want`, status 0. |
| Keyboard/gamepad input being the gate | Input doesn't exist (OHCI incomplete) and enabling USB changes nothing visible; the title is not waiting on input to *boot*. |
| A dead async job queue | A 16-slot table at `0x5923E0` exists with an empty dispatcher and **no enqueue site anywhere in the image** — dead code. |
| A specific "frozen" heap dword | Address-based freeze detection over a heap is unsound (addresses are recycled); retracted earlier. |
| Rendering being wrong | Genuinely correct — a person reads "loading" and "PRESENTED BY SEGA" in the window captures. |

---

## 6. Options as I see them

| Option | Cost | What it buys |
|---|---|---|
| **A. Model the AC'97 bus-master + DSP** (channels, run bit, CIV/LVI, PICB, DMA ring into the APU sink, DSP command/doorbell handshake) | Multi-day, per-title emulation work | The submit completes instead of retrying; probably removes the race as a side effect, because the audio thread stops burning the CPU |
| **B. Find the timeout that decides the race first** | Hours, if the waiter is an `NtWaitForSingleObject` (name it from the watchdog chain, then find the waker) | Might be far cheaper than A *if* the timeout is our bug (e.g. a guest timer we under-serve) rather than a genuine audio dependency |
| **C. Make execution deterministic** (single-threaded round-robin, or virtual time with lockstep) so a race can be bisected | Days, and it changes the whole runtime | Every future bug becomes reproducible; right now every experiment is one sample. Highest value, most invasive |
| **D. Accept the wall, bank the work** | Zero | A working pipeline, a complete disc extraction, a rendered boot sequence, and a fully documented failure |

I lean **B → then A**, with **C** only if races keep defeating measurement.

---

## 7. Questions I would like advice on

1. **Is the audio device the right thing to model, or is there a third path?**
   We have two states: *no codec* → `DSERR_NODRIVER` → the engine doesn't check it
   → NULL object → wild-pointer crash; *codec "ready"* → create succeeds → submit
   loop. On real hardware `DSERR_NODRIVER` cannot happen (there is a codec). Is
   there a legitimate third answer a DirectSound HLE could give — e.g. a *valid
   device that accepts and discards audio*, with the bus-master channels always
   reporting "buffer consumed" — that lets the engine finish init without us
   implementing the DSP? If so, what must it expose?

2. **The hoisted read.** The wait in `sub_003EBED6` is `mov cl,[reg]; and cl,2;
   test/jne` with the load hoisted (the pointer is not `volatile` in the original),
   so the title reads the register **once**, a few instructions after writing it.
   "Clear it afterwards from a thread" therefore loses by construction. Is there a
   cheaper technique than trapping + single-stepping every write to that page
   (which measurably regresses) — short of making the write never land? We have an
   instruction decoder available (`platform/mmio_decode.h`), but the existing code
   deliberately avoided decoding this path.

3. **The race.** The outcome splits cleanly on the number of disc files opened
   (35 vs 45; the difference is the sound bank). What is the *right* way to
   instrument a boot-time race in a recompiler without a full deterministic
   scheduler? Available already: stack-walk watchdog, hardware write watch,
   per-thread PC sampling, file-open tracing.

4. **Are the 233 unresolved kernel imports relevant?** They are stubs returning 0.
   The audio path calls few ordinals — the hot ones in the stall are
   `RtlEnter/LeaveCriticalSection` (277/294), `NtYieldExecution` (238),
   `KeWaitForSingleObject` (159). Close the import list first, or does that code
   run fine with them stubbed?

5. **Ghidra?** The toolkit ships an optional headless Ghidra pass
   (`tools/ghidra_naming`) that recovers real names via FidDb and exports
   decompiled C. Worth installing (it needs a JDK) for name recovery and readable
   decompilation of the DSOUND functions, or a distraction versus A/B/C?

6. **Is the reasoning in §4 sound?** If you think the NULL-object path is the real
   bug to fix first — rather than the submit loop — say so. We treated
   `DSERR_NODRIVER` as "our codec model is missing" because the runtime's own notes
   say that, but we have not proven the title *cannot* run with a failed create on
   hardware.

---

## 8. Appendix — reproduction

```bash
# 1. extract the disc (idempotent, 2.5 GB)
cd /mnt/docker/sites/shadow
python3 tools/xdvdfs.py extract --quiet --out WORKSPACE/files --manifest WORKSPACE/MANIFEST.tsv

# 2. build
cd xboxrecomp/shadow_game/build_win && cmake --build . --target shadow_recomp -j4
cp shadow_recomp.exe ../../shadow_recomp.exe

# 3. run headless with everything we know switched on
Xvfb :77 -screen 0 1024x768x24 &
export DISPLAY=:77 WINEDEBUG=-all
cd /mnt/docker/sites/shadow/xboxrecomp
RECOMP_FILE_TRACE=1 RECOMP_MISSING_FILES=all RECOMP_WATCHDOG_SECS=12 \
RECOMP_AC97_READY=1 wine shadow_recomp.exe > /tmp/run.log 2>&1
# capture the window BY TITLE, never the root window (a stale dialog once lied):
W=$(DISPLAY=:77 xdotool search --onlyvisible --name Shadow | head -1)
DISPLAY=:77 xdotool getwindowname "$W"; DISPLAY=:77 import -window "$W" /tmp/shot.png
```

**Where to look first**

| Path | What |
|---|---|
| `xboxrecomp/XBR_ATTEMPT.md` | 25 passes of history, with what each ruled out |
| `xboxrecomp/CLAUDE_PROMPT.md` | the original diagnosis brief (§5 findings, §6 ruled out, §9 method warnings) |
| `xboxrecomp/CLAUDE_PROMPT_EXTRACTION.md` | the disc-extraction bug and its A/B |
| `shadow_game/src/xbr_audit.c` | our audit module (sites marked `XBR-AUDIT` in generated code) |
| `src/kernel/xbox_memory_layout.c` | AC'97 trap, MCPX/APU apertures, the watchdog |
| `shadow_game/src/recomp/gen/recomp_0153.c` | the DSOUND functions named above |
| `WORKSPACE/reports/ab-2026-11-07/` | every log and screenshot referenced here (pass24/, pass25/) |

**Numbers to sanity-check any suggestion against:** 39,378 lifted functions;
145/378 kernel imports bridged; 45 disc opens maximum; 6 worker threads + main;
`draws 6–16` per frame; the two screens are `loading` (35 opens, `draws 8`) and
`logo` (45 opens, `draws 16→6`).



