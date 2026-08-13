import serial
import time
import sys
import os
import tempfile

PORT = 'COM3'
BAUD_RATE = 115200

# Safely place the log file in the system's temp folder to avoid permission blocks
OUTPUT_LOG = os.path.join(tempfile.gettempdir(), 'tron_telemetry.log')

def run_telemetry_bridge():
    print(f"Connecting to target board on {PORT} at {BAUD_RATE} baud...")
    try:
        ser = serial.Serial(PORT, BAUD_RATE, timeout=0.1)
    except Exception as e:
        print(f"Error opening serial port {PORT}: {e}")
        input("\nPress Enter to exit...")
        sys.exit(1)

    print(f"Successfully connected! Listening for board output...")
    print(f"Log file location: {OUTPUT_LOG}\n" + "-"*50)
    
    log_file = None
    try:
        log_file = open(OUTPUT_LOG, 'a', encoding='utf-8')
    except Exception as e:
        print(f"Notice: Running in console-only mode (file logging disabled: {e})\n")

    try:
        while True:
            line = ser.readline()
            if line:
                decoded_line = line.decode('utf-8', errors='ignore').strip()
                if decoded_line:
                    timestamp = time.strftime('%Y-%m-%d %H:%M:%S')
                    log_entry = f"[{timestamp}] {decoded_line}"
                    
                    # Print live to your screen
                    print(log_entry)
                    
                    # Write to file if open
                    if log_file:
                        log_file.write(log_entry + '\n')
                        log_file.flush()
            else:
                time.sleep(0.01)
                
    except KeyboardInterrupt:
        print("\nTelemetry transfer stopped by user.")
    except Exception as e:
        print(f"\nAn unexpected error occurred: {e}")
    finally:
        if log_file:
            log_file.close()
        if 'ser' in locals() and ser.is_open:
            ser.close()
        print("Serial connection closed.")
        input("\nPress Enter to exit...")

if __name__ == '__main__':
    run_telemetry_bridge()