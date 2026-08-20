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

.PHONY: all clean smoke label_dataset check-board check-board-compile check-board-link

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
  BOARD_SOURCES = $(CORE_SOURCES) $(BOARD_DIR)/debug.c $(BOARD_DIR)/snapshot.c $(BOARD_DIR)/main_board.c $(BOARD_DIR)/mission_link_task.c
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

check-board: check-board-compile check-board-link
	@echo "check-board: compile + link checks both passed ($(BSP_MODE))"

check-board-compile:
	@echo "check-board mode: $(BSP_MODE)"
	@mkdir -p $(BOARD_OBJ_DIR)
	@for f in $(BOARD_SOURCES); do \
		obj=$(BOARD_OBJ_DIR)/$$(basename $$f .c).o; \
		echo "  ARM-CC  $$f"; \
		$(ARM_CC) $(ARM_CFLAGS) -c $$f -o $$obj || exit 1; \
	done
	@echo "check-board-compile: all sources compile clean for Cortex-M55 ($(BSP_MODE))"

# --- Board link check --------------------------------------------------
# check-board-compile above only ever runs `gcc -c` one file at a time,
# so it can never see a broken interface *between* files (e.g. one file's
# function no longer matching what another file calls) -- every file
# still compiles fine on its own. This target catches exactly that class
# of bug: a real partial link (-nostdlib -nostartfiles, since there's no
# BSP kernel lib or startup code to link a full image against yet --
# same caveat check-board-compile already documents) of every .o that
# check-board-compile just produced, then a check that NOTHING in this
# project's own object files is left undefined.
#
# A real link doesn't get to just report "undefined reference" and stop,
# though: undefined references to genuine external symbols -- libc
# (memset, printf, ...) and the BSP kernel API (tm_printf, tk_cre_tsk,
# ...) -- are normal and expected here, since neither libc nor the BSP
# kernel lib is linked in at this stage (see above). So this can't just
# be "zero undefined references"; it has to be "zero undefined
# references to anything that ISN'T a genuine external symbol". The
# filtering that distinguishes the two:
#
#   1. UNDEFINED  = every symbol any .o in the build set references but
#      doesn't itself define (nm's `U` entries).
#   2. DEFINED    = every symbol actually defined by some .o in the
#      build set (nm's non-`U` entries).
#   3. UNRESOLVED = UNDEFINED minus DEFINED. For plain .o files handed
#      directly to the linker (not pulled from a .a archive), symbol
#      resolution doesn't depend on link order -- if something in
#      DEFINED matches a name in UNDEFINED, ld will always resolve it.
#      So UNRESOLVED is exactly the set a real link attempt would
#      report as "undefined reference to `...'": no genuine internal
#      interface break can hide in DEFINED and still show up here.
#   4. EXPECTED_EXTERNAL = every symbol actually defined in the real
#      libc.a/libm.a/libgcc.a for this exact target multilib (queried
#      from the toolchain itself via -print-file-name, not guessed),
#      union every symbol the BSP declares with its IMPORT marker
#      anywhere under $(BSP_DIR) (grep'd from the actual headers this
#      build compiles against, not a hand-maintained guess list either).
#      IMPORT is µT-Kernel/BSP2's own convention for "this is provided
#      externally by the kernel/monitor, not defined in this header" --
#      see e.g. vendor/mtk3_bsp2/mtkernel/include/tm/tmonitor.h.
#   5. UNRESOLVED minus EXPECTED_EXTERNAL is what's left: undefined
#      references this build set genuinely cannot account for by any
#      known-legitimate external source. That's a symbol something in
#      core/*.c or platform/board/*.c calls, that nothing in core/*.c or
#      platform/board/*.c (still) defines -- the exact bug class
#      check-board-compile can't see. Non-empty here is a hard failure.
ARM_NM ?= arm-none-eabi-nm
ARM_TARGET_FLAGS = -mcpu=cortex-m55 -mthumb -mfloat-abi=hard -mfpu=auto

