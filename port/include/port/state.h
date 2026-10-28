#ifndef GUARD_PORT_STATE_H
#define GUARD_PORT_STATE_H

// The runtime's own part of the machine state: variables tagged PORT_STATE live in one
// section, which save states (port/frontend/core.c) copy whole along with GBA memory.
// Only plain values: no pointers, which would not survive into another run.
#if defined(__ELF__)
#define PORT_STATE __attribute__((section("wl4_state")))
#else
#define PORT_STATE
#endif

#endif  // GUARD_PORT_STATE_H
