// The port's core for the SDL frontend: loads the ROM and the save, runs the game one frame
// at a time on the runtime in port/src, hands pictures, sound and rumble to the host, and
// takes and restores save states.
#include "port.h"
#include "port_assets.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

#ifdef __ANDROID__
#include <android/log.h>
#endif

#include "port/audio.h"
#include "port/bios.h"
#include "port/frame.h"
#include "port/gba_functions.h"
#include "port/gba_io.h"
#include "port/gba_memory.h"
#include "port/md5.h"
#include "port/os.h"
#include "port/ppu.h"
#include "port/rom.h"

#define REG_KEYINPUT 0x130
#define SRAM_SIZE 0x8000u  // the 32 KiB the cartridge has
// SRAM is written to the save file once it has stopped changing for this many frames: the
// game writes a save over several frames.
#define SAVE_QUIET_FRAMES 30

static bool sQuit;
static uint64_t sFrames;
static uint8_t sSavedSram[SRAM_SIZE];  // what the save file holds
static unsigned int sSramQuiet;        // frames since SRAM last changed, while it differs
static uint8_t sLastSram[SRAM_SIZE];
static uint32_t sPicture[GBA_SCREEN_WIDTH * GBA_SCREEN_HEIGHT];

void Port_RequestQuit(void)
{
    sQuit = true;
}

bool Port_QuitRequested(void)
{
    return sQuit;
}

uint64_t Port_FrameCount(void)
{
    return sFrames;
}

void Port_WideSetWidth(int width)
{
    (void)width;
}

int Port_WideWidth(void)
{
    return GBA_SCREEN_WIDTH;
}

void Port_Log(const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
#ifdef __ANDROID__
    __android_log_vprint(ANDROID_LOG_INFO, "wl4", fmt, ap);
#else
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
#endif
    va_end(ap);
}

#ifdef __ANDROID__
static void LogError(const char* message)
{
    __android_log_write(ANDROID_LOG_ERROR, "wl4", message);
}
#endif

// ---- ROM ----------------------------------------------------------------------------

PortRomStatus Port_CheckRom(const uint8_t* rom, size_t size)
{
    uint8_t digest[16];
    char hex[33];

    if (size != GBA_ROM_SIZE)
        return PORT_ROM_WRONG_SIZE;
    PortMd5(rom, size, digest);
    for (int i = 0; i < 16; i++)
        snprintf(hex + i * 2, 3, "%02x", digest[i]);
    return strcmp(hex, PORT_ROM_MD5) == 0 ? PORT_ROM_OK : PORT_ROM_WRONG_GAME;
}

const char* Port_RomStatusText(PortRomStatus status)
{
    switch (status) {
    case PORT_ROM_OK:
        return "OK";
    case PORT_ROM_WRONG_SIZE:
        return "Not the Wario Land 4 ROM (it must be 8 MiB).";
    default:
        return "Not an unmodified Wario Land 4 (USA, Europe) ROM.";
    }
}

const char* Port_RomRegionName(void)
{
    return "USA, Europe";
}

const char* Port_RomExpectedMd5(void)
{
    return PORT_ROM_MD5;
}

// ---- save -------------------------------------------------------------------------------

static uint8_t* Sram(void)
{
    return (uint8_t*)(uintptr_t)GBA_SRAM_BASE;
}

// Writes SRAM to the save file once the game has finished changing it.
static void FlushSave(bool now)
{
    uint8_t* sram = Sram();

    if (memcmp(sram, sSavedSram, SRAM_SIZE) == 0) {
        sSramQuiet = 0;
        return;
    }
    if (memcmp(sram, sLastSram, SRAM_SIZE) != 0) {
        memcpy(sLastSram, sram, SRAM_SIZE);
        sSramQuiet = 0;
    } else {
        sSramQuiet++;
    }
    if (now || sSramQuiet >= SAVE_QUIET_FRAMES) {
        Port_HostWriteSave(sram, SRAM_SIZE);
        memcpy(sSavedSram, sram, SRAM_SIZE);
        sSramQuiet = 0;
    }
}

// The picture for the host, opaque.
static void CopyPicture(void)
{
    const uint32_t* frame = PortPpuFrame();
    for (int i = 0; i < GBA_SCREEN_WIDTH * GBA_SCREEN_HEIGHT; i++)
        sPicture[i] = frame[i] | 0xFF000000u;
}

