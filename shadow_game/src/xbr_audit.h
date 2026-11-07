/*
 * xbr_audit.h - temporary diagnostic: name the operands of the DSOUND copy that
 * faults on the full disc tree (Pass 23 / Pass 24 of XBR_ATTEMPT.md).
 *
 * Hand-written, NOT generated. Enabled at run time with RECOMP_XBR_AUDIT=1;
 * silent otherwise (one getenv, cached). Call sites live in the generated
 * recomp_0152.c / recomp_0153.c and are marked "XBR-AUDIT" -- those edits do
 * not survive regeneration by tools/xlate.py and must be re-applied.
 */
#ifndef XBR_AUDIT_H
#define XBR_AUDIT_H

#include <stdint.h>

/* 1 when RECOMP_XBR_AUDIT is set in the environment. */
int  xbr_audit_on(void);

/* Segment counter: xbr_mark(id) counts, and prints the vector once a second. */
void xbr_mark(int id);

/* Distinct-value histogram: xbr_value(tag, v). */
void xbr_value(const char *tag, uint32_t v);

/* Distinct (caller, value) pairs: xbr_pair(tag, caller, value). */
void xbr_pair(const char *tag, uint32_t a, uint32_t b);

/* Record and print each distinct caller (sp[0]) once. */
void xbr_audit_callsite(const char *what, uint32_t ret);

/* RECOMP_PUMP_YIELD=<ms>: sleep every 256 iterations of the queue pump. */
void xbr_pump_backoff(void);

/* Print a call site: site name, guest esp, and four operands with a class tag
 * (ram / low / HOLE / mmio) so a wild pointer is unmistakable. */
void xbr_audit(const char *site, uint32_t esp,
               uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3);

/* Print a frame's parameters from [ebp+8] and, for each that looks like a
 * pointer, its [0x8] and [0xC] fields. */
void xbr_audit_args(const char *site, uint32_t esp, uint32_t ebp, int nargs);
void xbr_audit_global(const char *name, uint32_t va);

/* Follow this -> [0x14] -> [0x8] -> [0x10] and say where the chain breaks. */
void xbr_audit_obj(const char *site, uint32_t esp, uint32_t this_);

/* The `rep movsd` helper's view: ctx is `this` in ecx, and it derives its
 * destination from *(*(ctx+0x10)) + off. Prints every piece and says which of
 * them is already outside guest RAM. */
void xbr_audit_copy(const char *site, uint32_t esp, uint32_t ctx,
                    uint32_t size, uint32_t src, uint32_t off);

#endif /* XBR_AUDIT_H */
