#!/bin/bash
# Standalone build script for the Raspberry Pi Pico (RP2040) firmware.
# Bypasses the vendored BSP's own (Eclipse-only, xPack-toolchain) project
# format entirely - that toolchain plugin isn't installed in this
# environment's STM32CubeIDE, and the BSP's generic build_make/makefile
# defaults to a NUCLEO_H723 target (leftover shared file across every
# branch, per vendor/README.md - not actually wired for pico_rp2040).
# Drives arm-none-eabi-gcc directly instead, using exactly the compile
# flags the top-level Makefile's check-pico target already validated.
#
# Uses bash arrays throughout (not space-separated strings) because
# this repo's own path ("Tron Project") contains a space - word-splitting
# a plain string of paths silently truncates every path at that space.
set -e

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
PICO_DIR="$ROOT/board/Raspberry_Pi_Pico"
BSP="$PICO_DIR/vendor/mtk3_bsp"
CORE_DIR="$ROOT/core"
SX126X_DIR="$ROOT/core/vendor/sx126x_driver/src"
BUILD="$PICO_DIR/build"

CC=arm-none-eabi-gcc
LD=arm-none-eabi-gcc

CFLAGS=(-std=gnu11 -Wall -mcpu=cortex-m0plus -mthumb -ffreestanding -fno-builtin
  -D_PICO_RP2040_ -DTHIS_BOARD_ID=1 -Os -g
  -I"$CORE_DIR" -I"$PICO_DIR" -I"$SX126X_DIR"
  -I"$BSP/config" -I"$BSP/include" -I"$BSP/kernel/knlinc")

mkdir -p "$BUILD/obj"

APP_SOURCES=()
while IFS= read -r -d '' f; do APP_SOURCES+=("$f"); done < <(find "$CORE_DIR" -maxdepth 1 -name '*.c' -print0)
APP_SOURCES+=(
  "$PICO_DIR/debug.c" "$PICO_DIR/snapshot.c" "$PICO_DIR/main_board.c"
  "$PICO_DIR/mission_link_task.c" "$PICO_DIR/syscalls.c"
  "$SX126X_DIR/sx126x.c" "$PICO_DIR/sx1262_hal.c"
)

# BSP sources: portable kernel/lib/device files plus every armv6m/
# rp2040/pico_rp2040-specific sysdepend variant, excluding every OTHER
# CPU/board target this same multi-target BSP repo also carries
# (rx231/rza2m/stm32l4/stm32h7/rx65n/armv7a/armv7m/rxv2/iote_*/nucleo_*/
# rsk_*/rtb_*) and excluding app_program/ (its app_main.c defines its
# own strong usermain(), which would conflict with board/Raspberry_Pi_
# Pico/main_board.c's own usermain() - our board file replaces it).
BSP_SOURCES=()
while IFS= read -r -d '' f; do BSP_SOURCES+=("$BSP/$f"); done < <(
  cd "$BSP" && find kernel lib device \( -name '*.c' -o -name '*.S' \) -print0 \
    | tr '\0' '\n' \
    | grep -v -E 'rx231|rza2m|stm32l4|stm32h7|rx65n|armv7a|armv7m|rxv2|iote_|nucleo_|rsk_|rtb_|app_program|no_device' \
    | tr '\n' '\0'
)

ALL_SOURCES=("${APP_SOURCES[@]}" "${BSP_SOURCES[@]}")

echo "=== Compiling ${#ALL_SOURCES[@]} source files ==="
OBJS=()
for f in "${ALL_SOURCES[@]}"; do
  base=$(basename "$f")
  base="${base%.*}"
  hash=$(echo "$f" | md5sum | cut -c1-8)
  obj="$BUILD/obj/${base}_${hash}.o"
  echo "  CC  $f"
  "$CC" "${CFLAGS[@]}" -c "$f" -o "$obj"
  OBJS+=("$obj")
done

echo "=== Linking ==="
LDSCRIPT="$BSP/etc/linker/pico_rp2040/tkernel_map.ld"
"$LD" -mcpu=cortex-m0plus -mthumb -nostartfiles -nostdlib \
  -T"$LDSCRIPT" -Wl,-Map="$BUILD/pico_rp2040.map" -Wl,--gc-sections \
  -o "$BUILD/pico_rp2040.elf" "${OBJS[@]}" \
  -Wl,--start-group -lc -lm -lgcc -Wl,--end-group

echo "=== Done: $BUILD/pico_rp2040.elf ==="
arm-none-eabi-size "$BUILD/pico_rp2040.elf"