// ---- save states -----------------------------------------------------------------------
// A state is GBA memory, the I/O registers as the host sees them, the runtime's own
// variables (the wl4_state section, port/include/port/state.h) and the functions the game
// copied to RAM. It is taken between two frames while the game waits for VBlank at the end
// of AgbMain's loop, so the game's native stack is the same when it is restored.

extern unsigned char __start_wl4_state[];
extern unsigned char __stop_wl4_state[];

#define STATE_MAGIC 0x54534C57u  // "WLST"
#define STATE_VERSION 1u

typedef struct StateHeader {
    uint32_t magic;
    uint32_t version;
    uint32_t sectionSize;
    uint32_t ramFunctionCount;
    uint32_t ramFunctions[PORT_MAX_RAM_FUNCTIONS * 2];
    uint64_t frames;
} StateHeader;

static const struct {
    uintptr_t base;  // 0: the I/O registers' host view
    size_t size;
} kStateRegions[] = {
    { GBA_EWRAM_BASE, GBA_EWRAM_SIZE },
    { GBA_IWRAM_BASE, GBA_IWRAM_SIZE },
    { 0, PORT_IO_SIZE },
    { GBA_PLTT_BASE, GBA_PLTT_SIZE },
    { GBA_VRAM_BASE, GBA_VRAM_SIZE },
    { GBA_OAM_BASE, GBA_OAM_SIZE },
    { GBA_SRAM_BASE, SRAM_SIZE },
};

static size_t SectionSize(void)
{
    return (size_t)(__stop_wl4_state - __start_wl4_state);
}

size_t Port_StateSize(void)
{
    size_t size = sizeof(StateHeader) + SectionSize();
    for (size_t i = 0; i < sizeof(kStateRegions) / sizeof(kStateRegions[0]); i++)
        size += kStateRegions[i].size;
    return size;
}

static uint8_t* RegionPointer(size_t i)
{
    return kStateRegions[i].base ? (uint8_t*)kStateRegions[i].base : PortIoHost();
}

// Copies byte by byte: the section holds many variables, which AddressSanitizer would take
// for overflows of the first one.
__attribute__((no_sanitize("address"))) static void RawCopy(void* dst, const void* src, size_t size)
{
    uint8_t* d = dst;
    const uint8_t* s = src;
    while (size--)
        *d++ = *s++;
}

void Port_StateSave(uint8_t* buf)
{
    StateHeader header;
    uint8_t* p = buf;

    memset(&header, 0, sizeof(header));
    header.magic = STATE_MAGIC;
    header.version = STATE_VERSION;
    header.sectionSize = (uint32_t)SectionSize();
    header.ramFunctionCount = PortRamFunctionsSave(header.ramFunctions);
    header.frames = sFrames;
    memcpy(p, &header, sizeof(header));
    p += sizeof(header);
    for (size_t i = 0; i < sizeof(kStateRegions) / sizeof(kStateRegions[0]); i++) {
        memcpy(p, RegionPointer(i), kStateRegions[i].size);
        p += kStateRegions[i].size;
    }
    RawCopy(p, __start_wl4_state, SectionSize());
}

PortStateResult Port_StateLoad(const uint8_t* buf, size_t size)
{
    StateHeader header;
    const uint8_t* p = buf;

    if (size < sizeof(header))
        return PORT_STATE_BAD_FILE;
    memcpy(&header, buf, sizeof(header));
    if (header.magic != STATE_MAGIC)
        return PORT_STATE_BAD_FILE;
    if (header.version != STATE_VERSION || header.sectionSize != SectionSize())
        return PORT_STATE_OTHER_BUILD;
    if (size < Port_StateSize())
        return PORT_STATE_BAD_FILE;
    if (PortRamFunctionsLoad(header.ramFunctions, header.ramFunctionCount))
        return PORT_STATE_OTHER_BUILD;
    p += sizeof(header);
    for (size_t i = 0; i < sizeof(kStateRegions) / sizeof(kStateRegions[0]); i++) {
        memcpy(RegionPointer(i), p, kStateRegions[i].size);
        p += kStateRegions[i].size;
    }
    RawCopy(__start_wl4_state, p, SectionSize());
    sFrames = header.frames;
    CopyPicture();
    Port_RumbleReset();
    return PORT_STATE_OK;
}

const char* Port_StateResultText(PortStateResult result)
{
    switch (result) {
    case PORT_STATE_OK:
        return "OK";
    case PORT_STATE_BAD_FILE:
        return "not a save state";
    default:
        return "made by another version of the port";
    }
}

