/*
 * xbr_audit.c - see xbr_audit.h.
 *
 * Why this exists rather than a debugger breakpoint: the fault is inside
 * recompiled guest code, so there is no guest PC to break on and the host
 * address changes every build. The three call sites are the only places that
 * matter, and printing their operands answers "which pointer went wild" in one
 * run. Every read is range-checked before it happens, so the audit itself
 * cannot fault on the very value it is trying to report.
 */
#include "xbr_audit.h"

#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include <string.h>
#include <time.h>

extern ptrdiff_t g_xbox_mem_offset;

/* Guest RAM: 65,536 KB mapped at 0x00010000, i.e. [0x00010000, 0x04010000). */
#define GUEST_LO 0x00010000u
#define GUEST_HI 0x04010000u

static int s_mode = -1;   /* 0 = off, 1 = report only wild, 2 = report every call */

static int mode(void)
{
    if (s_mode < 0) {
        const char *v = getenv("RECOMP_XBR_AUDIT");
        s_mode = !v ? 0 : (strcmp(v, "all") == 0 ? 2 : 1);
    }
    return s_mode;
}

int xbr_audit_on(void)
{
    return mode() != 0;
}

/* Both aliases of guest RAM are mapped too, and the audit must accept them or
 * it reports ordinary memcpys through the physical alias as wild:
 *   0x80000000 .. 0x84000000   contiguous window (physical-memory alias)
 *   0xF0000000 .. 0xF4000000   tiled aperture, aliases the contiguous window  */
static int in_ram(uint32_t a)
{
    if (a >= GUEST_LO && a < GUEST_HI)
        return 1;
    if (a >= 0x80000000u && a < 0x84000000u)
        return 1;
    if (a >= 0xF0000000u && a < 0xF4000000u)
        return 1;
    return 0;
}

/* A value that cannot be a guest pointer and is not a small integer: the
 * address holes [0x04010000, 0x80000000) and (0x84000000, 0xFD000000). */
static int wild(uint32_t a)
{
    if (a >= 0xFD000000u)
        return 0;
    return a >= 0x04010000u && (a < 0x80000000u || a >= 0x84000000u);
}

/* Read a guest dword, but only if the address is inside guest RAM. Returns 0
 * and sets *ok = 0 otherwise, so a wild pointer is reported rather than chased. */
static uint32_t peek32(uint32_t a, int *ok)
{
    if (!in_ram(a) || !g_xbox_mem_offset) {
        if (ok) *ok = 0;
        return 0;
    }
    if (ok) *ok = 1;
    return *(volatile uint32_t *)((uintptr_t)a + g_xbox_mem_offset);
}

/* Byte-wide variant, for the single-byte mode flags the boot checks. */
static uint32_t peek8(uint32_t a, int *ok)
{
    if (!in_ram(a) || !g_xbox_mem_offset) {
        if (ok) *ok = 0;
        return 0;
    }
    if (ok) *ok = 1;
    return *(volatile uint8_t *)((uintptr_t)a + g_xbox_mem_offset);
}

/* Print a guest C string (truncated), for naming task roots. */
static void xbr_str(const char *tag, uint32_t a)
{
    char buf[64];
    int i;

    if (!in_ram(a) || !g_xbox_mem_offset)
        return;
    for (i = 0; i < (int)sizeof buf - 1; i++) {
        char c = *(volatile char *)((uintptr_t)(a + (uint32_t)i) + g_xbox_mem_offset);
        if (!c)
            break;
        buf[i] = (c >= 0x20 && c < 0x7F) ? c : '?';
    }
    buf[i] = '\0';
    if (i > 0)
        fprintf(stderr, "[%s] \"%s\"\n", tag, buf);
}

static const char *cls(uint32_t a)
{
    if (in_ram(a))          return "ram";
    if (a < GUEST_LO)       return "low";
    if (a < 0xFD000000u)    return "HOLE";   /* unmapped guest address hole */
    return "mmio";
}

