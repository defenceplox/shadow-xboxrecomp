/**
 * YOUR_GAME_NAME - Recompiled Game Entry Point
 *
 * This is the Windows executable that hosts the recompiled game code.
 * It performs the following initialization sequence:
 *
 * 1. Load the original XBE file from disk
 * 2. Initialize the Xbox memory layout (map data sections to original VAs)
 * 3. Initialize the Xbox kernel replacement layer
 * 4. Initialize the kernel bridge (thunk table in Xbox memory)
 * 5. Set up game file paths for I/O redirection
 * 6. Initialize the stack pointer
 * 7. Install VEH crash handler for diagnostics
 * 8. Call the game's original entry point (recompiled)
 *
 * Customize this file for your game:
 *   - Set YOUR_GAME_ENTRY_POINT to the XBE entry point address
 *   - Set YOUR_GAME_XBE_PATH to where the XBE file lives
 *   - Set YOUR_GAME_DIR to the game data directory
 *   - Add any CRT global pre-initialization your game needs
 *   - Customize the VEH handler for game-specific crash diagnosis
 *
 * XBE Details (fill in from xbe_parser output):
 *   Title:       YOUR_GAME_NAME
 *   Title ID:    0x00000000
 *   Base addr:   0x00010000
 *   Entry point: 0x00000000
 *   Code size:   ~??? KB (.text)
 *   Sections:    ?? (list them)
 *   Kernel imports: ??
 */

#include <windows.h>
#include <dbghelp.h>
#include <tlhelp32.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <math.h>

/* xboxrecomp runtime headers */
#include <xbox/xboxrecomp.h>

/* NV2A register model (src/nv2a, exported PUBLIC by the xbox_nv2a target).
 * Not in the umbrella header, so it is included directly. */
#include "nv2a_mmio_hook.h"
/* Win32 instruction decoder for trapped MMIO (platform target). */
#include "mmio_decode.h"

/*
 * If xboxrecomp.h is not an umbrella header in your setup, include
 * the individual headers directly:
 *
 * #include "kernel.h"
 * #include "xbox_memory_layout.h"
 * #include "d3d8_xbox.h"
 * #include "dsound_xbox.h"
 * #include "xinput_xbox.h"
 */

/* ── Global register state (defined in xbox_memory_layout.c) ── */

/* RECOMP_TLS is not optional here. The runtime defines these thread-local, and
 * a plain `extern` referencing a __declspec(thread) variable does not resolve
 * to the calling thread's copy -- it resolves to the image's TLS template. The
 * host side then writes g_esp somewhere the generated code never reads, so the
 * guest starts with every register at zero and faults immediately, having
 * apparently ignored the setup that visibly ran. */
extern RECOMP_TLS uint32_t g_eax, g_ecx, g_edx, g_esp;
extern RECOMP_TLS uint32_t g_ebx, g_esi, g_edi;
extern RECOMP_TLS uint32_t g_seh_ebp;
/* x87 and SSE state. Global for the same reason the volatile GPRs are: one
 * guest routine can lift to several C functions, so a value written in one
 * body is read in the next. Defined in xbox_memory_layout.c like the rest of
 * the register file. */
extern RECOMP_TLS double g_fp_stack[8];
extern RECOMP_TLS int g_fp_top;
extern RECOMP_TLS uint16_t g_fp_control_word;
extern RECOMP_TLS uint32_t g_mxcsr;
extern RECOMP_TLS int g_fp_cmp;
extern RECOMP_TLS RecompXmm g_xmm0, g_xmm1, g_xmm2, g_xmm3;
extern RECOMP_TLS RecompXmm g_xmm4, g_xmm5, g_xmm6, g_xmm7;
extern ptrdiff_t g_xbox_mem_offset;

/* ── XBE Constants ─────────────────────────────────────────── */

/*
 * TODO: Set these from your xbe_parser output.
 * Run: py -3 -m tools.xbe_parser game/default.xbe
 */
#define YOUR_GAME_ENTRY_POINT   0x002AFB53  /* Shadow the Hedgehog: mainCRTStartup */
/* Path defaults. The real paths are resolved at run time from the directory the
 * exe lives in, not from the current working directory: launching from a UNC
 * path, a shortcut, or a network share leaves the CWD somewhere else (often
 * C:\Windows\system32) and every relative path then fails. */
#define XBE_REL_PATH   "game_files\\default.xbe"
#define DATA_REL_PATH  "..\\WORKSPACE\\files"


/* ── Forward declarations ──────────────────────────────────── */

static BOOL load_xbe(const char *path, void **out_data, size_t *out_size);

/* Game data directory, resolved from the exe's location at startup. */
static char g_game_dir[MAX_PATH];

/* Directory holding this executable, so paths work regardless of the CWD. */
static void exe_dir(char *out, size_t cap)
{
    char full[MAX_PATH];
    DWORD n = GetModuleFileNameA(NULL, full, (DWORD)sizeof full);
    char *slash;

    out[0] = '\0';
    if (n == 0 || n >= (DWORD)sizeof full)
        return;
    slash = strrchr(full, '\\');
    if (slash)
        *slash = '\0';
    snprintf(out, cap, "%s", full);
}

/* Locate the XBE and the disc tree relative to where the exe actually sits.
 *
 * Resolving both against the exe's own directory is right for the shipped
 * layout (exe beside game_files\, disc at ..\WORKSPACE\files) but breaks the
 * moment a copy is run from a subdirectory: a build output in
 * shadow_game\ then looks for shadow_game\game_files (present) and
 * shadow_game\..\WORKSPACE\files (absent, because it resolves to
 * xboxrecomp\WORKSPACE), so the title loads its XBE and finds no disc -- a
 * black screen rather than a loading screen. Walk up a few parents and use the
 * first location that exists, so any copy of this exe behaves the same.
 */
