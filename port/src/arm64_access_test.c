// wl4 --test-arm64: the AArch64 fault handler's emulated loads and stores
// (port/src/os_linux_arm64.c) against the same instructions run on ordinary memory.
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "port/arm64_access.h"
#include "port/os.h"

#if defined(__aarch64__)

#define TEST_BASE 0x0C000000u  // unused by the GBA map; the test's own trapped region
#define TEST_RESERVE 0x10000u
#define TEST_SIZE 0x1000u

static uint8_t *sHost;
static unsigned int sBeforeCalls, sAfterCalls, sLastWrite;
static uintptr_t sLastAddress;
static int sFailures;

static void Before(uintptr_t address, int isWrite, uintptr_t pc)
{
    (void)pc;
    sBeforeCalls++;
    sLastAddress = address;
    sLastWrite = isWrite;
}

static void After(uintptr_t address, int isWrite, uintptr_t pc)
{
    (void)address; (void)isWrite; (void)pc;
    sAfterCalls++;
}

static void Check(int ok, const char *what)
{
    if (!ok) {
        fprintf(stderr, "arm64 access test FAILED: %s\n", what);
        sFailures++;
    }
}

// Runs one instruction form on ordinary memory and on the trapped region, and compares the
// registers it produced and the memory it left.
#define FORM(name, asmtext)                                                                  \
    static void name(uint8_t *mem, uint64_t *out)                                            \
    {                                                                                        \
        register uint64_t x0 __asm__("x0") = (uint64_t)(uintptr_t)mem;                       \
        register uint64_t x1 __asm__("x1") = 0x1122334455667788ull;                          \
        register uint64_t x2 __asm__("x2") = 0x99AABBCCDDEEFF00ull;                          \
        register uint64_t x3 __asm__("x3") = 8;                                              \
        register uint64_t x4 __asm__("x4") = 0;                                              \
        register uint64_t x5 __asm__("x5") = 0;                                              \
        __asm__ volatile(asmtext                                                             \
                         : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3), "+r"(x4), "+r"(x5)        \
                         :                                                                   \
                         : "memory", "v0", "v1");                                            \
        out[0] = x0; out[1] = x1; out[2] = x2; out[3] = x3; out[4] = x4; out[5] = x5;        \
    }

FORM(StrbImm, "strb w1, [x0, #3]")
FORM(StrhImm, "strh w1, [x0, #6]")
FORM(StrWImm, "str w1, [x0, #8]")
FORM(StrXImm, "str x1, [x0, #16]")
FORM(LdrbImm, "ldrb w4, [x0, #1]")
FORM(LdrhImm, "ldrh w4, [x0, #2]")
FORM(LdrWImm, "ldr w4, [x0, #4]")
FORM(LdrXImm, "ldr x4, [x0, #8]")
FORM(LdrsbW, "ldrsb w4, [x0, #7]")
FORM(LdrsbX, "ldrsb x4, [x0, #7]")
FORM(LdrshW, "ldrsh w4, [x0, #6]")
FORM(LdrshX, "ldrsh x4, [x0, #6]")
FORM(Ldrsw, "ldrsw x4, [x0, #4]")
FORM(Stur, "stur w1, [x0, #-0]\n sturh w2, [x0, #5]")
FORM(Ldur, "ldur x4, [x0, #3]")
FORM(PostIndex, "str w1, [x0], #4\n ldrh w4, [x0], #-2")
FORM(PreIndex, "strh w2, [x0, #10]!\n ldrb w4, [x0, #-1]!")
FORM(RegOffset, "strh w1, [x0, x3]\n ldr w4, [x0, x3]")
FORM(RegOffsetShift, "str w2, [x0, x3, lsl #2]\n ldrh w4, [x0, x3, lsl #1]")
FORM(RegOffsetUxtw, "mov w5, #12\n strb w1, [x0, w5, uxtw]\n ldrsh x4, [x0, w5, sxtw]")
FORM(Pair32, "stp w1, w2, [x0, #8]\n ldp w4, w5, [x0, #4]")
FORM(Pair64, "stp x1, x2, [x0, #16]!\n ldp x4, x5, [x0], #-16")
FORM(PairSigned, "stp w2, w1, [x0]\n ldpsw x4, x5, [x0]")
FORM(Zero, "str xzr, [x0, #8]\n strh wzr, [x0, #2]")
FORM(Acquire, "stlrh w1, [x0]\n ldarb w4, [x0]\n add x5, x0, #8\n stlr x2, [x5]\n ldar w5, [x5]")
FORM(Simd, "movi v0.16b, #0\n movi v1.16b, #0\n ldr q0, [x0]\n str q0, [x0, #16]\n ldr d1, [x0, #4]\n str s1, [x0, #32]\n"
           "stp q0, q0, [x0, #48]\n ldr h1, [x0, #2]\n fmov w4, s1\n mov x5, v0.d[1]")