/* The guest stack still holds return addresses pushed by the lifted calls, so
 * scanning up from esp names the chain that got us here. Same technique (and
 * the same caveat -- it also sees stale slots) as the [FILE] caller chain. */
static void chain(uint32_t esp)
{
    const uint32_t *sp;
    int i, shown = 0;

    if (!in_ram(esp) || !g_xbox_mem_offset) {
        printf("    chain: esp 0x%08X (%s) not readable\n", esp, cls(esp));
        return;
    }
    sp = (const uint32_t *)((uintptr_t)esp + g_xbox_mem_offset);
    printf("    chain:");
    for (i = 0; i < 256 && shown < 6; i++) {
        uint32_t v = sp[i];
        if (v > 0x00011000u && v < 0x0069CBA0u) {
            printf(" 0x%08X", v);
            shown++;
        }
    }
    printf("\n");
}

/* Print a frame's parameters from [ebp+8] onward, and for each one that looks
 * like a pointer, its [0x8] and [0xC] fields. Used at the DSOUND entry that
 * hands a bad `this` down to the copy. */
void xbr_audit_args(const char *site, uint32_t esp, uint32_t ebp, int nargs)
{
    int i, ok;

    printf("[XBR-AUDIT] %s esp=0x%08X ebp=0x%08X\n", site, esp, ebp);
    for (i = 0; i < nargs; i++) {
        uint32_t a = peek32(ebp + 8u + (uint32_t)i * 4u, &ok);
        printf("    p%-2d [ebp+0x%02X] = 0x%08X (%s%s)", i + 1, 8 + i * 4,
               a, cls(a), ok ? "" : " UNREADABLE");
        if (ok && in_ram(a)) {
            int o1 = 0, o2 = 0;
            uint32_t f8  = peek32(a + 0x8u, &o1);
            uint32_t f12 = peek32(a + 0xCu, &o2);
            printf("   ->[0x8]=0x%08X(%s) ->[0xC]=0x%08X(%s)",
                   f8, cls(f8), f12, cls(f12));
        }
        printf("\n");
    }
    chain(esp);
    fflush(stdout);
}

/* Read a guest global for the record. */
void xbr_audit_global(const char *name, uint32_t va)
{
    int ok = 0;
    uint32_t v = peek32(va, &ok);
    printf("    global %-10s @0x%08X = 0x%08X%s\n", name, va, v,
           ok ? "" : " UNREADABLE");
}

/* The sound object chain these functions walk:
 *     this -> [0x14] -> (sub-object X) -> [0x8] -> (sub-object Y) -> [0x10] -> base
 * A zero anywhere names the field that was never initialised. */
void xbr_audit_obj(const char *site, uint32_t esp, uint32_t this_)
{
    int ok1 = 0, ok2 = 0, ok3 = 0;
    uint32_t x = peek32(this_ + 0x14u, &ok1);
    uint32_t y = ok1 ? peek32(x + 0x8u, &ok2) : 0;
    uint32_t z = ok2 ? peek32(y + 0x10u, &ok3) : 0;

    printf("[XBR-AUDIT] %s esp=0x%08X this=0x%08X(%s)\n",
           site, esp, this_, cls(this_));
    printf("    this->[0x14]=0x%08X(%s%s)  ->[0x8]=0x%08X(%s%s)  ->[0x10]=0x%08X(%s%s)\n",
           x, cls(x), ok1 ? "" : " UNREADABLE",
           y, cls(y), ok2 ? "" : " UNREADABLE",
           z, cls(z), ok3 ? "" : " UNREADABLE");
    {
        /* Six dwords of the sub-object, which is what identifies it. */
        int i, ok;
        printf("    X[0x00..0x14]:");
        for (i = 0; i < 6; i++) {
            uint32_t v = peek32(x + (uint32_t)i * 4u, &ok);
            printf(" %08X%s", v, ok ? "" : "!");
        }
        printf("\n    this[0x00..0x14]:");
        for (i = 0; i < 6; i++) {
            uint32_t v = peek32(this_ + (uint32_t)i * 4u, &ok);
            printf(" %08X%s", v, ok ? "" : "!");
        }
        printf("\n");
    }
    if (!x || !y || !z)
        printf("    >>> ZERO LINK: the chain is broken at %s\n",
               !x ? "this->[0x14]" : !y ? "X->[0x08]" : "Y->[0x10]");
    {
        /* Raw, unfiltered: sp[0] is the return address the caller pushed. The
         * filtered chain below skips out-of-range words and can therefore point
         * at a stale slot instead. */
        int i, ok;
        printf("    raw esp[0..7]:");
        for (i = 0; i < 8; i++) {
            uint32_t v = peek32(esp + (uint32_t)i * 4u, &ok);
            printf(" %08X%s", v, ok ? "" : "!");
        }
        printf("\n");
    }
    chain(esp);
    fflush(stdout);
}

