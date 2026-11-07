/*
 * mmio_trap_x64.h -- x86-64 POSIX MMIO trap decoder (ucontext-based).
 *
 * The toolkit ships mmio_decode.h (Win32, PCONTEXT) and mmio_decode_a64.h
 * (arm64). There is no x86-64 POSIX equivalent, and src/nv2a's POSIX
 * nv2a_hook_init is an empty stub noting "SIGSEGV-based MMIO trapping deferred
 * to main.c port". This is that port for x86-64 Linux: decode the instruction
 * that faulted on a PROT_NONE device page, service the register access against
 * the device model, and step over the instruction.
 *
 * Scope, deliberately narrow: the ops generated code actually emits against
 * MEM32()/volatile device pointers -- the move forms both ways, the immediate
 * store, the zero-extending load, and the ALU/TEST group a poll loop is built
 * from. Anything else returns 0 rather than guessing, because stepping over an
 * instruction that was not understood corrupts the guest silently.
 *
 * Flags: the lifter recomputes eflags in C (CMP_AE(_fa,_fb) and friends), so a
 * serviced access does not need to reproduce x86 flag results.
 */
#ifndef MMIO_TRAP_X64_H
#define MMIO_TRAP_X64_H

#if defined(__x86_64__) && !defined(_WIN32)

#include <ucontext.h>
#include <stdint.h>
#include <stdio.h>

typedef uint64_t (*mmio_x64_read_fn)(void *dev, uint32_t off, int size);
typedef void     (*mmio_x64_write_fn)(void *dev, uint32_t off, uint64_t val, int size);

/* modrm reg numbers (0=rax .. 7=rdi, 8..15=r8..r15) -> glibc gregs indices. */
static const int mmio_x64_greg[16] = {
    REG_RAX, REG_RCX, REG_RDX, REG_RBX,
    REG_RSP, REG_RBP, REG_RSI, REG_RDI,
    REG_R8,  REG_R9,  REG_R10, REG_R11,
    REG_R12, REG_R13, REG_R14, REG_R15
};

#define MMIO_X64_RIP(uc) ((uc)->uc_mcontext.gregs[REG_RIP])

static inline uint64_t *mmio_x64_regp(ucontext_t *uc, int r)
{
    return (uint64_t *)&(uc)->uc_mcontext.gregs[mmio_x64_greg[r & 15]];
}

static inline uint64_t mmio_x64_get(ucontext_t *uc, int r, int size)
{
    uint64_t v = *mmio_x64_regp(uc, r);
    if (size == 1) return v & 0xFFULL;
    if (size == 2) return v & 0xFFFFULL;
    if (size == 4) return v & 0xFFFFFFFFULL;
    return v;
}

static inline void mmio_x64_set(ucontext_t *uc, int r, int size, uint64_t v)
{
    uint64_t *p = mmio_x64_regp(uc, r);
    if (size == 1)      *p = (*p & ~0xFFULL) | (v & 0xFFULL);
    else if (size == 2) *p = (*p & ~0xFFFFULL) | (v & 0xFFFFULL);
    else if (size == 4) *p = v & 0xFFFFFFFFULL;   /* 32-bit writes zero-extend */
    else                *p = v;
}

/* Zero-flag in the host EFLAGS, which the retry loop after a lock cmpxchg
 * reads with `jne`. Servicing the access without it steps over the
 * compare and the loop takes the wrong exit. */
static inline void mmio_x64_set_zf(ucontext_t *uc, int zf)
{
    greg_t *efl = &(uc)->uc_mcontext.gregs[REG_EFL];
    if (zf) *efl |= 0x40;      /* ZF */
    else    *efl &= ~0x40;
}

/* Length of a ModRM (plus SIB/displacement), and its decoded fields. */
static inline int mmio_x64_modrm(const uint8_t *ip, int rex_r, int rex_b,
                                 int *reg_out, int *rm_out)
{
    uint8_t m = ip[0];
    int mod = (m >> 6) & 3;
    int reg = ((m >> 3) & 7) | (rex_r ? 8 : 0);
    int rm  = (m & 7) | (rex_b ? 8 : 0);
    int len = 1;

    if (reg_out) *reg_out = reg;
    if (rm_out)  *rm_out  = rm;
    if (mod == 3) return len;

    if ((m & 7) == 4) len += 1;                       /* SIB */
    if (mod == 1)      len += 1;                      /* disp8 */
    else if (mod == 2) len += 4;                      /* disp32 */
    else if ((m & 7) == 5) len += 4;                  /* RIP-relative disp32 */
    return len;
}

/*
 * Service the trapped access at RIP against (rd, wr).
 * off is the device offset of the faulting data address (from si_addr).
 * Returns 1 when handled and RIP advanced, 0 when the opcode is not supported.
 */
