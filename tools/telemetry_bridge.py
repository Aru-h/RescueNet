import serial
import serial.tools.list_ports
import time
import sys
import os
import csv
import struct
import tempfile

# None = auto-detect the serial port at startup (works the same way
# on Windows and macOS, since it goes through pyserial's own port
# enumeration rather than an OS-specific device name). Set this to a
# specific string (e.g. 'COM3' on Windows, '/dev/cu.usbmodem14201'
# on macOS) to skip detection and force a fixed port - useful for
# scripted/non-interactive runs where prompting isn't wanted, or if
# multiple boards are plugged in and auto-detect can't disambiguate.
PORT = None
BAUD_RATE = 115200

# Safely place the log file in the system's temp folder to avoid permission blocks
OUTPUT_LOG = os.path.join(tempfile.gettempdir(), 'tron_telemetry.log')

# Decoded binary telemetry frames go to their own CSV, next to the
# other tools/*.py-produced datasets (tron_snapshot_dataset.csv etc.)
# in the working directory - kept separate on purpose, per
# core/telemetry_frame.h: this is a 4-column board-side ring dump
# (run_id, cycle, team_id, event_type), not the laptop sim's 48-column
# schema, and the two should never be merged into one file.
OUTPUT_CSV = 'tron_board_telemetry.csv'
CSV_HEADER = ['run_id', 'cycle', 'team_id', 'event_type']

# --- Wire format constants, mirroring core/telemetry_frame.h -------
# Kept in sync by hand since this tool has no C header access; if
# core/telemetry_frame.h's layout/magic/checksum ever changes, this
# block must change with it.
FRAME_MAGIC0 = 0xA5
FRAME_MAGIC1 = 0x5A
FRAME_PAYLOAD_SIZE = 10          # run_id(u32) + cycle(u32) + team_id(u8) + event_type(u8)
FRAME_WIRE_SIZE = 2 + FRAME_PAYLOAD_SIZE + 1   # magic + payload + checksum = 13
FRAME_PAYLOAD_STRUCT = struct.Struct('<IIBB')  # little-endian: run_id, cycle, team_id, event_type


def frame_checksum(payload_bytes):
    """1-byte additive checksum over the 10-byte payload, matching
    TELEMETRY_FRAME_CHECKSUM() in core/telemetry_frame.h (sum of all
    payload bytes, mod 256). Not a real CRC - deliberately cheap,
    same tradeoff documented in the header."""
    return sum(payload_bytes) & 0xFF


def open_csv_writer(path):
    """Opens OUTPUT_CSV for append, writing the header only if the
    file is new/empty so repeated bridge runs accumulate into the
    same dataset rather than clobbering it (matches OUTPUT_LOG's
    append behavior below)."""
    file_is_new = not os.path.exists(path) or os.path.getsize(path) == 0
    csv_file = open(path, 'a', newline='', encoding='utf-8')
    writer = csv.writer(csv_file)
    if file_is_new:
        writer.writerow(CSV_HEADER)
        csv_file.flush()
    return csv_file, writer


def log_text_line(decoded_line, log_file):
    """Existing plain-text debug log behavior, unchanged: timestamp,
    print live, write to OUTPUT_LOG if open. Anything read off the
    wire that isn't a valid framed TelemetryFrame_t ends up here,
    exactly as every line did before this file had any framing
    logic."""
    if not decoded_line:
        return
    timestamp = time.strftime('%Y-%m-%d %H:%M:%S')
    log_entry = f"[{timestamp}] {decoded_line}"
    print(log_entry)
    if log_file:
        log_file.write(log_entry + '\n')
        log_file.flush()


def log_frame(run_id, cycle, team_id, event_type, csv_writer, csv_file):
    """Writes one decoded frame to OUTPUT_CSV and echoes it to the
    console for live visibility, same spirit as log_text_line()."""
    timestamp = time.strftime('%Y-%m-%d %H:%M:%S')
    print(f"[{timestamp}] FRAME run_id={run_id} cycle={cycle} "
          f"team_id={team_id} event_type={event_type}")
    csv_writer.writerow([run_id, cycle, team_id, event_type])
    csv_file.flush()