/* ---- segment counters: where inside a function the time actually goes ------
 *
 * A stall is only actionable once you know which *segment* of the function is
 * repeating. xbr_mark(id) counts and, once a second, prints the whole vector,
 * so a guest loop becomes a rate you can read off: the segment whose counter is
 * climbing is the one spinning. Gated by RECOMP_XBR_AUDIT; silent otherwise.
 */
#define XBR_MARKS 32
static unsigned long s_marks[XBR_MARKS];
static time_t        s_mark_last;
static unsigned long s_mark_total;

/* ---- pump backoff ---------------------------------------------------------
 *
 * The title's queue pump (sub_00328E80) loops over six queues with no backoff,
 * and four producers push into them through one guest lock. On a console that
 * spin is tolerable; here it thrashes every core and the threads that need to
 * load (and render) get starved. RECOMP_PUMP_YIELD=<ms> inserts a sleep every
 * 256 iterations - a diagnostic to answer "is the pump starving the rest of
 * the title", not a fix.
 */
#if defined(_WIN32)
#include <windows.h>
static void xbr_sleep_ms(unsigned ms) { Sleep((DWORD)ms); }
#else
#include <time.h>
static void xbr_sleep_ms(unsigned ms)
{
    struct timespec ts;
    ts.tv_sec  = (time_t)(ms / 1000u);
    ts.tv_nsec = (long)(ms % 1000u) * 1000000L;
    nanosleep(&ts, NULL);
}
#endif

static int s_pump_ms = -1;
static unsigned long s_pump_n;

void xbr_pump_backoff(void)
{
    if (s_pump_ms < 0) {
        const char *v = getenv("RECOMP_PUMP_YIELD");
        s_pump_ms = v ? atoi(v) : 0;
        if (s_pump_ms < 0) s_pump_ms = 0;
        if (s_pump_ms > 50) s_pump_ms = 50;
    }
    if (!s_pump_ms)
        return;
    if ((++s_pump_n & 0xFFu) == 0)
        xbr_sleep_ms((unsigned)s_pump_ms);
}

/* ---- distinct-caller collector --------------------------------------------
 *
 * For a function reached only through a vtable, there is no direct call site to
 * grep for. The pushed return address at entry (sp[0]) is authoritative where a
 * stack scan is not, so record each distinct one and print it once. Bounded, so
 * a 33,000-calls-a-second target costs 16 lines, not a flooded log.
 */
#define XBR_SITES 16
static uint32_t s_site[XBR_SITES];
static unsigned long s_site_n[XBR_SITES];
static int      s_sites;
static time_t   s_site_last;