static inline int mmio_emulate_x64(ucontext_t *uc, uint32_t off, void *dev,
                                   mmio_x64_read_fn rd, mmio_x64_write_fn wr)
{
    const uint8_t *ip = (const uint8_t *)(uintptr_t)MMIO_X64_RIP(uc);
    int i = 0, rex_w = 0, rex_r = 0, rex_b = 0, has66 = 0;
    int size = 4, mlen, reg, rm, ilen = 0;
    uint64_t v, imm;
    const uint8_t *op;

    if (!rd || !wr)
        return 0;

    /* Prefixes: operand/segment/repeat and REX. */
    for (;;) {
        uint8_t b = ip[i];
        if (b == 0x66) { has66 = 1; i++; continue; }
        if (b == 0x67 || b == 0xF0 || b == 0xF2 || b == 0xF3 ||
            b == 0x2E || b == 0x36 || b == 0x3E ||
            b == 0x26 || b == 0x64 || b == 0x65) { i++; continue; }
        if (b >= 0x40 && b <= 0x4F) {
            rex_w = (b >> 3) & 1; rex_r = (b >> 2) & 1; rex_b = b & 1;
            i++; continue;
        }
        break;
    }

    op   = ip + i;
    size = rex_w ? 8 : (has66 ? 2 : 4);

    if (op[0] == 0x0F) {
        if (op[1] == 0xB6 || op[1] == 0xB7) {          /* MOVZX r, r/m8|16 */
            int s = (op[1] == 0xB6) ? 1 : 2;
            uint64_t mask = (s == 1) ? 0xFFULL : 0xFFFFULL;
            mlen = mmio_x64_modrm(op + 2, rex_r, rex_b, &reg, &rm);
            v = (rm < 8) ? mmio_x64_get(uc, rm, s) : rd(dev, off, s);
            mmio_x64_set(uc, reg, size, v & mask);
            MMIO_X64_RIP(uc) += i + 2 + mlen;
            return 1;
        }
        if (op[1] == 0xAB) {                           /* BTS r/m, r (RMW) */
            int bit;
            mlen = mmio_x64_modrm(op + 2, rex_r, rex_b, &reg, &rm);
            bit  = (int)(mmio_x64_get(uc, reg, size) & (size * 8 - 1));
            v    = rd(dev, off, size) | (1ULL << bit);
            wr(dev, off, v, size);
            MMIO_X64_RIP(uc) += i + 2 + mlen;
            return 1;
        }
        if (op[1] == 0xB0 || op[1] == 0xB1) {           /* CMPXCHG r/m, r */
            int s = (op[1] == 0xB0) ? 1 : (rex_w ? 8 : (has66 ? 2 : 4));
            uint64_t mask = (s == 8) ? ~0ULL : ((1ULL << (s * 8)) - 1);
            mlen = mmio_x64_modrm(op + 2, rex_r, rex_b, &reg, &rm);
            v = rd(dev, off, s) & mask;                /* destination      */
            imm = mmio_x64_get(uc, 0, s);              /* accumulator (eax)*/
            if (v == (imm & mask)) {
                wr(dev, off, mmio_x64_get(uc, reg, s), s);
                mmio_x64_set_zf(uc, 1);
            } else {
                mmio_x64_set(uc, 0, s, v);             /* fail: eax = dest */
                mmio_x64_set_zf(uc, 0);
            }
            MMIO_X64_RIP(uc) += i + 2 + mlen;
            return 1;
        }
        if (op[1] == 0xC0 || op[1] == 0xC1) {           /* XADD r/m, r    */
            int s = (op[1] == 0xC0) ? 1 : (rex_w ? 8 : (has66 ? 2 : 4));
            mlen = mmio_x64_modrm(op + 2, rex_r, rex_b, &reg, &rm);
            v = rd(dev, off, s);                       /* old destination */
            wr(dev, off, v + mmio_x64_get(uc, reg, s), s);
            mmio_x64_set(uc, reg, s, v);               /* reg gets the old */
            MMIO_X64_RIP(uc) += i + 2 + mlen;
            return 1;
        }
        return 0;
    }

    /* ALU r/m, r -- 0x00..0x3D, ALU index in bits 5:3, byte form in bit 0.
     * A `lock or dword [reg], rx` against a device register lands here (gcc
     * emits it for a volatile read-modify-write). ADC/SBB are refused. */
    if ((op[0] & 0xC0) == 0) {
        int alu = (op[0] >> 3) & 7;
        if (alu == 2 || alu == 3)
            return 0;
        size = (op[0] & 1) ? (rex_w ? 8 : (has66 ? 2 : 4)) : 1;
        mlen = mmio_x64_modrm(op + 1, rex_r, rex_b, &reg, &rm);
        v = rd(dev, off, size);
        imm = mmio_x64_get(uc, reg, size);
        switch (alu) {
        case 0: v = v + imm; break;                    /* add */
        case 1: v = v | imm; break;                    /* or  */
        case 4: v = v & imm; break;                    /* and */
        case 5: v = v - imm; break;                    /* sub */
        case 6: v = v ^ imm; break;                    /* xor */
        case 7:                                        /* cmp: no writeback */
            MMIO_X64_RIP(uc) += i + 1 + mlen;
            return 1;
        }
        wr(dev, off, v, size);
        MMIO_X64_RIP(uc) += i + 1 + mlen;
        return 1;
    }

    switch (op[0]) {
    case 0x88:                                        /* MOV r/m8, r8 (write) */
    case 0x89: {                                      /* MOV r/m, r  (write) */
        if (op[0] == 0x88) size = 1;
        mlen = mmio_x64_modrm(op + 1, rex_r, rex_b, &reg, &rm);
        wr(dev, off, mmio_x64_get(uc, reg, size), size);
        MMIO_X64_RIP(uc) += i + 1 + mlen;
        return 1;
    }
    case 0x8A:                                        /* MOV r8, r/m8 (read) */
    case 0x8B: {                                      /* MOV r, r/m   (read) */
        if (op[0] == 0x8A) size = 1;
        mlen = mmio_x64_modrm(op + 1, rex_r, rex_b, &reg, &rm);
        mmio_x64_set(uc, reg, size, rd(dev, off, size));
        MMIO_X64_RIP(uc) += i + 1 + mlen;
        return 1;
    }
    case 0xC6:                                        /* MOV r/m8, imm8 */
    case 0xC7: {                                      /* MOV r/m, imm32 */
        if (op[0] == 0xC6) size = 1;
        mlen = mmio_x64_modrm(op + 1, rex_r, rex_b, &reg, &rm);
        ilen = (size == 8 || size == 4) ? 4 : size;
        imm  = 0;
        for (int k = ilen - 1; k >= 0; k--)
            imm = (imm << 8) | op[1 + mlen + k];
        wr(dev, off, imm, size);
        MMIO_X64_RIP(uc) += i + 1 + mlen + ilen;
        return 1;
    }

    case 0x84:                                        /* TEST r/m8, r8 */
    case 0x85: {                                      /* TEST r/m, r   */
        if (op[0] == 0x84) size = 1;
        mlen = mmio_x64_modrm(op + 1, rex_r, rex_b, &reg, &rm);
        (void)(rd(dev, off, size) & mmio_x64_get(uc, reg, size));
        MMIO_X64_RIP(uc) += i + 1 + mlen;             /* flags recomputed in C */
        return 1;
    }
    case 0x80:                                        /* group 1 r/m8, imm8 */
    case 0x81:                                        /* group 1 r/m, imm32 */
    case 0x83: {                                      /* group 1 r/m, imm8  */
        int alu = (op[1] >> 3) & 7;
        if (op[0] == 0x80) size = 1;
        mlen = mmio_x64_modrm(op + 1, rex_r, rex_b, &reg, &rm);
        if (op[0] == 0x83) {
            imm  = (uint64_t)(int64_t)(int8_t)op[1 + mlen];
            ilen = 1;
        } else {
            ilen = (size == 8 || size == 4) ? 4 : size;
            imm  = 0;
            for (int k = ilen - 1; k >= 0; k--)
                imm = (imm << 8) | op[1 + mlen + k];
        }
        v = rd(dev, off, size);
        switch (alu) {
        case 0: v = v + imm; break;                   /* add */
        case 1: v = v | imm; break;                   /* or  */
        case 4: v = v & imm; break;                   /* and */
        case 5: v = v - imm; break;                   /* sub */
        case 6: v = v ^ imm; break;                   /* xor */
        case 7:                                       /* cmp: no writeback */
            MMIO_X64_RIP(uc) += i + 1 + mlen + ilen;
            return 1;
        default: return 0;                            /* adc/sbb (2,3) */
        }
        wr(dev, off, v, size);
        MMIO_X64_RIP(uc) += i + 1 + mlen + ilen;
        return 1;
    }
    case 0xF6:                                        /* TEST r/m8, imm8 */
    case 0xF7: {                                      /* TEST r/m, imm32 */
        if (op[0] == 0xF6) size = 1;
        if (((op[1] >> 3) & 7) > 1) return 0;         /* /1 aliases /0 */
        mlen = mmio_x64_modrm(op + 1, rex_r, rex_b, &reg, &rm);
        ilen = (size == 8 || size == 4) ? 4 : size;
        (void)rd(dev, off, size);
        MMIO_X64_RIP(uc) += i + 1 + mlen + ilen;
        return 1;
    }
    default:
        return 0;
    }
}


#endif /* __x86_64__ && !_WIN32 */
#endif /* MMIO_TRAP_X64_H */
