# STM32F407 UART Bootloader

Bare-metal two-stage boot for the STM32F407VET6 (DevEBox "F4VE" board).
Register-level, no HAL.

## Status
- [x] Phase 1: application linked at 0x08010000, VTOR relocated
- [x] Phase 2: bootloader at 0x08000000, jump-to-application with SP + vector handoff
- [ ] Phase 3: UART receive, flash self-programming, framed update protocol
- [ ] Phase 4: Python/C# host flashing tool

## Layout
- `f407-boot/` — bootloader project (linked at 0x08000000, 64 KB reserved)
- `f407-app/`  — application project (linked at 0x08010000, 448 KB)

## Hardware
- STM32F407VET6, ST-LINK/V2, USB-C for power
- LEDs: D2/PA6 (bootloader indicator), D2+D3/PA6+PA7 (app running)