void xbr_audit_callsite(const char *what, uint32_t ret)
{
    int i;
    time_t now;

    if (!xbr_audit_on() || !ret)
        return;
    for (i = 0; i < s_sites; i++) {
        if (s_site[i] == ret) {
            s_site_n[i]++;
            break;
        }
    }
    if (i == s_sites && s_sites < XBR_SITES) {
        s_site[s_sites] = ret;
        s_site_n[s_sites] = 1;
        s_sites++;
        fprintf(stderr, "[XBR-CALLER] %s <- 0x%08X  (new, %d distinct)\n",
                what, ret, s_sites);
        fflush(stderr);
    }
    /* Once a second, print the per-site counts: whoever is at the top is the
     * loop that owns the poll. */
    now = time(NULL);
    if (s_site_last == 0)
        s_site_last = now;
    if (now != s_site_last) {
        fprintf(stderr, "[XBR-CALLER] %s counts:", what);
        for (i = 0; i < s_sites; i++)
            fprintf(stderr, " 0x%08X=%lu", s_site[i], s_site_n[i]);
        fprintf(stderr, "\n");
        fflush(stderr);
        s_site_last = now;
    }
}


/* Dump the guest's six work-queue slots once a second alongside the marks.
 *
 * The table is 0x5608A0 with a stride of 0x48 - sub_0032D430 computes
 * `esi = arg; edi = esi + esi*8; edi = edi*8 + 0x5608A0`, i.e. arg*72. (Reading
 * it at a 0x40 stride shows all zeros, which is how this was first got wrong.)
 * If loading is stalled by a producer with no consumer, one slot's counters grow
 * and never drain - visible here, invisible in a stack dump.
 */
#define XBR_QUEUE_TABLE 0x5608A0u
#define XBR_QUEUE_SLOTS 6
#define XBR_QUEUE_STRIDE 0x48u
#define XBR_QUEUE_WORDS 8

/* Dump the live NowLoadingTask so its completion condition is visible.
 *
 * SetFinished (sub_00059890) does nothing unless [task+0x28] is non-zero, and the
 * constructor (sub_000598D0) sets it to 0 - so if it stays 0 the loading task can
 * never finish no matter what the driver does. Fields, from the ctor and setters:
 *   +0x0C byte flags   +0x28 child/sub-resource   +0x2C state bits   +0x30 progress
 */
#define XBR_LOADING_TASK 0x544F40u
#define XBR_LOAD_PROGRESS 0x541814u