static int path_is_file(const char *p)
{
    DWORD a = GetFileAttributesA(p);
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

static int path_is_dir(const char *p)
{
    DWORD a = GetFileAttributesA(p);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

static void resolve_paths(const char *dir, char *xbe_out, size_t xbe_cap,
                          char *data_out, size_t data_cap)
{
    char root[MAX_PATH], cand[MAX_PATH], alt[MAX_PATH], up[MAX_PATH];
    int i;

    snprintf(root, sizeof root, "%s", dir);
    (void)snprintf(xbe_out, xbe_cap, "%s\\%s", root, XBE_REL_PATH);
    for (i = 1; i <= 3 && !path_is_file(xbe_out); i++) {
        char *slash = strrchr(root, '\\');
        if (!slash)
            break;
        *slash = '\0';
        (void)snprintf(xbe_out, xbe_cap, "%s\\%s", root, XBE_REL_PATH);
    }

    /* Shipped layout first: <root>\..\WORKSPACE\files */
    (void)snprintf(cand, sizeof cand, "%s\\%s", root, DATA_REL_PATH);
    if (!path_is_dir(cand)) {
        (void)snprintf(alt, sizeof alt, "%s\\WORKSPACE\\files", root);
        if (path_is_dir(alt)) {
            (void)snprintf(cand, sizeof cand, "%s", alt);
        } else {
            (void)snprintf(up, sizeof up, "%s", root);
            {
                char *slash = strrchr(up, '\\');
                if (slash)
                    *slash = '\0';
            }
            (void)snprintf(alt, sizeof alt, "%s\\%s", up, DATA_REL_PATH);
            if (path_is_dir(alt))
                (void)snprintf(cand, sizeof cand, "%s", alt);
        }
    }
    (void)snprintf(data_out, data_cap, "%s", cand);
}

/* Recompiled entry point (generated by recomp pipeline) */
extern void xbe_entry_point(void);

/* ── VEH crash handler ─────────────────────────────────────── */

/*
 * Vectored Exception Handler for crash diagnostics.
 *
 * When the recompiled game hits an access violation, this handler prints
 * the faulting address, all Xbox register values, and a native stack trace.
 * This is your primary debugging tool during bring-up.
 *
 * Customize this for your game:
 *   - Add game-specific address checks (GPU register probes, etc.)
 *   - Add dumps of game-specific globals (heap handles, state flags)
 *   - Add SEH simulation if your game uses __try/__except
 */
/* Name the guest function a fault happened in, and recover the call chain.
 *
 * Recompiled code faults as ordinary native code, so the exception record
 * carries a host RIP and nothing else -- there is no guest program counter to
 * report, and the host address changes every build. Two things recover the
 * guest view:
 *
 *   - every generated function is a real symbol in the image (sub_005A03C0
 *     and so on), so the linker's PDB already maps host address back to guest
 *     function. dbghelp turns an anonymous RIP into that name.
 *
 *   - every lifted call pushes its guest return address onto the guest stack
 *     before jumping, so the stack still holds the chain. Scanning up from esp
 *     for values inside the code sections recovers it.
 *
 * ponytail: the stack scan is a scan, not a frame walk -- these are FPO frames
 * with no reliable ebp chain, so there is nothing to walk. It over-reports,
 * since addresses from returned-from calls linger below esp, but naming the
 * guest function is the whole question at a fault.
 *
 * Requires linking dbghelp and keeping the .pdb beside the .exe.
 */
static void print_guest_context(void *rip)
{
    /* SYMBOL_INFO is variable-length: the name is written past the struct, so
     * it must be over-allocated with MaxNameLen set to the slack. */
    char buf[sizeof(SYMBOL_INFO) + 256];
    SYMBOL_INFO *sym = (SYMBOL_INFO *)buf;
    DWORD64 disp = 0;

    memset(buf, 0, sizeof(buf));
    sym->SizeOfStruct = sizeof(SYMBOL_INFO);
    sym->MaxNameLen = 255;
    if (SymFromAddr(GetCurrentProcess(), (DWORD64)(uintptr_t)rip, &disp, sym))
        fprintf(stderr, "  in %s+0x%llX\n",
                sym->Name, (unsigned long long)disp);

    if (g_xbox_mem_offset && g_esp) {
        const uint32_t *sp =
            (const uint32_t *)((uintptr_t)g_xbox_mem_offset + g_esp);
        int shown = 0, i;
        fprintf(stderr, "  guest stack (return addresses, innermost first):\n");
        for (i = 0; i < 256 && shown < 24; i++) {
            uint32_t v = sp[i];
            if (v > g_xbox_code_lo && v < g_xbox_code_hi) {
                fprintf(stderr, "    [esp+%-4d] 0x%08X\n", i * 4, v);
                shown++;
            }
        }
        /* When the frame is gone, say so, and fall back on what is left.
         *
         * A fault inside a block copy leaves no return address in range: the
         * copy is a host memmove and its caller's frame is further up than
         * this reaches. Worse, a copy that ran past its buffer overwrites the
         * stack itself, so the list comes out empty exactly when it is most
         * needed -- and an empty list under a promising heading reads like
         * "nothing to see" rather than "the evidence is destroyed". */
        if (!shown)
            fprintf(stderr, "    (no return addresses in range)\n");

        /* Everything below only pays for itself when the scan came back
         * empty. A crash where the chain was recovered already says what
         * happened, and burying that under another twenty lines makes the
         * common report worse to read. */
        if (!shown) {
            uint32_t k;

            /* The indirect-call history, which the stack cannot overwrite.
             * recomp_manual.c prints this too, but only from
             * recomp_icall_fail_log -- a page fault inside a block copy
             * never reaches that path, and this is the same information by
             * the route the fault actually took.
             *
             * Declared here rather than included: main.c does not pull in
             * recomp_types.h, and recomp_manual.c spells it the same way. */
            extern volatile uint32_t g_icall_trace[16];
            extern volatile uint32_t g_icall_trace_idx;

            fprintf(stderr, "  recent ICALL targets:");
            for (k = 0; k < 16; k++)
                fprintf(stderr, " %08X",
                        g_icall_trace[(g_icall_trace_idx + k) & 15]);
            fprintf(stderr, "\n");

            /* The frame unfiltered. The pointers and lengths handed to
             * whatever faulted live here and look nothing like code, so the
             * filter above drops exactly what is wanted. On the crash this
             * was written for, one of these words was the end of the mapped
             * RAM mirror -- which is what identified the fault as a copy
             * running off its buffer rather than a stray pointer. */
            for (i = 0; i < 20; i++)
                fprintf(stderr, "    raw[esp+%-4d] 0x%08X\n", i * 4, sp[i]);
        }
    }
}

/* The emulated APU.
 *
 * Two entry points that had no callers anywhere in the tree. apu_mmio_hook.c
 * documents apu_hook_handle_mmio as "called from VEH in main.c" and no main.c
 * called it; apu_decode_and_handle's first line is `if (!g_apu_state) return
 * false` and nothing assigned g_apu_state. So with RECOMP_AC97_READY set the
 * APU's registers were unmapped to be trapped, and every trap was then
 * declined and surfaced as an access violation on the first register
 * DirectSound touched. The VEH below now reaches apu_hook_handle_mmio
 * through the toolkit's xbox_VehMmioFault.
 *
 * Declared rather than included so this does not depend on src/apu being on
 * the include path. */
typedef struct MCPXAPUState MCPXAPUState;
extern MCPXAPUState *mcpx_apu_init_standalone(uint8_t *ram_ptr);
extern MCPXAPUState *g_apu_state;

/* ── PC sampler (opt-in: RECOMP_PC_SAMPLE=<seconds>) ──────────────────
 *
 * Windows has no ITIMER_PROF/SIGPROF, so the POSIX sampler cannot be reused.
 * This suspends each of the process's other threads in turn, reads its RIP
 * from the CONTEXT, and resumes it. RIPs are reported against the PE's
 * preferred image base so `addr2line -f -e shadow_recomp.exe <addr>` resolves
 * them straight to sub_XXXXXXX.
 */
#define WP_SAMPLES 8192
static struct { DWORD tid; DWORD64 rip; DWORD64 base; } g_ws[WP_SAMPLES];
static volatile LONG g_wn;

static DWORD WINAPI sampler_thread(LPVOID arg)
{
    int secs = (int)(intptr_t)arg;
    DWORD until = GetTickCount() + (DWORD)secs * 1000u;
    HANDLE self = GetCurrentProcess();
    DWORD me = GetCurrentThreadId();

    while ((int)(GetTickCount() - until) < 0) {
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        if (snap != INVALID_HANDLE_VALUE) {
            THREADENTRY32 te;
            te.dwSize = sizeof te;
            if (Thread32First(snap, &te)) {
                do {
                    HANDLE h;
                    CONTEXT c;
                    if (te.th32OwnerProcessID != GetCurrentProcessId()) continue;
                    if (te.th32ThreadID == me) continue;
                    h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT,
                                   FALSE, te.th32ThreadID);
                    if (!h) continue;
                    if (SuspendThread(h) != (DWORD)-1) {
                        memset(&c, 0, sizeof c);
                        c.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
                        if (GetThreadContext(h, &c)) {
                            LONG i = InterlockedIncrement(&g_wn) - 1;
                            if (i < WP_SAMPLES) {
                                HMODULE mod = GetModuleHandleA(NULL);
                                g_ws[i].tid  = te.th32ThreadID;
                                g_ws[i].rip  = (DWORD64)c.Rip;
                                g_ws[i].base = (DWORD64)(uintptr_t)mod;
                            }
                        }
                        ResumeThread(h);
                    }
                    CloseHandle(h);
                } while (Thread32Next(snap, &te));
            }
            CloseHandle(snap);
        }
        (void)self;
        Sleep(5);
    }

    {
        LONG n = g_wn;
        if (n > WP_SAMPLES) n = WP_SAMPLES;
        fprintf(stderr, "PTS count=%ld\n", (long)n);
        for (LONG i = 0; i < n; i++) {
            /* Rebase to the PE's preferred base so addr2line matches. */
            DWORD64 a = g_ws[i].rip - g_ws[i].base + 0x140000000ULL;
            fprintf(stderr, "PTS %lu %llx\n", (unsigned long)g_ws[i].tid,
                    (unsigned long long)a);
        }
        fflush(stderr);
    }
    _exit(0);
}

/* ── APU/AC97 kick acknowledge (RECOMP_KICK_ACK=0 disables) ────────────
 *
 * DirectSound's submit path writes 2 to the AC97 byte at 0xFEC0011B and then
 * spins on the register until bit 1 reads back 0 -- "the engine consumed the
 * command". On this path that page is plain RAM, nothing clears the bit, so
 * the audio thread never returns. The main thread is left in
 * KeWaitForSingleObject/NtYieldExecution on it (millions of calls) and the
 * title stops drawing after its first few frames. Clearing the byte from the
 * host is the same handshake acknowledgement the APU model makes for the GP
 * doorbell: it says the command was taken, and computes nothing.
 */
static DWORD WINAPI kick_acker(LPVOID unused)
{
    unsigned char *base;
    (void)unused;

    for (;;) {
        base = (unsigned char *)(uintptr_t)g_xbox_mem_offset;
        if (base) {
            base[0xFEC0011B] = 0;
            base[0xFEC0017B] = 0;

            /* The D3D "GPU finished" event the title builds itself
             * (0x3E27DC, a notification event whose SignalState lives at
             * +4). With RECOMP_TITLE_KEVENTS the wait resolves to a real
             * host event, so the title blocks there instead of spinning --
             * KeWaitForSingleObject fell from 632,817 to ~4,700 calls -- but
             * nothing ever signals it, so drawing stops at 8 frames. A blocked
             * wait sits on the host event, so the SignalState word alone is not
             * enough: the event has to be set. The simulator is finished the
             * moment the pushbuffer executor is idle, so acknowledging it is
             * truthful, not a stub. RECOMP_KICK_ACK=0 disables all of this. */
            {
                extern int xbox_GuestEventSignal(uint32_t guest_va);
                uint32_t dev = *(uint32_t *)(base + 0x3E0A18);

                /* The PUT/GET fence is handled by the runtime's own mirror
                 * (xbox_Nv2aMirrorFence, registered at startup), which follows
                 * the device pointer each poll. Nothing to poke here. */
                (void)dev;
                xbox_GuestEventSignal(0x3E27DCu);
            }
        }
        Sleep(1);
    }
    return 0;
}

/* ── AC97 codec page trap ──────────────────────────────────────────────
 *
 * DirectSound's submit writes 2 to 0xFEC0011B and re-reads it until bit 1
 * clears. Clearing it from a 1 ms host thread is far too coarse -- the guest
 * re-reads within microseconds, so the thread just spins -- which is why
 * sub_003EBF48 burns CPU. Trapping the page and answering the read with the
 * bit already cleared makes the handshake instant, exactly as the POSIX build
 * does it. Uses the toolkit's Win32 instruction decoder (mmio_decode.h).
 */
#define AC97_BASE 0xFEC00000u
#define AC97_SIZE 0x1000u
static uint32_t g_ac97[AC97_SIZE / 4];
static int      g_ac97_ready;

static uint64_t ac97_rd(void *dev, uint32_t off, int size)
{
    uint32_t w = off >> 2, v;
    (void)dev;
    if (w >= AC97_SIZE / 4) return 0;
    v = g_ac97[w];
    if (size == 1)      v = (v >> ((off & 3) * 8)) & 0xFFu;
    else if (size == 2) v = (v >> ((off & 2) * 8)) & 0xFFFFu;
    if (size == 1 && (off == 0x11B || off == 0x17B)) {
        v &= ~0x02u;                                  /* presented as consumed */
        g_ac97[w] &= ~(2u << ((off & 3) * 8));
    }
    return v;
}

static void ac97_wr(void *dev, uint32_t off, uint64_t val, int size)
{
    uint32_t w = off >> 2, sh = (off & 3) * 8, cur;
    (void)dev;
    if (w >= AC97_SIZE / 4) return;
    cur = g_ac97[w];
    if (size == 1)
        cur = (cur & ~(0xFFu << sh)) | (((uint32_t)val & 0xFFu) << sh);
    else if (size == 2) {
        uint32_t s = (off & 2) * 8;
        cur = (cur & ~(0xFFFFu << s)) | (((uint32_t)val & 0xFFFFu) << s);
    } else
        cur = (uint32_t)val;
    if (off == 0x11B || off == 0x17B)
        cur &= ~(2u << sh);                           /* acknowledged on write */
    g_ac97[w] = cur;
}

static void ac97_trap_init(void)
{
    unsigned char *base = (unsigned char *)(uintptr_t)g_xbox_mem_offset;
    DWORD old;

    if (!base || g_ac97_ready) return;
    memcpy(g_ac97, base + AC97_BASE, AC97_SIZE);
    base[AC97_BASE + 0x11B] &= (unsigned char)~0x02;
    base[AC97_BASE + 0x17B] &= (unsigned char)~0x02;
    g_ac97[(0x11B >> 2)] &= ~(2u << ((0x11B & 3) * 8));
    g_ac97[(0x17B >> 2)] &= ~(2u << ((0x17B & 3) * 8));
    if (VirtualProtect(base + AC97_BASE, AC97_SIZE, PAGE_NOACCESS, &old)) {
        g_ac97_ready = 1;
        fprintf(stderr, "[AC97] codec page trapped (instant kick handshake)\n");
    } else {
        fprintf(stderr, "[AC97] could not trap codec page (error %lu)\n",
                (unsigned long)GetLastError());
    }
}

/* ── guest .data narrowing (opt-in: RECOMP_STATE_SNAP=1) ─────────────────
 *
 * The screen is a loading screen, so the game is waiting on a value that
 * stopped moving. Rather than guess which one, snapshot .data three times:
 *
 *   A at 6 s   (loading well underway)
 *   B at 20 s  (stalled)
 *   C at 34 s  (still stalled)
 *
 * and report the dwords that changed between A and B but NOT between B and C.
 * Recently-active-then-frozen is a much smaller list than "all zeros", and the
 * gate is in it. */
#define SNAP_BASE 0x00F80000u
#define SNAP_END  0x03000000u
#define SNAP_LEN  ((SNAP_END - SNAP_BASE) / 4u)

static DWORD WINAPI state_snap_thread(LPVOID arg)
{
    unsigned char *base;
    uint32_t *a, *b, *c;
    unsigned i, n_frozen = 0, shown = 0;

    (void)arg;
    /* Resolved here, not captured at start: this thread is created before
     * main assigns g_xbox_mem_offset, so reading the global would see 0.
     * Poll rather than sleep a fixed amount -- a fixed 2 s wait put every
     * snapshot after loading had already finished, which read as "the game is
     * idle" when in fact the load window had been missed. */
    {
        extern ptrdiff_t xbox_GetMemoryOffset(void);
        int k;
        for (k = 0; k < 2000 && !base; k++) {
            base = (unsigned char *)(uintptr_t)xbox_GetMemoryOffset();
            if (!base) Sleep(10);
        }
    }
    if (!base) { fprintf(stderr, "[SNAP] no guest memory; skipped\n"); return 0; }
    a = (uint32_t *)malloc(SNAP_LEN * 4u);
    b = (uint32_t *)malloc(SNAP_LEN * 4u);
    c = (uint32_t *)malloc(SNAP_LEN * 4u);
    if (!a || !b || !c) { free(a); free(b); free(c); return 0; }

    Sleep(300);   memcpy(a, base + SNAP_BASE, SNAP_LEN * 4u);
    Sleep(1200);  memcpy(b, base + SNAP_BASE, SNAP_LEN * 4u);
    Sleep(2500);  memcpy(c, base + SNAP_BASE, SNAP_LEN * 4u);

    {
        unsigned n_early = 0, n_late = 0, n_both = 0;
        for (i = 0; i < SNAP_LEN; i++) {
            int e = (a[i] != b[i]), l = (b[i] != c[i]);
            if (e) n_early++;
            if (l) n_late++;
            if (e && l) n_both++;
        }
        fprintf(stderr, "[SNAP] v3 window 0x%08X..0x%08X (%u dwords):"
                        " changed A->B: %u ; B->C: %u ; both: %u\n",
                SNAP_BASE, SNAP_END, SNAP_LEN, n_early, n_late, n_both);
    }
    for (i = 0; i < SNAP_LEN; i++) {
        if (a[i] != b[i] && b[i] == c[i]) n_frozen++;
    }
    fprintf(stderr, "[SNAP] %u dwords changed early and then froze:\n", n_frozen);
    for (i = 0; i < SNAP_LEN && shown < 40; i++) {
        if (a[i] != b[i] && b[i] == c[i]) {
            fprintf(stderr, "[SNAP]  0x%08X  %lu -> %lu  (still %lu)\n",
                    SNAP_BASE + i * 4u, (unsigned long)a[i], (unsigned long)b[i],
                    (unsigned long)c[i]);
            shown++;
        }
    }
    fflush(stderr);
    free(a); free(b); free(c);
    return 0;
}

/* Diff the engine object the per-frame tick operates on (opt-in, same switch).
 *
 * 0x00576 30 sets esi = [0x541FB4], calls 0xE53B0, then `call [esi+4]` -- the
 * pump -- and the stage calls after it. So [0x541FB4] is the engine object and
 * its fields are the state the frame depends on. The address is built at run
 * time, so it has to be read, not assumed. Sample it twice and print only the
 * fields that changed: a field that stopped moving while the screen loops is a
 * better candidate for the gate than anything in a whole-heap sweep. */
static DWORD WINAPI engine_snap_thread(LPVOID arg)
{
    extern ptrdiff_t xbox_GetMemoryOffset(void);
    unsigned char *base = NULL;
    uint32_t obj = 0, before[64], after[64];
    int k, i, shown = 0;

    (void)arg;
    for (k = 0; k < 3000; k++) {
        base = (unsigned char *)(uintptr_t)xbox_GetMemoryOffset();
        if (base) {
            obj = *(uint32_t *)(base + 0x541FB4u);
            if (obj > 0x10000u && obj < 0x04000000u)
                break;
            obj = 0;
        }
        Sleep(10);
    }
    if (!base || !obj) {
        fprintf(stderr, "[ENGINE] engine object never appeared\n");
        return 0;
    }
    fprintf(stderr, "[ENGINE] engine object at 0x%08X\n", obj);
    Sleep(2000);
    memcpy(before, base + obj, sizeof before);
    Sleep(8000);
    memcpy(after, base + obj, sizeof after);

    for (i = 0; i < 64; i++) {
        if (before[i] != after[i]) {
            fprintf(stderr, "[ENGINE]  +0x%02X  %08X -> %08X  *\n", i * 4,
                    before[i], after[i]);
            shown++;
        } else {
            fprintf(stderr, "[ENGINE]  +0x%02X  %08X\n", i * 4, before[i]);
        }
    }
    fprintf(stderr, "[ENGINE] %d/64 fields changed over 8 s\n", shown);
    fflush(stderr);
    return 0;
}

/* Dump every guest thread's stack after RECOMP_STACK_DUMP=<secs>, then exit.
 * Used when one thread is clearly parked and the others need naming. */
/* Shadow's task dispatcher (thread 0, sub_00332BD0) scans a 16-entry table at
 * 0x5923E0, stride 0x44, and processes the entries whose state at +0x14 is 1,
 * then clears the state and sleeps 10 ms per sweep. Dumping it says whether the
 * queue is empty (no producer) or full and unprocessed (no worker). */
#define JOB_BASE  0x5923E0u
#define JOB_STRIDE 0x44u
#define JOB_SLOTS 16

static void dump_job_table(void)
{
    unsigned char *base = (unsigned char *)(uintptr_t)g_xbox_mem_offset;
    int i, busy = 0;

    if (!base) return;
    fprintf(stderr, "[JOBS] table 0x%08X, %d slots\n", JOB_BASE, JOB_SLOTS);
    for (i = 0; i < JOB_SLOTS; i++) {
        uint32_t off = JOB_BASE + (uint32_t)i * JOB_STRIDE;
        uint32_t id = *(uint32_t *)(base + off + 0x00);
        uint32_t st = *(uint32_t *)(base + off + 0x14);
        uint32_t a  = *(uint32_t *)(base + off + 0x34);
        uint32_t b  = *(uint32_t *)(base + off + 0x38);
        if (id || st) busy++;
        fprintf(stderr, "[JOBS]  slot %2d id=0x%08X state=%u  +0x34=0x%08X"
                        " +0x38=0x%08X\n", i, id, st, a, b);
    }
    fprintf(stderr, "[JOBS] %d/%d slots in use\n", busy, JOB_SLOTS);
    /* The dispatcher's own counters: sub_00332B60 does `lock xadd` on 0x560BD8
     * each time it processes a slot, 0x560BE0 is its quit flag, 0x560BD0 an
     * optional callback. A non-moving 0x560BD8 means it has never dispatched
     * anything. */
    fprintf(stderr, "[JOBS] dispatcher: processed=0x%08X quit=0x%08X"
                    " callback=0x%08X frame_flag=0x%08X\n",
            *(uint32_t *)(base + 0x560BD8u),
            *(uint32_t *)(base + 0x560BE0u),
            *(uint32_t *)(base + 0x560BD0u),
            *(uint32_t *)(base + 0x560330u));
    /* The control block the job system lives in. Anything here that is a
     * plausible code address names a callback the subsystem registered, and
     * anything that points into .data names the queue it feeds. */
    {
        int k;
        for (k = 0; k < 0x80; k += 16) {
            uint32_t a0 = 0x560BC0u + (uint32_t)k;
            fprintf(stderr, "[JOBS]  0x%08X: %08X %08X %08X %08X\n", a0,
                    *(uint32_t *)(base + a0), *(uint32_t *)(base + a0 + 4),
                    *(uint32_t *)(base + a0 + 8), *(uint32_t *)(base + a0 + 12));
        }
    }
    fflush(stderr);
}

static DWORD WINAPI stack_dumper(LPVOID arg)
{
    int secs = (int)(intptr_t)arg;
    extern void xbox_DumpGuestStacks(void);

    Sleep((DWORD)secs * 1000u);
    xbox_DumpGuestStacks();
    dump_job_table();
    {
        /* The peek only printed at a hang or a clean exit, and _exit() below is
         * neither -- so a RECOMP_PEEK run alongside RECOMP_STACK_DUMP printed
         * nothing at all. Sample here, where the interest actually is. */
        extern void xbox_PeekSample(const char *label);
        xbox_PeekSample("dump");
    }
    _exit(0);
    return 0;
}

static LONG CALLBACK veh_handler(PEXCEPTION_POINTERS ep)
{
    if (ep->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION) {
        uintptr_t fault_addr = ep->ExceptionRecord->ExceptionInformation[1];

        /*
         * GPU registers -> the NV2A model.
         *
         * xbox_VehMmioFault below covers the APU and USB OHCI only. The NV2A
         * aperture is trapped by nv2a_hook_init (called from WinMain), so its
         * accesses fault here and have to be decoded and stepped over by the
         * model -- otherwise the title's D3D8 sees an access violation instead
         * of a GPU and nothing is ever drawn. The old code returned
         * EXCEPTION_CONTINUE_SEARCH for this whole range as a TODO, which is
         * exactly the "no GPU" behaviour.
         */
        {
            int is_write = ep->ExceptionRecord->ExceptionInformation[0] ? 1 : 0;
            uint32_t va = (uint32_t)(fault_addr - (uintptr_t)g_xbox_mem_offset);

            if (va >= 0xFD000000u && va < 0xFE000000u) {
                if (nv2a_hook_handle_mmio(ep->ContextRecord, fault_addr, va,
                                          is_write))
                    return EXCEPTION_CONTINUE_EXECUTION;
                if (!is_write && nv2a_hook_handle_vram(fault_addr, va))
                    return EXCEPTION_CONTINUE_EXECUTION;
            }

            /* AC97 codec page: answer from the shadow so the voice-kick
             * handshake completes immediately rather than depending on a host
             * thread winning a race with the guest's next read. */
            if (va >= AC97_BASE && va < AC97_BASE + AC97_SIZE && g_ac97_ready) {
                if (mmio_emulate(ep->ContextRecord, va - AC97_BASE, NULL,
                                 ac97_rd, ac97_wr))
                    return EXCEPTION_CONTINUE_EXECUTION;
            }
        }

        /* Trapped hardware registers (APU, USB OHCI) -> their models.
         *
         * The other half of RECOMP_AC97_READY: reporting the codec ready is
         * what lets DirectSoundCreate past its first gate, and from there it
         * drives the APU directly, so the registers have to fault to be seen.
         * "Handled" means the access was decoded and the instruction stepped
         * over, so execution resumes instead of unwinding. */
        if (xbox_VehMmioFault(ep))
            return EXCEPTION_CONTINUE_EXECUTION;

        fprintf(stderr, "[CRASH] Access violation at RIP=0x%llX, fault addr=0x%llX (%s)\n",
            (unsigned long long)ep->ContextRecord->Rip,
            (unsigned long long)fault_addr,
            ep->ExceptionRecord->ExceptionInformation[0] ? "write" : "read");
        fprintf(stderr, "  Xbox regs: eax=0x%08X ecx=0x%08X edx=0x%08X esp=0x%08X\n",
            g_eax, g_ecx, g_edx, g_esp);
        fprintf(stderr, "  Xbox regs: ebx=0x%08X esi=0x%08X edi=0x%08X\n",
            g_ebx, g_esi, g_edi);
        fprintf(stderr, "  Xbox VA of fault: 0x%08X\n",
            (uint32_t)(fault_addr - (uintptr_t)g_xbox_mem_offset));
        print_guest_context((void *)ep->ContextRecord->Rip);

        /*
         * TODO: Add game-specific diagnostics here. Examples:
         *
         * Dump CRT heap handle:
         *   uint32_t heap = *(uint32_t *)((uint8_t *)g_xbox_mem_offset + HEAP_HANDLE_VA);
         *   fprintf(stderr, "  CRT heap handle: 0x%08X\n", heap);
         *
         * Dump game state:
         *   uint32_t state = *(uint32_t *)((uint8_t *)g_xbox_mem_offset + GAME_STATE_VA);
         *   fprintf(stderr, "  Game state: %u\n", state);
         */

        /* Print native stack return addresses for debugging */
        {
            uintptr_t *sp = (uintptr_t *)ep->ContextRecord->Rsp;
            fprintf(stderr, "  Native stack (first 8 return addrs):\n");
            for (int i = 0; i < 64 && sp[i]; i++) {
                if (sp[i] >= 0x140000000ULL && sp[i] < 0x150000000ULL) {
                    fprintf(stderr, "    [%d] 0x%llX\n", i, (unsigned long long)sp[i]);
                }
            }
        }
        fflush(stderr);
    }

    return EXCEPTION_CONTINUE_SEARCH;
}

/* ── WinMain ───────────────────────────────────────────────── */

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance,
                   LPSTR lpCmdLine, int nCmdShow)
{
    void *xbe_data = NULL;
    size_t xbe_size = 0;

    (void)hInstance;
    (void)hPrevInstance;
    (void)lpCmdLine;
    (void)nCmdShow;

    /* Unbuffered output for immediate visibility during debugging */
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    /* Optional PC sampler: RECOMP_PC_SAMPLE=<seconds>. Dumps per-thread RIPs
     * and exits; used to find where the title spins. */
    if (getenv("RECOMP_PC_SAMPLE")) {
        DWORD secs = (DWORD)atoi(getenv("RECOMP_PC_SAMPLE"));
        if (secs)
            CreateThread(NULL, 0, sampler_thread, (LPVOID)(intptr_t)secs,
                         0, NULL);
    }
    if (getenv("RECOMP_STACK_DUMP")) {
        int secs = atoi(getenv("RECOMP_STACK_DUMP"));
        if (secs > 0)
            CreateThread(NULL, 0, stack_dumper, (LPVOID)(intptr_t)secs,
                         0, NULL);
    }
    if (getenv("RECOMP_STATE_SNAP")) {
        CreateThread(NULL, 0, state_snap_thread, NULL, 0, NULL);
        CreateThread(NULL, 0, engine_snap_thread, NULL, 0, NULL);
    }


    printf("=== YOUR_GAME_NAME - Static Recompilation ===\n");
    printf("Loading XBE...\n");

    /* Install VEH handler (first handler in chain) */
    /* Load symbols up front rather than from inside the handler: at fault
     * time the process is already in a bad way, and SymInitialize
     * allocates. Failure is not fatal -- the handler prints no name. */
    SymSetOptions(SYMOPT_DEFERRED_LOADS | SYMOPT_UNDNAME);
    SymInitialize(GetCurrentProcess(), NULL, TRUE);
    AddVectoredExceptionHandler(1, veh_handler);

    /* Step 1: Load XBE, resolved against the exe's own directory. */
    {
        char dir[MAX_PATH], xbe_path[MAX_PATH], game_dir[MAX_PATH];

        exe_dir(dir, sizeof dir);
        resolve_paths(dir, xbe_path, sizeof xbe_path, game_dir, sizeof game_dir);

        /* RECOMP_DATA_DIR overrides the disc tree, so an A/B between two
         * extractions is an environment variable instead of a rename. That
         * matters because the two trees behave completely differently: the full
         * one reaches DirectSound and (without a codec) dies on the NULL sound
         * object, while the truncated one falls through to the loading screen. */
        {
            const char *ov = getenv("RECOMP_DATA_DIR");
            if (ov && *ov && path_is_dir(ov))
                snprintf(game_dir, sizeof game_dir, "%s", ov);
        }

        /* A path on the command line wins, but only if it names an .xbe:
         * some launchers (and wine when given a full path) put the program's
         * own name in lpCmdLine, and loading the exe as the game is worse than
         * ignoring the argument. */
        if (lpCmdLine && *lpCmdLine) {
            char *p = lpCmdLine;
            size_t len;
            if (*p == '"') { p++; len = strcspn(p, "\""); }
            else           { len = strcspn(p, " \t"); }
            if (len > 4 && len < sizeof xbe_path &&
                p[len - 4] == '.' &&
                (p[len - 3] == 'x' || p[len - 3] == 'X') &&
                (p[len - 2] == 'b' || p[len - 2] == 'B') &&
                (p[len - 1] == 'e' || p[len - 1] == 'E')) {
                memcpy(xbe_path, p, len);
                xbe_path[len] = '\0';
            }
        }

        printf("exe dir:   %s\n", dir);
        printf("XBE:       %s\n", xbe_path);
        printf("game dir:  %s\n", game_dir);

        if (!load_xbe(xbe_path, &xbe_data, &xbe_size)) {
            char msg[MAX_PATH + 256];
            snprintf(msg, sizeof msg,
                     "Failed to load the XBE:\n\n%s\n\n"
                     "Put default.xbe in a 'game_files' folder beside this exe.",
                     xbe_path);
            MessageBoxA(NULL, msg, "Recomp", MB_ICONERROR);
            return 1;
        }
        printf("XBE loaded: %zu bytes\n", xbe_size);

        /* Stashed for the kernel's path translation below. */
        strncpy(g_game_dir, game_dir, sizeof g_game_dir - 1);
    }

    /* Step 2: Initialize Xbox memory layout */
    printf("Initializing Xbox memory layout...\n");
    if (!xbox_MemoryLayoutInit(xbe_data, xbe_size)) {
        MessageBoxA(NULL, "Failed to initialize Xbox memory layout.\n"
                    "The required virtual address range may be unavailable.",
                    "Recomp", MB_ICONERROR);
        free(xbe_data);
        return 1;
    }

    g_xbox_mem_offset = xbox_GetMemoryOffset();
    printf("Xbox memory mapped. Offset: 0x%llX\n", (unsigned long long)g_xbox_mem_offset);

    /*
     * Rendering chain, in the order it has to come up:
     *
     *   nv2a_hook_init      gives the GPU register aperture real semantics and
     *                       traps the PVIDEO page. Nothing in the runtime calls
     *                       it -- the host main is responsible -- and without it
     *                       every GPU register read returns 0, so the title's
     *                       D3D8 (its own lifted code, not ours) never gets a
     *                       device off the ground.
     *   RECOMP_PB_EXEC      runs the pushbuffer executor, which is what turns
     *                       the title's GPU commands into a framebuffer and
     *                       calls xbox_FramebufferWindowStart().
     *   RECOMP_FB_WINDOW    opens the window that presents that framebuffer.
     *
     * The two env vars are set only when the user has not, so explicit
     * overrides still win; RECOMP_HEADLESS suppresses the window.
     */
    nv2a_hook_init(g_xbox_mem_offset);

    /* USB / XID.
     *
     * The gamepad is reached the same way the GPU is: the title's own XID
     * driver talks to the host controller's registers at 0xFED00000, and the
     * MCPX aperture is backed as plain zeroed RAM, so without this every
     * register reads 0 and no controller is ever seen. Nothing in the runtime
     * calls it either -- same as nv2a_hook_init, the host main is responsible.
     *
     * Still opt-in on RECOMP_USB inside xbox_OhciInit(); this only makes the
     * call happen. */
    {
        extern void xbox_OhciInit(void);
        xbox_OhciInit();
    }

    /* _putenv_s, not SetEnvironmentVariableA: the runtime reads these with the
     * CRT's getenv(), which keeps its own copy of the environment that the
     * Win32 call does not update. */
    if (!getenv("RECOMP_PB_EXEC"))
        _putenv_s("RECOMP_PB_EXEC", "1");
    if (!getenv("RECOMP_FB_WINDOW") && !getenv("RECOMP_HEADLESS"))
        _putenv_s("RECOMP_FB_WINDOW", "1");
    /* The GPU interrupt line is driven by the vblank scheduler, and it is off
     * when RECOMP_VBLANK is unset. Measured with it on: 3,000 vblanks at
     * ~60 Hz, ack give-ups 0 -- the guest ISR claims every one. Without it the
     * title waits on an interrupt that is never raised. */
    if (!getenv("RECOMP_VBLANK"))
        _putenv_s("RECOMP_VBLANK", "paced");
    /* Recognise events the title builds itself: without this, waits on them
     * resolve to an invalid handle and return immediately (632,817 spins in a
     * minute); with it they resolve to real host events. */
    if (!getenv("RECOMP_TITLE_KEVENTS"))
        _putenv_s("RECOMP_TITLE_KEVENTS", "1");

    printf("Rendering: PB exec %s, window %s\n",
           getenv("RECOMP_PB_EXEC") ? getenv("RECOMP_PB_EXEC") : "off",
           getenv("RECOMP_FB_WINDOW") ? getenv("RECOMP_FB_WINDOW") : "off");

    /* Bring up the emulated APU.
     *
     * Gated on the same variable that unmaps its registers, because the two
     * halves are useless apart: a trap with no model declines every access,
     * and a model nothing traps into never sees a register. The APU walks
     * Xbox physical RAM to find voice buffers, so it gets the guest RAM
     * base. */
    if (getenv("RECOMP_AC97_READY")) {
        g_apu_state = mcpx_apu_init_standalone((uint8_t *)xbox_GetMemoryBase());
        fprintf(stderr, "[BOOT] emulated APU %s\n",
                g_apu_state ? "up" : "FAILED to initialise");
    }

    /* Step 3: Initialize Xbox kernel */
    printf("Initializing Xbox kernel replacement...\n");
    xbox_kernel_init();

    /* Step 4: Set game directory for file I/O path translation */
    {
        extern void xbox_path_init(const char *game_dir, const char *save_dir);
        xbox_path_init(g_game_dir, NULL);
    }

    /* Step 5: Initialize kernel bridge (thunk table in Xbox memory) */
    printf("Initializing kernel bridge...\n");
    xbox_kernel_bridge_init();

    /* APU/AC97 kick handshake, unless disabled. */
    if (!getenv("RECOMP_KICK_ACK") || atoi(getenv("RECOMP_KICK_ACK")) != 0)
        CreateThread(NULL, 0, kick_acker, NULL, 0, NULL);

    /* Trap the codec page so the kick handshake is answered by the decoder.
     * OPT-IN: measured worse as a default (FPS 17.8 -> 5.0, draws 8 -> 2) --
     * every codec access becomes a signal + decode, and the title polls this
     * page hard. The 1 ms acker above is cheaper in practice. */
    if (getenv("RECOMP_AC97_TRAP"))
        ac97_trap_init();

    /* Shadow's D3D waits for the GPU to consume its pushbuffer by spinning on
     * a fence: PUT at device+0x2C, a pointer to the GPU's GET at device+0x30,
     * with the device pointer held at 0x3E0A18. The runtime already knows how
     * to mirror that (xbox_Nv2aMirrorFence, polled from its own thread), and
     * without it the title sits at ~280% CPU with no disk I/O and never draws
     * past the loading screen. Registering the pair is one call. */
    if (!getenv("RECOMP_NO_FENCE") &&
        xbox_Nv2aMirrorFence(0x3E0A18u, 0x2Cu, 0x30u) != 0)
        fprintf(stderr, "[BOOT] fence mirror table full\n");

    /* Step 6: Initialize stack */
    g_esp = XBOX_STACK_TOP;

    /*
     * TODO: Pre-initialize CRT globals if needed.
     *
     * Many Xbox games use the MSVC CRT. The CRT's __heap_init sets up a
     * heap descriptor at a game-specific address. You may need to
     * pre-initialize __active_heap to avoid small-block heap issues:
     *
     *   uint32_t *active_heap = (uint32_t *)((uint8_t *)g_xbox_mem_offset + ACTIVE_HEAP_VA);
     *   *active_heap = 1;  // 1 = system heap (HeapAlloc), avoids SBH init
     *
     * Find ACTIVE_HEAP_VA by searching the disassembly for __heap_init
     * or by looking for the CRT's __active_heap global in the data section.
     */

    printf("\n=== Initialization complete ===\n");
    printf("Entry point: 0x%08X\n", YOUR_GAME_ENTRY_POINT);
    printf("ESP: 0x%08X\n", g_esp);

    /* Build the flat dispatch table before the guest runs.
     *
     * Without this recomp_lookup falls back to a binary search over the
     * whole function table -- roughly log2(n) branches on *every*
     * indirect call, which for a 45,000-function C++ title is about 16
     * every time the game goes through a vtable. Half-Life 2 spent most
     * of its static initialisation inside recomp_lookup for exactly this
     * reason, and it read as a hang.
     *
     * Optional by design: if the allocation fails the search still works,
     * so a failure is worth one line and not a fatal error. */
    if (!recomp_dispatch_init())
        fprintf(stderr, "[BOOT] flat dispatch unavailable; "
                        "indirect calls will use the binary search\n");

    /* Arm the hang watchdog. Does nothing unless RECOMP_WATCHDOG_SECS is set,
     * and must be called from this thread -- the guest registers it samples are
     * thread-local, so it has to be handed the copies belonging to the thread
     * that runs guest code.
     *
     * Not optional boilerplate: without this call RECOMP_WATCHDOG_SECS is
     * silently inert, and the one diagnostic that tells a hang from slowness
     * does nothing while appearing to be set. */
    xbox_WatchdogStart();

    /* Step 7: Call the recompiled entry point */
    printf("\nStarting game...\n");
    fflush(stdout);

    xbe_entry_point();

    printf("\nGame returned. Cleaning up...\n");

    /* Cleanup */
    xbox_kernel_shutdown();
    xbox_MemoryLayoutShutdown();
    free(xbe_data);

    return 0;
}

/* ── XBE Loading ───────────────────────────────────────────── */

static BOOL load_xbe(const char *path, void **out_data, size_t *out_size)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "Cannot open XBE: %s\n", path);
        return FALSE;
    }

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (size <= 0) {
        fclose(f);
        return FALSE;
    }

    void *data = malloc((size_t)size);
    if (!data) {
        fclose(f);
        return FALSE;
    }

    if (fread(data, 1, (size_t)size, f) != (size_t)size) {
        free(data);
        fclose(f);
        return FALSE;
    }

    fclose(f);
    *out_data = data;
    *out_size = (size_t)size;
    return TRUE;
}

/* Console entry point (for debugging -- lets you see printf output) */
int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    return WinMain(GetModuleHandle(NULL), NULL, GetCommandLineA(), SW_SHOW);
}
