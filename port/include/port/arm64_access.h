#ifndef GUARD_PORT_ARM64_ACCESS_H
#define GUARD_PORT_ARM64_ACCESS_H

#include <stdint.h>

// AArch64 has no single-step for user code, so an access to memory the port traps or
// redirects is not run again: the fault handler decodes the instruction and performs it
// itself (port/src/os_linux_arm64.c). These are the loads and stores compilers emit for
// plain C: LDR/STR and their byte, halfword and sign-extending forms (immediate, unscaled,
// pre/post-indexed and register offsets), LDP/STP (including LDPSW), the same for SIMD&FP
// registers, and LDAR/STLR (volatile accesses with -fms-volatile).

struct PortArm64Access {
    uint8_t isLoad;
    uint8_t simd;         // Rt (and Rt2) are V registers
    uint8_t signExtend;   // loads: sign-extend each element
    uint8_t regBits;      // loads into general registers: 32 or 64
    uint8_t size;         // bytes per element: 1, 2, 4, 8 or 16
    uint8_t count;        // elements: 1, or 2 for a pair
    uint8_t rt, rt2;
    uint8_t writeback;    // Rn is set to newBase afterwards
    uint8_t rn;           // 31 is SP
    uint64_t address;     // the first byte accessed
    uint64_t newBase;
};

// Decodes insn with the general registers x[0..30] and the stack pointer sp. Returns 0 for an
// instruction it does not handle.
int PortArm64Decode(uint32_t insn, const uint64_t *x, uint64_t sp, struct PortArm64Access *out);

// The total number of bytes the load or store at code accesses, or 0 if it is not one.
unsigned int PortArm64AccessSize(uint32_t insn);

#endif  // GUARD_PORT_ARM64_ACCESS_H