static void xbr_dump_queues(void)
{
    int i, k;
    char line[900];
    int n = 0;
    uint32_t task;

    if (!g_xbox_mem_offset)
        return;
    for (i = 0; i < XBR_QUEUE_SLOTS; i++) {
        uint32_t base = XBR_QUEUE_TABLE + (uint32_t)i * XBR_QUEUE_STRIDE;
        n += snprintf(line + n, sizeof line - (size_t)n, " q%d:", i);
        for (k = 0; k < XBR_QUEUE_WORDS && n < (int)sizeof line - 12; k++) {
            uint32_t v = *(volatile uint32_t *)((uintptr_t)(base + (uint32_t)k * 4u)
                                                + g_xbox_mem_offset);
            n += snprintf(line + n, sizeof line - (size_t)n, " %08X", v);
        }
    }
    fprintf(stderr, "[XBR-QUEUES]%s\n", line);

    /* the loading task, then its four interesting fields */
    task = *(volatile uint32_t *)((uintptr_t)XBR_LOADING_TASK + g_xbox_mem_offset);
    if (task) {
        const uint32_t *p = (const uint32_t *)((uintptr_t)task + g_xbox_mem_offset);
        uint32_t progress = *(volatile uint32_t *)
                ((uintptr_t)XBR_LOAD_PROGRESS + g_xbox_mem_offset);
        fprintf(stderr, "[XBR-TASK] task=0x%08X vtbl=0x%08X name=0x%08X"
                        " flags=%02X child(+28)=0x%08X state(+2C)=0x%08X"
                        " prog(+30)=%d | LOADPROG=0x%08X\n",
                task, p[0], p[2],
                (unsigned)(*(volatile uint8_t *)((uintptr_t)task + 0x0Cu
                                                 + g_xbox_mem_offset)),
                p[0x28 / 4], p[0x2C / 4], (int)p[0x30 / 4], progress);
        /* Follow the child one level: SetFinished releases [task+0x28], so the
         * child is the thing the loading task is waiting on. Its name field (+8)
         * is a pointer to a class-name string, same convention as the parent. */
        if (p[0x28 / 4]) {
            const uint32_t *c = (const uint32_t *)
                    ((uintptr_t)p[0x28 / 4] + g_xbox_mem_offset);
            fprintf(stderr, "[XBR-CHILD]  ptr=0x%08X vtbl=0x%08X name=0x%08X"
                            " child(+28)=0x%08X state(+2C)=0x%08X prog(+30)=%d\n",
                    p[0x28 / 4], c[0], c[2],
                    c[0x28 / 4], c[0x2C / 4], (int)c[0x30 / 4]);
        }
    } else {
        fprintf(stderr, "[XBR-TASK] [0x544F40] = 0 (no loading task)\n");
    }

    /* The CSD (.xncp) path the loader is currently working on. FUN_000184f0 does
     * `sprintf(&DAT_00537790, "%s.xncp", name)`, and FUN_00017a80 only reaches the
     * loading task's finish (FUN_00025810(0)) when FUN_002b47c0 on that path
     * succeeds. Printing it names the asset the boot is waiting for. */
    {
        const char *p = (const char *)((uintptr_t)0x00537790u + g_xbox_mem_offset);
        char path[80];
        int i;
        for (i = 0; i < (int)sizeof path - 1 && i < 64; i++) {
            char c = p[i];
            path[i] = (c >= 0x20 && c < 0x7F) ? c : '\0';
            if (c == '\0')
                break;
        }
        path[i] = '\0';
        fprintf(stderr, "[XBR-XNCP] \"%s\"\n", path);
    }

    /* The two loader entries in FUN_00280f90's table (0x5258C8, stride 0x18).
     * sub_00017A80 runs its body -- the sprintf, the CSD load and the finish --
     * only while [entry+8] == 0. If that field is set but the CSD load failed,
     * the failure is latched: no retry, no finish, and the boot stalls with the
     * last name still sitting in the buffer above. */
    {
        int k;
        for (k = 0; k < 2; k++) {
            uint32_t slot = (k == 0) ? 0x005258C8u : 0x005258E0u;
            int ok = 0;
            uint32_t obj = peek32(slot, &ok);
            int ok2 = 0;
            uint32_t latch = ok ? peek32(obj + 8u, &ok2) : 0;
            fprintf(stderr, "[XBR-LATCH] entry%d slot=0x%08X this=0x%08X"
                            " (+8)=0x%08X%s\n",
                    k, slot, obj, latch,
                    (ok && obj && latch) ? "  LATCHED - body skipped" : "");
        }
    }

    /* TGameInfomation's constructor only creates TGiHissatu (and its other
     * sub-objects) when DAT_0054DDE4 and DAT_0054DDE2 are zero, so those two
     * bytes decide whether the last load step exists at all. DAT_0054DDE1 gates
     * a third sub-object, and 0x559568 caches the "number.txd" texture. */
    {
        int ok = 0;
        fprintf(stderr, "[XBR-FLAGS] 54DDE4=%u 54DDE2=%u 54DDE1=%u 559568=0x%08X\n",
                (unsigned)peek8(0x0054DDE4u, &ok),
                (unsigned)peek8(0x0054DDE2u, &ok),
                (unsigned)peek8(0x0054DDE1u, &ok),
                peek32(0x00559568u, &ok));
    }

    /* TaskManager::GetTaskRoot(index) reads this[1 + index] -- i.e. 4 bytes past
     * the instance -- and asserts non-null. The instance is the singleton cached
     * at 0x53948C by FUN_00019450. FUN_0027D710 asks for slot 3 (to build
     * TGameInfomation, and with it TGiHissatu and the finish) and SetFinished
     * asks for slot 5, so dumping the slots says whether the roots those calls
     * want even exist. */
    {
        int ok = 0;
        uint32_t tm = peek32(0x0053948Cu, &ok);
        fprintf(stderr, "[XBR-TASKMGR] this=0x%08X%s\n", tm, tm ? "" : "  (not created)");
        if (ok && tm) {
            int i;
            uint32_t slot[16];
            for (i = 0; i < 16; i++) {
                int ok2 = 0;
                slot[i] = peek32(tm + 4u + (uint32_t)i * 4u, &ok2);
            }
            fprintf(stderr, "[XBR-TASKMGR] slots:");
            for (i = 0; i < 16; i++)
                fprintf(stderr, " %d=0x%08X", i, slot[i]);
            fprintf(stderr, "\n");
            for (i = 0; i < 16; i++) {
                if (!slot[i])
                    continue;
                {
                    int ok3 = 0;
                    uint32_t vt = peek32(slot[i], &ok3);
                    uint32_t nm = peek32(slot[i] + 8u, &ok3);
                    fprintf(stderr, "[XBR-TASKMGR] slot%d task=0x%08X vtbl=0x%08X"
                                    " name=0x%08X\n", i, slot[i], vt, nm);
                    xbr_str("XBR-TASKNAME", nm);
                }
            }
        }
    }
    fflush(stderr);
}

