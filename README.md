# STM32F407 UART Bootloader

Bare-metal two-stage boot for the STM32F407VET6 (DevEBox "F4VE" board).
Register-level, no HAL.

## Status
- [x] Phase 1: application at 0x08010000, VTOR relocated
- [x] Phase 2: bootloader with jump-to-application (SP + vector handoff)
- [x] Phase 4: flash self-programming (unlock, sector erase, word program, verify)
- [x] Phase 3a: button-triggered mode select (K_UP within 5s window)
- [ ] Phase 3b: UART receive over CH340
- [ ] Phase 5: framed protocol + Python host flasher
- [ ] Phase 6 (stretch): AES-128-CMAC seed-key auth, ECDSA image signature

## Layout
- `f407-boot/` — bootloader project (linked at 0x08000000, 64 KB reserved)
- `f407-app/`  — application project (linked at 0x08010000, 448 KB)

## Hardware
- STM32F407VET6, ST-LINK/V2, USB-C for power
- LEDs: D2/PA6 (bootloader indicator), D2+D3/PA6+PA7 (app running)