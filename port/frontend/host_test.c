// A host without a window for the frontend's core (core.c), to test it headless:
//
//   wl4_core_test ROM FRAMES [--input KEYS] [--save-at F] [--load-at F] [--shot F FILE.ppm]
//
// Prints one line per frame: the frame number and a hash of the picture. --save-at takes a
// save state in memory at the first frame boundary from frame F on, --load-at restores it at
// the first one from frame F on; the frames after a load must repeat those after the save.
// Keys come from an input script (port/include/port/input_script.h), by main-loop step, so
// they repeat too.
#include "port.h"
#include "port_assets.h"

#include <stdio.h>
#include <stdlib.h>

#include "port/frame.h"
#include "port/input_script.h"

static const char* sRomPath;
static unsigned long sFrames;
static unsigned long sSaveAt = ~0ul, sLoadAt = ~0ul, sShotAt = ~0ul;
static const char* sShotPath;
static bool sScripted;
static uint8_t* sState;
static size_t sStateSize;

bool Port_HostInit(const PortHostConfig* cfg)
{
    (void)cfg;
    return true;
}

void Port_HostShutdown(void)
{
}

bool Port_HostPresent(const uint32_t* abgr, int width, int height)
{
    uint64_t hash = 1469598103934665603ull;
    for (int i = 0; i < width * height; i++)
        hash = (hash ^ abgr[i]) * 1099511628211ull;
    printf("%llu %016llx\n", (unsigned long long)Port_FrameCount(), (unsigned long long)hash);
    if (Port_FrameCount() == sShotAt && sShotPath) {
        FILE* f = fopen(sShotPath, "wb");
        if (f) {
            fprintf(f, "P6\n%d %d\n255\n", width, height);
            for (int i = 0; i < width * height; i++) {
                const uint8_t rgb[3] = { (uint8_t)abgr[i], (uint8_t)(abgr[i] >> 8), (uint8_t)(abgr[i] >> 16) };
                fwrite(rgb, 1, 3, f);
            }
            fclose(f);
        }
    }
    if (Port_FrameCount() >= sFrames)
        Port_RequestQuit();
    return true;
}

uint16_t Port_HostPollKeys(void)
{
    return sScripted ? PortInputScriptKeys(PortFrameSteps() + 1) : 0;
}

void Port_HostQueueAudio(const int16_t* samples, int frames)
{
    (void)samples;
    (void)frames;
}

void Port_HostWaitFrame(void)
{
}

void Port_HostRumble(float low, float high, float pulse, int pulseMs)
{
    if (pulse > 0.0f)
        fprintf(stderr, "rumble at frame %llu: %.2f %.2f for %d ms\n", (unsigned long long)Port_FrameCount(), low,
                high, pulseMs);
}

uint8_t* Port_HostLoadRom(size_t* size, const char* error)
{
    FILE* f;
    uint8_t* data;
    long len;

    if (error) {
        fprintf(stderr, "%s: %s\n", sRomPath, error);
        return NULL;
    }
    f = fopen(sRomPath, "rb");
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    len = ftell(f);
    fseek(f, 0, SEEK_SET);
    data = malloc(len > 0 ? (size_t)len : 1);
    if (data && fread(data, 1, (size_t)len, f) != (size_t)len) {
        free(data);
        data = NULL;
    }
    fclose(f);
    *size = (size_t)len;
    return data;
}

void Port_HostRomAccepted(const uint8_t* rom, size_t size)
{
    (void)rom;
    (void)size;
}

void Port_HostLoadSave(uint8_t* data, size_t size)
{
    (void)data;
    (void)size;
}

void Port_HostWriteSave(const uint8_t* data, size_t size)
{
    (void)data;
    fprintf(stderr, "save written at frame %llu (%zu bytes)\n", (unsigned long long)Port_FrameCount(), size);
}

void Port_HostFrameBoundary(void)
{
    if (!Port_StateAllowed())
        return;
    if (Port_FrameCount() >= sSaveAt && !sState) {
        sStateSize = Port_StateSize();
        sState = malloc(sStateSize);
        Port_StateSave(sState);
        fprintf(stderr, "state saved at frame %llu (%zu bytes)\n", (unsigned long long)Port_FrameCount(), sStateSize);
        sSaveAt = ~0ul;
    }
    if (Port_FrameCount() >= sLoadAt && sState) {
        const unsigned long long at = Port_FrameCount();
        PortStateResult r = Port_StateLoad(sState, sStateSize);
        fprintf(stderr, "state loaded at frame %llu: %s (now frame %llu)\n", at, Port_StateResultText(r),
                (unsigned long long)Port_FrameCount());
        sLoadAt = ~0ul;
    }
}

int main(int argc, char** argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: wl4_core_test ROM FRAMES [--input KEYS] [--save-at F] [--load-at F] "
                        "[--shot F FILE.ppm]\n");
        return 2;
    }
    sRomPath = argv[1];
    sFrames = strtoul(argv[2], NULL, 10);
    for (int i = 3; i < argc; i++) {
        if (strcmp(argv[i], "--input") == 0 && i + 1 < argc) {
            if (PortInputScriptLoad(argv[++i]))
                return 1;
            sScripted = true;
        } else if (strcmp(argv[i], "--save-at") == 0 && i + 1 < argc) {
            sSaveAt = strtoul(argv[++i], NULL, 10);
        } else if (strcmp(argv[i], "--load-at") == 0 && i + 1 < argc) {
            sLoadAt = strtoul(argv[++i], NULL, 10);
        } else if (strcmp(argv[i], "--shot") == 0 && i + 2 < argc) {
            sShotAt = strtoul(argv[++i], NULL, 10);
            sShotPath = argv[++i];
        } else {
            fprintf(stderr, "unknown option %s\n", argv[i]);
            return 2;
        }
    }
    return Port_Main(argc, argv);
}