static void xbr_dump_queues(void);


/* Distinct-value collector with its own table (the callsite collector's slots
 * are already used by the lock wrappers, so they must not share state).
 * Prints each new value once, then a per-second histogram.
 */
static uint32_t s_vals[16];
static unsigned long s_val_n[16];
static int  s_vals_c;
static time_t s_val_t;

/* Distinct (caller, value) pair collector, with its own table: the TLS setter
 * needs to be judged as "who writes what", and one-value-in-isolation cannot say
 * that. Prints each new pair once, then a per-second histogram.
 */
static uint32_t s_pair_a[16], s_pair_b[16];
static unsigned long s_pair_n[16];
static int  s_pairs_c;
static time_t s_pair_t;

void xbr_pair(const char *tag, uint32_t a, uint32_t b)
{
    int i;
    time_t now;

    if (!xbr_audit_on())
        return;
    for (i = 0; i < s_pairs_c; i++) {
        if (s_pair_a[i] == a && s_pair_b[i] == b) {
            s_pair_n[i]++;
            goto histogram;
        }
    }
    if (s_pairs_c < 16) {
        s_pair_a[s_pairs_c] = a;
        s_pair_b[s_pairs_c] = b;
        s_pair_n[s_pairs_c] = 1;
        s_pairs_c++;
        fprintf(stderr, "[XBR-PAIR] %s new: caller=0x%08X value=%u (0x%X)\n",
                tag, a, b, b);
        fflush(stderr);
    }
histogram:
    now = time(NULL);
    if (s_pair_t == 0)
        s_pair_t = now;
    if (now != s_pair_t) {
        fprintf(stderr, "[XBR-PAIR] %s:", tag);
        for (i = 0; i < s_pairs_c; i++)
            fprintf(stderr, " 0x%08X<-v%u(x%lu)",
                    s_pair_a[i], s_pair_b[i], s_pair_n[i]);
        fprintf(stderr, "\n");
        fflush(stderr);
        s_pair_t = now;
    }
}

void xbr_value(const char *tag, uint32_t v)
{
    int i;
    time_t now;

    if (!xbr_audit_on())
        return;
    for (i = 0; i < s_vals_c; i++) {
        if (s_vals[i] == v) {
            s_val_n[i]++;
            goto histogram;
        }
    }
    if (s_vals_c < 16) {
        s_vals[s_vals_c] = v;
        s_val_n[s_vals_c] = 1;
        s_vals_c++;
        fprintf(stderr, "[XBR-VAL] %s new = %u (0x%X)\n", tag, v, v);
        fflush(stderr);
    }
histogram:
    now = time(NULL);
    if (s_val_t == 0)
        s_val_t = now;
    if (now != s_val_t) {
        fprintf(stderr, "[XBR-VAL] %s:", tag);
        for (i = 0; i < s_vals_c; i++)
            fprintf(stderr, " %u(x%lu)", s_vals[i], s_val_n[i]);
        fprintf(stderr, "\n");
        fflush(stderr);
        s_val_t = now;
    }
}