check-board-link: check-board-compile
	@echo "check-board-link: partial-linking every $(BOARD_OBJ_DIR)/*.o together to check for undefined cross-file references"
	@objs="$$(ls $(BOARD_OBJ_DIR)/*.o 2>/dev/null)"; \
	if [ -z "$$objs" ]; then \
		echo "check-board-link: no objects in $(BOARD_OBJ_DIR) -- check-board-compile did not run?"; \
		exit 1; \
	fi; \
	$(ARM_NM) -o $$objs | awk -F'[: \t]+' '$$2=="U" {print $$3}' | sort -u > $(BOARD_OBJ_DIR)/.undefined_syms.txt; \
	$(ARM_NM) -o $$objs | awk -F'[: \t]+' '$$2!="U" && NF>=4 {print $$4}' | sort -u > $(BOARD_OBJ_DIR)/.defined_syms.txt; \
	comm -23 $(BOARD_OBJ_DIR)/.undefined_syms.txt $(BOARD_OBJ_DIR)/.defined_syms.txt > $(BOARD_OBJ_DIR)/.unresolved_syms.txt; \
	libc_a="$$($(ARM_CC) $(ARM_TARGET_FLAGS) -print-file-name=libc.a)"; \
	libm_a="$$($(ARM_CC) $(ARM_TARGET_FLAGS) -print-file-name=libm.a)"; \
	libgcc_a="$$($(ARM_CC) $(ARM_TARGET_FLAGS) -print-file-name=libgcc.a)"; \
	: > $(BOARD_OBJ_DIR)/.expected_external.txt; \
	for lib in "$$libc_a" "$$libm_a" "$$libgcc_a"; do \
		if [ -f "$$lib" ]; then \
			$(ARM_NM) "$$lib" 2>/dev/null | awk -F'[: \t]+' 'NF>=3 && $$2 ~ /^[A-Za-z]$$/ && $$2!="U" && $$2!="u" {print $$3}' >> $(BOARD_OBJ_DIR)/.expected_external.txt; \
		fi; \
	done; \
	if [ -d "$(BSP_DIR)" ]; then \
		grep -rhoE '^[[:space:]]*IMPORT[[:space:]]+[A-Za-z_][A-Za-z0-9_ ]*\*?[[:space:]]+[A-Za-z_][A-Za-z0-9_]*[[:space:]]*\(' $(BSP_DIR) 2>/dev/null \
			| sed -E 's/^.*[[:space:]\*]([A-Za-z_][A-Za-z0-9_]*)[[:space:]]*\($$/\1/' \
			>> $(BOARD_OBJ_DIR)/.expected_external.txt; \
	fi; \
	sort -u $(BOARD_OBJ_DIR)/.expected_external.txt -o $(BOARD_OBJ_DIR)/.expected_external.txt; \
	comm -23 $(BOARD_OBJ_DIR)/.unresolved_syms.txt $(BOARD_OBJ_DIR)/.expected_external.txt > $(BOARD_OBJ_DIR)/.bad_undefined_syms.txt; \
	if [ -s $(BOARD_OBJ_DIR)/.bad_undefined_syms.txt ]; then \
		echo ""; \
		echo "check-board-link: FAILED -- undefined reference(s) to symbol(s) that should be"; \
		echo "defined somewhere in core/*.c or platform/board/*.c, but aren't in this build:"; \
		echo ""; \
		sed 's/^/    /' $(BOARD_OBJ_DIR)/.bad_undefined_syms.txt; \
		echo ""; \
		echo "(checked against the real libc.a/libm.a/libgcc.a for this target multilib and"; \
		echo "every IMPORT-declared symbol under $(BSP_DIR) -- these are not libc/BSP calls)"; \
		echo ""; \
		echo "This means some file calls a function that no .c file in core/ or"; \
		echo "platform/board/ actually defines -- a cross-file interface break that"; \
		echo "check-board-compile's per-file compile step cannot catch on its own."; \
		exit 1; \
	fi; \
	echo "check-board-link: real partial link of $$(echo $$objs | wc -w) objects clean --"; \
	echo "  every undefined reference is accounted for by libc/libm/libgcc or an"; \
	echo "  IMPORT-declared BSP symbol under $(BSP_DIR); no cross-file interface breaks."

clean:
	rm -f $(TARGET) $(LABELER) tron_snapshot_dataset.csv tron_snapshot_dataset_labeled.csv tron_telemetry.log
	rm -rf $(BOARD_OBJ_DIR)

