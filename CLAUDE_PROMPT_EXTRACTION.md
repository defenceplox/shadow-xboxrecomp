# Brief: the loading-screen hang is (probably) a truncated disc extraction

> **RESULT — 2026-11-07. The experiment in §4 has been run. Outcome: brief §5 row 2.**
>
> * Extractor fixed in `tools/xdvdfs.py`; `list | grep -c '^-'` = **3,510**, and the
>   full path/size list is byte-identical to the independent reader
>   `xboxrecomp/tools/xiso/xdvdfs.py`.
> * Re-extracted to `WORKSPACE/files` (3,510 files / 2,533,482,876 bytes). All
>   1,509 old manifest rows are present with the **same sha256**.
> * **A** (truncated, 60 s): 35 open attempts, 23 successes, 12 failures (9 of them
>   `d:\sound\*`), 30 reads all `got == want`, loading screen loops at
>   `FPS 30.5 | draws: 8`, word "loading" legible in a game-window capture.
> * **B** (full tree, 60 s): 16 attempts, 15 successes, **1 failure**
>   (`t:\$u\contentmeta.xbx`), `d:\sound\sonicteamfx.bin` **opens and its 27,672
>   bytes are read** — and then the title **faults**:
>   `[CRASH] Access violation at RIP=0x140DB709B, fault addr=0x758E5618 (read)`,
>   guest VA `0x758D5618` (≈1.8 GB, far outside the 64 MiB window), inside the
>   recompiled DSOUND copy helper `sub_003EAD45` called from `sub_003E6853`
>   (`loc_003E68C2`). No window at all — Wine `Program Error`.
> * **B with `RECOMP_NO_APU=1`**: byte-identical fault (same RIP, same VA, same
>   registers) — the emulated APU is **not** implicated.
> * So: truncation hid the sound stage (it was a gate *upstream* of the loop), but
>   fixing it does not end the loading screen — it crashes *before* it. Next
>   frontier is the wild pointer DSOUND copies through, not the loading screen.
>
> **PASS 24–25 UPDATE (same day) — the crash is named, and the loading screen is a
> race, not a wait.**
> Instrumenting the copy (`RECOMP_XBR_AUDIT`) and watching the object pointer
> (`RECOMP_WATCH=0x550310`) gave the whole chain in one run:
>
> * `sub_003E35B5` is entered with **p1 = NULL**; `MEM32(0+0xC)` holds `1`, which
>   becomes the `this` of `sub_003E3473 → sub_003E6853 → sub_003EAD45`, so the copy
>   runs with `ctx = 0` and writes `0x818` bytes at guest VA `0x6400` (silent
>   corruption below the image base) before `sub_003E65E6` faults at `0x758E5618`.
> * `[0x550310]` (the game's DirectSound object) is **never written** — a hardware
>   watch on its page records zero write hits. Its only writer is the success path
>   of `sub_003E5A27`, and that call returns **`0x88780078` = `DSERR_NODRIVER`**:
>   the AC'97 codec poll never comes up, so `DirectSoundCreate` fails, the engine
>   stores nothing, and it then calls `IDirectSound8` methods on NULL.
> * With `RECOMP_AC97_READY=1` the create returns **S_OK** and the whole cascade
>   disappears: 45 opens instead of 15 — including `bank0.pac` and all seven
>   `AudioData*.bin` — no crash, and the window shows the **"PRESENTED BY SEGA"**
>   logo.
> * **But it is a race, not a fix (Pass 25, six runs of one build):** 35 opens →
>   loading screen (3 of 6 runs, `FPS ~26`, `draws: 8`); 45 opens → past it (2 of 6,
>   `draws: 16→6`); 40 opens → dark, `FPS 0` (1 of 6). The ten files between the two
>   outcomes are the sound bank. So the loading screen is the **fallback shown when
>   the sound-bank load does not win the race**.
> * The spin is a **submit retry**, not one bit: `sub_003EBED6` writes RR (bit 1) to
>   NABM+0x0B and spins on a hoisted read; `RECOMP_AC97_LOG=1` shows channels
>   `0x11B`/`0x17B` re-reset ~6,770 times in 20 s from two threads.
>
> **The loading screen was never waiting on a file: it is the fallback drawn when the
> audio device does not come up in time.** Details: **Pass 24–25 of
> `xboxrecomp/XBR_ATTEMPT.md`**; logs and captures in
> `WORKSPACE/reports/ab-2026-11-07/pass24/` and `…/pass25/`.
>
> Pass 23 details (extractor fix, re-extraction, the A/B): **Pass 23 of
> `xboxrecomp/XBR_ATTEMPT.md`**; raw logs and the harness in
> `WORKSPACE/reports/ab-2026-11-07/`.

Follow-up to `CLAUDE_PROMPT.md`. That brief asked what the looping loading
screen is waiting for. This one records the answer found, separates what was
**measured** from what is **guessed**, and gives the experiment that settles it.
Do the experiment before building anything else on top of it.

---

## 1. The finding (measured)

`WORKSPACE/files` — the disc tree the build loads from — holds only **43% of the
disc's files**. The opens that fail in every run are failing because the files
were never extracted, **not** because they are absent from the master disc.

Listing the ISO with the toolkit's own reader
(`xboxrecomp/tools/xiso/xdvdfs.py`, which re-syncs at every sector boundary) and
comparing with `WORKSPACE/files`:

| | On the disc | Extracted |
|---|---|---|
| Root entries | **310** | 70 |
| Files | **3,510** (2,533,482,876 bytes) | 1,509 (1.6 GB) |
| `sound/` | **present, 50 files**: `bank0.pac` (1,108,480 bytes, header `24000000 20000000 a0e71000`), `sonicteamfx.bin` (27,672), `AudioDataSystem.bin` (2,082), `AudioData*.bin`, `bank2…bank81.pac` | absent |
| `stg0005` … `stg0911` (~55 stage directories) + `stg_cmn_gdt.one` | present | absent |
| `csdFiles/` | 138 | 66 — 72 missing, incl. `Loading_GR/JP/SP.xncp`, `Warning.xncp`, `SelectMode.xncp`, `Options.xncp`, `MissionFailed_*`, `Result_*`, `mediarom_title.xncp`, `pause/`, `stageTitle/` |
| `event/` | 265 | 57 |
| root `sng_stg*.adx`, `sng_sys*.adx`, `sng_vox*.adx`, `sng_jin_*.adx` | present | absent |
| `dvd/` | **absent** | absent — this failed open is genuine |

Missing-file count by top-level directory: csdFiles 355 (incl. subdirs), event
208, root 183, sound 50, stg0403 90, stg0200 88, … total **2,001 files,
907,886,812 bytes**.

The XBE corroborates this independently: it carries the literal strings
`SOUND/SONICTEAMFX.BIN`, `SOUND/BANK0.PAC` … `SOUND/BANK81.PAC`, and
`STG0911/STG0911_DAT.ONE` etc.

## 2. The bug that caused it (measured)

`/mnt/docker/sites/shadow/tools/xdvdfs.py`, `Volume.read_dir()`, walks a
directory table linearly across its **whole** size. XDVDFS never lets a
directory entry span a 2,048-byte sector; the tail of each sector is padded
with `0xFF`. At offset 2020 of the root's first sector the walker meets that
padding (`l=0xFFFF r=0xFFFF`, `name_len=0xFF`), the printable-name check fails,
and it `break`s — so **only the first sector of every multi-sector directory is
ever read**. Root is 5 sectors (10,240 bytes), `csdFiles` 3, `event` 5,
`stg0200`/`stg0403` 2: every one of them was cut short.

Consequences that must be corrected:

- `MEMORY_BANK/MediaValidation.md` and `PROJECT_SHADOW.md` state "70 root
  entries" and "1,576 entries (1,509 files, 67 directories)". Both are artefacts
  of this bug. The real figures are 310 root entries, 3,510 files and 126
  directories (3,636 entries).
- The "70/70 root entries matched to their expected file signature" validation
  (docstring of `tools/xdvdfs.py`, `SessionLogs.md`) only ever saw sector 0.
- `CLAUDE_PROMPT.md` §6, row **"A missing asset"**, is **wrong** and must be
  marked **RETRACTED**: `d:\sound\bank0.pac` and `d:\sound\AudioData*.bin` *do*
  exist on the master. Only `d:\dvd\` (and `t:\$u\contentmeta.xbx`, a temp-drive
  probe) genuinely fail on hardware.
- The `RECOMP_NO_APU=1` result ("22 files, no change") was taken on the
  truncated tree and should be re-measured.

## 3. The hypothesis (a guess — not yet proven)

On real hardware the sound-resource opens (`bank0.pac`, `sonicteamfx.bin`, the
seven `AudioData*.bin`) succeed. Here they fail, and the file count freezes at
31 immediately after that batch. The most likely reading: the sound/resource
stage never completes, so the loading state machine never advances to request
the next stage's files (`stg*`, the remaining `csdFiles`), and the loading
screen loops.

What is **not** known: the exact mechanism by which the code waits on those
files (retry loop, a "bank ready" flag, a resource refcount). Do not assert one
until it is measured.

## 4. Task — the single cheapest experiment

Change the data, **nothing else**, and A/B it.

### 4.1 Fix the extractor

In `/mnt/docker/sites/shadow/tools/xdvdfs.py`, make `read_dir()` walk the table
one sector at a time and stop *per sector*, not per directory:

```python
def read_dir(self, sector, size):
    """Return the entries of one directory table in stored order.

    XDVDFS never lets an entry span a sector; the tail of each 2048-byte
    sector is 0xFF padding. Walk each sector separately and resync at the
    next boundary instead of stopping at the first padding byte.
    """
    self._fh.seek(self.absolute_offset(sector))
    blob = self._fh.read(size)
    entries = []
    for base in range(0, len(blob), SECTOR_SIZE):
        chunk = blob[base:base + SECTOR_SIZE]
        pos = 0
        while pos + 14 <= len(chunk):
            left, right, file_sector, file_size = struct.unpack_from("<HHII", chunk, pos)
            attrib = chunk[pos + 12]
            name_len = chunk[pos + 13]
            if (left == 0xFFFF and right == 0xFFFF) or name_len == 0:
                break                       # padding: end of this sector
            if pos + 14 + name_len > len(chunk):
                break
            name = chunk[pos + 14:pos + 14 + name_len].decode("latin-1")
            if any(ord(c) < 0x20 or ord(c) > 0x7E for c in name):
                break
            entries.append(DirEntry(name, attrib, file_sector, file_size))
            pos += (14 + name_len + 3) & ~3
    return entries
```

Also correct the docstring's "70/70 root entries" claim.

*Pre-verified:* this exact function, monkey-patched onto `xdvdfs.Volume` and run
against the ISO, walks **3,510 files, 126 directories, 310 root entries** —
matching the toolkit reader.

**Acceptance:** `python3 tools/xdvdfs.py list | grep -c '^-'` reports **3,510**,
identical to the toolkit reader:

```bash
cd /mnt/docker/sites/shadow/xboxrecomp/tools && python3 -c "
from xiso.xdvdfs import Xiso
x=Xiso('/mnt/docker/sites/shadow/Shadow the Hedgehog (USA) (En,Ja,Fr,De,Es,It).iso')
print(sum(1 for _ in x.walk()))"     # expect 3510
```

### 4.2 Re-extract side by side (keep the old tree for the A/B)

```bash
cd /mnt/docker/sites/shadow
python3 tools/xdvdfs.py extract --quiet \
    --out WORKSPACE/files_full --manifest WORKSPACE/MANIFEST_full.tsv
```

~2.5 GB; the volume has 154 GB free. Then prove nothing regressed: every one of
the 1,509 rows in the old `WORKSPACE/MANIFEST.tsv` must appear in
`MANIFEST_full.tsv` with the **same sha256**.

### 4.3 A/B run

The build resolves `<exe>\..\WORKSPACE\files` (`DATA_REL_PATH` in
`shadow_game/src/main.c`), so swap by rename — do not change the code:

```bash
cd /mnt/docker/sites/shadow/WORKSPACE
mv files files_trunc && mv files_full files      # B: full tree
# ... run ...
mv files files_full && mv files_trunc files      # back to A if needed
```

Run each configuration for 60 s headless under Xvfb (`1024x768x24`) with
`RECOMP_FILE_TRACE=1`, and record for **both**:

1. number of successful file opens (A baseline: 31);
2. whether every `d:\sound\*` open now succeeds, and every read is `got == want`;
3. whether any `d:\stg*` or new `d:\csdFiles\*` opens appear, and in what order;
4. the remaining failed opens (expected: only `d:\dvd\` ×2 and `t:\$u\contentmeta.xbx`);
5. window title FPS / `draws`, and a capture of the **game window by title**
   (never `-window root`).

Then repeat B with `RECOMP_NO_APU=1` to re-measure the audio-disabled case.

## 5. How to read the result

| Outcome on the full tree | Meaning | Next |
|---|---|---|
| Open count rises past 31, `stg*`/`csdFiles` opens appear, screen changes | **Confirmed**: the truncated extraction was the gate | Record it; move on to the next frontier (title/menu) |
| `sound\*` opens succeed, count rises a little, then stalls again | The sound path is now actually exercised and stalls *inside* it | Trace the audio stack with real `bank0.pac` data; compare `RECOMP_NO_APU=1` |
| Every `sound\*` open succeeds, only `dvd\` + `contentmeta.xbx` still fail, and the count **still** freezes with the same loop | **Hypothesis killed**: the files were real but not the gate | Return to `CLAUDE_PROMPT.md` §10 direction 2: enumerate the app commands reaching `0x11270` and find the one that never arrives |

## 6. Records to update afterwards

- `xboxrecomp/XBR_ATTEMPT.md` — add **Pass 23** with the extractor bug, the
  disc/extracted counts above, and the A/B numbers.
- `MEMORY_BANK/MediaValidation.md`, `PROJECT_SHADOW.md` — replace
  "70 root entries" / "1,576 entries (1,509 files, 67 directories)" with the
  measured figures; note the earlier ones came from the truncating walker.
- `CLAUDE_PROMPT.md` §6 — mark the "A missing asset" row **RETRACTED** in the
  same style as the existing retractions, pointing at this file.

## 7. Method warning this adds to the list

6. **A tool's agreement with itself is not validation.** The "70/70 root
   entries verified" check and the "1,576 entries" count were both produced by
   the same truncating walker, so they agreed perfectly and were both wrong.
   Cross-check a format reader against an *independent* implementation (here,
   `tools/xiso/xdvdfs.py`) before treating its output as ground truth.