void xbr_mark(int id)
{
    time_t now;

    if (!xbr_audit_on() || id < 0 || id >= XBR_MARKS)
        return;
    s_marks[id]++;
    s_mark_total++;
    now = time(NULL);
    if (s_mark_last == 0) {
        /* First call: report immediately, so a function that is entered only a
         * handful of times still names its segment. */
        s_mark_last = now;
        s_marks[id] += 0;
        fprintf(stderr, "[XBR-MARK] first id=%d\n", id);
        fflush(stderr);
        return;
    }
    if (now != s_mark_last) {
        int i;
        fprintf(stderr, "[XBR-MARK]");
        for (i = 0; i < XBR_MARKS; i++)
            fprintf(stderr, " m%d=%lu", i, s_marks[i]);
        fprintf(stderr, "  (total %lu)\n", s_mark_total);
        fflush(stderr);
        xbr_dump_queues();
        s_mark_last = now;
    }
}

void xbr_audit(const char *site, uint32_t esp,
               uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3)
{
    if (mode() != 2 && !(wild(a0) || wild(a1) || wild(a2) || wild(a3)))
        return;
    printf("[XBR-AUDIT] %s esp=0x%08X\n", site, esp);
    printf("    a0=0x%08X(%s)  a1=0x%08X(%s)  a2=0x%08X(%s)  a3=0x%08X(%s)\n",
           a0, cls(a0), a1, cls(a1), a2, cls(a2), a3, cls(a3));
    chain(esp);
    fflush(stdout);
}

void xbr_audit_copy(const char *site, uint32_t esp, uint32_t ctx,
                    uint32_t size, uint32_t src, uint32_t off)
{
    int ok_p = 0, ok_b = 0;
    uint32_t p    = peek32(ctx + 0x10u, &ok_p);   /* ctx->[0x10]  */
    uint32_t base = ok_p ? peek32(p, &ok_b) : 0;  /* *ctx->[0x10] */
    uint32_t dest = base + off;
    int is_bad = !in_ram(src) || !in_ram(dest) || !in_ram(base) || !in_ram(p);

    if (mode() != 2 && !is_bad)
        return;

    printf("[XBR-AUDIT] %s esp=0x%08X\n", site, esp);
    printf("    ctx=0x%08X(%s) size=%u(0x%X) src=0x%08X(%s) off=0x%X\n",
           ctx, cls(ctx), size, size, src, cls(src), off);
    printf("    ctx+0x10=0x%08X(%s%s)  base=*(ctx+0x10)=0x%08X(%s%s)  dest=base+off=0x%08X(%s)\n",
           p, cls(p), ok_p ? "" : " UNREADABLE",
           base, cls(base), ok_b ? "" : " UNREADABLE",
           dest, cls(dest));

    /* Dump the source's first dwords and the destination base's first dwords:
     * if the copy is about to write through a wild base, what sits there is
     * what the sound init left behind. */
    {
        int i, ok;
        printf("    src[0..3] :");
        for (i = 0; i < 4; i++) {
            uint32_t v = peek32(src + (uint32_t)i * 4u, &ok);
            printf(" %08X%s", v, ok ? "" : "!");
        }
        printf("\n    base[0..3]:");
        for (i = 0; i < 4; i++) {
            uint32_t v = peek32(base + (uint32_t)i * 4u, &ok);
            printf(" %08X%s", v, ok ? "" : "!");
        }
        printf("\n");
    }
    if (!in_ram(src) || !in_ram(dest) || !in_ram(base) || !in_ram(p)) {
        printf("    >>> WILD: this copy will fault (dest=%s src=%s)\n",
               cls(dest), cls(src));
        /* Where each of them came from is on the stack. */
        chain(esp);
    }
    fflush(stdout);
}
