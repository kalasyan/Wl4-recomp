#include "port/arm64_access.h"

#include <string.h>

#define BITS(v, hi, lo) (((v) >> (lo)) & ((1u << ((hi) - (lo) + 1)) - 1))

static int64_t SignExtend(uint64_t v, unsigned int bits)
{
    uint64_t m = 1ull << (bits - 1);
    v &= (m << 1) - 1;
    return (int64_t)((v ^ m) - m);
}

// Rn: 31 is the stack pointer.
static uint64_t Base(const uint64_t *x, uint64_t sp, unsigned int n)
{
    return n == 31 ? sp : x[n];
}

// Rm in a register offset: 31 is the zero register.
static uint64_t Index(const uint64_t *x, unsigned int m)
{
    return m == 31 ? 0 : x[m];
}

// Sets size, load and sign/width for a single-register load/store from size:V:opc.
// Returns 0 for prefetches and unallocated encodings.
static int SingleKind(unsigned int size, unsigned int v, unsigned int opc, struct PortArm64Access *a)
{
    if (v) {
        a->simd = 1;
        if (size == 0 && (opc & 2)) {
            a->size = 16;
            a->isLoad = opc & 1;
        } else if (opc & 2) {
            return 0;
        } else {
            a->size = 1u << size;
            a->isLoad = opc & 1;
        }
        return 1;
    }
    a->size = 1u << size;
    switch (opc) {
    case 0:  // STR
        a->isLoad = 0;
        break;
    case 1:  // LDR, zero-extending
        a->isLoad = 1;
        a->regBits = size == 3 ? 64 : 32;
        break;
    case 2:  // LDRS* to 64 bits; size 3 is PRFM, size 2 is LDRSW
        if (size == 3)
            return 0;
        a->isLoad = 1;
        a->signExtend = 1;
        a->regBits = 64;
        break;
    default:  // LDRS* to 32 bits
        if (size >= 2)
            return 0;
        a->isLoad = 1;
        a->signExtend = 1;
        a->regBits = 32;
        break;
    }
    return 1;
}

int PortArm64Decode(uint32_t insn, const uint64_t *x, uint64_t sp, struct PortArm64Access *a)
{
    memset(a, 0, sizeof(*a));
    a->count = 1;
    a->rt = BITS(insn, 4, 0);
    a->rn = BITS(insn, 9, 5);

    // LDAPR (RCpc load-acquire): size 111000 101 11111 110000 Rn Rt.
    if ((insn & 0x3FFFFC00u) == 0x38BFC000u) {
        unsigned int size = BITS(insn, 31, 30);
        a->size = 1u << size;
        a->isLoad = 1;
        a->regBits = size == 3 ? 64 : 32;
        a->address = Base(x, sp, a->rn);
        return 1;
    }

    // Load/store register, unsigned immediate offset: size 111 V 01 opc imm12 Rn Rt.
    if ((insn & 0x3B000000u) == 0x39000000u) {
        unsigned int size = BITS(insn, 31, 30), v = BITS(insn, 26, 26), opc = BITS(insn, 23, 22);
        if (!SingleKind(size, v, opc, a))
            return 0;
        a->address = Base(x, sp, a->rn) + ((uint64_t)BITS(insn, 21, 10) * a->size);
        return 1;
    }

    // Load/store register, unscaled/post/pre/unprivileged or register offset:
    // size 111 V 00 opc ...
    if ((insn & 0x3B000000u) == 0x38000000u) {
        unsigned int size = BITS(insn, 31, 30), v = BITS(insn, 26, 26), opc = BITS(insn, 23, 22);
        uint64_t base;
        if (!SingleKind(size, v, opc, a))
            return 0;
        base = Base(x, sp, a->rn);
        if (insn & (1u << 21)) {
            // Register offset: ... 1 Rm option S 10 Rn Rt.
            unsigned int option = BITS(insn, 15, 13), shift = 0, m = BITS(insn, 20, 16);
            uint64_t offset = Index(x, m);
            if (BITS(insn, 11, 10) != 2)
                return 0;  // atomics and others
            if (insn & (1u << 12)) {
                unsigned int s = a->size;
                while (s > 1) {
                    shift++;
                    s >>= 1;
                }
            }
            switch (option) {
            case 2:  // UXTW
                offset = (uint32_t)offset;
                break;
            case 3:  // LSL (UXTX)
                break;
            case 6:  // SXTW
                offset = (uint64_t)(int64_t)(int32_t)offset;
                break;
            case 7:  // SXTX
                break;
            default:
                return 0;
            }
            a->address = base + (offset << shift);
            return 1;
        } else {
            int64_t imm = SignExtend(BITS(insn, 20, 12), 9);
            switch (BITS(insn, 11, 10)) {
            case 0:  // unscaled (LDUR/STUR)
            case 2:  // unprivileged (LDTR/STTR)
                a->address = base + imm;
                break;
            case 1:  // post-index
                a->address = base;
                a->writeback = 1;
                a->newBase = base + imm;
                break;
            default:  // pre-index
                a->address = base + imm;
                a->writeback = 1;
                a->newBase = a->address;
                break;
            }
            return 1;
        }
    }

    // Load/store pair: opc 101 V 0 mode L imm7 Rt2 Rn Rt.
    if ((insn & 0x3A000000u) == 0x28000000u) {
        unsigned int opc = BITS(insn, 31, 30), v = BITS(insn, 26, 26), mode = BITS(insn, 24, 23);
        uint64_t base = Base(x, sp, a->rn);
        int64_t offset;
        a->isLoad = BITS(insn, 22, 22);
        a->rt2 = BITS(insn, 14, 10);
        a->count = 2;
        if (v) {
            if (opc == 3)
                return 0;
            a->simd = 1;
            a->size = 4u << opc;
        } else if (opc == 0) {
            a->size = 4;
            a->regBits = 32;
        } else if (opc == 1) {
            if (!a->isLoad || mode == 0)
                return 0;  // only LDPSW
            a->size = 4;
            a->regBits = 64;
            a->signExtend = 1;
        } else if (opc == 2) {
            a->size = 8;
            a->regBits = 64;
        } else {
            return 0;
        }
        offset = SignExtend(BITS(insn, 21, 15), 7) * (int64_t)a->size;
        switch (mode) {
        case 0:  // no-allocate, signed offset
        case 2:  // signed offset
            a->address = base + offset;
            break;
        case 1:  // post-index
            a->address = base;
            a->writeback = 1;
            a->newBase = base + offset;
            break;
        default:  // pre-index
            a->address = base + offset;
            a->writeback = 1;
            a->newBase = a->address;
            break;
        }
        return 1;
    }

    // Load-acquire/store-release register (LDAR/STLR and byte/halfword forms):
    // size 001000 1 L 0 11111 1 11111 Rn Rt.
    if ((insn & 0x3FA07C00u) == 0x08807C00u) {
        unsigned int size = BITS(insn, 31, 30);
        a->size = 1u << size;
        a->isLoad = BITS(insn, 22, 22);
        a->regBits = size == 3 ? 64 : 32;
        a->address = Base(x, sp, a->rn);
        return 1;
    }

    return 0;
}

unsigned int PortArm64AccessSize(uint32_t insn)
{
    static const uint64_t zero[31];
    struct PortArm64Access a;

    if (!PortArm64Decode(insn, zero, 0, &a))
        return 0;
    return (unsigned int)a.size * a.count;
}
