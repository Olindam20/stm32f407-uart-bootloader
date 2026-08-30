import serial, struct, time, sys
from binascii import crc32

CHUNK_SIZE = 1020
BASE_ADDR  = 0x08010000
LOG_FILE   = "flash_log.txt"

log_handle = None


def log(msg):
    print(msg)
    if log_handle:
        log_handle.write(msg + "\n")

def log_frame(direction, data):
    hex_str = data.hex(' ')
    msg = f"  [{direction}] {hex_str}"
    print(msg)
    if log_handle:
        log_handle.write(msg + "\n")

def enter_bootloader(port):
    """Wake a running app into the bootloader via software reset.
    Sends a full valid PING frame — harmless whether caught by a
    running app (triggers reset) or an already-running bootloader
    (gets ACK'd normally, no desync)."""
    log("Requesting bootloader mode (in case app is running)...")
    port.reset_input_buffer()

    # Send a complete PING frame, not a bare 0x7F —
    # this way, even if the bootloader is already listening,
    # it gets a fully valid frame instead of a dangling SYNC byte
    header = bytes([0x01, 0x00, 0x00])  # CMD_PING, LEN=0
    checksum = crc32(header) & 0xFFFFFFFF
    ping_frame = bytes([0x7F]) + header + struct.pack(">I", checksum)
    port.write(ping_frame)
    port.flush()

    log("  Sent PING frame, waiting for reset/response to settle...")
    time.sleep(1.5)
    port.reset_input_buffer()

def send_cmd(port, cmd, payload=b""):
    header = bytes([cmd, len(payload) >> 8, len(payload) & 0xFF])
    checksum = crc32(header + payload) & 0xFFFFFFFF
    frame = bytes([0x7F]) + header + payload + struct.pack(">I", checksum)
    log_frame("TX", frame)
    port.write(frame)
    port.flush()
    resp = port.read(1)
    if not resp:
        log_frame("RX", b"<timeout>")
        raise Exception("Timeout — no response from bootloader")
    # Read any extra bytes (NACK error code, VERIFY CRC)
    extra = port.read(port.in_waiting)
    full_resp = resp + extra
    log_frame("RX", full_resp)
    return resp, extra

def flash_firmware(port_name, bin_path):
    global log_handle
    log_handle = open(LOG_FILE, "w")
    log_handle.write(f"=== Flash Log — {time.strftime('%Y-%m-%d %H:%M:%S')} ===\n\n")

    with open(bin_path, "rb") as f:
        firmware = f.read()

    size_kb = len(firmware) / 1024
    chunks = (len(firmware) + CHUNK_SIZE - 1) // CHUNK_SIZE
    log(f"Firmware: {bin_path} ({len(firmware)} bytes, {size_kb:.1f}KB, {chunks} chunks)")

    port = serial.Serial(port_name, 115200, timeout=5)
    port.dtr = False
    time.sleep(0.3)              # let any DTR-triggered reset settle
    port.reset_input_buffer()    # clear garbage from that reset

    # Try to wake a running app first — no-op if bootloader already active
    enter_bootloader(port)

    # Wait for greeting — shows every byte, hard 5s timeout instead of hanging
    log("Waiting for bootloader...")
    buffer = b""
    deadline = time.time() + 5
    got_ready = False
    while time.time() < deadline:
        data = port.read(port.in_waiting or 1)
        if data:
            buffer += data
            log(f"  [raw] {data}")
        if b"Ready" in buffer:
            log(f"  [RX] {buffer.decode(errors='ignore').strip()}")
            log("Connected!")
            got_ready = True
            break
        time.sleep(0.1)

    if not got_ready:
        log(f"  TIMEOUT — buffer so far: {buffer!r}")
        port.close()
        log_handle.close()
        return

    time.sleep(0.2)
    port.reset_input_buffer()

    start_time = time.time()

    # 1. PING
    log("PING...")
    resp, _ = send_cmd(port, 0x01)
    if resp != b'\x79':
        log("  FAIL"); return
    log("  ACK")

    # 2. ERASE
    log("ERASE sectors 4-7...")
    t = time.time()
    resp, _ = send_cmd(port, 0x02)
    if resp != b'\x79':
        log("  FAIL"); return
    log(f"  ACK ({time.time()-t:.1f}s)")

    # 3. WRITE chunks
    t = time.time()
    for i in range(chunks):
        offset = i * CHUNK_SIZE
        chunk = firmware[offset:offset + CHUNK_SIZE]
        addr = BASE_ADDR + offset
        payload = struct.pack(">I", addr) + chunk
        log(f"WRITE {i+1}/{chunks} @ 0x{addr:08X} ({len(chunk)} bytes)...")
        resp, _ = send_cmd(port, 0x03, payload)
        if resp != b'\x79':
            log("  FAIL"); return
        log("  ACK")
    write_time = time.time() - t
    speed = len(firmware) / write_time / 1024
    log(f"Write complete ({write_time:.1f}s, {speed:.1f} KB/s)")

    # 4. VERIFY
    log("VERIFY...")
    payload = struct.pack(">II", BASE_ADDR, len(firmware))
    resp, extra = send_cmd(port, 0x04, payload)
    if resp == b'\x79' and len(extra) >= 4:
        flash_crc = struct.unpack(">I", extra[:4])[0]
        local_crc = crc32(firmware) & 0xFFFFFFFF
        if flash_crc == local_crc:
            log(f"  MATCH (CRC: 0x{flash_crc:08X})")
        else:
            log(f"  MISMATCH (flash=0x{flash_crc:08X} local=0x{local_crc:08X})")
            return
    else:
        log("  FAIL"); return

    # 5. JUMP
    log("JUMP...")
    resp, extra = send_cmd(port, 0x05)
    if resp == b'\x79':
        if extra and extra[0] == 0x1F:
            log("  No valid app!")
        else:
            log("  App launched!")
    else:
        log("  FAIL")

    total_time = time.time() - start_time
    log(f"\nDone in {total_time:.1f}s")
    log(f"Log saved to {LOG_FILE}")

    port.close()
    log_handle.close()

if __name__ == "__main__":
    if len(sys.argv) != 3:
        print("Usage: py flasher.py COM5 firmware.bin")
        sys.exit(1)
    flash_firmware(sys.argv[1], sys.argv[2])