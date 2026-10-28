// Linux and Android AArch64 backend. All GBA execution and trapped accesses stay on one thread.
//
// AArch64 user code cannot be single-stepped, so unlike the x86-64 backends, a trapped or
// redirected access is not run again with the page open: the fault handler decodes the load
// or store (port/src/arm64_access.c), performs it on the host's view of the memory, updates
// the registers and moves the pc past it. The trap callbacks see the same before/after order.
#define _GNU_SOURCE
#include "port/os.h"
#include "port/arm64_access.h"
#include "port/gba_memory.h"
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <ucontext.h>
#include <unistd.h>

#if !defined(__linux__) || !defined(__aarch64__)
#error This backend requires Linux or Android on AArch64
#endif

#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0x100000
#endif

int PortOsReserveFixed(uintptr_t address, size_t size)
{
    void *p = mmap((void *)address, size, PROT_NONE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    if (p == (void *)address) return 0;
    if (p != MAP_FAILED) munmap(p, size);
    return -1;
}

int PortOsProtect(uintptr_t address, size_t size, enum PortProtection protection)
{
    static const int flags[] = {PROT_NONE, PROT_READ, PROT_READ | PROT_WRITE, PROT_NONE};
    return mprotect((void *)address, size, flags[protection]);
}

void PortOsDescribeAddress(uintptr_t address, char *buffer, size_t size)
{
    int reason = errno;
    (void)address;
    snprintf(buffer, size, "fixed mmap failed: %s", strerror(reason));
}
void PortOsShowErrors(int show) { (void)show; }

static void (*sErrorSink)(const char *message);
void PortOsSetErrorSink(void (*sink)(const char *message)) { sErrorSink = sink; }

void PortOsWriteError(const char *message)
{
    size_t remaining = strlen(message);
    if (sErrorSink) sErrorSink(message);
    while (remaining) {
        ssize_t n = write(STDERR_FILENO, message, remaining);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        message += n; remaining -= (size_t)n;
    }
}

uintptr_t PortOsExecutableOffset(uintptr_t pc)
{
    extern char __executable_start __attribute__((weak)), _end __attribute__((weak));
    if (&__executable_start == NULL) return 0;
    return pc >= (uintptr_t)&__executable_start && pc < (uintptr_t)&_end ? pc : 0;
}

// The SIMD&FP registers in the signal frame (the kernel's struct fpsimd_context).
#define FPSIMD_CONTEXT_MAGIC 0x46508001u
struct FpsimdContext {
    uint32_t magic, size;
    uint32_t fpsr, fpcr;
    __uint128_t vregs[32];
};

static uintptr_t sTrapBase;
static size_t sTrapSize;
static uint8_t *sTrapHost;
static PortTrapCallback sBefore, sAfter;
static PortRedirectCallback sRedirect;
static PortFaultCallback sFault;
static struct sigaction sPreviousSegv, sPreviousBus;
static int sInstalled;
static _Thread_local int sInTrap;

// With SVE state live, the kernel restores the vector registers from the SVE record
// instead (each V register is the low 128 bits of a Z register, vl bytes each).
#define SVE_CONTEXT_MAGIC 0x53564501u
struct SveContext {
    uint32_t magic, size;
    uint16_t vl, flags, reserved[2];
};

__attribute__((no_sanitize("address", "undefined")))
static void *FindRecord(ucontext_t *uc, uint32_t wanted)
{
    uint8_t *p = (uint8_t *)uc->uc_mcontext.__reserved;
    uint8_t *end = p + sizeof(uc->uc_mcontext.__reserved);
    while (p + 8 <= end) {
        uint32_t magic = ((uint32_t *)p)[0], size = ((uint32_t *)p)[1];
        if (magic == wanted) return p;
        if (magic == 0 || size == 0) break;
        p += size;
    }
    return NULL;
}

// The bytes of V register r: in the SVE record when it holds the registers, else in the
// FP/SIMD record. The SVE register's bytes past 16 are returned in *rest (or NULL).
__attribute__((no_sanitize("address", "undefined")))
static uint8_t *VectorRegister(ucontext_t *uc, unsigned int r, uint8_t **rest, unsigned int *restSize)
{
    struct SveContext *sve = FindRecord(uc, SVE_CONTEXT_MAGIC);
    struct FpsimdContext *fp = FindRecord(uc, FPSIMD_CONTEXT_MAGIC);
    *rest = NULL;
    *restSize = 0;
    if (sve != NULL && sve->size > sizeof(*sve) && sve->vl >= 16) {
        uint8_t *z = (uint8_t *)sve + 16 + (size_t)r * sve->vl;
        *rest = z + 16;
        *restSize = sve->vl - 16u;
        return z;
    }
    return fp != NULL ? (uint8_t *)&fp->vregs[r] : NULL;
}

__attribute__((no_sanitize("address", "undefined")))
static void CopyBytes(volatile uint8_t *dst, const volatile uint8_t *src, unsigned int n)
{
    while (n--) *dst++ = *src++;
}

// Performs the decoded access on host memory. Returns 0 if it cannot.
__attribute__((no_sanitize("address", "undefined")))
static int Perform(ucontext_t *uc, const struct PortArm64Access *a, uint8_t *host)
{
    uint64_t *x = (uint64_t *)uc->uc_mcontext.regs;
    unsigned int i;

    for (i = 0; i < a->count; i++) {
        unsigned int r = i ? a->rt2 : a->rt;
        uint8_t *p = host + (size_t)i * a->size;
        if (a->simd) {
            uint8_t *rest, *v;
            unsigned int restSize;
            if ((v = VectorRegister(uc, r, &rest, &restSize)) == NULL) return 0;
            if (a->isLoad) {
                uint8_t bytes[16] = {0};
                CopyBytes(bytes, p, a->size);
                CopyBytes(v, bytes, 16);
                while (restSize--) *rest++ = 0;  // a SIMD&FP write clears the rest of Z
            } else {
                CopyBytes(p, v, a->size);
            }
        } else if (a->isLoad) {
            uint64_t value = 0;
            CopyBytes((uint8_t *)&value, p, a->size);
            if (a->signExtend && a->size < 8) {
                uint64_t m = 1ull << (a->size * 8 - 1);
                value = (value ^ m) - m;
            }
            if (a->regBits == 32) value = (uint32_t)value;
            if (r != 31) x[r] = value;
        } else {
            uint64_t value = r == 31 ? 0 : x[r];
            CopyBytes(p, (uint8_t *)&value, a->size);
        }
    }
    return 1;
}

__attribute__((no_sanitize("address", "undefined")))
static int Emulate(ucontext_t *uc)
{
    mcontext_t *m = &uc->uc_mcontext;
    struct PortArm64Access a;
    uintptr_t pc = m->pc, host = 0;
    int trapped;

    if (!PortArm64Decode(*(const uint32_t *)pc, (const uint64_t *)m->regs, m->sp, &a)) return 0;
    trapped = sTrapSize && a.address - sTrapBase < sTrapSize;
    if (trapped) {
        if (sInTrap) return 0;
        host = (uintptr_t)sTrapHost + (a.address - sTrapBase);
    } else if (a.address <= UINT32_MAX && sRedirect) {
        host = sRedirect(a.address);
    }
    if (!host) return 0;
    if (trapped) {
        sInTrap = 1;
        sBefore(a.address, !a.isLoad, pc);
    }
    if (!Perform(uc, &a, (uint8_t *)host)) {
        sInTrap = 0;
        return 0;
    }
    if (trapped) {
        sAfter(a.address, !a.isLoad, pc);
        sInTrap = 0;
    }
    if (a.writeback) {
        if (a.rn == 31) m->sp = a.newBase;
        else m->regs[a.rn] = a.newBase;
    }
    m->pc = pc + 4;
    return 1;
}

__attribute__((no_sanitize("address", "undefined")))
static void SignalHandler(int sig, siginfo_t *info, void *context)
{
    int savedErrno = errno;
    ucontext_t *uc = context;
    uintptr_t address = (uintptr_t)info->si_addr, pc = uc->uc_mcontext.pc;

    if (sig == SIGSEGV && Emulate(uc)) {
        errno = savedErrno;
        return;
    }
    if (sig == SIGSEGV && sFault) {
        // ESR's WnR bit is in the esr_context record; reads and writes are not told apart here.
        enum PortFaultKind kind = address == pc ? PORT_FAULT_EXECUTE : PORT_FAULT_READ;
        if (address >= PORT_GAME_STACK_RESERVE_BASE && address < PORT_GAME_STACK_LIMIT + 4096)
            kind = PORT_FAULT_STACK_OVERFLOW;
        sFault(kind, address, pc, uc->uc_mcontext.sp);
    }
    struct sigaction *previous = sig == SIGSEGV ? &sPreviousSegv : &sPreviousBus;
    if ((previous->sa_flags & SA_SIGINFO) && previous->sa_sigaction) {
        previous->sa_sigaction(sig, info, context);
    } else if (previous->sa_handler != SIG_DFL && previous->sa_handler != SIG_IGN) {
        previous->sa_handler(sig);
    } else {
        char report[192];
        snprintf(report, sizeof(report),
                 "Fatal signal %d (code %d): address=%p pc=%p sp=%p insn=%08x\n",
                 sig, info->si_code, info->si_addr, (void *)pc, (void *)uc->uc_mcontext.sp,
                 *(const uint32_t *)pc);
        PortOsWriteError(report);
        struct sigaction action = {.sa_handler = SIG_DFL};
        sigemptyset(&action.sa_mask);
        sigaction(sig, &action, NULL);
        raise(sig);
    }
    errno = savedErrno;
}

static void InstallHandlers(void)
{
    if (sInstalled) return;
    stack_t stack;
    if (sigaltstack(NULL, &stack)) { perror("sigaltstack"); exit(1); }
    if (stack.ss_flags & SS_DISABLE) {
        stack.ss_size = 128 * 1024;
        stack.ss_sp = mmap(NULL, stack.ss_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        stack.ss_flags = 0;
        if (stack.ss_sp == MAP_FAILED || sigaltstack(&stack, NULL)) { perror("signal stack"); exit(1); }
    }
    struct sigaction action = {.sa_sigaction = SignalHandler, .sa_flags = SA_SIGINFO | SA_ONSTACK};
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGSEGV, &action, &sPreviousSegv) || sigaction(SIGBUS, &action, &sPreviousBus)) {
        perror("sigaction"); exit(1);
    }
    sInstalled = 1;
}
void PortOsInstallFaultHandler(PortFaultCallback callback) { sFault = callback; InstallHandlers(); }
void PortOsRedirectFaults(PortRedirectCallback redirect) { sRedirect = redirect; InstallHandlers(); }

int PortOsMapTrappedRegion(uintptr_t address, size_t reserve, size_t size, uint8_t **hostView,
                           PortTrapCallback before, PortTrapCallback after)
{
    // The game view is never opened, so it only needs to fault: a plain inaccessible
    // reservation. The host view is ordinary memory.
    void *host;
    if (PortOsReserveFixed(address, reserve)) return -1;
    host = mmap(NULL, reserve, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (host == MAP_FAILED) { munmap((void *)address, reserve); return -1; }
    sTrapBase = address; sTrapSize = size; sTrapHost = host; sBefore = before; sAfter = after;
    *hostView = host;
    InstallHandlers();
    return 0;
}

unsigned int PortOsTrapAccessSize(uintptr_t pc)
{
    return PortArm64AccessSize(*(const uint32_t *)pc);
}
