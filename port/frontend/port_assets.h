// The player's ROM: checks (core.c).
#ifndef PORT_ASSETS_H
#define PORT_ASSETS_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum PortRomStatus {
    PORT_ROM_OK,
    PORT_ROM_WRONG_SIZE,
    PORT_ROM_WRONG_GAME,
} PortRomStatus;

PortRomStatus Port_CheckRom(const uint8_t* rom, size_t size);
const char* Port_RomStatusText(PortRomStatus status);
// The only ROM the port runs: "USA, Europe" (one cartridge for both).
const char* Port_RomRegionName(void);
// MD5 of that ROM, for the ROM screen.
const char* Port_RomExpectedMd5(void);

#ifdef __cplusplus
}
#endif

#endif // PORT_ASSETS_H