def flush_text_buffer(text_buf, log_file, final=False):
    """Splits text_buf on newlines, logging each complete line via
    log_text_line() (matching the old readline()+strip()+log
    behavior line-for-line). Returns the leftover bytes after the
    last newline, which stay buffered until either the next newline
    arrives or `final` is True (end of run - flush whatever's left
    as one last line, same as a stream ending without a trailing
    newline)."""
    while b'\n' in text_buf:
        line, _, text_buf = text_buf.partition(b'\n')
        decoded_line = line.decode('utf-8', errors='ignore').strip()
        log_text_line(decoded_line, log_file)

    if final and text_buf:
        decoded_line = text_buf.decode('utf-8', errors='ignore').strip()
        log_text_line(decoded_line, log_file)
        text_buf = b''

    return text_buf


def process_buffer(buf, text_buf, csv_writer, csv_file, log_file):
    """Byte-level scan of `buf` for sync-prefixed TelemetryFrame_t
    frames (core/telemetry_frame.h). Everything that isn't part of a
    validated frame - including a lone 0xA5 that turns out not to be
    followed by 0x5A, and a magic pair whose checksum doesn't check
    out - is handed to `text_buf` for the unchanged plain-text log
    path, one byte at a time if needed, so resync after a false
    match or a torn frame costs at most a few bytes (per the resync
    behavior documented in telemetry_frame.h).

    Returns (buf, text_buf) with `buf` trimmed down to only the
    bytes still needed (an in-progress possible frame waiting on
    more serial data, or empty)."""
    while True:
        magic_index = buf.find(bytes([FRAME_MAGIC0]))

        if magic_index == -1:
            # No possible frame start in what we have - all of it is
            # plain text.
            text_buf += buf
            buf = b''
            break

        # Everything before the candidate magic byte is plain text.
        if magic_index > 0:
            text_buf += buf[:magic_index]
            buf = buf[magic_index:]
            magic_index = 0

        if len(buf) < 2:
            # Not enough bytes yet to know if this is MAGIC0 MAGIC1 -
            # wait for more data.
            break

        if buf[1] != FRAME_MAGIC1:
            # False positive: 0xA5 wasn't the start of a frame.
            # Pass just that one byte through as text and keep
            # scanning the rest for a real sync prefix.
            text_buf += buf[:1]
            buf = buf[1:]
            continue

        if len(buf) < FRAME_WIRE_SIZE:
            # Valid sync prefix, but the full frame hasn't arrived
            # over serial yet - wait for more bytes before deciding.
            break

        payload = buf[2:2 + FRAME_PAYLOAD_SIZE]
        received_checksum = buf[2 + FRAME_PAYLOAD_SIZE]

        if frame_checksum(payload) == received_checksum:
            run_id, cycle, team_id, event_type = FRAME_PAYLOAD_STRUCT.unpack(payload)
            log_frame(run_id, cycle, team_id, event_type, csv_writer, csv_file)
            buf = buf[FRAME_WIRE_SIZE:]
        else:
            # Checksum mismatch: this magic pair was either a
            # coincidental byte sequence inside text/other data, or a
            # torn frame. Per telemetry_frame.h's resync guidance,
            # reject just the 2 magic bytes as text and keep scanning
            # - don't skip the full frame width, since a real frame
            # may start one byte later than this false match did.
            text_buf += buf[:2]
            buf = buf[2:]

    return buf, text_buf


