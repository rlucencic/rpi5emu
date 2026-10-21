# rpi5emu

Shared Linux runtime for retro-computer emulators on Raspberry Pi 5.

This repository is the common platform layer for the `rpi5*` machine projects. Machine-specific projects supply their computer's hardware emulation and connect it to this runtime.

## Initial target

- Raspberry Pi 5, 64-bit ARM Linux
- Ubuntu 26.04 LTS aarch64
- SDL 3 for video, audio, keyboard, mouse, and controller input

## Planned runtime

The Linux runtime will provide frame pacing, presentation of machine-rendered frames, audio output, input dispatch, settings, and file-backed ROM and disk-image storage. Its interface will be shaped by the existing `emu_machine_t` boundary in [esp32emu](https://github.com/rlucencic/esp32emu), while keeping ESP-IDF and FreeRTOS out of Linux builds.

The first planned machine project is [rpi5zx](https://github.com/rlucencic/rpi5zx), a ZX Spectrum emulator based on the machine implementation in [esp32zx](https://github.com/rlucencic/esp32zx).

## Status

Project scaffold only. The Linux runtime and emulator integration have not been implemented yet.
