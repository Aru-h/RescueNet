CC ?= gcc
CFLAGS ?= -std=gnu11 -Wall -Wextra -Wpedantic
LDFLAGS ?= -lm

# --- Layout ---------------------------------------------------------
# core/     portable simulator: no stdio/OS/filesystem dependencies.
#           Verified freestanding-clean for Cortex-M55 (see check-board).
# platform/laptop/  desktop entry point, CLI parsing, CSV snapshot
#           writer, printf-based debug sink, --live SSE pacing.
# platform/board/   STM32N6570-DK entry point + debug/snapshot backends.
#           Stubs pending BSP (contest-provided, not the public
#           mtk3_bsp repo) — see TODO(bsp)/TODO(usb) markers.
# tools/    laptop-only offline tooling: snapshot labeler, Flask
#           dashboard, dataset comparison scripts. Never built for
#           the board.

CORE_DIR = core
LAPTOP_DIR = platform/laptop
BOARD_DIR = platform/board
TOOLS_DIR = tools

CORE_SOURCES = $(wildcard $(CORE_DIR)/*.c)
LAPTOP_SOURCES = $(CORE_SOURCES) \
	$(LAPTOP_DIR)/main.c \
	$(LAPTOP_DIR)/debug.c \
	$(LAPTOP_DIR)/snapshot.c

LAPTOP_INCLUDES = -I$(CORE_DIR) -I$(LAPTOP_DIR)

LABELER = label_snapshots
LABELER_SOURCES = $(TOOLS_DIR)/label_snapshots.c

TARGET = tron

.PHONY: all clean smoke label_dataset check-board

all: $(TARGET) $(LABELER)

$(TARGET): $(LAPTOP_SOURCES)
	$(CC) $(CFLAGS) $(LAPTOP_INCLUDES) -o $(TARGET) $(LAPTOP_SOURCES) $(LDFLAGS)

$(LABELER): $(LABELER_SOURCES)
	$(CC) $(CFLAGS) -I$(CORE_DIR) -o $(LABELER) $(LABELER_SOURCES) $(LDFLAGS)

smoke: $(TARGET)
	./$(TARGET) 1 1 200

label_dataset: $(LABELER)
	./$(LABELER) "$(INPUT)" "$(OUTPUT)" "$(N)"

INPUT ?= tron_snapshot_dataset.csv
OUTPUT ?= tron_snapshot_dataset_labeled.csv
N ?= 5

# --- Board compile check ---------------------------------------------
# Compiles core/ + platform/board/ for real, against the actual
# µT-Kernel 3.0 BSP2 headers (github.com/tron-forum/mtk3_bsp2) when
# vendor/mtk3_bsp2 is present (see vendor/README.md to fetch it) --
# this is a genuine header-accurate check, not a guess: it validates
# real tk/tkernel.h and tm/tmonitor.h symbols (T_CTSK, tk_cre_tsk,
# tm_printf, etc), not just freestanding-C-with-no-headers.
#
# Falls back to a plain freestanding compile (no BSP headers) if
# vendor/mtk3_bsp2 isn't present, which still catches accidental
# libc/OS dependency creep in core/ but can't validate platform/board/
# against real BSP symbols.
#
# This is NOT a real firmware build (no linker script, no HAL init,
# no STM32CubeIDE project) -- see vendor/README.md for what's still
# needed to get a flashable image. Run this after every change to
# core/ or platform/board/.
ARM_CC ?= arm-none-eabi-gcc
BSP_DIR ?= vendor/mtk3_bsp2
ARM_CFLAGS_BASE = -std=gnu11 -Wall -Wextra -mcpu=cortex-m55 -mthumb \
	-mfloat-abi=hard -mfpu=auto -ffreestanding -fno-builtin \
	-I$(CORE_DIR) -I$(BOARD_DIR)
BOARD_OBJ_DIR = build/board-check

ifneq ($(wildcard $(BSP_DIR)/include),)
  ARM_CFLAGS = $(ARM_CFLAGS_BASE) -D_STM32CUBE_DISCOVERY_N657_ \
	-I$(BSP_DIR)/config -I$(BSP_DIR)/include -I$(BSP_DIR)
  BOARD_SOURCES = $(CORE_SOURCES) $(BOARD_DIR)/debug.c $(BOARD_DIR)/snapshot.c $(BOARD_DIR)/main_board.c
  BSP_MODE = real BSP2 headers ($(BSP_DIR))
else
  ARM_CFLAGS = $(ARM_CFLAGS_BASE)
  # platform/board/debug.c and main_board.c call real BSP APIs
  # (tm_printf, tk_cre_tsk, ...) now, so they genuinely can't compile
  # without vendor/mtk3_bsp2 -- that's correct, not a check-board bug.
  # Fallback mode checks core/ only: still a real, useful check (catches
  # libc/OS dependency creep in the portable simulator), just weaker
  # than the real-BSP mode above.
  BOARD_SOURCES = $(CORE_SOURCES)
  BSP_MODE = plain freestanding, core/ only (vendor/mtk3_bsp2 not found -- see vendor/README.md; platform/board/*.c needs real BSP headers so it's skipped in this mode)
endif

check-board:
	@echo "check-board mode: $(BSP_MODE)"
	@mkdir -p $(BOARD_OBJ_DIR)
	@for f in $(BOARD_SOURCES); do \
		obj=$(BOARD_OBJ_DIR)/$$(basename $$f .c).o; \
		echo "  ARM-CC  $$f"; \
		$(ARM_CC) $(ARM_CFLAGS) -c $$f -o $$obj || exit 1; \
	done
	@echo "check-board: all sources compile clean for Cortex-M55 ($(BSP_MODE))"

clean:
	rm -f $(TARGET) $(LABELER) tron_snapshot_dataset.csv tron_snapshot_dataset_labeled.csv tron_telemetry.log
	rm -rf $(BOARD_OBJ_DIR)
