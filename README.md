# STM32F407 UART Bootloader

Bare-metal two-stage boot for the STM32F407VET6 (DevEBox "F4VE" board).  
Register-level, no HAL, no vendor SDK.

## Status

- [x] Phase 1: Application at 0x08010000, VTOR relocated
- [x] Phase 2: Bootloader with jump-to-application (SP + vector handoff)
- [x] Phase 3a: Button-triggered mode select (K0/PA0 within timeout window)
- [x] Phase 3b: UART driver — USART1 PA9/PA10, 115200-8N1, bare-metal register config
- [x] Phase 4: Flash self-programming (unlock, sector erase, word program, verify)
- [x] Phase 5: Framed protocol + Python host flasher (PING, ERASE, WRITE, VERIFY, JUMP)
- [ ] Phase 6: File restructure (uart.c, flash.c, protocol.c)
- [ ] Phase 7 (stretch): CAN transport, AES-128 encryption, ECDSA image signature

## Protocol

Custom binary framing over UART:

```
[SYNC 0x7F] [CMD] [LEN_H] [LEN_L] [PAYLOAD...] [CRC32]
```

| Command | Code | Payload | Response |
|---------|------|---------|----------|
| PING    | 0x01 | none | ACK (0x79) |
| ERASE   | 0x02 | none (sectors 4–7 hardcoded) | ACK |
| WRITE   | 0x03 | 4-byte addr + up to 1024 data | ACK |
| VERIFY  | 0x04 | 4-byte addr + 4-byte length | ACK + CRC32 |
| JUMP    | 0x05 | none | ACK → app launches |

- CRC32 over CMD+LEN+PAYLOAD (standard polynomial 0xEDB88320)
- Address fencing: WRITE/VERIFY reject addresses outside 0x08010000–0x0807FFFF
- Bootloader sectors (0–3) are never erased or written — hardcoded protection
- NACK (0x1F) + error code returned on failure

### Error Codes

| Code | Meaning |
|------|---------|
| 0x01 | Payload too large (>1024 bytes) |
| 0x02 | CRC mismatch |
| 0x03 | Payload too short for WRITE |
| 0x04 | Address out of range |
| 0x05 | Invalid VERIFY payload length |
| 0x06 | No valid app (JUMP rejected) |
| 0xFF | Unknown command |

## Flash Memory Map

```
Sector 0:  0x08000000  16KB  ┐
Sector 1:  0x08004000  16KB  │ Bootloader (64KB)
Sector 2:  0x08008000  16KB  │ Protected — never erased by protocol
Sector 3:  0x0800C000  16KB  ┘
────────────────────────────────
Sector 4:  0x08010000  64KB  ┐
Sector 5:  0x08020000  128KB │ Application (448KB)
Sector 6:  0x08040000  128KB │ Erased + programmed via UART
Sector 7:  0x08060000  128KB ┘
```

## Boot Flow

```
Power On → Intro Blinks (6 toggles on PA6)
    │
    ├── K0 pressed within timeout?
    │   ├── Yes → UART init → "BOOT> Ready" → Protocol Loop
    │   │         (PING / ERASE / WRITE / VERIFY / JUMP)
    │   └── No  → Valid app at 0x08010000?
    │               ├── Yes → Clean peripherals → VTOR + MSP + Jump to app
    │               └── No  → Fast blink (no valid app found)
```

### App Validation

The bootloader reads the first word at 0x08010000 (the app's initial stack pointer).  
If it points into SRAM (0x20000000–0x20020000), the image is considered valid.  
Erased flash (0xFFFFFFFF) or test patterns fail this check — the bootloader refuses to jump.

### Jump Sequence (Register Level)

```
1. Read initial SP from 0x08010000
2. Read Reset_Handler address from 0x08010004
3. Disable USART1, reset GPIOA to defaults
4. Set SCB->VTOR = 0x08010000
5. Load MSP with app's stack pointer (MSR instruction)
6. Branch to Reset_Handler (function pointer call)
```

## Demo Output

```
$ py flasher.py COM5 f407-app.bin
Firmware: f407-app.bin (936 bytes, 0.9KB, 1 chunks)
Waiting for bootloader...
  [RX] BOOT> Ready
Connected!
PING...
  [TX] 7f 01 00 00 fe 83 b3 25
  [RX] 79
  ACK
ERASE sectors 4-7...
  [TX] 7f 02 00 00 fc c5 0d 7c
  [RX] 79
  ACK (4.0s)
WRITE 1/1 @ 0x08010000 (936 bytes)...
  [TX] 7f 03 03 ac 08 01 00 00 ... (936 bytes of firmware)
  [RX] 79
  ACK
VERIFY...
  [TX] 7f 04 00 08 08 01 00 00 00 00 03 a8 e6 34 08 6e
  [RX] 79 69 27 f1 49
  MATCH (CRC: 0x6927F149)
JUMP...
  [TX] 7f 05 00 00 f9 8a 1b f9
  [RX] 79
  App launched!
Done in 4.1s
```

## Hardware

- **MCU:** STM32F407VET6 (DevEBox F4VE board)
- **Debugger:** ST-LINK/V2 (SWD interface)
- **UART adapter:** CH340 USB-to-TTL module (3.3V logic)
- **Power:** USB-C (board powered independently from adapter)
- **LEDs:** D2/PA6 (bootloader indicator), D2+D3/PA6+PA7 (app running)
- **Boot mode entry:** K0/PA0 held during intro blinks → update mode

### Wiring

```
CH340 Adapter          STM32F407 (USART1 Header)
─────────────          ─────────────────────────
TXD            →       RX1 (PA10)
RXD            →       TX1 (PA9)
GND            →       GND
VCC/5V/3V3     →       NOT CONNECTED (board powered via USB-C)

Yellow jumper on adapter: set to 3.3V side
```

## Build

- **IDE:** STM32CubeIDE
- **Toolchain:** arm-none-eabi-gcc
- **No dependencies:** no HAL, no CMSIS middleware, no RTOS — just CMSIS device headers for register definitions

### Projects

| Project | Flash origin | Size | Description |
|---------|-------------|------|-------------|
| f407-boot | 0x08000000 | 64KB reserved | Bootloader |
| f407-app | 0x08010000 | 448KB available | Application |

### Generate .bin from .elf

Command line:
```
arm-none-eabi-objcopy -O binary f407-app.elf f407-app.bin
```

Or add as a post-build step in CubeIDE:  
Project Properties → C/C++ Build → Settings → Build Steps → Post-build command:
```
arm-none-eabi-objcopy -O binary "${BuildArtifactFileBaseName}.elf" "${BuildArtifactFileBaseName}.bin"
```

## Host Tool

`flasher.py` — Python 3 CLI flashing tool

**Requirements:**
```
pip install pyserial
```

**Usage:**
```
py flasher.py COM5 f407-app.bin
```

**Features:**
- Automatic bootloader greeting detection
- CRC32-verified frame transmission
- Chunked firmware transfer (1024 bytes per chunk)
- Full TX/RX frame logging to `flash_log.txt`
- Timing and transfer speed statistics
- Address validation (rejects writes outside app region)


## License

MIT
