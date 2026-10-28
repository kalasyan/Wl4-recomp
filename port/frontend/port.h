// Interface between the SDL frontend (host_sdl.c, menu.cpp) and the port's core
// (core.c), which runs the game on the runtime in port/src.
//
// The frontend comes from the sibling project fire-emblem-ss-recomp and keeps its
// shape: the core calls Port_Host* to show frames, play sound and read input, and
// the host takes save states at Port_HostFrameBoundary().
#ifndef PORT_FRONTEND_H
#define PORT_FRONTEND_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "port/audio.h"

#define GBA_SCREEN_WIDTH 240
#define GBA_SCREEN_HEIGHT 160
// Widest frame the port renders.
#define PORT_MAX_SCREEN_WIDTH 320

// GBA KEYINPUT bit layout.
enum {
    PORT_KEY_A = 1 << 0,
    PORT_KEY_B = 1 << 1,
    PORT_KEY_SELECT = 1 << 2,
    PORT_KEY_START = 1 << 3,
    PORT_KEY_RIGHT = 1 << 4,
    PORT_KEY_LEFT = 1 << 5,
    PORT_KEY_UP = 1 << 6,
    PORT_KEY_DOWN = 1 << 7,
    PORT_KEY_R = 1 << 8,
    PORT_KEY_L = 1 << 9,
    PORT_KEY_MASK = 0x3FF,
};

#ifdef __cplusplus
extern "C" {
#endif

uint64_t Port_FrameCount(void);

// Picture width. Only 240 (the GBA screen) for now.
void Port_WideSetWidth(int width);
int Port_WideWidth(void);

// ---- host (frontend) interface: host_sdl.c ----------------------------------------
typedef struct PortHostConfig {
    int argc;
    char** argv;
} PortHostConfig;

int Port_Main(int argc, char** argv);

bool Port_HostInit(const PortHostConfig* cfg);
void Port_HostShutdown(void);
// Present a finished frame. Returns false if the user asked to quit.
bool Port_HostPresent(const uint32_t* abgr, int width, int height);
// Pressed keys, as PORT_KEY_* bits (1 = pressed).
uint16_t Port_HostPollKeys(void);
// One frame of sound: interleaved stereo, PORT_AUDIO_RATE Hz.
void Port_HostQueueAudio(const int16_t* samples, int frames);
// Block until the next 1/59.73 s tick (no-op when running unthrottled).
void Port_HostWaitFrame(void);
// Rumble for this frame (port/frontend/rumble.c). low/high: strength 0..1 of the
// low / high frequency motors of a gamepad, sent every frame. pulse > 0: a
// vibration of that strength and length starts now (for vibrators that only
// take one-shot pulses, like a phone's).
void Port_HostRumble(float low, float high, float pulse, int pulseMs);
// Once per frame: works out the rumble from what the game reported
// (port/include/port/rumble_snapshot.h) and calls Port_HostRumble.
void Port_RumbleFrame(void);
// After a save state was loaded: stop the rumble.
void Port_RumbleReset(void);
// The player's ROM. Returns a malloc'd copy of the whole file, or NULL if
// the user gave up (the game then quits). `error` is set on a retry after
// a ROM was rejected, so the host can tell the user why.
uint8_t* Port_HostLoadRom(size_t* size, const char* error);
// Remember a validated ROM for next time (copy it into the app's storage).
void Port_HostRomAccepted(const uint8_t* rom, size_t size);
// Persistent save data (the 32 KiB SRAM image, mGBA's .sav format).
void Port_HostLoadSave(uint8_t* data, size_t size);
void Port_HostWriteSave(const uint8_t* data, size_t size);

void Port_RequestQuit(void);
bool Port_QuitRequested(void);

// ---- save states (core.c) ------------------------------------------------------------
typedef enum PortStateResult {
    PORT_STATE_OK,
    PORT_STATE_BAD_FILE,    // not a save state, or truncated
    PORT_STATE_OTHER_BUILD, // made by another version of the port
} PortStateResult;

// Called between two frames: the menu and save state hotkeys run here. Save
// states also need Port_StateAllowed(): the game waiting for VBlank at the end
// of its main loop, the only point where its native stack is always the same.
void Port_HostFrameBoundary(void);
// Can a state be taken or restored now? Not while the game waits for VBlank anywhere but at
// the end of its main loop (cutscenes wait elsewhere).
bool Port_StateAllowed(void);
size_t Port_StateSize(void);
void Port_StateSave(uint8_t* buf);
PortStateResult Port_StateLoad(const uint8_t* buf, size_t size);
const char* Port_StateResultText(PortStateResult result);

void Port_Log(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

#ifdef __cplusplus
}
#endif

#endif // PORT_FRONTEND_H