def discover_port():
    """Finds a serial port to use when PORT is None, via pyserial's
    own cross-platform enumeration (serial.tools.list_ports) rather
    than any OS-specific device-naming assumption - this is the same
    mechanism on Windows (COMx) and macOS (/dev/cu.*), just returning
    different device strings.

    Returns a device string, or None if no port was found/chosen (in
    which case the caller should treat it like the old hardcoded-PORT
    path failing to open)."""
    ports = list(serial.tools.list_ports.comports())

    if not ports:
        print("No serial ports found. Is the board plugged in?")
        return None

    if len(ports) == 1:
        chosen = ports[0].device
        print(f"Auto-detected serial port: {chosen} ({ports[0].description})")
        return chosen

    print("Multiple serial ports found:")
    for i, p in enumerate(ports):
        print(f"  [{i}] {p.device} ({p.description})")

    try:
        selection = input(f"Select a port [0-{len(ports) - 1}]: ").strip()
        index = int(selection)
        if index < 0 or index >= len(ports):
            raise ValueError
    except (ValueError, EOFError):
        print("Invalid selection.")
        return None

    return ports[index].device


def run_telemetry_bridge():
    port = PORT
    if port is None:
        port = discover_port()
        if port is None:
            print("No serial port selected/available - set PORT explicitly "
                  "in telemetry_bridge.py to bypass auto-detect.")
            input("\nPress Enter to exit...")
            sys.exit(1)

    print(f"Connecting to target board on {port} at {BAUD_RATE} baud...")
    try:
        ser = serial.Serial(port, BAUD_RATE, timeout=0.1)
    except Exception as e:
        print(f"Error opening serial port {port}: {e}")
        input("\nPress Enter to exit...")
        sys.exit(1)

    print(f"Successfully connected! Listening for board output...")
    print(f"Log file location: {OUTPUT_LOG}")
    print(f"Telemetry CSV location: {os.path.abspath(OUTPUT_CSV)}\n" + "-" * 50)

    log_file = None
    try:
        log_file = open(OUTPUT_LOG, 'a', encoding='utf-8')
    except Exception as e:
        print(f"Notice: Running in console-only mode (file logging disabled: {e})\n")

    csv_file = None
    csv_writer = None
    try:
        csv_file, csv_writer = open_csv_writer(OUTPUT_CSV)
    except Exception as e:
        print(f"Notice: Telemetry CSV logging disabled: {e}\n")

    buf = b''       # bytes not yet classified as frame or text
    text_buf = b''  # bytes classified as text, pending a newline

    try:
        while True:
            # Byte-level read instead of readline(): a binary frame
            # has no line-ending semantics and can contain 0x0A as
            # part of run_id/cycle, so line-splitting has to happen
            # only on bytes already identified as plain text, after
            # frame scanning, not on the raw serial read.
            chunk = ser.read(max(1, ser.in_waiting or 1))

            if chunk:
                buf += chunk
                if csv_writer is not None:
                    buf, text_buf = process_buffer(buf, text_buf, csv_writer, csv_file, log_file)
                else:
                    # No CSV sink available: still keep the debug-log
                    # path working, but frame bytes have nowhere
                    # useful to go, so just treat everything as text
                    # rather than silently discarding candidate
                    # frames into an unused decode path.
                    text_buf += buf
                    buf = b''

                text_buf = flush_text_buffer(text_buf, log_file)
            else:
                time.sleep(0.01)

    except KeyboardInterrupt:
        print("\nTelemetry transfer stopped by user.")
    except Exception as e:
        print(f"\nAn unexpected error occurred: {e}")
    finally:
        # Whatever's left in buf is an incomplete frame or sync
        # prefix - by definition not decodable, so it is treated as
        # text and flushed rather than silently dropped.
        text_buf += buf
        flush_text_buffer(text_buf, log_file, final=True)

        if log_file:
            log_file.close()
        if csv_file:
            csv_file.close()
        if 'ser' in locals() and ser.is_open:
            ser.close()
        print("Serial connection closed.")
        input("\nPress Enter to exit...")


if __name__ == '__main__':
    run_telemetry_bridge()