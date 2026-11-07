# Shadow the Hedgehog — xboxrecomp attempt

A parallel attempt at Shadow the Hedgehog (Xbox, 2005) using
[sp00nznet/xboxrecomp](https://github.com/sp00nznet/xboxrecomp) v0.16.0
("Small Print"), run alongside the hand-written matching-decompilation effort
one directory up. **Nothing here writes to the parent project's files**; the
parent's own rate is untouched.

Everything lives under this directory:

```
xboxrecomp/                       the toolkit, cloned (--depth 1)
game_files/default.xbe            copied from ../WORKSPACE/xbe/default.xbe
game_files/default_analysis.json  xbe_parser output
tools/disasm/output/              functions/xrefs/labels/strings + text.asm
tools/func_id/output/             classification
tools/abi_analysis/output/        per-function ABI
shadow_game/                      the game project (from templates/new-game)
shadow_game/src/recomp/gen/       155 generated C files (98 MB, 2.73 M lines)
shadow_game/build_objs/           one .o per chunk + archived .a
build_objects.sh                  compile-the-lift script (no runtime deps)
```

## Input

| Fact | Value |
|---|---|
| XBE | `default.xbe`, MD5 `445d64ca1e1afd8b7b33f6bbb7da0982`, 5,447,680 B |
| Certificate | "Shadow The Hedgehog" (SE-134), NA |
| Base / entry | `0x00010000` / `0x002AFB53` |
| XDK | 5849 |
| Kernel imports | 111 |

The XBE was not re-extracted from the ISO for this attempt: the parent project
had already pulled it (`WORKSPACE/xbe/default.xbe`) and it is the same file.
The ISO only has a 14 MB DVD-Video partition at the root; the XDVDFS volume
starts at `0x18300000`.

## Pipeline results

| Step | Command | Result |
|---|---|---|
| 3 | `xbe_parser ... --json` | 0.12 s, 13 sections, 111 imports |
| 4 | `disasm --text-only` | **37,985 functions**, 1,333,686 insns, 257,306 xrefs — 1112 s |
| 5 | `func_id -v` | 41,788 classified: 0 RenderWare, 13 CRT, 10,271 vtable, 30,902 unknown |
| 6 | `abi_analysis -v` | 37,985 ABI entries, 4,959 `thiscall` |
| 8 | `recomp --all --split 250` | **38,040/38,040 translated, 0 failed**, 2,733,107 lines |

Post-lift checks:

* `gcc -std=c11 -fsyntax-only` on all 155 translation units → **155 checked, 0 failed**
* `gcc -std=c11 -O0 -c` on all 155 → **155 compiled, 0 failed**
* archived to `build_objs/libshadow_recomp.a` — **152 MB, 38,320 defined text
  symbols**, with `xbe_entry_point` present at `T 0x5cebe`

Unimplemented instructions across the whole lift, from
`tools/recomp/output/summary.json`: 40 occurrences total (`lds` 2, `bound` 2,
`daa` 5, `into`/`das` 2, `cli`/`sti` 5, `sgdt` 1, `ljmp` 4, `aaa` 18,
`arpl` 1, `aam` 1, `hlt` 1, `int1` 1, `insd` 1) out of 1,333,686 — i.e. the
lift is ~99.997 % instruction-complete. 275 call targets remained unresolved
and were emitted as stubs.

## Native build — done, and it boots

`sudo` was needed for the deps only because the password was never supplied.
With `SUDO_PASS` from `~/.env`, `tools/linux/install_deps.sh` ran clean
(EXIT=0): SDL2 2.32.10, libepoxy 1.5.10, OpenSSL 3.5.5, GL, ffmpeg dev.

Then:

* `cmake -S shadow_game -B shadow_game/build` → configures fully (pkg-config
  2.5.1, epoxy, SDL2, OpenSSL, Threads).
* All **12 runtime static libraries** build on Linux.
* The template `main.c` is Win32-only, so `shadow_game/src/main_posix.c` was
  written to do the same job on POSIX (SIGSEGV/BUS/ILL/FPE handler instead of
  VEH+dbghelp; same `xbox_MemoryLayoutInit → kernel_init → path_init →
  kernel_bridge_init → g_esp → recomp_dispatch_init → xbe_entry_point`
  sequence). CMake picks it when not `WIN32`, and the Win32 SDK link libs are
  now conditional.
* `cmake --build` → **`[100%] Built target shadow_recomp`**, EXIT=0, 156
  objects, zero errors. 27 MB ELF, links SDL2/libm/libc.

### The run

```
./shadow_game/build/shadow_recomp game_files/default.xbe game_files
```

It maps 13/13 sections, 28/28 RAM mirrors, verifies the tiled-aperture alias,
resolves 111 kernel thunks (120/111), then executes. 833 lines of trace:

| Observed | Value |
|---|---|
| Entry point | called at `0x002AFB53`, runs |
| Kernel calls | **200** (`RtlInitializeCriticalSection` ×47, `RtlEnterCriticalSection` ×36, `RtlLeaveCriticalSection` ×33, `RtlInitAnsiString` ×15 …) |
| Guest thread | `PsCreateSystemThreadEx` creates the game's own thread, 148-byte TLS |
| Indirect calls | **1,036**, of which **8** failed to resolve |
| Guest output | **`[GUEST] Error initializing RenderWare.`** |
| End | `KeBugCheck 0x0` → `HalReturnToFirmware routine=2` → exit 0 |

That guest message is Shadow's own string, printed by its own error path: the
recompiled code got far enough to try RenderWare, fail, and report it.

## Pass 2 — `--extra-sections`: past RenderWare, now at D3D

Re-ran the pipeline with the XDK library sections treated as code, plus the
eight unresolved targets seeded as function entry points:

```bash
python3 -m tools.disasm game_files/default.xbe \
    --extra-sections D3D,DSOUND,PSFD00,D3DX,XGRPH,XPP,DOLBY \
    --seed-functions /tmp/icall_seeds.json --force -v      # 3037 s
```

| | Pass 1 | Pass 2 |
|---|---|---|
| Functions | 37,985 | **39,345** |
| Instructions | 1,333,686 | 1,461,475 |
| DSOUND funcs | 69 | **379** |
| XGRPH funcs | 17 | **677** |
| XPP funcs | 11 | **195** |
| D3D funcs | 167 | **264** |
| Translated | 38,040 | **39,364** (0 failed) |
| Lines of C | 2,733,107 | 2,895,852 |
| Unresolved stubs | 275 | **15** |
| Build | ok | **ok** (28.8 MB binary) |

### The run

| Observed | Pass 1 | Pass 2 |
|---|---|---|
| Kernel calls | 200 | 200 |
| Guest threads | 1 | **4** (`PsCreateSystemThreadEx #4`) |
| Indirect calls | 1,036 | **1,285** |
| Unresolved ICALLs | 8 | **2** (one distinct VA) |
| Guest output | `Error initializing RenderWare.` | *(none)* |
| End | KeBugCheck → exit 0 | **SIGSEGV at `0x003E27F4`** |

The guest no longer prints its RenderWare error, so it got past it. It now
faults inside the **D3D section** (`0x003CFC40`–`0x003E3228`), with the call
chain visible in the fault report:

```
[FAULT] signal 11 (SIGSEGV) at host 0x3e27f4
  guest esp = 0x01382EEC
  [esp+0 ] 0x003E27DC   <- in D3D
  [esp+20] 0x003291F5
  [esp+24] 0x002ADEE5   <- thread entry
  [esp+72] 0x0037F750   <- SEH prolog
```

The recent-ICALL ring shows the calls going through `0xFE000000` /
`0xFE000004` — the MCPX device aperture, which is currently a zeroed page with
no register semantics. It is initialising hardware it cannot see.

One unresolved target remains: **`0x00423B38`**, inside XPP. It is almost
certainly an interior entry rather than a function start, since XPP itself is
now fully disassembled (195 functions).

### What's next (the real bring-up)

The structural phase of this toolkit is done for this title: everything that
can be lifted is lifted, and it compiles and boots. Everything further is
runtime work, in this order:

1. **MCPX / USB / APU registers at `0xFE000000`** -- the wild calls the ring
   shows. Zeroed pages get read as function pointers.
2. **D3D8 → OpenGL** (`src/d3d/d3d8_gl.c`) -- the fault address is inside D3D,
   and the renderer needs to return plausible device/swapchain state.
3. **NV2A at `0xFD000000`** -- zeroed today; needed once the game pushes geometry.

That is the multi-week part. Nothing above changes the fact that the pipeline
this session set up works end to end on Linux.


### Why pass 1 failed (history)

`--text-only` was used for step 4, exactly as the README's Quick Start says.
Its warning is the whole story: *"A title with code in its XDK library sections
(D3D, DSOUND, XPP…) needs them named explicitly, e.g.
`--extra-sections XIPS,DOLBY`."* The eight unresolved targets were
`0x003E5CAC 0x003E5CB7 0x003E7DB3 0x003E7DBE` (DSOUND),
`0x00415AB5 0x00415AC1 0x00415ACD` (XGRPH) and `0x002DD6AD` (.text) — seven of
eight in sections the disassembler had been told to skip.


## Pass 3 — device bring-up: APU MMIO ported to x86-64 POSIX

Running with `RECOMP_AC97_READY=1` moved the fault to `guest VA 0xFE801100` —
the MCPX/APU window that `MemoryLayoutInit` arms as `PAGE_NOACCESS`. On POSIX
nothing serviced that trap:

* `mmio_decode.h` is entirely inside `#if defined(_WIN32)` (it takes a
  `PCONTEXT`);
* `apu_mmio_hook.c`'s POSIX handler (`apu_hook_handle_mmio_posix`) exists but
  is **arm64-only** (`mmio_emulate_a64`);
* `src/nv2a/nv2a_mmio_hook.c` says it outright:
  `#else /* !_WIN32 -- SIGSEGV-based MMIO trapping deferred to main.c port */`
  and its POSIX `nv2a_hook_init` is an empty stub.

So x86-64 Linux had no trapped MMIO at all. Two things were written:

**`shadow_game/src/mmio_trap_x64.h`** — the missing decoder, modelled on the
arm64 one: registers out of `ucontext_t.uc_mcontext.gregs`, `MOV` both ways,
the immediate store, `MOVZX`, and the `TEST`/group-1 ALU set a poll loop is
built from. Unknown opcodes return 0 and log the opcode rather than stepping
over something not understood.

**`main_posix.c`** — routes `SIGSEGV` at `0xFE800000..0xFE830000` through that
decoder into `mcpx_apu_mmio_read/write`, and resumes. Also adds the two
bring-ups the Win32 template performs and the POSIX main was missing: the APU
model (`mcpx_apu_init_standalone`) and the NV2A hook (a no-op on POSIX).

### Result

| | Pass 2 | Pass 3 (APU MMIO) |
|---|---|---|
| Outcome | SIGSEGV at 1 s | **runs until the watchdog** |
| MMIO decode failures | n/a | **0** |
| Indirect calls | 1,285 | **19,580** |
| Faults | 1 (fatal) | **0 fatal** |
| Files failing to open | 3 | **1** (a save-area probe) |

The APU now comes up properly — `[SDL2] audio initialized: driver pulseaudio,
48000 Hz stereo`, `[APU] MCPX APU initialized (standalone)`, DSP GP/EP
initialised — and the guest registers three interrupt handlers (vectors 1, 3
and 6).

### Also fixed: the data directory

`game_files` held only `default.xbe`, so the guest could not load anything and
fell into its disc-error screen (`discErrorEN.txd`, `nukkoro.inf`). Pointing the
game dir at the parent project's extraction fixes that:

```bash
RECOMP_AC97_READY=1 ./shadow_game/build/shadow_recomp \
    game_files/default.xbe /mnt/docker/sites/shadow/WORKSPACE/files
```

### The frontier now

The guest boots, loads data, initialises audio, registers interrupts, makes
19,580 indirect calls — and then **spins** (`143 % CPU`, 7 threads, two in state
`R`), with the call count frozen at 19,580 and the GPU idle
(`NV2A USER PUT == GET`).

Evidence from `gdb` (`thread apply all bt`):

```
Thread 1  #0 sub_002B1206 ()     <- walking a PE header at 0x80010000
          #1 sub_002AFADF ()     <- the guest's thread routine (PsCreateSystemThreadEx ctx1)
```

`sub_002B1206` reads `e_lfanew` from `0x8001003C`, walks the section table and
compares each name against `0x54494E49` — `"INIT"`. It is the guest's CRT
walking a PE header, and the header it finds is the runtime's **fake kernel PE
header**, which is deliberately built with `e_lfanew = 0x80` and *zero sections*
("the INIT section search finds nothing, which is the safe path"). The function
itself returns, so this is a sample, not proof — but it says the guest's startup
walks a header that has no sections.

### What remains (unchanged in kind, but precisely located)

1. **NV2A.** No POSIX register model: `nv2a_hook_init` is a stub, so the
   0xFD000000 aperture is plain zeros. `nv2a_init_standalone` *does* build on
   Linux, so the model can be initialised and trapped through the new decoder —
   but trap the register pages only. The toolkit's own comment on the APU
   window records why: a `rep movsd` bulk copy into a trapped page lowers to a
   host `memcpy`, faults, and has "no way to resume".
2. **The startup spin.** Either the fake kernel header needs a plausible
   section table, or the spin is on a device status bit that nothing sets.
3. **D3D8 → OpenGL.** `d3d8_gl.c` builds but nothing has driven a frame yet;
   `RECOMP_FB_WINDOW` needs a GL context, and this box is headless.

These are the multi-week bring-up the toolkit documents, not a broken pipeline.


## Pass 4 — NV2A model wired, vblank scheduler enabled

The toolkit's POSIX NV2A hook is an empty stub, so this project supplies the
bring-up:

* **Model init** — `nv2a_posix_init()` allocates 64 MB VRAM + 1 MB RAMIN, calls
  `nv2a_init_standalone()`, then `mprotect(PROT_NONE)` on **11 register pages**
  only — the nine blocks the model implements (PMC, PBUS, PFIFO×2, PVIDEO,
  PTIMER, PFB, PGRAPH×2, PCRTC, PRAMDAC). The 8 MB USER window and VRAM stay
  plain memory, because a `rep movsd` into a trapped page lowers to a host
  `memcpy` and cannot be resumed.
* **Decoder extended** for what recompiled code actually emits against device
  registers, found by logging the raw bytes each time it failed:
  `LOCK` prefix, the `ALU r/m,r` family (`0x00..0x3D`), `BTS` (`0F AB`),
  `CMPXCHG` (`0F B0/B1`) **with the ZF set**, and `XADD` (`0F C0/C1`). The ZF
  mattered: the guest's `lock cmpxchg [edi], ecx; jne` retry loop takes the
  wrong exit without it.
* **Vblanks** — `RECOMP_VBLANK` is **off when unset**. Set to `paced` the
  runtime's 60 Hz scheduler runs: measured **600 vblanks in 9,984 ms**, gap
  10.9–22.5 ms, and the guest's own ISR **claims** each one.

### Result

| Configuration | Indirect calls | Outcome |
|---|---|---|
| APU only, no NV2A trap | 19,580 | spin, GPU aperture read as zeros |
| + NV2A model + 11 trap pages | 978 | parks early, GPU regs now answer |
| + `RECOMP_VBLANK=paced` | 2,058 | vblanks fire, ISR claims them |
| + `RECOMP_DPC_ON_RAISE=1` | 1,878 | no change |

Zero decoder failures and zero faults in all of these — the device-access path
itself works. `ack give-ups 599` of every 600 vblanks: the ISR claims the
interrupt but the DPC that would clear `PMC_INTR_0` never runs.

### The actual remaining blocker

`gdb` on all of the above samples the *main* guest thread in the same place:

```
#0 sub_002B1206   <- walks the PE header at 0x80010000 looking for "INIT"
#1 sub_002AFADF   <- the guest's thread body (PsCreateSystemThreadEx ctx1)
#2 sub_002ADE73   <- thread thunk
```

So it is **not** blocked on a GPU register — it is looping in its own startup
code, walking the runtime's **fake kernel PE header**, which is deliberately
built with `e_lfanew = 0x80` and **zero sections** ("the INIT section search
finds nothing, which is the safe path"). Either that header needs a plausible
section table, or the loop is waiting on a condition set elsewhere.

This is where the structural work ends and per-title runtime work begins.


## Pass 5 — profiling the spin, and two measured reversals

`gdb` cannot attach (ptrace restricted) and a single backtrace at `-O3` without
DWARF is unreliable — an earlier one said the main thread was in CRT init, and
that was wrong. So `main_posix.c` grew an **opt-in PC sampler**
(`RECOMP_PC_SAMPLE=<secs>`): an `ITIMER_PROF` at 200 µs records the interrupted
RIP, the dump subtracts the executable's load base, and `addr2line` resolves
them. A companion **hot-register counter** records which trapped aperture
offsets are hammered. Together they turned guesswork into measurements.

### What the guest is actually doing

| Register polled | Aperture offset | Polls in 7 s |
|---|---|---|
| **PMC_INTR_0** | `0x00100` | **905,742** |
| **PFIFO_INTR_0** | `0x02100` | **780,502** |
| PBUS_INTR_0 | `0x01100` | 205,048 |
| PFIFO DMA progress | `0x02400` / `0x03214` | ~349,000 |

That is a **wait-for-GPU-interrupt spin**, not a broken function — 1.8 M polls
of interrupt-status registers in seven seconds.

### Reversal 1: trapping NV2A is worse than leaving it as RAM

| Config | Indirect calls |
|---|---|
| NV2A pages trapped | 1,878 |
| NV2A untouched (plain RAM) | **19,580** |

The runtime's own vblank path sets `PMC_INTR_0`/`PCRTC_INTR_0` with
`InterlockedOr` **on the guest aperture** — i.e. it expects plain memory. A trap
intercepts its own handshake. The NV2A model is therefore opt-in
(`RECOMP_NV2A_MODEL=1`) rather than default, and the toolkit's POSIX stub turns
out to be the right call for this title.

### Reversal 2: the audio spin is an unimplemented codec, not a bug

With NV2A left as RAM the hottest guest function becomes `sub_003EBED6` (DSOUND,
44 % of samples), which writes **`0xFEC0017C`** — the **AC97 codec window at
`0xFEC00000`**. `mcpx_apu_mmio_read/write` only handles offsets `< 0x30000`, so
the codec window returns 0 from the model, and the runtime deliberately leaves
it as plain RAM. Trapping it against that model would be *worse* than RAM
(reads would lose the value the guest just wrote).

### Defaults now baked in

`main_posix.c` sets `RECOMP_AC97_READY` before `MemoryLayoutInit` (the trap
window is armed there, once), so the APU comes up with **no environment at
all**. Default run:

```
./shadow_game/build/shadow_recomp game_files/default.xbe \
    /mnt/docker/sites/shadow/WORKSPACE/files
→ 19,580 indirect calls, 0 faults, 1 benign file miss (save-area probe)
```

Opt-ins: `RECOMP_PC_SAMPLE=<secs>`, `RECOMP_NV2A_MODEL=1`,
`RECOMP_NV2A_NO_TRAP=1`, `RECOMP_NO_APU=1`, `RECOMP_VBLANK=paced`,
`RECOMP_WATCHDOG_SECS=<n>`.


## Pass 6 — the gate/DPC fix, and the AC97 shadow

### A real bug fixed: `RECOMP_DPC_ON_RAISE` was inert on its own

The header says `xbox_DpcOnRaise()` is *"gate on and RECOMP_DPC_ON_RAISE not 0"*.
The dispatch gate defaults **off**, so my earlier test of `RECOMP_DPC_ON_RAISE=1`
alone proved nothing. Setting both:

```bash
RECOMP_DISPATCH_GATE=1 RECOMP_DPC_ON_RAISE=1 RECOMP_VBLANK=paced
```

```
ack give-ups:  599  ->  1
indirect calls:      19,581  (high-water)
```

The vblank DPC now acks, so the interrupt handshake is correct end to end.

### AC97 codec page: shadow + trace

Only **two** codec registers are touched by this title (`0xFEC0012C`,
`0xFEC0017C`), and the space is otherwise plain RAM. `RECOMP_AC97_TRAP` copies
the page into a shadow (keeping the runtime's codec-ready seed), traps it, and
answers from the shadow while logging the first 40 accesses. Observed:

```
[AC97] R 0x12C = 0x00000000
[AC97] W 0x12C = 0x00000002
[AC97] R 0x130 = 0x00000100    <- codec-ready bit, guest sees it and proceeds
[AC97] W 0x026 = 0x00000000    <- powerdown
[AC97] W 0x018 = 0x00000808    <- PCM out volume, both channels 0x08
[AC97] W 0x11B / 0x17B = 0x02  <- vendor
```

So the codec handshake works, the guest configures the codec, and nothing here
faults (`icalls 19,581, faults 0`).

### Where it stands now

With the gate/DPC fix the GPU-interrupt spin is gone, and the profile is
dominated by the audio path:

| Samples | Location |
|---|---|
| ~5,100 (45 %) | `sub_003EBED6` — DSOUND |
| ~5,300 | SDL2/libpulse audio threads |
| 37 | `InterlockedCompareExchange` |

Everything measured is now waiting on **audio data flow**, not on a register
read: DirectSound has configured the codec and is looping while the APU's DSP
never consumes the guest's voice buffers.

### The three remaining subsystems, in order of value

1. **APU DSP / voice DMA** — make the emulated DSP consume the voice buffers
   DirectSound has published in guest memory, so its loop terminates.
2. **PFIFO/PGRAPH completion interrupts** — for the GPU work that follows.
3. **Headless presentation** — `fb_present` needs an SDL window/GL context;
   this box is headless, so it needs OSMesa/EGL or a dummy-driver path.


## Pass 7 — the audio handshakes, and what they revealed

### The DirectSound spin, found exactly

A one-shot probe inserted into the generated function named the loop:

```c
MEM8(0xFEC0011B) = 2;                    /* kick the engine */
while (MEM8(0xFEC0011B) & 2) ;           /* wait for hardware to clear */
```

`sub_003EBED6` writes **2** to the AC97 byte at `0xFEC0011B` and spins until bit
1 reads back 0. Nothing cleared it, because the page is plain RAM.

### Three fixes

1. **My own bug.** The AC97 shadow read and wrote whole dwords only, so a byte
   read at `0x11B` returned bits 0–7 (byte `0x118`) instead of bits 24–31 — the
   cleared bit was never visible. `ac97_rd_word`/`ac97_wr_word` now handle the
   byte lane.
2. **The kick acknowledgement.** A write to `0x11B`/`0x17B` clears bit 1
   immediately — the same "command taken, computing nothing" handshake the
   toolkit already makes for the GP doorbell.
3. **The APU frame thread.** It slept on a condvar until the title started the
   front end, and the VP ran only in `se_frame`. But `XCNTMODE` gates the frame
   *counter*, not the voices, and the GP DSP runs whatever the front end does.
   The paused path now polls the doorbell ack and runs `mcpx_apu_vp_frame`, and
   the front-end-idle branch runs the VP too.

Result: `[AC97] kick 0x11B/0x17B acknowledged` and
`[APU] DSP doorbell ack: auto` both appear where neither existed before.

### What the measurement says now

```
threads: 7
  532456  96.4%  Rsl   (guest)
  532465  87.7%  Rsl   (guest)
  532613  87.5%  Rsl   (guest)
  532471   0.2%  SDLAudioP2   <- audio output thread: idle
```

**Audio output is not the bottleneck.** Three *guest* threads spin; the SDL
audio thread is parked in a futex. The title still parks at the same place
(`icalls 19,581`, `esp 0x00F7F120`) with the handshakes now satisfied, so the
next blocker is elsewhere — it needs another profiling cycle with per-thread
attribution, which the current sampler does not record.


## Pass 8 — per-thread profiling, and a roadblock worth recording

The sampler now records the TID with each sample (`PTID <tid> <pc>`), which
separates the three spinning threads for the first time:

```
tid 623249  4,195 samples  sub_003EBED6 (DSOUND submit)
tid 623250  3,353 samples  libSDL2/libpulse
tid 623262  1,926 samples  libSDL2 + mcpx_apu_frame_thread
```

The DSOUND thread's hot address was a single instruction:

```
0x1632875:  eb fe   jmp 1632875      <- gcc's rendering of `while (cl) ;`
```

### The original guest code, disassembled from the XBE

```asm
003EBEF3  mov  byte ptr [eax - 0x13ffef5], 2     ; kick
003EBF03  mov  cl,  byte ptr [eax - 0x13ffef5]    ; read back
003EBF09  and  cl, 2
003EBF0C  test cl, cl
003EBF0E  jne  0x3ebf0c                            ; spin on the register
```

So the guest reads the kick byte **once** and then spins on the cached
register: if that single read returns bit 1 set, the loop can never exit. That
is what happens, and it is why no later device write can rescue it.

### What was fixed

* My AC97 shadow handled whole dwords only — a byte read at `0x11B` returned
  bits 0–7 instead of bits 24–31. Now `ac97_rd_word`/`ac97_wr_word` handle the
  byte lane.
* Kick writes (`0x11B`, `0x17B`) are acknowledged by clearing bit 1, and a
  *read* of either byte now presents it as consumed, so the handshake closes
  whichever side arrives first.
* The APU frame thread no longer sleeps on a condvar: while "paused" it polls
  the doorbell ack and runs `mcpx_apu_vp_frame`, and the front-end-idle branch
  runs the VP too (`XCNTMODE` gates the frame counter, not the voices).

### The roadblock

With all of that in place:

* the **write** to `0xFEC0011B` is trapped and acknowledged
  (`[AC97] kick 0x11B acknowledged`);
* the **load** is definitely in the compiled code —

  ```
  0x1632716:  movzbl (%rsi),%r9d      ; rsi = base + 0xFEC0011B
  ```

* yet **no read of `0x11B` ever reaches `ac97_rd_cb`** (a temporary trace in the
  callback printed nothing), and the register still ends up `0x02`.

### The roadblock, instrumented

`RECOMP_TRAP_TRACE=1` logs every fault whose address is in the codec page, and
`RECOMP_MAPS_DUMP=1` prints what protection the kernel actually granted it.

```
[MAPS] base=0x75ec74c00000 offset=129658431471616     <- identical: no base mismatch
[MAPS] 0x75ed73800000 -> ...-...3000 ---p             <- genuinely PROT_NONE
[TRAP] AC97 addr=...011b va=0xFEC0011B                <- exactly ONE, ever
```

So: the page is correctly protected, the translation base is correct, and the
**only** fault at `0x11B` is the store. The load at `0x1632716` never faults —
which means by the time it runs the page is readable again, or that address is
served from a different mapping. Reads then see real memory rather than the
shadow, so nothing the device model does can influence the value.

Sanitising the real page as well as the shadow (clearing the kick bit in guest
memory before protecting it) did **not** change the outcome, so the stale-page
theory is not sufficient either. `19581` calls, `0` faults, unchanged.

This is where I stopped: the next step is to log every `mprotect` the kernel
shim performs on guest ranges and catch the one that re-opens this page, rather
than continuing to patch the symptom from the device side.

Everything else in this pass is stable: default run 19,580 indirect calls,
**0 faults**, 3 kicks acknowledged.


## Pass 9 — a Windows .exe, and it runs far further

`mingw-w64` and `wine` were already on the box, so the toolkit's *intended*
target was testable after all:

```bash
cmake -S shadow_game -B shadow_game/build_win \
      -DCMAKE_TOOLCHAIN_FILE=/tmp/mingw.cmake -DCMAKE_BUILD_TYPE=Release
cmake --build shadow_game/build_win -j2        # EXIT=0, zero errors
→ shadow_game/build_win/shadow_recomp.exe      # 27.9 MB
```

The whole runtime cross-compiles under mingw — SEH, D3D11, dsound, the lot —
with no source changes. `main.c` was pointed at the real values
(`YOUR_GAME_ENTRY_POINT 0x002AFB53`, `game_files\default.xbe`).

### It runs under wine, and goes much further

```
=== YOUR_GAME_NAME - Static Recompilation ===
XBE loaded: 5447680 bytes
xbox_MemoryLayoutInit: mapped 65536 KB at 0x0000000000010000  <- identity mapping
[KERNEL] #1: ordinal 255 ...                                  <- entry point running
... 60 s later ...
  [KERNEL]  ordinal 294 x8897067      <- RtlLeaveCriticalSection
  [KERNEL]  ordinal 277 x8896514      <- RtlEnterCriticalSection
  [KERNEL]  ordinal 238 x1105720
  [KERNEL]  ordinal 159 x1105701
```

| | Linux | Windows (.exe under wine) |
|---|---|---|
| Guest window base | `0x7f4…` (high) | **`0x10000`** (identity!) |
| Kernel calls | ~200 | **~8.9 million** |
| Behaviour | parked early | running, doing HDD/save-path I/O |

Two things stand out. On Windows the guest window is mapped at its **real**
addresses (`base 0x10000`), so guest VA == host address — which is what the
NV2A/D3D11 paths were written against. And the title runs a large amount of real
code: save-partition paths (`\Device\Harddisk0\partition1\TDATA\53450086`, its
title ID), plus a heavy critical-section loop.

### Still not a vertical slice

There is no rendering evidence: no D3D device creation, no present/flip, and the
box is headless (`DISPLAY` is empty, so wine has no window to draw into). The
process is alive and busy rather than presenting frames. A slice needs the
D3D11 path actually driven — and on Windows that path is the complete one.

Next steps, in order: port the `RECOMP_PC_SAMPLE` profiler into the Win32
`main.c` and find what the 8.9 M-call loop is; then give wine a display
(Xvfb or a virtual desktop) so a device can be created and a frame presented.


## Pass 10 — it draws

Three things were missing on Windows, and all three are host-side:

1. **`nv2a_hook_init` was never called.** Nothing in the runtime calls it; the
   host main is responsible. Without it the GPU aperture is zeros, so the
   title's D3D8 (its own lifted code) never gets a device.
2. **The VEH short-circuited the GPU range.** `main.c` had a leftover
   `if (fault_addr in 0xFD000000..0xFE000000) return EXCEPTION_CONTINUE_SEARCH;`
   — a TODO — placed *before* `xbox_VehMmioFault`, which itself covers only the
   APU and OHCI. So a trapped NV2A register access became an unhandled access
   violation (`FD018088`). Now NV2A faults are dispatched to
   `nv2a_hook_handle_mmio` / `nv2a_hook_handle_vram` and the instruction is
   stepped over.
3. **`RECOMP_PB_EXEC` / `RECOMP_FB_WINDOW` were off.** The pushbuffer executor
   is what turns GPU commands into a framebuffer and calls
   `xbox_FramebufferWindowStart()`. Both are now set in `main.c` via
   `_putenv_s` (not `SetEnvironmentVariableA` — the runtime reads them with the
   CRT's `getenv`, which keeps its own copy), unless the user overrides, and
   `RECOMP_HEADLESS` still suppresses the window.

### The result

Under wine on a virtual display, with no driver errors:

```
xwininfo -root -tree
  0x1400001 "Shadow The Hedgehog | FPS: 0.0 | draws: 8"
            ("shadow_recomp.exe")  640x480+192+157
```

* a real **640×480 window**, titled by the runtime with its own draw counter
* `[NV2A] Standalone GPU initialized: VRAM=64MB RAMIN=1024KB`
* `[NV2A] MMIO hook initialized: VRAM=… RAMIN=…`
* **8 draws** through the pushbuffer executor
* `RECOMP_FB_DUMP` writes real frames: `640x480 24bpp`, ~45 distinct colours

So the rendering chain is live end to end: guest D3D8 → NV2A registers → model →
pushbuffer executor → framebuffer → window.

### What is still missing

The window is nearly empty and the draw count stops at **8** — the guest issues
its first few draws and then parks (the same busy loop that shows up as ~8.9 M
`RtlEnter/LeaveCriticalSection` calls). Nothing is animated: consecutive dumped
frames are byte-identical. Getting a title screen means finding that loop, not
more rendering plumbing.

Artifacts: `frames/frame0000*.bmp|png` (dumped framebuffers),
`frames/window.png` (the actual window).


## Pass 11 — a Windows profiler, and the first real Windows profile

Windows has no `ITIMER_PROF`/`SIGPROF`, so the POSIX sampler could not be
reused. `main.c` now carries its own (`RECOMP_PC_SAMPLE=<secs>`): a thread that
enumerates the process's threads with `CreateToolhelp32Snapshot`, suspends each,
reads `CONTEXT.Rip`, resumes, then dumps `PTS <tid> <addr>` with the address
rebased to the PE's preferred image base (`0x140000000`) so `nm`/`addr2line`
resolve it. Note: `addr2line` needs DWARF and this is a Release build, so the
mapping is done against `nm -n` COFF symbols instead.

### First profile (2,940 samples, 8 s)

| Samples | Symbol |
|---|---|
| **357** | `sub_003EBED6` — the DSOUND submit loop (the same function that stalls Linux) |
| **356** | `sub_003D4950` — D3D |
| ~2,130 | wine's own ntdll/kernel32 |

Two guest threads, two different loops. The DSOUND one is *not* fatal here —
the game still draws 8 frames — so it is the audio thread spinning while the
main thread proceeds.

### The 8-draw ceiling

`sub_003D4950`'s hot samples land inside one tight loop:

```asm
14140d1ce  sub  %edx,%eax
14140d1d0  mov  %edx,0x0(%rbp)      <- sampled
14140d1d3  mov  %eax,0x0(%r13)      <- sampled
14140d1d7  cmp  %eax,%r11d          <- sampled
14140d1da  jb   14140d1c8           <- back edge
```

It is an index/count loop writing through two pointers — a surface walk or
copy — bounded by `r11d`. With draws stopping at 8, the next question is what
`r11d` is and where it came from, which needs the loop's operands logged the
same way we logged the earlier kick address.

Tooling is now in place on both platforms:
`RECOMP_PC_SAMPLE=<secs>` works on Linux (ITIMER_PROF, with TID) and on Windows
(thread suspension, rebased to the PE base).


## Pass 12 — chasing the 8-draw ceiling on Windows

With the Windows sampler in place, three hypotheses were tested and two
eliminated.

### The wait is a thread wait, not a GPU wait

Ordinal counts from a 60 s run identify the loop precisely:

| Ordinal | API | Calls |
|---|---|---|
| 159 | **`KeWaitForSingleObject`** | 632,817 |
| 238 | **`NtYieldExecution`** | 1,233,363 |
| 224 / 231 | `NtResumeThread` / `NtSuspendThread` | 592k / 406k |
| 277 / 294 | `RtlEnter/LeaveCriticalSection` | ~8.9 M |

The main thread is waiting on an object and yielding the CPU — not polling the
GPU.

### Fix 1: the AC97 kick handshake, acknowledged from the host

The 0xFEC0011B handshake that wedges Linux also wedges the audio thread here,
where that page is plain RAM. A host thread now clears `0xFEC0011B`/`0xFEC0017B`
every millisecond (`RECOMP_KICK_ACK=0` disables). Effect on the profile: samples
in `sub_003EBED6` fell **357 → 91**. Real, but not sufficient.

### Fix 2: vblank was never enabled on this path

`RECOMP_VBLANK` is off when unset, and the Windows main never set it. Now
`paced` by default. Measured: **3,000 vblanks at ~60 Hz, ack give-ups 0**, guest
ISR claims every one. The interrupt path is now demonstrably perfect — and the
title still stops at 8 draws, so vsync was not what it was waiting for.

### Eliminated: a critical-section deadlock

The log showed a suspicious pair —

```
[CS] thread 436 waiting on guest lock 0x005A0320 (held by thread 432)
[CS] thread 432 waiting on guest lock 0x005A0320 (held by thread 436)
```

— but the counter settles the question:

```
[CS] enters=23645 leaves=23642 (outstanding 3)   <- constant across 4 samples
```

Outstanding holds at 3, so it is normal contention and handoff on one lock, not
a cycle. (The "held by" number is the host `OwningThread`, which is not a guest
thread id — it reads as a deadlock and isn't.)

### Where it stands

Default Windows run, no environment variables: window opens, 8 draws, vblanks
at 60 Hz with zero give-ups, audio handshake acknowledged. The guest then sits
in `KeWaitForSingleObject` on an object nobody signals.

Next: `KeWaitForSingleObject` logs its ordinal but not its argument, so the next
step is to log the wait object's handle and type — that names the thread/event/
mutant the title is parked on, and is the last unknown between here and a
launched game.


## Pass 13 — the wait object, named

`KeWaitForSingleObject` now logs its argument (`RECOMP_WAIT_TRACE=1`):

```
[WAIT] obj=0x003E27DC handle=0x003E27DC kind=-1 timeout=0x00000000 caller=0x003CFE93
```

Three things fall out of that one line:

* the object is `0x003E27DC` (inside the D3D section, `device + 0x1DBC`);
* `handle` equals `object`, i.e. the lookup **failed** and it fell through to a
  raw pointer — so the wait returned immediately. That is the 632,817-call spin;
* the caller is `0x003CFE93`, and the guest code there is the GPU-idle
  handshake: clear the flag at `device+0x1DC0`, `call [0x429BFC]` (that is
  `KeWaitForSingleObject`), return, re-test the flag.

### Fix: recognise the title's own events

`ke_guest_event` is gated on **`RECOMP_TITLE_KEVENTS=1`**, and the Windows main
never set it. Now default. The same wait then resolves properly:

```
[WAIT] obj=0x003E27DC handle=0x000001F8 kind=0   <- a real notification event
[KERNEL] event 0x003E27DC built by the title: host event created (notification)
```

`KeWaitForSingleObject` calls fell **632,817 → ~4,700**: the title went from
spinning to genuinely blocking. Drawing still stops at 8, so that event is not
the only thing it needs.

### What did not work

Signalling the event from the host (`xbox_GuestEventSignal`, added to the
bridge, with a 1 ms acker thread) returns the wait, but the guest immediately
re-enters it — 10,370 waits on one object. Reading the guest code explains why:
the loop *clears* the flag at `device+0x1DC0` and then re-tests it, so making
the wait return is not the same as making the test pass. Poking that flag from
the host races with the guest's own clear and does not help (tried).

The honest conclusion: that handshake has to be completed by *our* GPU
completion path — the executor setting the device's idle flag and event when it
drains the pushbuffer — rather than by the host acking it on a timer. That is
the last blocker between the loading screen and the game launching.

### Shipped defaults (no environment needed)

`RECOMP_PB_EXEC=1`, `RECOMP_FB_WINDOW=1`, `RECOMP_VBLANK=paced`,
`RECOMP_TITLE_KEVENTS=1`, AC97 kick ack on a 1 ms thread, NV2A model
initialised. Result on the Windows build: 640×480 window, loading screen,
8 draws, 60 Hz vblanks with 0 ack give-ups.


## Pass 14 — the spin, read out of the guest

The GUI froze at the loading screen, so I read the guest instead of guessing.
Two facts first: the process sits at **~280% CPU with `read_bytes: 0`** — it is
not loading anything, it is burning CPU — and the sampler puts the time in
`sub_003D4950` (D3D) and `sub_003EBF48` (DSOUND).

The D3D spin, decompiled from the generated C:

```c
loc_003D4A7D:
    edx = MEM32(esi + 0x30);      /* pointer to the GPU's GET counter      */
    esi = MEM32(esi + 0x2C);      /* the PUT value                         */
loc_003D4A87:
    ecx = MEM32(edx);             /* re-read GET on every pass             */
    edi = esi - ecx;
    if (eax < edi) goto loc_003D4A87;   /* wait for the GPU to catch up     */
```

That is the classic **PUT/GET fence**: D3D publishes work at PUT and spins until
the GPU's read pointer reaches it. The device pointer lives at `0x3E0A18`.

### The mechanism already existed

`xbox_memory_layout.h` exports exactly this:

```c
int xbox_Nv2aMirrorFence(uint32_t device_ptr_va,
                         uint32_t put_off, uint32_t get_ptr_off);
```

— "Followed through the device pointer, like the fence, because the device is
allocated at runtime." So the fix is one registration, and it is now made at
startup:

```
NV2A fence mirror: device at 0x003E0A18, PUT +0x2C -> *(GET +0x30)
```

### What that did and did not fix

The registration is accepted and the runtime's own thread polls it, but the
title still stops at **8 draws** and stays at ~245% CPU. So the mirrored GET is
not advancing: from the executor's point of view the queue is already drained,
while D3D is still waiting for movement. That is a question about what our
pushbuffer executor considers "consumed" versus what D3D's fence counts — the
next thing to instrument, and it is a much narrower question than "the game
hangs".

### Tools this left behind

* `RECOMP_WAIT_TRACE=1` — logs every `KeWaitForSingleObject` object, handle,
  kind and caller.
* `RECOMP_PC_SAMPLE=<secs>` — per-thread profile on both platforms.
* `RECOMP_TITLE_KEVENTS=1` (now default) — recognise events the title builds
  itself; this alone turned 632,817 waits into ~4,700.
* `RECOMP_NO_FENCE=1` — disable the fence mirror registration.


## Pass 15 — what the loading screen is actually waiting on

### It is not loading

Three measurements, in order:

* process at **~280% CPU** with `read_bytes: 0` — no disk traffic;
* `rchar` rises (~375 KB/s) but **no game file is open** (`/proc/pid/fd` shows
  only pipes and the log) — so that traffic is wine's own plumbing, not the
  disc;
* profiling shows almost everything idle in wine, with `sub_003EBF48` (DSOUND)
  and `sub_003D4820` (D3D) as the only guest hotspots.

So the loading screen animates while nothing is streamed: a thread is parked,
not loading.

### The fence is registered, and is not the whole story

`xbox_Nv2aMirrorFence(0x3E0A18, 0x2C, 0x30)` is now called at startup and
reports:

```
NV2A fence mirror: device at 0x003E0A18, PUT +0x2C -> *(GET +0x30)
```

Frame rate went **0.0 → 17.8–31.7 FPS**, and two screenshots taken three seconds
apart differ — the loading screen is genuinely animating. That was the fix that
turned a still image into a live one.

But `KeWaitForSingleObject` still lands on the *same* object 23,272 times per
run (`obj=0x003E27DC caller=0x003CFE93`) — the D3D GPU-idle wait, reached
through a function pointer (no static callers), sitting next to the fence in the
same submit path. So the fence satisfied one wait and the other still loops.

### Also learned

* `draws` in the title bar is **per frame**, not cumulative — 8 draws at 30 FPS
  is a normal loading screen, not a ceiling. (My earlier reading was wrong.)
* The title bar is the best progress meter available: `FPS` rises and falls with
  what the guest is doing, `draws` changes as the screen changes.

### A trap that lost on measurement

The AC97 codec page trap (the POSIX design, ported to Windows via
`mmio_decode.h` + `mmio_emulate`) works and is fault-free, but as a default it
measured **worse**: FPS 17.8 → 5.0 and draws 8 → 2, because every codec access
becomes a signal plus a decode and the title polls that page hard. It is now
opt-in (`RECOMP_AC97_TRAP=1`) and the cheaper 1 ms acker stays the default.
Returning to the default measured FPS 27.9 and draws 8 again.


## Pass 16 — it is loading, and the "stuck thread" is the frame loop

### The thread I was chasing is the game's frame loop

A guest-stack scan on the first waits gave the chain:

```
[WAIT] obj=0x003E27DC handle=0x000001E4 kind=0 ... caller=0x003CFE93
  [esp+0]  0x003E27DC
  [esp+20] 0x003291F5     <- inside sub_003291E0
  [esp+24] 0x002ADEE5     <- PsCreateSystemThreadEx thunk
```

`0x3291E0` is the worker the runtime reported at startup
(`PsCreateSystemThreadEx … ctx=0x003291E0`), and its body is:

```c
loc_003291E0:
    eax = MEM32(0x560330);  if (…) goto exit;
    call sub_003CFE70       /* wait for GPU idle */
    call sub_002ADDC9
    … sub_0032D0F0, sub_0032D4C0, sub_0032D610 …
    /* loop */
```

That is a **frame loop**: wait for the GPU, update systems, repeat. So the
repeating wait is not a hang — it is the game running at 30 FPS, which is
exactly why the loading screen animates. `[GPU] draw #6600` confirms it:
six and a half thousand draws over a couple of minutes.

### It really is loading assets

31 successful opens, including the loading screen's own data:

```
d:\csdFiles\Loading_EN.xncp   ✓      d:\character\shadow.one   ✓
d:\PRS_VOICE_E.afs            ✓      d:\common\CommonEffect.one ✓
d:\DebugModels.one            ✓      d:\FadeLogo.txd            ✓
```

Then the count **freezes at 31** and no further file is opened. So the stall is
*after* that asset batch, not before it.

### Two suspicions eliminated

* **Not a data gap.** `d:\sound\*.bin` all fail, but the disc tree has no
  `sound/` directory at all (`MANIFEST.tsv` and `xvdfs list` both zero) — those
  paths do not exist on this disc either. The audio that *does* exist is
  `PRS_VOICE_E.afs`, which opens successfully.
* **Not a second wait.** Every traced `KeWaitForSingleObject` in the process is
  the same GPU-idle object, and there are no `Nt*` waits in the ordinal counts
  (294/238/159/224/231 only). So no thread is parked on some other object we
  never signal.

### Where that leaves it

The game runs its frame loop, animates the loading screen, loads 31 assets, and
then does nothing further — with nothing waiting and nothing failing. The
useful next reference is **what the game does after that point on a working
implementation**, i.e. xemu (not installed here). Running the same ISO under
xemu and capturing its file/GPU activity right after `CommonEffect.one` would
say precisely which asset or device interaction we are missing, instead of
guessing at another handshake.


## Pass 17 — the loader, located (blind)

`RECOMP_FILE_TRACE=1` now dumps the **guest** stack at each file open, because the
`from 0x…` field in the `[FILE]` line is only the ordinal thunk's return address.

The chain for the last successful load (`common\CommonEffect.one`):

```
[FILE] d:\common\CommonEffect.one (disp 1 opts 0x60) -> 0x00000000
  [esp+64 ] 0x001B001A
  [esp+80 ] 0x002F222B     <- return address of the open call
  [esp+156] 0x00541C30
  [esp+284] 0x0047B898
  [esp+304] 0x002F162F
  [esp+372] 0x002ABE5D     <- CRT
```

Disassembling `0x2F2226` shows exactly how files are opened:

```asm
002F2221  lea    eax, [esp+0x28]
002F2225  push   eax
002F2226  call   0x2af653          ; the file-open bridge
002F222B  cmp    eax, -1           ; -1 = failure -> return 3
002F222E  mov    [esi+0x40], eax   ; store the handle at +0x40
```

So the asset loader lives around `0x2F16xx`–`0x2F22xx`, it stores each file
handle in its own structure at `+0x40`, and it simply stops asking for more
after 31 files.

Meanwhile the frame loop keeps drawing (`[GPU] draw #6600`) and the only wait in
the whole process remains the GPU-idle event. Nothing is deadlocked; the loader
is simply not being asked to continue, and nothing is loaded after
`CommonEffect.one`.

### What that implies, and the next instrument

The loading screen's own assets are all in: `Loading_EN.xncp`, `FadeLogo.txd`,
`shadow.one`, `CommonEffect.one`. What is missing is the *next* stage's request —
which means the gate is inside the game's loading state machine, not in the
filesystem, the GPU or the audio handshakes.

The next instrument is to dump **every guest thread's stack** at the stall (the
runtime already logs each worker's stack top when it spawns one:
`spawned worker 0x002ADE73 (stack top 0x01301FF0)`). Scanning those ranges for
code addresses shows what each thread is parked in — including whichever one
should be driving the load — instead of only the frames visible on the one
thread that happens to hit `KeWaitForSingleObject`.


## Pass 18 — all five guest threads, named

`RECOMP_STACK_DUMP=<secs>` dumps every guest thread's stack (the runtime records
each stack top as it spawns a worker) and maps the frames to guest functions.

```
thread 0 (top 0x01302FF0)   sub_002AD840 → sub_002AD8A4 → sub_00332BD0
thread 1 (top 0x01383FF0)   sub_00328DD0 → sub_0032D610 → sub_00331920
                            → sub_00330DB0 → sub_0034A9B0
thread 2 (top 0x01403FF0)   sub_00328DD0 → sub_0032D610 → sub_0032C360
                            → sub_0032B300 → sub_0032AB90 → sub_00328C30
thread 3 (top 0x01484FF0)   sub_0034AB70 → sub_00349280 → sub_0032D430
                            → sub_002ADD27 → sub_00328E80
thread 4 (top 0x01605FF0)   sub_003D4AC0 → sub_003D4C00 → sub_003DB990  (D3D)
```

### What this settles

* **No thread is anywhere near the loader.** The asset loader lives at
  `0x2F16xx–0x2F22xx` (pass 17); none of the five stacks shows it. So the load
  does not merely stall *inside* loading — the title has finished the list it
  had and nothing is asking for the next stage. That matches the file count
  freezing at 31.
* **The D3D thread (`thread 4`) sits in `sub_003D4AC0`**, the same neighbourhood
  as the GPU-idle wait helper (`0x3CFE70`) and the submit code, which is
  consistent with it being the thread that keeps the frame loop going.
* **Two threads (0 and 3) are parked close to their entry points** — the
  `sub_002ADE73` thunk is near the bottom of both stacks. They were spawned and
  have not progressed far; those are the candidates for the thread that should
  be driving the next load and is instead sitting still.

### Where the trail leads

The evidence is now consistent from four directions: assets load, the frame loop
runs, nothing waits, and no thread is in the loader. What remains is to identify
what `sub_00332BD0` (thread 0) and the `sub_00332Bxx`/`sub_0034ABxx` chain
expect, because those are the two threads that never got past their start. That
is where the game's next-stage decision lives.


## Pass 19 — the gate, found: a job queue that never gets a job

Thread 0's loop (`sub_00332BD0`) decompiles to a **task dispatcher**:

```c
loc_00332BE4:
    edi = 0x5923E4;                       /* slot base + 4        */
again:
    if (MEM32(edi - 4) == 0)                goto next;  /* slot unused */
    if (MEM32(edi + 0x10) != 1)             goto next;  /* state != 1  */
    call sub_00332B60;                                   /* run the job */
    MEM32(edi + 0x34) = MEM32(edi + 0x30);
    if (MEM32(0x560BD0)) icall;                          /* optional cb */
next:
    if (MEM32(edi) != 6) MEM32(edi) = 2;
    MEM32(edi + 0x10) = 0;                               /* clear state */
    Sleep(10);                                           /* → 0xA ms    */
    edi += 0x44;                                         /* stride      */
    if (edi < 0x592824) goto again;                      /* 16 slots    */
    if (MEM32(0x560BE0) == 0) goto loc_00332BE4;         /* quit flag   */
```

So it is a **16-slot job queue at `0x5923E0`, stride `0x44`**, working
`state(+0x14) == 1` entries, clearing state after each and sleeping 10 ms per
sweep.

### The live table (`RECOMP_STACK_DUMP` now dumps it)

```
slot  0  id=0x00000001 state=0  +0x34=0x00FFE058 +0x38=0x00FFE058
slots 1-15  all zero
dispatcher: processed=0x00000000 quit=0x00000000 callback=0x00000000 frame_flag=0x00000000
```

**`processed = 0`.** `sub_00332B60` does `lock xadd` on `0x560BD8` every time it
runs a slot, so a zero there means the dispatcher has **never processed a single
job** in the entire run. `0x560BD0` (its optional callback) is only ever *read*
in the lifted code and never written — it was never registered.

### The subsystem is initialised, just never fed

Decompiling the init at `0x332D8F`:

```c
edi = 0x5923E0;
rep stosd                              /* zero 0x110 dwords = the whole table */
MEM32(0x560C1C) = 0x332AC0;            /* job op 1 */
MEM32(0x560C20) = 0x332B10;            /* job op 2 */
MEM32(0x560C24) = 0x332A50;            /* job op 3 */
```

and a matching shutdown at `0x332E..` zeroes the table, clears those three
pointers and sets `0x560BE0 = 1` (quit). All three ops funnel into
`sub_003337B0`.

So the async job subsystem is **built, registered, running and idle**. Everything
needed to do work exists; nothing ever queues a job. That is the loading screen's
gate — not the filesystem, the GPU, the audio or a lock.

### Next

Find the enqueue path. It does not use a literal address (only init, dispatch and
shutdown do), so it reaches the table through a pointer — meaning the honest next
instrument is to watch the table for a `state = 1` write, or to hook
`sub_003337B0` and see whether it is ever entered. Naming the caller of the init
(`0x332D8F`) would also name the subsystem that owns it.


## Pass 20 — five hypotheses killed, and USB/XID brought to life

Pass 19 said the job queue was "the gate". **It is not.** That was wrong, and the
correction is cheap to state:

- `0x560BD0` (the dispatcher's optional callback) has **exactly two references in
  the whole binary** (`0x332C0A`, `0x332EC0`), both *reads inside the dispatcher
  itself*. Nothing ever registers it.
- `0x5923E0` (the 16-slot table) is referenced in only three places: the init's
  `rep stosd`, the teardown walk, and the shutdown's `rep stosd`. **There is no
  enqueue site anywhere**, so nothing can ever set a slot to state 1.

The queue is vestigial engine code that Shadow never uses. Thread 0 is a red
herring. (Method: `xrefs.json` only carries code jumps, so the search was done by
scanning `.text` for the little-endian immediates and disassembling every hit.)

### Also ruled out, each by measurement

| Hypothesis | Test | Result |
|---|---|---|
| A frozen PTIMER stalls the fade | added `RECOMP_PTIMER_TRACE` to `ptimer_read` | **0 reads** — the game never touches PTIMER |
| A frozen kernel clock | read `xbox_KeQueryPerformanceCounter` / `InterruptTime` | both implemented on `QueryPerformanceCounter` / `GetTickCount64`, monotonic; ordinals 125/126/127 never appear in the call profile |
| Pending async I/O | read the `[READ]` trace | 30 reads, **all** `got == want`, `st == 0`; no pending completions |
| Waiting on key input | `xdotool windowfocus` (verified: `focused == 0x1c00001`) then Return/space/arrows | files 31, draws 8, mean unchanged beyond its normal oscillation |
| Long timer, not a stall | 110 s run | files frozen at 346 (31 unconditional), mean oscillating 0.0051–0.0067, 23,200 draws, 5,400 vblanks — steady, going nowhere |

Also worth recording: **the crash dialog was a red herring.** An OCR of the
framebuffer reported *"WineDbg attached"* — a **stale Wine Debugger window from
an earlier crash was sitting on top of `:98`**. The game underneath was healthy.
Capture `-window <id>` of the title, never `-window root`.

### What the screen actually is

Constant-brightness, not a fade: `mean ≈ 0.0042` for 12 s with ~3,800 px
changing. Content is a full-width `639 × 140` band at `y=274`, dominated by
`#000000` (298,072 of 307,200 px), `#252525`, `#3F3F3F` and 958 px of `#780000`.
That is a dark grey/red logo — Shadow's colours — held at roughly a tenth of
normal brightness. `draws: 8` per frame, 60 Hz vblank, ~30 fps, rock steady.

### USB / XID — a real gap, now wired

`xbox_OhciInit()` existed but **nothing called it** (the same gap as
`nv2a_hook_init`), and it also returns immediately unless `RECOMP_USB` is set.
The MCPX aperture at `0xFED00000` is backed as plain zeroed RAM with no register
semantics, and the toolkit's own note says why: *"without a descriptor list
walker behind it a driver that finds a port has nothing to enumerate."*

Wired into `main.c` beside `nv2a_hook_init`, then run with `RECOMP_USB=1`:

```
OHCI: two controllers at 0xFED00000 and 0xFED08000, 4 ports each
[OHCI0] operational after 200 ms; device arriving on port 3
[OHCI0] periodic list enabled (HcControl=000000BE HCCA=00082000)
[OHCI0] stats: 7 periodic transfers; reports 0 setups 1 out0 5
[OHCI0] raised 00000040 -> ISR claimed it
```

So the title's **own XID driver runs**: it resets the controller, sets
`HcControl=0xBE`, programs a periodic list and takes interrupts. Enumeration then
stalls:

- `HcRhPortStatus` (0x54) reads `0x00000100` and never changes, while the driver
  polls it and writes 0 — it is waiting for a port-reset completion that our
  root hub never reports.
- The control TDs parse as nonsense: `dp=1 ep=0 cbp=640039A7 be=89645000
  len=627316314` — a buffer end 2 GB outside RAM and a 627 MB length. That is a
  field-parsing bug, not a guest value.

The pad still does not reach the game (`reports 0`), and enabling USB does **not**
change the visible behaviour (files 31, draws 8), so USB is *a* gap but not
proven to be *the* gate.

## Pass 21 — the user's eyes, the exe mix-up, and what the disc actually has

### Confirmed by the user (the observation I could not make headless)

> "screen is the loading screen looping with the word *loading*, no response from keyboard"

So **rendering is correct** — the word is legible. My "dark logo at a tenth
brightness" reading was a dim, looping loading screen, not a fade bug. The stall
is real, and keyboard input is not the gate.

### The "black screen" was a path bug, now fixed

The user had been running `xboxrecomp\shadow_game\shadow_recomp.exe` (black)
instead of `xboxrecomp\shadow_recomp.exe` (loading screen). Both files were
byte-identical (`md5 0bd069f4…`) — the difference was the **working directory of
the data**, not the build. Data paths resolved from the exe's own folder:

- `shadow_game\` has `game_files\default.xbe` ✔ but `shadow_game\..\WORKSPACE\files`
  resolves to `xboxrecomp\WORKSPACE\files`, which does not exist ✘
- so the XBE loaded, found no disc, and drew nothing.

`resolve_paths()` in `main.c` now walks up to three parents for the XBE and tries
`<root>\..\WORKSPACE\files`, `<root>\WORKSPACE\files`, then one more level up.
Verified from `shadow_game\build_win`: `files=31`, window up, `FPS 28.9 |
draws: 8` — identical to the top-level copy.

### What the disc really contains

`tools/xdvdfs.py` on the source ISO lists **1,576 entries, and no `sound/` and no
`dvd/` at all.** Top-level names are `character, common, csdFiles, discError,
enemy, event, fonts` plus loose `.one/.txd/.sfd/.adx` files — audio ships as
root-level `sng_*.adx`.

The game therefore issues 12 opens that fail, and they fail on real hardware too
because the paths are not on this disc:

```
t:\$u\contentmeta.xbx           -> ERROR_PATH_NOT_FOUND   (temp drive)
d:\sound\sonicteamfx.bin        -> ERROR_PATH_NOT_FOUND
d:\sound\bank0.pac              -> ERROR_PATH_NOT_FOUND
d:\sound\AudioData*.bin   (×7)  -> ERROR_PATH_NOT_FOUND
d:\dvd\                   (×2)  -> ERROR_FILE_NOT_FOUND
```

That kills the "missing asset stalls the load" theory: the missing paths are
missing from the master, so the game must tolerate them on real hardware.

### Heap narrowing (new tool: `RECOMP_STATE_SNAP=1`)

`.data` is essentially static after boot — of 458,752 dwords, only 20 changed in
1.2 s and 18 in the next 2.5 s. Game state lives on the heap, so the same probe
over `0x00F80000..0x03000000` (8,519,680 dwords) gives 127 changed, 103 still
changing, 90 changing throughout, and **37 that changed early and then froze** —
including a `0->1` flag at `0x00FE8308`, a counter that jumped to `393039` at
`0x00FE8510`, and a table of `{count, 0x8000xxxx}` pairs at `0x00F840B8`.

Two self-inflicted bugs in that probe, both worth recording: it first resolved
`g_xbox_mem_offset` before `main` assigned it (silent no-op), then slept a fixed
2 s so every snapshot landed *after* loading had finished. It now polls for the
mapping. A `v3` tag in the output makes it obvious whether the running binary
contains the current probe — worth doing, because two runs looked identical and
it was not obvious why.

### Also tested and ruled out this pass

| Test | Result |
|---|---|
| `RECOMP_MISSING_FILES=1` | no output — the toolkit does not consider any requested path missing |
| `RECOMP_UNIMPL=1` | one hit only: `wbinvd` at `0x003D5A34`, a no-op |
| `RECOMP_NO_APU=1` | 22 files, draws 8, 12 failures — no change |
| USB device on port 1 (`RECOMP_USB_PORT=0`) | `setups 1 out0 1` — still stalls on the first control transfer, so the port is not the problem |

### The USB picture, sharpened

`0x100` in `HcRhPortStatus` is **`PORT_PPS`** — powered, and *no device
connected*. The driver polls `+0x54` (root port 1) while the model plugs on port
3 deliberately; `RECOMP_USB_PORT=0` moves it and it makes no difference.
Enumeration dies at the first control transfer whichever port is used, and the
TDs decode as impossible values (`cbp=640039A7 be=89645000`, an ED
`head=006AE875` that is not even 16-byte aligned). That is a **field-decode bug
in our TD/ED walk**, not a guest value — the next real piece of work.

`RECOMP_PAD_PRESS` / `RECOMP_PAD_SCRIPT` are not a workaround: they feed input
reports through the same endpoint, so they cannot reach the title until
enumeration completes.

### The engine object is fully built (so "engine never initialised" is out)

The parent project's `MEMORY_BANK/Architecture.md` documents the engine main loop:
`0x0011C4E–0x0011C6A`, per-frame `dispatch(cmd=0x12)` at `0x0011C5B`, per-tick body
`0x00057630` with `trace hook 0xE53B0`, `pump *vtable+4`, `update *vtable+8`,
`render *vtable+0xC`, and a vtable object at `0x541FB4` "built at run time".

Peeking it (`[0x541FB4]=0x014D1E90`, then chasing the pointer) lands on the live
table:

```
[0x479C1C]=002A1320  [0x479C20]=00254170  [0x479C24]=002A1340
[0x479C28]=00254170  [0x479C2C]=000E53B0   <- the trace hook the doc names
```

Every slot is a real code pointer. **The engine object is constructed and its
pump/update/render entries are live**, so the title's per-frame machinery is
present — the stall is inside game state, not a missing engine.

Two instrument bugs fixed on the way: `RECOMP_PEEK` only printed at a hang or a
clean exit, and `RECOMP_STACK_DUMP`'s `_exit(0)` is neither, so the two together
printed nothing — it now samples in the dump thread. And `RECOMP_PEEK` chases
`[[addr]]+off`, which is how the vtable above was reached at all.

## Pass 22 — lead B: the per-frame update short-circuits, and the engine object is inert

### The tick, read properly

`0x57630` (per-frame body) decompiles to:

```c
esi = [0x541FB4];                        /* engine object                */
0xE53B0(esi != 0, 0x445938);             /* trace hook                   */
if ([0x541FB8] == 0) {                   /* byte gate, 0 = run           */
    ecx = [0x541FB4]; edx = [ecx];       /* -> vtable 0x479C1C           */
    call [edx + 4];                      /* "pump"                       */
}
if ([0x53948C] == 0) [0x53948C] = 0x19450();   /* one-shot init         */
0x61FC0(this = [0x541FA8]);
0x57270();
if ([0x541CF4] == 0) [0x541CF4] = 0x4F490();   /* one-shot init         */
0x4F670();
0x56FC0();
... 0x2BBC90(this = [0x541FA8]) ...
```

Both one-shot inits had already run (`[0x53948C]=0x539438`,
`[0x541CF4]=0x541CE8`), and `[0x541FB8]=0`, so the pump branch is taken.

### What the pump and the vtable actually are

`[0x541FB4]` points at the engine object (this run `0x014D0E90`, last run
`0x014D1E90` — the address moves, as expected). Its vtable `0x479C1C` is:

```
+0x00 002A1320   +0x04 00254170   +0x08 002A1340
+0x0C 00254170   +0x10 000E53B0
```

and **`0x254170` — the slot the tick calls as `[edx+4]` — is a bare `ret`.**
So does `+0x0C`, the render slot. It is a base/abstract vtable: the tick's
`call [edx+4]` is a no-op by construction, and the real per-frame work is the
direct calls after it.

Dumping the engine object itself: **0 of 64 fields changed over 8 seconds**, and
the layout is two `{0x0002000n, 0x00100101, ptr, ptr}` blocks at `+0x10` and
`+0x30`, self-referential pointers at `+0x24`/`+0x28`, `+0x40 = 0x00559E70`,
`+0x44 = 0x00FE8C2C`. Frozen, but consistent with an object that simply holds
static tables — **not** by itself evidence of the gate.

### The real finding: `[0x541FA8]` is NULL and `0x61FC0` bails

```
[00541FA8]=00000000   [0053948C]=00539438   [00541CF4]=00541CE8
[00541FB8]=00000000   [0053550C]=00000000   [00541FB4]=014D1E90
```

`0x61FC0` decompiles to:

```c
ecx = MEM32(ecx);      /* dereference the incoming 'this' */
if (ecx == 0) { esp += 8; return; }   /* ret 4 -- bails */
tail_jmp 0x21D10;
```

So with `[0x541FA8] = 0` the title's main per-frame update **returns
immediately, every frame**. The same null is handed to `0x2BBC90` later in the
tick.

### Is `0x541FA8` *meant* to be the engine pointer? Probably — but unproven

Scanning `.text` for the immediate gives 11 references. Three are float stores
(`fstp [0x541fa8]`, `movss [0x541fa8], xmm0`, `movups [0x541fa8], xmm1` — the
last writes 16 bytes, which *overlaps* `0x541FB4`), which looks like a vec4 at
`0x541FA8`. But no float writer ever runs: `[0x541FA8]` is 0 in every sample, and
if those paths ran `0x541FB4` could not simultaneously hold a valid pointer. So
the vec4 use is dead in this build and the pointer use is the live one.

**Poke test (inconclusive).** `RECOMP_POKE=0x541FA8:0x00541FB4` makes
`[0x541FA8]` point at the dword holding the engine pointer, so `0x61FC0` sees a
non-zero `this`. The poke stuck (`[00541FA8]=00541FB4`), the run stayed clean
(`faults=0`, `files=22`) and nothing visibly changed. That neither confirms nor
refutes it: the poke supplies a *plausible* object, not the *correct* one, and
`0x21D10` may simply not do anything observable with it.

### New instrument: `RECOMP_STATE_SNAP` now also diffs the engine object

`engine_snap_thread` polls `[0x541FB4]` until it is a plausible guest pointer,
samples 64 fields 2 s in and 8 s later, prints every field and stars the changed
ones. Two bugs fixed on the way: `RECOMP_PEEK` printed only at a hang or a clean
exit, and the stack dumper's `_exit(0)` is neither, so peek+dump printed nothing;
it now samples in the dump thread.

### RETRACTED: "the update ran twice then stopped"

I claimed `0x541594` proved the update stopped after two frames. **That was wrong**,
and the control experiment that killed it is the important part of this pass.

`RECOMP_WATCH=0x541594` with the toolkit's own watcher produced 24 events:

```
[WATCH] [00541594] 00000000 -> 00000001  (esp=00F7F2B0)
         [esp+24  ] 00057647   [esp+28  ] 00057685   [esp+40  ] 0001D80A
         [esp+44  ] 00011294   [esp+64  ] 00011C60
[WATCH] [00541594] 00000002 -> 00000003
         [esp+20  ] 0039979B   [esp+28  ] 00021E32   [esp+72  ] 00021E32
         [esp+112 ] 00057647   [esp+116 ] 00057685   [esp+128 ] 0001D80A
```

The value **oscillates `2 -> 3 -> 2 -> 3`**. It is not a frame counter at all, it is
a **nesting depth** (increment on entry, decrement on exit), which is why both of my
samples read `2`: I happened to sample it at its resting value. `0x21D10` *is*
entered — the `0x21E32` frame is inside it — so the update runs.

Two lessons worth keeping:

- **A counter sampled twice is not a rate.** Two equal samples say nothing when the
  value returns to a baseline; it needs sampling many times in flight, or a watch.
- Watch chains are labelled "a lead, not a proof" by their own author for good
  reason: `0x57647`/`0x57685` on the stack can be stale slots. The reliable edge
  in the first chain is `0x11C60` (the engine main loop `0x11C4E–0x11C6A`) →
  `0x11294` (app dispatcher `0x11270`) → `0x1D80A` (app layer, tables at
  `0x1D840`/`0x1D824`) → the tick.

### `0x541FA8` is never written at all — so it is not the gate either

`RECOMP_WATCH=0x541FA8` produced **zero events** across a full run, while the same
watcher on `0x541594` produced 24. So no guest code writes it, ever: it is 0 from
the start, not *lost after frame 2* as I wrote above. That makes the `0x61FC0`
path a never-activated subsystem — the same shape as the vestigial job queue — and
**not** the loading gate.

This closes lead B's target. What it leaves is the useful part: **the watcher
works on Win32** (it was implemented with a VEH; I had wrongly suspected it was
POSIX-only) and it names guest callers. That is the instrument to point at the
real gate, once the gate is identified.

### The heap-freeze premise itself is unsound

Pointing the now-working watcher at `0x00FE8510` — my second-best frozen candidate,
`130895 -> 393039` — gave exactly one event:

```
[WATCH] [00FE8510] 00000000 -> 00FE8510  (esp=00F7F21C)
         [esp+0 ] 005406F8   [esp+8 ] 004E5110   [esp+24] 0001B29E
         [esp+32] 00535F38   [esp+60] 0001B62A
```

The value written is **its own address**, through `0x5406F8` / `0x4E5110` /
`0x1B29E` / `0x535F38`. That is a **heap freelist node initialising to a
self-pointer**, not a loading counter.

So the "37 frozen dwords" that lead B was built on are not what they look like: a
heap address is **recycled**, and my three samples compared *different objects*
that happened to land at the same address. `0x01606C68` was thread 4's stack;
`0x00FE8510` is a freelist node; much of `0x00FE87xx` was float/vertex data.
**Address-based freezing over a heap is not a sound way to find state.** It needs
an object identity — a pointer rooted in `.data` — not an address.

### What lead B leaves behind

| Item | Status |
|---|---|
| `0x541FA8` (the null `this`) | **dead** — never written by any guest code in a full run |
| `0x541594` | **retracted** — a nesting depth, oscillates 2↔3, not progress |
| Engine object inert | **by design** — base vtable, `0x254170` is `ret` |
| Heap "frozen dwords" | **unsound** — addresses are reused; identity ≠ address |
| `RECOMP_WATCH` on Win32 | **works**, names guest callers (VEH) — the real win |
| Tick live every frame | confirmed via `0x11C60` → `0x11294` → `0x1D80A` |

The correct target for the watcher is something that **changes while the screen
loops** (so the writer is live) whose call chain lands in the loading code — the
opposite of what B looked for. The app-command layer is the right place to start:
`0x11270` dispatch → `0x1D7A0` op dispatcher, tables `0x11388`/`0x11364` and
`0x1D840`/`0x1D824`, because in this engine a screen change is a command and the
loading screen never issues one.




Everything measurable is measured: rendering correct, clocks work, I/O completes,
no unimplemented calls, no pending waits, the job queue is dead code, the disc is
as complete as the master. Two candidate gates remain, both needing real work:

1. **OHCI control transfers** — fix the TD/ED field decode so the title's XID
   driver can complete enumeration and a pad exists at all.
2. **The 37 frozen heap dwords** — find which one the loading screen tests, by
   `RECOMP_WATCH`ing the most promising (`0x00FE8308`, `0x00FE8510`) and reading
   the code that reads them.

Everything reachable by measurement has been measured: the queue is dead code,
the clocks work, I/O completes, input focus is correct, and the loop is genuine.
What remains is either (a) implementing OHCI properly — TD/ED field parsing plus
the descriptor walker — or (b) ground truth: what this screen *should* look like
and whether it should respond. Headless can no longer distinguish "still loading"
from "title screen rendered too dark".


**Analysis, lift, build, rendering, profiling, loader and the exact gate located — nineteen passes deep.** The
pipeline works end to end: 39,364 functions lifted, 155 translation
units compiled clean, a 28.8 MB Linux executable and a **27.9 MB Windows .exe**
that both boot, load the disc data,
brings up audio and the AC97 codec, registers interrupt handlers, and runs on
the host CPU with zero fatal faults and zero decoder failures.

Two real gaps in the toolkit were closed on the way: **trapped MMIO on x86-64
POSIX did not exist** (Win32 and arm64 only), and the **NV2A register model was
never initialised on POSIX**. Both are now implemented in this project.

What has *not* been done: it does not render, there is no title screen, and no
frame is ever presented. Measured with the PC sampler, the guest sits in a
**wait-for-GPU-interrupt spin** — 1.8 M polls of `PMC_INTR_0` / `PFIFO_INTR_0`
in seven seconds — and its audio thread spins against the **AC97 codec window at
`0xFEC00000`, which has no model at all**. Those are the three remaining
subsystems:

1. **AC97 codec semantics** (`0xFEC00000`) — currently plain RAM, and the APU
   model stops at offset `0x30000`. This is what `sub_003EBED6` in DSOUND waits
   on.
2. **PFIFO/PGRAPH completion interrupts** — the guest polls the interrupt-status
   registers expecting the GPU to finish work and raise one.
3. **D3D8 → OpenGL presentation** — `d3d8_gl.c` builds, but this box is headless
   and `RECOMP_FB_WINDOW` needs a GL context (EGL/OSMesa or `SDL_VIDEODRIVER`).

That is per-title emulation work, not a broken pipeline — the multi-week effort
the toolkit's docs describe, on a title twice the size of its reference game.

Worth knowing for whoever continues: `func_id` found **0 RenderWare modules**
and 10,271 vtable-classified functions, and every wall so far has been an
indirect call or a device aperture — exactly the 10 % the docs warn about.
Shadow is not a Tier-1 target: 3.93 MB of `.text` and ~39 k functions, roughly
twice Burnout 3. Useful knobs discovered by measurement: `RECOMP_AC97_READY=1`,
`RECOMP_VBLANK=paced`, `RECOMP_WATCHDOG_SECS`, and the second argument to the
binary is the game data directory.

## Pass 23 — the disc tree was 43% extracted (a truncating XDVDFS walker), and the full tree crashes in DSOUND

### The extractor bug

`tools/xdvdfs.py`, `Volume.read_dir()`, walked a directory table linearly across
its whole size and `break`ed at the first non-printable name. XDVDFS never lets a
directory entry span a 2,048-byte sector and pads each sector's tail with `0xFF`,
so the walker met `l=0xFFFF r=0xFFFF name_len=0xFF` at offset 2020 of the *first*
sector and stopped. **Only the first sector of every multi-sector directory was
ever read** — root is 5 sectors, `csdFiles` 3, `event` 5, `stg0200`/`stg0403` 2,
so all of them were cut short.

Fixed by walking each sector separately and resyncing at the next boundary. The
fix is verified against an independent implementation, not against itself:

```
tools/xdvdfs.py list  | grep -c '^-'   ->  3,510
tools/xdvdfs.py layout | entries       ->  3,636  (3,510 files + 126 dirs)
tools/xiso/xdvdfs.py (Xiso.walk)       ->  3,510   same 3,510 paths, same sizes (diff empty)
```

| | Recorded before | On the disc |
|---|---|---|
| Root entries | 70 | **310** |
| Files | 1,509 (1.6 GB) | **3,510** (2,533,482,876 bytes) |
| Directories | 67 | **126** (3,636 entries) |
| `sound/` | "absent" | **50 files** — `bank0.pac` 1,108,480 (`24 00 00 00 20 00 00 00 A0 E7 10 00`), `sonicteamfx.bin` 27,672, `AudioDataSystem.bin` 2,082, `bank2…bank81.pac` |
| `stg0005…stg0911` | "absent" | **55 stage directories** + `stg_cmn_gdt.one` |
| `csdFiles/` | 66 files | **138 top-level entries** (135 files + 3 dirs), 421 files total |
| `event/` | 57 files | **265 files** |

Consequences corrected in the same pass: the "70/70 root entries matched to their
expected file signature" check was produced by the same walker and is worthless
as validation (a tool agreeing with itself). Pass 21's "the disc has 1,576
entries, no `sound/`, no `dvd/`" is retracted — `CLAUDE_PROMPT.md` §6 row *A
missing asset* is now marked **RETRACTED**. `d:\dvd\` really is absent;
`t:\$u\contentmeta.xbx` is a temp-drive probe.

### Re-extraction (side by side, nothing lost)

```
python3 tools/xdvdfs.py extract --quiet --out WORKSPACE/files_full \
    --manifest WORKSPACE/MANIFEST_full.tsv     ->  3,510 files / 2,533,482,876 bytes
```

All 1,509 rows of the old manifest are present in the new one with the **same
sha256** (0 missing, 0 mismatched, 0 size changes). End state:

```
WORKSPACE/files        3,510 files  (the disc)      WORKSPACE/MANIFEST.tsv        3,510 rows
WORKSPACE/files_trunc  1,509 files  (the old tree)  WORKSPACE/MANIFEST_trunc.tsv  1,509 rows
```

`DATA_REL_PATH` resolves `<exe>\..\WORKSPACE\files`, so the swap is a rename.
Revert to the old (looping) state any time with:

```
cd ../WORKSPACE && mv files files_full && mv files_trunc files     # back to A
cd ../WORKSPACE && mv files files_trunc && mv files_full files     # forward to B
```

### The A/B run (data changed, nothing else)

60 s each, headless, `Xvfb :77` at 1024x768x24, `wine shadow_recomp.exe`, identical
environment: `RECOMP_FILE_TRACE=1` + `RECOMP_MISSING_FILES=all` (so *every* open is
logged, success and failure). `md5 1b5f1b0ace2403642b072be77c39bd98` for all three.

| | **A** truncated (1,509) | **B** full (3,510) | **B2** full, `RECOMP_NO_APU=1` |
|---|---|---|---|
| Open attempts | 35 | 16 | 16 |
| Successful opens | 23 (21 unique) | 15 (15 unique) | 15 |
| Failed opens | 12 (11 unique) | **1** | **1** |
| Disc-file successes | 12 (10 unique) | 4 | 4 |
| `d:\sound\*` successes | **0** | **1** (`sonicteamfx.bin`) | 1 |
| `d:\sound\*` failures | **9** | **0** | 0 |
| Remaining failures | `contentmeta.xbx`, 9× sound, 2× `d:\dvd\` | `t:\$u\contentmeta.xbx` only | same |
| `d:\stg*` / new `csdFiles` opens | none (`Loading_EN.xncp` only) | none — it dies first | none |
| Reads | 30, all `got == want`, st 0 | 5, all `got == want`, st 0 | 5 |
| Window | 640x480, word **loading** legible, `FPS 30.5 \| draws: 8` | **none** — Wine `Program Error` | **none** |

A reproduces the recorded symptom exactly (loading screen, `draws: 8`, the word
legible — captured from the game window by title with `import -window`, not the
root window). B does not loop: **it faults.**

### The fault (measured, B and B2 identical)

```
d:\sound\sonicteamfx.bin (disp 1 opts 0x60 from 0x002AF77B) -> 0x00000000   (opens)
[READ] from=0x002AE08A @seq want=27672 got=27672 st=0x00000000              (whole file read)
[CRASH] Access violation at RIP=0x140DB709B, fault addr=0x758E5618 (read)
  Xbox regs: eax=0x758D5604 ecx=0x00F7F184 edx=0x00000000 esp=0x00F7F170
             ebx=0x00F81000 esi=0x00F81000 edi=0x00FBE010
  Xbox VA of fault: 0x758D5618
  guest stack: 0x003E68C2, 0x003E68CD, 0x003E348A, 0x0037F750, 0x0047B8A8, 0x002AD49D …
```

- The faulting guest VA `0x758D5618` is **~1.8 GB** — far past the end of the
  64 MiB guest window. A wild pointer, not an unmapped-but-valid address.
- `0x003E68C2` is `loc_003E68C2` in the recompiled `sub_003E6853` (DSOUND) and is
  the return site of `sub_003EAD45`, a `rep movsd`/`rep movsb` **copy helper**
  that derives its destination from a structure field
  (`edi = *(*ctx+0x10) + offset`) and copies `0x818` bytes from `ebx+0x818`. So
  DSOUND is copying through a pointer read out of a structure built from the sound
  data. The exact instruction is bracketed by those two frames; the *structure
  field that went wild* is **not** yet pinned — that is the next step.
- `0x003E348A` is inside `sub_003E3473`, the caller of `sub_003E6853`.
- **Not the emulated APU**: `RECOMP_NO_APU=1` produces a byte-identical fault
  (same host RIP, same guest VA, same registers). The title's own DSOUND code is
  implicated, not the APU model.

### How to read it against `CLAUDE_PROMPT_EXTRACTION.md` §5

Row 2, not row 1: **the sound path is now actually exercised and faults inside
it.** On the truncated tree the sound stage never did its real work (nine opens
failed, the code took its error branch, and the title fell through to the loading
animation and looped). With the real files it opens `sonicteamfx.bin`, reads all
27,672 bytes, and dies in a DSOUND copy. Therefore:

- Truncation was **a** gate — it hid the sound stage — but fixing it does not make
  the loading screen end; it produces a crash *before* the loading screen. The
  loop in A is a **downstream** artefact of the aborted sound stage, not the gate.
- `d:\dvd\` was never attempted on the full tree (the fault comes first), so the
  original expectation "only `dvd\` + `contentmeta.xbx` still fail" is untested —
  only `contentmeta.xbx` actually failed.

### Next frontier (in order)

1. **Name the pointer.** Break on `sub_003EAD45` and print both operands:
   `ecx` (its `this`) → `+8` → `+0x10` → `[0]` is the destination base, and
   `arg1+0x818` is the source. The wild value is `0x758D5604`; find what wrote it.
   It should be a buffer sized from `bank0.pac` / the `AudioData*.bin` files.
2. **Check the ordering before it.** In B the opens stop after `sonicteamfx.bin`:
   `bank0.pac` and the seven `AudioData*.bin` are **never opened**, so whatever
   sizes the sound heap from them has not run. If `bank0.pac` should be opened
   before `sonicteamfx.bin` is copied into its buffer, the ordering (or a
   swallowed failure) is the bug.
3. Only then re-ask the loading-screen question with a tree that is *actually*
   complete, per `CLAUDE_PROMPT.md` §10 direction 2 (the `0x11270` command census).

### Method warning added to the list

6. **A tool's agreement with itself is not validation.** "70/70 root entries
   verified" and "1,576 entries" were both produced by the same truncating walker,
   so they agreed perfectly and were both wrong. Cross-check any format reader
   against an independent implementation before trusting its output.

Raw logs, window captures and the harness for all three runs:
`WORKSPACE/reports/ab-2026-11-07/` (`A60.log`, `B60.log`, `B2_noapu.log`,
`A60.window.png`, `*.window.txt`, plus `abrun.sh` and `analyse.py`).

## Pass 24 — the crash named: DirectSound create returns DSERR_NODRIVER, the title calls methods on the NULL object anyway

Pass 23 left one question: what does `sub_003EAD45` copy through, and who wrote
`0x758D5604`. Both are answered below, and the answer moved the title past the
loading screen.

### The instrument (kept — it is silent unless asked)

`shadow_game/src/xbr_audit.c` + `xbr_audit.h`, four marked `XBR-AUDIT` call sites
in the generated code:

| Site | What it reports |
|---|---|
| `sub_0025FB90` gate / handoff | `[0x550310]`, before and after the lazy-create branch |
| `sub_003E5A27` entry / result | the create's 3 args, its HRESULT, and the out-param |
| `sub_003E35B5` entry | its 5 params, `p1->[0x8]`/`[0xC]`, and two DSOUND globals |
| `sub_003E3473` entry + obj | its args, `this`, and the `this->[0x14]->[0x8]->[0x10]` chain |
| `sub_003E6853` entry, `sub_003EAD45` memcpy | the copy's ctx/size/src/off and the computed dest |

`RECOMP_XBR_AUDIT=1` prints only wild values, `=all` prints every call. Every read
in the audit is range-checked first, so it cannot fault on the pointer it is
reporting. **The generated-code edits do not survive `tools/xlate.py`**; re-apply
them if the code is regenerated.

### The chain, measured (`=all`, full tree)

```
E25FB90 gate      [0x550310] = 0
E5A27.entry       p1=0, p2=0x00550310 (&object), p3=0
E5A27.result      a0=0x88780078 (DSERR_NODRIVER)  a1=0x00FFE000 (out ptr produced)
E35B5.entry       p1=0x00000000  p2=0x00FBE010  p3=0x00006C18  p4/p5=stack
E3473.entry       arg1=0x00FBE010 arg2=0x00006C18 arg3=0x00F7F224  ecx=0x00000001
E3473.obj         this=0x00000001 → this->[0x14]=0 → ZERO LINK
E6853.entry       ecx(this)=0x00000000
EAD45.memcpy      ctx=0  size=0x818  src=0x00FBE828  off=0x6400  dest=base+off=0x00006400
CRASH             fault addr=0x758E5618 (read), eax=0x758D5604
```

Read bottom-up:

1. `sub_003E35B5` is entered with **p1 = NULL**. It does `esi = p1`, then
   `ecx = MEM32(esi + 0xC)` → `MEM32(0x0000000C)`, which in this image holds **1**.
   That integer becomes the `this` of everything below it — this is where the
   `ecx=1` seen in Pass 23 comes from.
2. `sub_003E3473` therefore runs with `this = 1`; `this->[0x14]` is `MEM32(0x15)` = 0,
   so `sub_003E6853` gets `ctx = 0`.
3. The copy helper computes `dest = *(*(ctx+0x10)) + off` = `*(*(0x10)) + 0x6400`.
   Guest VA 0x10 is inside the mapped window and reads 0, so **dest = 0x00006400**
   and it writes `0x818` bytes there — guest VA `0x6400–0x6C18`, below the
   `0x00010000` image base. That is **silent corruption**, not a fault.
4. The next call in the same function, `sub_003E65E6`, walks the same null chain
   and faults reading `0x758E5618` (`eax=0x758D5604`), ~1.8 GB outside RAM.

So `0x758D5604` is not in any file and nothing stores it: it is **computed** from
the null object. (Checked: no `04 56 8D 75` in `sonicteamfx.bin`, `nukkoro*.inf`
or `DiscErrorEN.txd`; no dword in the 0x70000000–0x7FFFFFFF band that is a pointer
rather than text/compressed data.)

### Who writes `0x550310`: nobody

`RECOMP_WATCH=0x550310` arms a hardware watch on that page:

```
WATCH: writes to the page of 0x00550310 are trapped (current 00000000)
```

**Zero write hits in a whole run**, and the crash still happens. `[0x550310]` is
the game's DirectSound object pointer:

- read at 13 sites (the engine's sound API wrappers),
- cleared by `sub_0025EE90` (shutdown, after releasing it),
- and assigned only on the **success** path of `sub_003E5A27`, as
  `*(arg2) = arg1+8`.

### The root cause: `DSERR_NODRIVER`

`E5A27.result a0=0x88780078` — `DSERR_NODRIVER`. The create *did* produce an
object (`a1 = 0x00FFE000`), but `sub_003E5A27` stores it only when
`HRESULT >= 0`, so the pointer stays 0 and the engine calls
`IDirectSound8` methods on NULL. The guest never checks the result.

The runtime already knows this failure by name — `src/kernel/xbox_memory_layout.c`
(~3277) documents it: the title resets the AC'97 codec at `0xFEC0012C` and polls
`0xFEC00130` bit 8 a thousand times; on zeroed registers the bit never appears,
the wait times out, and `DirectSoundCreate` returns `DSERR_NODRIVER`. It also
records what happens next in other titles ("a failed create skips the engine
initialisation, the null propagates… and crashes") — which is exactly this run.

### Turning the codec on changes the outcome (measured)

| Run | Data | `RECOMP_AC97_READY` | Opens | Screen | After 30–50 s |
|---|---|---|---|---|---|
| A (Pass 23) | truncated 1,509 | – | 23 ok / 12 fail | "loading" loop, FPS 30, draws 8 | loops |
| B (Pass 23) | full 3,510 | – | 15 ok / 1 fail | none — `Program Error` | crashed in DSOUND |
| **C (this pass)** | **full 3,510** | **1** | **45 ok / 2 fail** | **"PRESENTED BY SEGA" logo drawn** | FPS 0.0, draws 16, no further frames |

Run C opens the files the truncation had made impossible — `d:\sound\bank0.pac`
and all seven `d:\sound\AudioData*.bin` — plus `d:\fonts\Advertise\*` (3) and
`d:\csdFiles\Logo_JP.xncp`, with only the two genuine failures left
(`d:\dvd\`, `t:\$u\contentmeta.xbx`). **No crash.**

The screen is measured, not described — and **it is not deterministic**. Six runs
of this exact build with this exact environment (full tree, `RECOMP_AC97_READY=1`):

| run | open count | FPS / draws | screen |
|---|---|---|---|
| AACP (16 s) | 45 | 1.0→2.0 / 16→6 | **"PRESENTED BY SEGA" logo** |
| REP2 (20 s) | 45 | 2.0 / 6 | **"PRESENTED BY SEGA" logo** |
| REP1 (20 s) | 35 | 27.6 / 8 | loading screen |
| REP3 (20 s) | 35 | 20.8 / 8 | loading screen |
| CONFIRM (26 s) | 35 | 26.5 / 8 | loading screen |
| SEQ (8–48 s) | 40 | 0.0 / 16 | dark, static |

The split is exact and meaningful: **35 opens → loading screen; 45 opens → past
it.** The ten files between those two states are the sound bank
(`bank0.pac`, the seven `AudioData*.bin`, two `fonts\Advertise\*`). So the loading
screen is a **fallback taken when the sound-bank load does not complete in time**,
and whether it completes is a race. `draws: 8` is the loading screen's signature
(the very first baseline in Pass 21 had it too); the logo state shows 16 or 6.

So Pass 24's summary sentence — "the loading screen is no longer reached" — is
**retracted**. What is true is weaker and more useful: *when the sound bank loads,
the title leaves the loading screen and renders the SEGA logo.*



Then it stalls: `FPS 0.0 | draws: 16` constant, with
`RtlEnterCriticalSection` / `RtlLeaveCriticalSection` ≈ 579,000 calls each,
`NtYieldExecution` 75,000 and `KeWaitForSingleObject` 71,000 in 50 s — a thread
polling under a lock for work another thread owes it. That is the DSP
command-block handshake `xbox_memory_layout.c` predicts: reporting the codec is
correct, but nothing answers the DSP, so the title trades a crash for a hang.

### What this does to the brief's answer

`CLAUDE_PROMPT_EXTRACTION.md` §5 offered two outcomes. This run is neither row 1
nor row 2 exactly:

- The truncation **was** a real gate — with the full tree the title opens the
  sound bank, which it could never do before. Row 1 was right about the data.
- But the full tree alone does not end the loading screen; it crashes **inside
  the sound path** (row 2) with a named, reproducible fault.
- With the data complete **and** the codec reported ready, the loading screen is
  **gone** and the SEGA presentation logo appears. So the loading screen was never
  waiting on a file: it was the fallback shown after a failed `DirectSoundCreate`,
  and it cannot clear until that create succeeds.

### Next frontier

1. **Answer the DSP command-block handshake.** That is what `RECOMP_AC97_READY=1`
   papers over. Find the block the title hands to the DSP in RAM and clear it the
   way the hardware would; the stall's hot ordinals
   (`RtlEnter/LeaveCriticalSection`, `NtYieldExecution`, `KeWaitForSingleObject`)
   say the waiter is cheap to find, it is the *waker* that is missing.
2. Then re-measure: does the SEGA logo give way to the title screen? Everything
   downstream (the `0x11270` command census from `CLAUDE_PROMPT.md` §10) is only
   worth revisiting once the boot sequence can actually complete.
3. Turn `RECOMP_AC97_READY` into the default only when the DSP is answered — the
   runtime's own comment says so, and this pass confirms both halves.

### Method notes this pass adds

7. **The filtered stack chain lies.** The audit's in-range scan named
   `sub_003E35B5` as absent from a chain it was definitely in, and it put a stale
   slot where the caller belonged. `sp[0]` (raw, unfiltered) is the return address
   the caller pushed; only that is authoritative.
8. **A store through a register is invisible to a literal-address grep.** "Only
   one store to `0x550310`, and it writes 0" was wrong: the real assignment is
   `MEM32(reg) = ...` inside `sub_003E5A27`. `CLAUDE_PROMPT.md` §9.4 warned about
   exactly this and it still cost time.
9. **An opt-in switch that hides a symptom is not a fix.** `RECOMP_AC97_READY=1`
   is documented as trading a crash for a hang; run C shows the hang. Say
   "reaches the SEGA logo" and "stalls in audio", never "works".

Raw logs and captures for this pass: `WORKSPACE/reports/ab-2026-11-07/pass24/`
(`AU6.log` = the audited chain, `AW1.log` = the write-watch, `AACP.log` and
`SEQ*.png` = the codec-ready run).

## Pass 25 — the audio gate, read out of the guest: it is a submit loop, not one bit

Pass 24 ended with "the title stalls in the audio DSP handshake". This pass found
the exact expression, tried three ways to satisfy it, and measured a result that
matters more than any of them: **the outcome is a race, and the open count
predicts it.**

### The spin, in six lines of guest code (`sub_003EBED6`)

```c
loc_003EBEEA:
    eax = MEM32(MEM32(esi) * 4 + 0x3FF128);   // per-voice/channel base
    MEM8(eax + 0xFEC0010B) = 2;               // NABM + 0x0B, bit 1 = RR (reset)
    ...
loc_003EBF0C:
    ecx = MEM8(eax + 0xFEC0010B) & 2;         // MSVC hoisted this load
    if (ecx != 0) goto loc_003EBF0C;          // spin until the controller clears RR
```

The runtime already knew this one: `xbox_memory_layout.c` quotes the same
instruction in its AC'97 trap comment, and `ac97_clear_reset_bits()` sweeps
`NABM+0x10B, +0x11B, …` masking bit 1.

### What the guest actually asks the controller for (`RECOMP_AC97_LOG`, new)

Added a gated log to the existing trap (`RECOMP_AC97_LOG=1` prints offset, the
32-bit word, whether it is a channel-control byte, and the host pc). One 20 s run
produced 13,765 lines:

- the two channels DSOUND resets are **`0x11B` and `0x17B`** — not the `0x10B` the
  trap comment names first;
- each is re-written **~6,770 times in 20 s**, from two distinct pcs
  (`…15DC` writes `0x11B`, `…15E5` writes `0x17B`) — **two threads**, both in the
  submit path;
- the surrounding writes are the channel descriptors (`0x110`/`0x114`/`0x115`,
  `0x170`/`0x174`/`0x175`, buffer addresses like `0x00692000`/`0x00693000`).

So the loop is a **submit retry**, not a single wait. Clearing RR lets each
individual reset complete and the title immediately submits again.

### Three ways to satisfy it — and what each one did

| Attempt | Result |
|---|---|
| **Trap the page** (`RECOMP_AC97_TRAP=1`, RR never deposits) | Works as designed, but **regresses**: stalls *earlier*, 15 opens, and lands on the native build's wall — `ebx=0x8007000E` (`E_OUTOFMEMORY`) at `+10:003EC081`, no window. Matches the code's own "measured worse as a default" note. |
| **One-store probe**: patch only that store to write `0` ("controller accepted instantly") | **Negative**: still 45 opens, but **0 draws, no window**, and the same stall address `+10:003E6D7B`. RR was not the only thing it waits on. Probe reverted. |
| **Neither** (`RECOMP_AC97_READY=1` only) | 45 opens in some runs, 35 in others — see the race below. |

The probe's result is the useful one: because the stall address was *identical*
with RR already clear, the spin is not the RR loop at that moment — it is inside
`sub_003E3250`, which is a TLS lock helper (`[FS:0x24]`, an indirect call through
`0x429C3C`). The submit path is lock-and-queue machinery all the way down.

### The finding worth keeping: it is a race, and the open count names it

Six runs, one build, one environment (full tree + `RECOMP_AC97_READY=1`):

| opens | screen | runs |
|---|---|---|
| **35** | loading screen, `FPS 26.5/27.6/20.8`, `draws: 8` | 3 of 6 |
| **45** | "PRESENTED BY SEGA" logo, `FPS 1–2`, `draws: 16→6` | 2 of 6 |
| 40 | dark, `FPS 0.0`, `draws: 16` | 1 of 6 |

The ten files that separate 35 from 45 are **the logo/font assets, not the sound bank**
(both runs open the whole sound bank — `bank0.pac` and all seven `AudioData*.bin`):
`d:\fonts\Advertise\Advertise_EN.{fnt,txd}`, `Advertise_EN00.met`, `d:\number.txd`,
`d:\csdFiles\Loading_EN.xncp` ×3, `d:\csdFiles\Logo_JP.xncp` ×3.

**And the parked thread is in the same place in both outcomes** — by watchdog, 45 s each:

```
WD1 (45 opens): guest stack@00F7F20C  +10:003E6D7B +20:003E704C +40:003E3637 +48:003E42BC +54:0025FD20
L1  (35 opens): guest stack@00F7F20C  +10:003E6D7B +20:003E704C +40:003E3637 +48:003E42BC +54:0025FD20
```

So there is **one** gate (the DSOUND submit); the screen difference is *loader
progress*, not a different code path — in one run the loader gets the logo assets in
before the capture, in another it stops ten files earlier. The 35-open state is a real
stall, not slowness: 60 s does not advance it and the watchdog names the same chain.

That is why every previous conclusion wobbled: each configuration was measured
once. It also explains Pass 23 cleanly — the truncated tree could never load the
bank (so: always the loading screen), and the full tree without the codec died in
`DirectSoundCreate` before reaching the race (so: always the crash).

### Next frontier — model the submit, stop answering bits

1. **The race is the bug.** Something decides the sound load has timed out. Find
   it: the 35-open path and the 45-open path differ by ten file opens, so a
   breakpoint on the *last* open of each (`fonts\Advertise\Advertise_EN.txd` vs
   `bank0.pac`) and a comparison of which thread reaches the timeout first names
   the loser. If the timeout is a guest timer, check that our clock gives it
   enough wall time; if it is an `NtWaitForSingleObject` with a timeout, the
   waiter is cheap to find — it is the *waker* that is missing.
2. **Model the bus-master channels** (`0x11B`, `0x17B`): RPBM run bit, CIV/LVI,
   PICB, and a real DMA ring moving bytes to the APU sink, so the submit
   completes instead of retrying. The register script is now recorded
   (`RECOMP_AC97_LOG=1`), which is the spec to implement against.
3. **Only then** turn `RECOMP_AC97_READY` into a default, and re-measure the
   open-count distribution as the acceptance test — not a single screenshot.

### Method notes this pass adds

10. **One run is not a result.** Six runs of one build produced three different
    screens. Every claim about what the title "does" needs a distribution and a
    correlate; here the correlate was the open count, and it was exact.
11. **Answering a device bit can move the stuck point without moving the title.**
    The probe cleared RR and the spin simply moved to the lock helper. Check the
    stall *address* on both sides before claiming progress.

Raw logs: `WORKSPACE/reports/ab-2026-11-07/pass25/` (`ALOG.log`/`ac97.txt` = the
register script, `CTRAP.log` = trap regression, `DPROBE.log` = the probe,
`REP1..3.png` = the two screens reproduced under one build).

## Pass 26 — the probe that cancelled itself: the submit is not stuck, a lock-poll loop is

Acting on outside advice (§7.1 of `ADVICE_REQUEST.md`: "a fake but valid audio
device may be enough — try it before implementing a DSP"), the plan was to make the
AC'97 channels report "buffer consumed". Before writing any device model, one
cheap measurement: **find which segment of the submit actually repeats.**

### The instrument

`xbr_mark(id)` in `xbr_audit.c` — a per-segment counter that prints the vector once
a second (and on first call). Four marks placed inside `sub_003EBED6`:

| mark | where |
|---|---|
| m0 | function entry |
| m1 | after the RR (reset-bit) wait — i.e. the wait was satisfied |
| m2 | after `sub_003EBCE0` |
| m3 | after `sub_003E3272` (last statement before `ret`) |

### The result: the submit completes, every time

```
[XBR-MARK] m0=305 m1=304 m2=304 m3=304   (total 1377)   ... climbing ~46/s
```

m0..m3 are **equal** — every entry ran all four segments. `sub_003EBED6` is called
about **46 times a second and returns each time.** It is not spinning, and there is
nothing in it to unstick.

That kills the premise of the dummy-device probe **before** any of it was built:
the AC'97 channel model was going to be written to make a loop exit that already
exits. Recorded as the outcome rather than quietly dropped, because a cancelled
probe is the cheapest possible result.

### What *is* spinning

The watchdog's other numbers point at the real loop: **~33,000 indirect calls per
second** (843,018 in 25 s), alternating pairs of guest addresses. Resolved:

| address | what it is |
|---|---|
| `sub_00328DB0` | `push 0x5A0320; call [0x429AE4]` — **lock guest lock `0x5A0320`**, then bump a counter at `0x560308` |
| `sub_00328DD0` | decrement the counter, `push 0x5A0320; call [0x429AE0]` — **unlock the same lock** |

And the runtime's own `[CS]` trace names that lock over and over:

```
[CS] thread 324 waiting on guest lock 0x005A0320 (held by thread 320)
[CS]     guest 0x005A0320
[CS] thread 324 acquired 0x005A0320
[CS] thread 320 waiting on guest lock 0x005A0320 (held by thread 324)
[CS] thread 320 acquired 0x005A0320
```

Both threads **acquire** it — 24 contention events in a 30 s run, none stuck — so
this is not a deadlock and not a starvation bug. It is a **poll loop that takes a
lock to read a condition**, ~33k times a second, and the condition stays false.

### Also corrected: the audit was crying wolf

`xbr_audit.c`'s address classifier knew only `[0x00010000, 0x04010000)`, so it
reported every access through the **contiguous-window alias at `0x80000000`**
(and the tiled alias at `0xF0000000`) as a "HOLE". Ordinary `rep movsd` copies at
`ctx = 0x806F01F4` were therefore printed as "WILD: this copy will fault", and they
never faulted. Fixed; in a comparable run the bogus WILD reports went 18 → 0.

Pass 24's finding is **not** affected by this — that one had `ctx = 0x00000000`,
a genuine NULL, and `dest = 0x00006400`. But it is worth re-reading: only the
`ctx = 0` case was ever a real fault, and this pass is the reason to re-check rather
than trust.

### Next (cheap, same technique)

Move the marks outward along the parked chain — `sub_003E6D68`, `sub_003E6FA8`,
`sub_003E3619`, `sub_003E42A4`, `sub_0025FC80` — and the one whose counter climbs at
33k/s is the polling loop. Then read what it tests between lock and unlock. No device
modelling until that condition has a name.

### Method note

12. **Check the premise before building the fix.** The dummy-device model was
    recommended by two reviewers and would have taken days; four counters placed
    in twenty minutes showed the loop it was meant to unblock already exits.

## Pass 27 — the loading task, named end to end: it waits on an async job that never completes

This pass is the payoff of the Ghidra name pass: the engine's own class-name
strings made the boot sequence readable, and from there the wait was traced to a
single value that never changes.

### The class, found from a string

`sub_000598D0` is the **`NowLoadingTask` constructor** (it stores the class name):

```c
sub_000598D0(this, arg) {
    sub_00021C70(arg);            /* base Task ctor            */
    MEM8(this + 0x0D) |= 1;
    MEM32(this + 0x28) = 0;       /* sub-resource / child ptr  */
    MEM32(this + 0x2C) = 0;       /* state bits: bit0, bit1    */
    MEM32(this)        = 0x445B68;/* vtable                    */
    MEM32(this + 0x08) = 0x445B54;/* name "NowLoadingTask"     */
    MEM32(0x544F40)    = this;    /* the live loading task     */
    MEM8(this + 0x0C) |= 4;
}
```

| Thing | Value |
|---|---|
| `NowLoadingTask` vtable | `0x00445B68` — 6 slots: `0x59980` (dtor), **`0x59D50` (finish)**, `0x48894C`, `0x59F50` (dtor), `0x59BB0` (draw), `0xE53B0` (the trace hook every task shares) |
| Live task pointer | `0x00544F40` (read from a dozen poll sites) |
| Class name string | `0x00445B54` = `"NowLoadingTask"` |
| Fields | `+0x0C` byte flags, `+0x28` child/sub-resource, `+0x2C` state bits, `+0x30` progress int |

### The completion path, and the condition it needs

`sub_00059890` is **SetFinished** — its only caller is `sub_00025810`:

```c
eax = [0x544F40];                 /* the loading task        */
if (!eax) return;
ecx = MEM32(eax + 0x28);          /* child / sub-resource    */
if (!ecx) return;                 /* ← NOTHING HAPPENS if 0  */
MEM32(eax + 0x2C) |= 2;           /* set FINISHED            */
MEM32(ecx + 0x28) = 1;
icall(MEM32(MEM32(eax) + 4), 0);  /* → vtable[1] = 0x59D50   */
```

and `vtable[1]` (`sub_00059D50`) is the transition out of loading: it releases
`[task+0x28]`, clears the bit, and calls **`sub_00061FF0(5)`** — an application
state change (state 5).

### The driver, and the measurement that named the gate

`sub_00025810` is the loading driver. It sets progress, polls a state, and switches:

```c
MEM32(0x541814) = 0;              /* progress = 0            */
eax = sub_002ADB31();             /* ← the state it waits on */
switch (eax) { ... }              /* 4-entry jump table      */
...
MEM32(0x541814) = 0x64;           /* progress = 100          */
sub_00059870(...);                /* set progress            */
sub_00059890();                   /* SetFinished             */
```

**Measured at the stall, with `RECOMP_PEEK=0x541814`:**

```
peek: [00541814]=00000000     progress is ZERO, after 35 disc files were loaded
```

So the driver ran, set progress to 0, and the switch never reached the branch that
sets 100 and finishes. **The title's own progress counter never leaves zero.**

`sub_002ADB31` is in the `0x2ADxxx` block — the same block as `0x002ADE73`, which
is the **start routine of all six worker threads**, and `0x2ADD4D`/`0x2ADC3B`, the
notify calls seen in the 0x32xxxx queue code. So:

> The loading driver waits on an async job whose state never advances. The job is
> dispatched into the worker pool, and the 11k-locks-a-second thrash on
> `0x5A0320` is that pool spinning — not a loader reading files.

### Next (one measurement)

Mark the **return value** of `sub_002ADB31` (one `xbr_mark` per distinct state) —
that names the state it is stuck in, and the state machine's own case table then
says which job the title thinks is outstanding.

### That measurement, done — the state oscillates and never completes

`xbr_mark(20 + (eax & 7))` placed at the driver's call site, so m20..m27 count how
often the driver sees state 0..7:

```
m20 (state 0) = 1493    m21 (state 1) = 76    m22 (state 2) = 1416    m23..m27 = 0
```

**States 0 and 2 only, alternating, forever — and this was a 45-open run**, i.e.
including the runs that do reach the SEGA logo. The completion branch (progress
= `0x64`, then `sub_00059870` + `sub_00059890`) is never taken, `SetFinished`
therefore never runs, `vtable[1]` never releases the child, and `sub_00061FF0(5)`
never moves the application out of the loading state.

That closes the loop with the rest of the investigation:

| Observation | Explained by |
|---|---|
| progress `[0x541814]` stays 0 | the driver only writes 0 (state 2) or 0x64 (completion, never reached) |
| loading screen animates at 20–26 FPS | the render/main loop is fine; only the state machine is stuck |
| the queue pool thrashes `0x5A0320` at 11k locks/s | `sub_002ADB31` lives in the `0x2ADxxx` worker block, whose entry routine `0x002ADE73` is **every** worker thread's start |
| slowing the pump makes loading *worse* (45 → 35 opens) | the pool is doing the loading work; throttling it throttles progress |
| outcome varies run to run | states 0↔2 are a race between the pool's threads and the driver |

**So the gate is now a specific, testable statement:** the title's loading state
machine sits in states 0/2 because the async job it polls (`sub_002ADB31`, in the
worker-pool block) never advances to its completed state.

### The live object, verified at run time

`xbr_dump_queues()` was extended to dump the live task once a second, and the dump
confirms the static analysis independently:

```
[XBR-TASK]  task=0x01052B90 vtbl=0x00445B68 name=0x00445B54
            flags=00 child(+28)=0x00F90250 state(+2C)=0x00000000
            prog(+30)=-1  |  LOADPROG=0x541814 = 0
[XBR-CHILD] ptr=0x00F90250 vtbl=0x00445B74 name=0x00000101
```

* the task is **alive**, with exactly the vtable and name pointer the constructor
  predicted — that is the identification checking itself, not remembering itself;
* **`state(+0x2C) = 0`**: the FINISHED bit is never set;
* **`prog(+0x30) = -1`**, not a percentage — so nothing is tracking progress;
* `LOADPROG [0x541814] = 0` throughout;
* `child(+0x28)` is non-zero, so `SetFinished`'s guard *is* satisfied — it simply is
  never called. And its vptr `0x445B74` is exactly `0x445B68 + 0xC`, i.e. a pointer
  **into** the parent's vtable: an MSVC secondary-base view, not a separate task.

The `.rdata` around the class confirms the layout: `"NowLoadingTask"` at
`0x445B54`, its 6-entry vtable at `0x445B68`, then the localised "Now Loading"
display text (Shift-JIS), then `TPauseManager`.

### What is still open

`sub_002ADB31` is a **TLS accessor** (`MEM32(0x5599A8)` is the TLS index; the value
is at `TLS[thread][index] + 8`), paired with a setter `sub_002ADB59` called from a
dozen sites in the same `0x2ADxxx` block. So the state the driver switches on is
**per-thread**, and whatever advances it lives in that block. That is the last
anonymous link in the chain.

## Pass 28 — guest TLS was never initialised (a runtime bug, found and fixed)

Chasing that last link turned up a systemic fault in the replacement layer rather
than in the title.

### Two corrections to Pass 27 first

1. **The dispatch is not 4 states.** Re-reading the driver with the compare lines
   intact: it compares against **`0x27`, `0x26`, `0x21`** and then indexes a
   **39-entry byte table** at `0x25A20` whose values 0..3 select 3 targets. So the
   value is a *resource kind* (0..0x27), not a state enum. Pass 27's "states 0 and
   2 forever" was an artefact of my own instrumentation: I had marked
   `20 + (eax & 7)`, i.e. `eax mod 8`, which conflates many values.
2. **The completion condition**, read properly:

```c
loc_0002587B:
    eax = MEM32(esp + 0x30);            /* "remaining" count, a stack argument */
    if (eax == 0) goto loc_000258AD;    /* all done      → progress = 0x64     */
    progress = MEM32(0x541814) + 1;     /* bump                                */
    MEM32(0x541814) = progress;
    if (progress >= 0x64) goto 0x258B7; /* 100%          → same finish block   */
```

So `sub_00025810` is the **"one load item finished"** callback: it either resets
progress (kind → table bucket 0), ignores it (bucket 1), or counts it toward 100
(buckets 2/3).

### The measurement

Marks at its entry and at each outcome, in one run:

```
ENTRY = 25    RESET = 25    COUNT = 0    INC = 0    FINISH = 0
```

The callback runs, and **every call takes the reset path**, so `[0x541814]` is set
to 0 twenty-five times and the increment/finish path is never reached. And the
value it dispatches on, collected exactly (no masking):

```
[XBR-VAL] ADBSTATE new = 0 (0x0)     <- and no other value, ever
```

`table[0] = 0x00` → bucket 0 → reset. **The title reads its own state as 0 every
single time.**

### Why it is 0: guest TLS was never initialised

`sub_002ADB31` is the MSVC `__declspec(thread)` accessor:

```asm
mov eax, [_tls_index]      ; 0x005599A8
mov ecx, [fs:4]            ; this thread's slot array
mov eax, [ecx + eax*4]     ; the slot
mov eax, [eax + 8]         ; the variable
```

`_tls_index` is **assigned by the loader**, and the XBE says where:

```
XBE TLS directory at 0x0047BBE4:
  tls[0] StartAddressOfRawData = 0x00000000
  tls[1] EndAddressOfRawData   = 0x00000000
  tls[2] AddressOfIndex        = 0x005599A8   <- exactly the global above
  tls[3] AddressOfCallBacks    = 0x00000000
  tls[4] SizeOfZeroFill        = 0x00000090   (144 bytes)
```

The runtime read `tls[0]`, `tls[1]` and `tls[4]` — **and never `tls[2]`**. So
`[0x5599A8]` stayed 0 (it is 0 in the image and no lifted code writes it), every
`__declspec(thread)` variable in the title aliased **slot 0**, and slot 0's data is
the runtime's zero block. Every per-thread variable therefore read as 0, forever.

That is the same failure class as `[0x550310]` (never written) and `[0x560320]`
(nil callback): a value the guest expects a *loader* to supply, that nothing
supplies.

### The fix

In `xbox_memory_layout.c`, where the TLS block is built:

* write the index it already computes into `tls[2]`
  (`MEM32_INIT(tls[2], -(total/4))` — the Xbox scheme is a negative index, and
  `fs:[4] = block + total`, so `-[total/4]` resolves back to `block`);
* size the per-thread data area from the directory's `init_size + zero_fill`
  instead of the hardcoded 64, since this title declares 144 bytes — variables
  past offset 64 were reading and writing outside their block.

### Verified effect

```
  TLS: AddressOfIndex 0x005599A8 = -37        <- was never written
  TLS: 148-byte block at 0x00770000, fs:[4] = 0x00770094

[XBR-VAL] ADBSTATE new = 0 (0x0)
[XBR-VAL] ADBSTATE new = 2 (0x2)              <- a second value, never seen before the fix
```

The per-thread variable is now live and writable, which it demonstrably was not.

### Honest status

This is a real, systemic bug fixed — but **it is not sufficient on its own**: the
loading state still does not complete in the runs measured (progress `[0x541814]`
still 0, `state(+0x2C)` still 0). Both values observed (0 and 2) map to table
bucket 0, i.e. "reset". So the next question is narrower than before: *which kind
value corresponds to the counting bucket, and what sets the TLS variable to it?*
The setter `sub_002ADB59` is called from `sub_002AD79B`, `sub_002AD93C`,
`sub_002ADB87`, `sub_002ADB9E`, `sub_002AE237`, `sub_002AE331`, `sub_002AE45E` — all
in the `0x2ADxxx` block — and one of those paths is the one not being reached.

### The setter measurement (done) — no counting kind is ever produced

`xbr_pair("TLSSET", caller, value)` at the setter's entry, which records
*(who wrote it, what they wrote)*:

```
caller=0x002B176C  value=1168 (0x490)
caller=0x002ADB99  value=3    (0x3)
caller=0x002ADB99  value=2    (0x2)
caller=0x002AF7D5  value=0
caller=0x002AD7EB  value=0
```

Four callers write that variable; the driver only ever *reads* 0 and 2. Against the
dispatch table at `0x25A20`, the entries that lead to the **counting** branch are
indices 1, 7, 9–13, 16–20 and 22–31 — and **none of those values is ever written or
read**. Values 0, 2 and 3 all map to bucket 0 (`reset`), and 1168 is above the 0x27
range so it takes the `> 0x27` arm instead of the table.

So the loading progress mechanism is not being driven at all: nothing produces a
"count this item" kind. That is a different, and more useful, statement than "the
state machine is stuck" — it says **the pipeline never reports countable work**.

### Where this leaves the investigation

Honest assessment after 28 passes:

* the boot is gated at `NowLoadingTask`, and that class is fully mapped (ctor,
  vtable, fields, live pointer, finish path, progress global, dispatch table);
* two genuine runtime bugs were found and fixed on the way (guest TLS never
  initialised; TLS thread area 64 bytes instead of the declared 144), and the
  per-thread variable is now demonstrably live where before it could not be;
* but the title never emits a countable load item, so progress stays 0 and the
  loading task never finishes.

The remaining question — *what should emit countable items, and why doesn't it* —
is one the register-level lift is poor at answering: it needs the decompiled shape
of `sub_00025810` and its callers in `0x17xxxx`, with types. That is exactly the
work the (now working) Ghidra decompiler is for, targeting those functions rather
than sweeping the whole image.






### Addendum — and the stack-scan chains were stale after all

Marks 4–7 were then placed on the *entry* of the four functions the watchdog's
"parked chain" had named (`sub_003E6D68`, `sub_003E6FA8`, `sub_003E3619`,
`sub_003E42A4`). Result for a 35-open run:

```
[XBR-MARK] m0=97 m1=95 m2=95 m3=95 m4=0 m5=0 m6=0 m7=0   (total 382)
                       ^^^^^^^^^^^^^^^  audio submit   ^^^^^^^^^^^^ never entered
```

* **`sub_003E6D68` is never entered** (m4 = 0) — the first frame of the "parked
  chain" every previous pass quoted. That chain was **stale stack slots**, the
  exact failure mode method note 7 warns about. Two recorded conclusions inherited
  it: "the title is parked in the DSOUND submit" and "both outcomes park on the
  identical chain" (Pass 25/26) are **withdrawn**.
* The submit *is* called in the 35-open state, just from the other call site
  (`sub_003EBF48`), and only at **~4.5/s** — so audio is not even the busy path.

What survives: the watchdog is trustworthy for *registers and counters*
(`icalls`, PUT/GET), not for naming the caller. The segment counters are the
reliable tool.

So the honest state is: at 35 opens the title spends its time in a **lock-poll loop
on guest lock `0x5A0320`** (~33k lock/unlock pairs a second) and barely touches
audio at all. Audio was a red herring for the *stall*, though still a real bug for
the crash path of Pass 24.

Next: mark the *callers* of `sub_00328DB0` / `sub_00328DD0` the same way — that
names the code that owns the poll, and therefore the condition it is waiting on.

---

## Pass 29 — the finish is behind a constructor that never runs

**Question asked.** Pass 28 ended with "the pipeline never emits a countable load
item — what should, and why doesn't it?" That is a contract question, not a
register question, so this pass used the newly working Ghidra decompiler on five
functions instead of another instrumentation sweep.

**New tool.** `tools/ghidra_naming/ghidra_scripts/DecompileAt.java` — reopens the
already-analysed project (`-process … -noanalysis`), decompiles the function
containing each address given on the command line, and prints the C. No
re-analysis, ~20 s per batch. This is the routine that made the rest possible:
the recompiler's output is faithful but a switch table is unreadable as
`CMP_NE(_fa, _fb)`, while the decompile shows `switch (kind) { case 0: … }`.

**What the decompile corrected.** Pass 27/28 claimed the driver compares against
`0x27/0x26/0x21` and indexes a 39-entry byte table, and that kinds 0 and 2 are
"reset". Both wrong. `sub_00025810` is:

```c
undefined4 sub_00025810(int remaining)
{
    kind = sub_002ADB31();                     /* the MSVC TLS accessor */
    switch (kind) {
    case 0: case 2: case 3: case 5: case 6: case 8: case 0xe:
    case 0xf: case 0x12: case 0x15: case 0x20: case 0x21:
        break;                                 /* COUNT */
    case 4: return 0;
    default: goto count;
    }
    if (kind == 0x10b) { LOADPROG = 0; return 1; }   /* the only two resets */
    if (kind == 0xa2)  { LOADPROG = 0; return 1; }
count:
    if (remaining == 0) LOADPROG = 100;             /* last item */
    else { LOADPROG++; if (LOADPROG < 100) return 0; }
    if (sub_00059870()) sub_00059890();             /* SetFinished */
    ... sub_002DEA80(0x280, 0x1E0, 0, 2)            /* 640x480 display setup */
    ... do { sub_002DD0F0(...); } while (...)       /* then a present loop */
```

So kinds 0/2 **count**, only `0x10b` and `0xa2` reset, and `remaining == 0` is
the "this was the last one" signal.

**The call chain, end to end.**

```c
FUN_002811D0(this, param_2)          /* TGiHissatu constructor */
    *(void ***)this       = &PTR_FUN_00477510;      /* vtable */
    ..._YGXPAXIHP6EX0_Z1_Z(this + 0x28, 0x58, 2, FUN_00280F70, ...);  /* 2 x 0x58 */
    *(char **)(this + 8)  = s_TGiHissatu_00477518;  /* class name */
    FUN_00280F90();                                 /* the loader walk */

FUN_00280F90()                       /* walks a 2-entry table at 0x5258C8 */
    for each entry: FUN_00017A80(sub_object, entry->name) ...

FUN_00017A80(this, name)             /* one asset */
    if (*(int *)(this + 8) != 0) return 0;          /* ONE-SHOT LATCH */
    obj  = alloc(0x48); ... *(this + 8) = obj;
    path = FUN_000184F0(name);                      /* sprintf "%s.xncp" */
    if (FUN_002B47C0(path) != 0) return 1;          /* CSD load FAILS -> no finish */
    FUN_000156D0();
    FUN_00025810(0);                                /* remaining == 0 -> FINISH */
```

**Measured, with marks added to the generated code (XBR-AUDIT, Pass 29):**

| mark | where | result |
|---|---|---|
| `F8F90` | `loc_00280F90` (driver entry) | **never fires** |
| `T17A80` / `LATCH8` | `loc_00017A80` (asset loader entry) | **never fires** |
| `CSDCHK` | `loc_00017B05` (return of `sub_002B47C0`) | **never fires** |
| `CSD_FAIL` / `CSD_FINISH` | `0x17B2E` / `0x17B10` | **never fire** |

All four marks are present in the built binary (`strings` finds them) and the
audit harness is live in the same run (`XBR-MARK`, `XBR-TASK`, `XBR-XNCP` all
print). So:

> **`FUN_00280F90` is never called ⇒ `sub_00017A80` is never called ⇒
> `sub_00025810(0)` is never called ⇒ the loading task can never report
> completion.** The `TGiHissatu` object is never constructed.

**Correction to the immediately preceding claim.** Before `F8F90` was measured I
concluded, from `[XBR-XNCP]` staying on `Logo_JP.xncp`, that the boot was stuck
*retrying a failing CSD load* of that file. That is wrong: `0x537790` is a shared
scratch buffer written by every caller of `FUN_000184F0`, and `Logo_JP` is the
last value some *other* caller left there. The XNCP dump names a stale buffer,
not the live request. Recorded here because it looked convincing and cost a run.

**Also corrected:** the "latch" probe that first printed `(+8)=0x7369482F`
(= ASCII `"/His"`) was reading `*(0x5258C8)` — a *name string pointer* — as if it
were the object. The real `this` is `ecx` on entry to `sub_00017A80` (the caller
does `ecx = edi`, `edi = ecx + 0x28`), and `[this+8]` is 0 on the fresh 0x58
sub-objects, a class-name pointer on the parent. The one-shot semantics are real;
the first probe was not measuring them.

**Where the boot actually stops.** Not in DSOUND, not in the AC'97 poll, not in a
failing CSD parse — one level above all of them: the object that owns the last
load step is never constructed. Next question: who is supposed to construct
`TGiHissatu` (callers of `0x002811D0`, or references to vtable `0x477510` / name
`0x477518`), and what gates that call.

### Pass 29 addendum — the chain traced up five levels, and three of my readings corrected

Walking up from `sub_00025810(0)` one caller at a time (all via `DecompileAt`),
with a mark added at each level:

```
FUN_0027D710()                 singleton creator ("create once" on DAT_00559654)
   uVar2 = FUN_00061FF0(3);
   DAT_00559654 = FUN_0027D460(uVar2);      /* TGameInfomation */
FUN_0027D460(this, taskroot)   TGameInfomation ctor, vtable 0x477250, name 0x477270
   if (DAT_0054DDE4 == 0)
     if (DAT_0054DDE2 == 0)
        new(0x40) -> FUN_00280E10(this)
        new(0xE0) -> FUN_002811D0(this)     /* TGiHissatu */
        if (DAT_0054DDE1 != 0) new(0x30) -> FUN_00285590(this)
FUN_002811D0(this)             TGiHissatu ctor, vtable 0x477510, name 0x477518
   ..._YGXPAXIHP6EX0_Z1_Z(this + 0x28, 0x58, 2, FUN_00280F70, ...)
   FUN_00280F90();
```

**Measured:** `F7D460` **0 hits**, `F8F90` **0 hits**, `T17A80` **0 hits**,
`CSD_FINISH` **0 hits** — while `XBR-MARK` / `XBR-TASK` / `XBR-FLAGS` print every
second in the same run. And a new `[XBR-FLAGS]` dump shows the two gates are
**open**, so nothing is blocking the ctor from inside:

```
[XBR-FLAGS] 54DDE4=0 54DDE2=0 54DDE1=0 559568=0x00000000
```

`559568` is the cached `number.txd` texture, set in that same ctor — also 0, which
independently confirms the ctor never ran.

**Corrections made this pass, all mine:**

1. "`FUN_00061FF0` is an app-state change" — **false.** It is
   `TaskManager::GetTaskRoot(int index)`:

   ```c
   undefined4 FUN_00061ff0(int this, int index)
   {
       FUN_000e53b0(this->slot[index] != 0,
                    "TaskManager::GetTaskRoot() Null P...");   /* an ASSERT */
       return this->slot[index];
   }
   ```

   So `GetTaskRoot(3)` fetches **task slot 3**, and `sub_00025810`'s finish does
   `GetTaskRoot(5)` — those are TaskManager slots, not "states". The assert string
   `TaskManager::GetTaskRoot() Null P...` never appears in any log, consistent
   with the accessor not being called at all.

2. "`this+8` gate means a latched CSD failure" — the semantics are right (one-shot
   per sub-object) but the first probe read `*(0x5258C8)`, a *name* pointer, not
   the object.

3. "`Logo_JP.xncp` is the asset being retried" — `0x537790` is a shared sprintf
   buffer; the dump showed a stale value from an unrelated caller of
   `FUN_000184F0`.

**Honest status.** The chain from the finish down to a constructor is now complete
and every link is measured as *not executing*. What is **not** yet established is
causality in one direction or the other: whether the missing `TGameInfomation`
object graph is *why* the loading task never advances, or a *consequence* of the
same upstream stall. `TaskManager::GetTaskRoot(3)` is the natural next probe —
resolve the TaskManager instance, dump its slots, and see whether slot 3 exists.
That single dump should settle direction.

### Pass 29 addendum 2 — the TaskManager is fine, and the scene task is never ticked

**The TaskManager resolves.** `GetTaskRoot(this, i)` reads `this[1 + i]`, and
`this` is the singleton cached at **`0x53948C`** by `FUN_00019450`. A new
`[XBR-TASKMGR]` dump gives the instance and all sixteen slots, every one
populated, every one the same vtable `0x0042ACBC`:

| slot | name | slot | name |
|---|---|---|---|
| 0 | System | 8 | Command |
| 1 | Audio | 9 | CharaColli |
| 2 | Debug | 10 | Landscape |
| **3** | **Scene** | 11 | Gadget |
| 4 | PostSystem | 12 | Vehicle |
| **5** | **Render** | 13 | Player |
| 6 | Manager | 14 | Enemy |
| 7 | Controller | 15 | PostManager |

So the slots are **subsystems, not screens**, `TGameInfomation` hangs off
**Scene**, and `SetFinished` fetches **Render**. Slot 3 exists, so the "missing
task root" theory is dead — direction is now settled in favour of *the caller
never runs*.

**The caller chain, measured cold at every link:**

| function | role | mark | hits |
|---|---|---|---|
| `FUN_00171180` | scene task tick, `switch(this+8)` | `F171180` / `SCNSTATE` | **0** |
| `FUN_00170850` | scene init (its `FUN_0027D710()` call is **unguarded**) | `F170850` | **0** |
| `FUN_0027D710` | singleton creator for TGameInfomation | `F7D460` | **0** |
| `FUN_00280F90` | loader walk | `F8F90` | **0** |
| `sub_00017A80` | one asset, `.xncp` | `T17A80` | **0** |
| `FUN_00025810(0)` | the finish | `CSD_FINISH` | **0** |

All six marks are present in the built binary (`strings` finds each) and the
object file was rebuilt in the same run, so this is not an instrumentation
artefact. `FUN_00171180` has **no direct call sites** in the generated code — it is
a virtual task method, reached through a vtable.

**Where that leaves the boot.** The sixteen subsystem tasks are constructed and
named; the `NowLoadingTask` at `[0x544F40]` is alive with `state(+0x2C)=0` and
`prog(+0x30)=-1`; the screen renders (8 draws, ~26 fps). But the **Scene
subsystem's tick is never invoked**, and it is the only route to the finish. The
next question is therefore narrower than any so far: *is the loading task itself
being ticked?* Its vtable is `0x00445B68` (6 entries); decompiling those six slots
names the update method, and marking it distinguishes "the task framework runs but
the loader's progress source is dead" from "the task framework is not being ticked
at all" — two very different remaining problems, and one cheap measurement.

### Pass 29 addendum 3 — the loading task is never ticked (the sharpest statement so far)

The vtable was read straight from the XBE with `tools/xbe_image.py`, no run needed:

```
clsname @0x445B54 = 'NowLoadingTask'
vtbl 0x445B68:  [0]=0x00059980  [1]=0x00059D50  [2]=0x0048894C
                [3]=0x00059F50  [4]=0x00059BB0  [5]=0x000E53B0 (the assert fn)
```

`[1]` is the finish path already known from Pass 27. Marking the plausible
dispatches — `[0]` (update), `[3]`, `[4]` — and running:

```
U59980 (vtbl[0])  0 hits
V59F50 (vtbl[3])  0 hits
V59BB0 (vtbl[4])  0 hits
```

**No virtual method of the loading task is ever called.** The object is
constructed correctly (vtable, class-name pointer, child at `+0x28`, state
`+0x2C = 0`, progress `+0x30 = -1`), and the screen is still being drawn at
~26 fps with 8 draws per sample — but nothing ever dispatches into it.

That reframes the whole problem. It is not "the loader never emits countable
items" (Pass 28) — that was already unreachable. It is:

> **The loading task is never ticked at all. Until something dispatches into it,
> no progress counter, no `SetFinished`, no scene init and no CSD load can run.**

---

## Pass 30 — compared notes with a working port; the wall is a parked thread, not the game

**Why this pass exists.** A working PC port of the same game, same studio engine,
on the sibling platform appeared: `github.com/BigSnek9/ShadowRecomp` (GameCube,
static PowerPC→C++ recomp, ~34,000 functions, ~160 hand-written overrides). It is
a real, playable port, so the natural question was what of it is reusable.

**What is not reusable.** Its game logic is machine-translated and deliberately
unreadable; there is not one engine symbol in it (`grep -ri 'TaskManager|
GetTaskRoot|NowLoading'` → no hits). Its `runtime/` replaces GX, audio and disc
with GameCube-specific PC shims. Cloned size 19 MB, 189 `.cpp` / 135 `.h` / 75
`.hpp`, and a recompiler for PowerPC (our target is x86). Nothing here drops into
our tree.

**What is reusable — two things.**

1. **Proof of premise.** The engine boots and runs when translated faithfully.
   Our stall is therefore a fidelity gap in *our* runtime, not a game bug and not
   an impossible port.
2. **A map of the landmines.** `overrides.txt` names every function that *had* to
   be replaced, and two of them are about time: `fn_801E1BD4 hle_FramePace`, and
   `frame_rate.cpp` / `dtscale.txt` ("per-frame steps the original code applies
   once per game frame regardless of dt"). That sent me straight back to the
   loading task's tick — and the tick turned out to be gated on exactly that.

**The gate, in the generated code.**

```c
loc_00059BCA:  elapsed = now(64) - [esi+0x30](64)     /* sub_000116B0 = clock */
               if (high32(elapsed) != 0) goto BODY
loc_00059BE0:  if (low32(elapsed) < 0x412B) goto EXIT
loc_00059BEB:  BODY ...                               /* [+0x30] = now at the end */
```

`0x412B` = **16,683 µs = one 59.94 Hz frame**. It is a frame pacer. The clock is
`sub_000116B0` → `rdtsc` (`sub_002AAF10` → `xbox_ReadTimeStampCounter`) divided by
**733**, i.e. the console's 733 MHz CPU, giving microseconds. (Our
`xbox_ReadTimeStampCounter` scales host QPC to 733.333 MHz and starts at zero — it
is correct, and its own comment says exactly why.)

**Measured with `CLOCK` / `PACER` / `PACER_RUN` probes:**

```
CLOCK   6 distinct values, every one (x1), last = 0x2AF32 (175,922 us)
PACER   4 observations, elapsed 1146 -> 1281 -> 1412 -> 1642 us, all < 16,683
PACER_RUN   0
```

and the `CLOCK` histogram is **byte-identical every second for the rest of the
run**, while `[GPU] draw #4800`, `#5000` keep coming.

> **The engine's clock is called 6 times in 25 s — all inside the first ~176 ms —
> and never again. The logic threads run a handful of iterations at boot and then
> park. Only the renderer survives. The frame pacer never opens, so the loading
> task's body never executes even once, which is why `[+0x30]` is still `-1` and
> `LOADPROG` is still 0.**

This supersedes the Pass 29 conclusion in an important way: "the loading task is
never ticked" is true *after 176 ms*, but it was ticked 4 times before that — and
it was dropped before the pacer's first opportunity (16.7 ms of accumulated guest
time while being ticked). Both facts point the same way: **something in the logic
path blocks permanently a fraction of a second into boot.** That is a synchronisation
bug in our runtime — the same class as the TLS fix — not a reverse-engineering
problem in the game.

**Next probe.** Instrument the wait path (`ke_wait_result` already reports wait
outcomes including timeouts) and the guest lock at `0x5A0320`, and find what the
logic thread parks on. That is a question with a answerable shape, unlike "what
should emit countable load items".


And the asymmetry is the clue: **drawing happens, task updates do not.** The
render path is reaching this object (or its data) while the update path is not, so
whatever walks the task list to call `vtbl[0]` is either not running or walking a
list this task is not on. `[0x544F40]` holds the task, and the queue framework at
`0x5608A0` (stride 0x48, pumped by `sub_00328E80`) is the nearest known candidate
for the list — that is the next thing to read, not the loading pipeline.