static const struct {
    const char *name;
    void (*run)(uint8_t *, uint64_t *);
} kForms[] = {
#define F(n) { #n, n }
    F(StrbImm), F(StrhImm), F(StrWImm), F(StrXImm), F(LdrbImm), F(LdrhImm), F(LdrWImm),
    F(LdrXImm), F(LdrsbW), F(LdrsbX), F(LdrshW), F(LdrshX), F(Ldrsw), F(Stur), F(Ldur),
    F(PostIndex), F(PreIndex), F(RegOffset), F(RegOffsetShift), F(RegOffsetUxtw), F(Pair32),
    F(Pair64), F(PairSigned), F(Zero), F(Acquire), F(Simd),
#undef F
};

static void Fill(uint8_t *p)
{
    unsigned int i;
    for (i = 0; i < 128; i++)
        p[i] = (uint8_t)(i * 37 + 0x81);
}

static void TestForms(void)
{
    static uint8_t plain[256] __attribute__((aligned(16)));
    unsigned int i, k;

    for (i = 0; i < sizeof(kForms) / sizeof(kForms[0]); i++) {
        uint64_t expected[6], got[6];
        uintptr_t offset = 64;
        uint8_t *trapped = (uint8_t *)(uintptr_t)(TEST_BASE + offset);
        char what[96];

        Fill(plain + offset);
        Fill(sHost + offset);
        kForms[i].run(plain + offset, expected);
        sBeforeCalls = sAfterCalls = 0;
        kForms[i].run(trapped, got);
        for (k = 0; k < 6; k++) {
            uint64_t e = expected[k], g = got[k];
            if (k == 0) {  // the base register: compare as offsets
                e -= (uintptr_t)plain;
                g -= TEST_BASE;
            }
            snprintf(what, sizeof(what), "%s: x%u = %llx, expected %llx", kForms[i].name, k,
                     (unsigned long long)g, (unsigned long long)e);
            Check(e == g, what);
        }
        snprintf(what, sizeof(what), "%s: memory", kForms[i].name);
        Check(memcmp(plain + offset, sHost + offset, 128) == 0, what);
        if (memcmp(plain + offset, sHost + offset, 128) != 0)
            for (k = 0; k < 128; k++)
                if (plain[offset + k] != sHost[offset + k])
                    fprintf(stderr, "  byte %u: %02x, expected %02x\n", k, sHost[offset + k], plain[offset + k]);
        snprintf(what, sizeof(what), "%s: callbacks", kForms[i].name);
        Check(sBeforeCalls != 0 && sBeforeCalls == sAfterCalls, what);
    }
}

static void TestSizes(void)
{
    // strh w1, [x0, #6]; stp x1, x2, [x0]; ldr q0, [x0]; ldarb w4, [x0]; add x0, x0, #1
    Check(PortArm64AccessSize(0x79000c01u) == 2, "size of strh");
    Check(PortArm64AccessSize(0xa9000801u) == 16, "size of stp x");
    Check(PortArm64AccessSize(0x3dc00000u) == 16, "size of ldr q");
    Check(PortArm64AccessSize(0x08dffc04u) == 1, "size of ldarb");
    Check(PortArm64AccessSize(0x91000400u) == 0, "add is no access");
}

int PortArm64AccessTest(void)
{
    if (PortOsMapTrappedRegion(TEST_BASE, TEST_RESERVE, TEST_SIZE, &sHost, Before, After)) {
        fprintf(stderr, "arm64 access test: cannot map the test region\n");
        return 1;
    }
    TestSizes();
    TestForms();
    fprintf(stderr, "arm64 access test: %s\n", sFailures ? "FAILED" : "passed");
    return sFailures != 0;
}

#else

int PortArm64AccessTest(void)
{
    fprintf(stderr, "arm64 access test: not an AArch64 build\n");
    return 1;
}

#endif
