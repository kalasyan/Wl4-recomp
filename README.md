# Wario Land 4 recomp

A native port of *Wario Land 4* (Game Boy Advance) to Android, Linux and Windows, built
from the game's decompilation. It does not emulate the GBA's CPU: the decompiled C code is
compiled for the target and runs on a small model of the GBA hardware (video, sound, DMA,
interrupts).

**You need your own ROM.** No ROM or game data is included; everything is read from the
cartridge image you provide at run time. Supported: *Wario Land 4 (USA, Europe)*, MD5
`5fe47355a33e3fabec2a1607af88a404`.

Work in progress: see [docs/PORT.md](docs/PORT.md) once it lands.

## Credits

- [lilDavid/warioland4](https://github.com/lilDavid/warioland4) and
  [TsilaAllaoui/warioland4](https://github.com/TsilaAllaoui/warioland4): the decompilation.
- [BioRhythmicStudios/warioland4](https://github.com/BioRhythmicStudios/warioland4): the
  native PC port this one starts from (64-bit conversion of the game code, GBA memory map,
  I/O traps, software PPU and sound).

Licensed under the MIT license (see [LICENSE](LICENSE)). Not affiliated with Nintendo.