// Save states are only taken while the game waits for VBlank in AgbMain's loop. Elsewhere
// (cutscenes wait in WaitForVBlankInterrupt), the request waits for a later frame.
bool Port_StateAllowed(void)
{
    static const struct GbaFunction* sAgbMain;
    const struct GbaFunction* f = PortGbaFunctionBelow(PortHaltCaller());

    if (f == NULL)
        return false;
    if (sAgbMain == NULL && strcmp(f->name, "AgbMain") == 0)
        sAgbMain = f;
    return f == sAgbMain;
}

// ---- second screen (not used yet) --------------------------------------------------------

void Port_SecondScreenSize(int displayW, int displayH, int* width, int* height)
{
    (void)displayW;
    (void)displayH;
    *width = 256;
    *height = 192;
}

void Port_SecondScreenRender(uint32_t* pixels, int width, int height, int mode)
{
    (void)mode;
    for (int i = 0; i < width * height; i++)
        pixels[i] = 0xFF000000u;
}

int Port_SecondScreenTouch(int x, int y, int action)
{
    (void)x;
    (void)y;
    (void)action;
    return -1;
}

uint16_t Port_SecondScreenKeys(void)
{
    return 0;
}

// ---- running the game ----------------------------------------------------------------------

void AgbMain(void);
void irq_handler(void);

// What crt0's _start does: it points the interrupt vector at irq_handler in the ROM, calls
// AgbMain, and starts again if AgbMain returns.
static void GameEntry(void)
{
    for (;;) {
        *(volatile uint32_t*)(uintptr_t)0x03007FFC = PortNativeFunctionToGba(irq_handler) & ~1u;
        AgbMain();
    }
}

static bool LoadRom(void)
{
    const char* error = NULL;

    for (;;) {
        size_t size = 0;
        uint8_t* rom = Port_HostLoadRom(&size, error);
        if (rom == NULL)
            return false;
        PortRomStatus status = Port_CheckRom(rom, size);
        if (status == PORT_ROM_OK) {
            memcpy((void*)(uintptr_t)GBA_ROM_BASE, rom, GBA_ROM_SIZE);
            Port_HostRomAccepted(rom, size);
        }
        free(rom);
        if (status == PORT_ROM_OK)
            return PortProtectRom() == 0;
        error = Port_RomStatusText(status);
        Port_Log("ROM rejected: %s", error);
    }
}

static void QueueAudio(void)
{
    int16_t frames[2048 * 2];
    size_t n;

    while ((n = PortAudioTake(frames, 2048)) != 0)
        Port_HostQueueAudio(frames, (int)n);
}

int Port_Main(int argc, char** argv)
{
    PortHostConfig cfg = { argc, argv };

#ifdef __ANDROID__
    PortOsSetErrorSink(LogError);
#endif
    // The GBA address space first, before anything else can take it.
    if (PortReserveGbaMemory())
        return 1;
    PortInstallFaultHandler();
    if (!Port_HostInit(&cfg))
        return 1;
    if (!LoadRom()) {
        Port_HostShutdown();
        return 1;
    }
    Port_HostLoadSave(Sram(), SRAM_SIZE);
    memcpy(sSavedSram, Sram(), SRAM_SIZE);
    memcpy(sLastSram, Sram(), SRAM_SIZE);

    PortPpuReset(PORT_PPU_COLORS_GBA);
    if (PortFrameStart(GameEntry)) {
        Port_HostShutdown();
        return 1;
    }
    while (!sQuit) {
        const uint16_t keys = Port_HostPollKeys() & PORT_KEY_MASK;
        PortIoWrite16(REG_KEYINPUT, (uint16_t)(~keys & PORT_KEY_MASK));
        if (!PortFrameRun()) {
            Port_Log("the game stopped after %llu frames", (unsigned long long)sFrames);
            break;
        }
        sFrames++;
        QueueAudio();
        Port_RumbleFrame();
        FlushSave(false);
        // The frame is copied first: a state loaded at the boundary shows its own.
        CopyPicture();
        Port_HostFrameBoundary();
        if (sQuit)
            break;
        if (!Port_HostPresent(sPicture, GBA_SCREEN_WIDTH, GBA_SCREEN_HEIGHT))
            Port_RequestQuit();
        Port_HostWaitFrame();
    }
    FlushSave(true);
    Port_HostShutdown();
    return 0;
}
