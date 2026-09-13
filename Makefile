CC ?= gcc
CFLAGS ?= -std=gnu11 -Wall -Wextra -Wpedantic
# Laptop/desktop build simulates every team in one process - override
# the board-split divisor so all NUM_TEAMS resolve to board 0 and
# comms_transport takes the same-board path uniformly. Real per-board
# firmware targets (ARM_CFLAGS below) do NOT get this override and
# keep comms_transport.c's default of 5.
CFLAGS += -DCOMMS_TRANSPORT_TEAMS_PER_BOARD=NUM_TEAMS
LDFLAGS ?= -lm

# --- Layout ---------------------------------------------------------
# core/                    portable simulator: no stdio/OS/filesystem
#                          dependencies. Verified freestanding-clean
#                          for Cortex-M55/Cortex-M0+ (see check-board/
#                          check-pico).
# core/vendor/             vendored (redistribution-licensed, unlike
#                          either BSP) SHARED dependencies - currently
#                          just sx126x_driver, the SX1262 radio driver
#                          core. Excluded from CORE_SOURCES's wildcard
#                          (subdirectory, doesn't match core/*.c), so
#                          the laptop build never links it.
# laptop/                  desktop entry point, CLI parsing, CSV
#                          snapshot writer, printf-based debug sink,
#                          --live SSE pacing.
# laptop/tools/            laptop-only offline tooling: snapshot
#                          labeler, Flask dashboard, dataset comparison
#                          scripts, board-side selftests. Never built
#                          for either board target.
# board/STM32N6570-DK/     STM32N6570-DK entry point + debug/snapshot
#                          backends, plus its own vendor/mtk3_bsp2 BSP
#                          (see that folder's own vendor/README.md).
# board/Raspberry_Pi_Pico/ Raspberry Pi Pico entry point + debug/
#                          snapshot backends, plus its own
#                          vendor/mtk3_bsp (v1 — NOT the DK's BSP2)
#                          BSP (see that folder's own vendor/README.md).

CORE_DIR = core
LAPTOP_DIR = laptop
BOARD_DIR = board/STM32N6570-DK
TOOLS_DIR = laptop/tools

CORE_SOURCES = $(wildcard $(CORE_DIR)/*.c)
LAPTOP_SOURCES = $(CORE_SOURCES) \
	$(LAPTOP_DIR)/main.c \
	$(LAPTOP_DIR)/debug.c \
	$(LAPTOP_DIR)/snapshot.c

LAPTOP_INCLUDES = -I$(CORE_DIR) -I$(LAPTOP_DIR)

LABELER = label_snapshots
LABELER_SOURCES = $(TOOLS_DIR)/label_snapshots.c

TARGET = tron

.PHONY: all clean smoke label_dataset check-board check-board-compile check-board-link check-pico check-pico-compile check-pico-link check-pico-bsp check-sx126x-driver check-two-board-harness check-two-board-cbba-harness check-board-ids-distinct check-evidence-vector-parity check-fault-comms-receive check-fault-comms-broadcast-pacing check-fault-comms-abort-gate check-recovery-predictor-stress check-realloc-gate check-environment-e2e-stress check-consensus-weighting

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
# Compiles core/ + board/STM32N6570-DK/ for real, against the actual
# µT-Kernel 3.0 BSP2 headers (github.com/tron-forum/mtk3_bsp2) when
# board/STM32N6570-DK/vendor/mtk3_bsp2 is present (see that folder's
# README.md to fetch it) -- this is a genuine header-accurate check,
# not a guess: it validates real tk/tkernel.h and tm/tmonitor.h symbols
# (T_CTSK, tk_cre_tsk, tm_printf, etc), not just freestanding-C-with-
# no-headers.
#
# Falls back to a plain freestanding compile (no BSP headers) if
# board/STM32N6570-DK/vendor/mtk3_bsp2 isn't present, which still
# catches accidental libc/OS dependency creep in core/ but can't
# validate board/STM32N6570-DK/ against real BSP symbols.
#
# This is NOT a real firmware build (no linker script, no HAL init,
# no STM32CubeIDE project) -- see board/STM32N6570-DK/vendor/README.md
# for what's still needed to get a flashable image. Run this after
# every change to core/ or board/STM32N6570-DK/.
#
# BSP_DIR's default moved here (from the old top-level vendor/) as
# part of splitting each physical board into its own self-contained
# board/<name>/ folder -- see board/STM32N6570-DK/vendor/README.md's
# own note on why. A Pico/RP2040 target, when it exists, gets its own
# board/Raspberry_Pi_Pico/vendor/mtk3_bsp (v1, NOT this v2 BSP -- see
# the original handoff doc's BSP/toolchain section) and its own build
# rule entirely; it does NOT reuse BSP_DIR/ARM_CFLAGS below, which are
# STM32N6570-DK/Cortex-M55-specific (-mcpu=cortex-m55,
# -D_STM32CUBE_DISCOVERY_N657_).
ARM_CC ?= arm-none-eabi-gcc
BSP_DIR ?= board/STM32N6570-DK/vendor/mtk3_bsp2
ARM_CFLAGS_BASE = -std=gnu11 -Wall -Wextra -mcpu=cortex-m55 -mthumb \
	-mfloat-abi=hard -mfpu=auto -ffreestanding -fno-builtin \
	-I$(CORE_DIR) -I$(BOARD_DIR)
BOARD_OBJ_DIR = build/board-check

ifneq ($(wildcard $(BSP_DIR)/include),)
  # -I$(SX126X_DIR): sx1262_hal.c #includes "sx126x.h"/"sx126x_hal.h", which
  # live in core/vendor/sx126x_driver/src, not board/STM32N6570-DK/.
  # -Itron_board/Drivers/CMSIS/...: sx1262_hal.c also #includes the real
  # "stm32n657xx.h" (register defs) which pulls in "core_cm55.h" and
  # "system_stm32n6xx.h" - these ship as part of the checked-in
  # STM32CubeIDE project (tron_board/), not board/STM32N6570-DK/vendor/
  # mtk3_bsp2/, so they need their own -I here even in real-BSP mode.
  ARM_CFLAGS = $(ARM_CFLAGS_BASE) -D_STM32CUBE_DISCOVERY_N657_ -DTHIS_BOARD_ID=0 \
	-I$(BSP_DIR)/config -I$(BSP_DIR)/include -I$(BSP_DIR) \
	-I$(SX126X_DIR) \
	-Itron_board/Drivers/CMSIS/Include -Itron_board/Drivers/CMSIS/Device/ST/STM32N6xx/Include
  # sx126x.c (vendored driver core) + sx1262_hal.c (this board's HAL,
  # implementing sx126x_hal_write/_read/_reset/_wakeup) now fold into
  # the real per-board build, so check-board-link genuinely link-checks
  # the HAL instead of leaving those four symbols undefined.
  BOARD_SOURCES = $(CORE_SOURCES) $(BOARD_DIR)/dk_debug.c $(BOARD_DIR)/snapshot.c $(BOARD_DIR)/main_board.c $(BOARD_DIR)/mission_link_task.c $(SX126X_DIR)/sx126x.c $(BOARD_DIR)/sx1262_hal.c
  BSP_MODE = real BSP2 headers ($(BSP_DIR))
else
  ARM_CFLAGS = $(ARM_CFLAGS_BASE)
  # board/STM32N6570-DK/debug.c and main_board.c call real BSP APIs
  # (tm_printf, tk_cre_tsk, ...) now, so they genuinely can't compile
  # without board/STM32N6570-DK/vendor/mtk3_bsp2 -- that's correct,
  # not a check-board bug. Fallback mode checks core/ only: still a
  # real, useful check (catches libc/OS dependency creep in the
  # portable simulator), just weaker than the real-BSP mode above.
  BOARD_SOURCES = $(CORE_SOURCES)
  BSP_MODE = plain freestanding, core/ only (board/STM32N6570-DK/vendor/mtk3_bsp2 not found -- see that folder's README.md; board/STM32N6570-DK/*.c needs real BSP headers so it's skipped in this mode)
endif

check-board: check-board-ids-distinct check-board-compile check-board-link
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
#      see e.g. board/STM32N6570-DK/vendor/mtk3_bsp2/mtkernel/include/tm/tmonitor.h.
#   5. UNRESOLVED minus EXPECTED_EXTERNAL is what's left: undefined
#      references this build set genuinely cannot account for by any
#      known-legitimate external source. That's a symbol something in
#      core/*.c or board/STM32N6570-DK/*.c calls, that nothing in core/*.c or
#      board/STM32N6570-DK/*.c (still) defines -- the exact bug class
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
		echo "defined somewhere in core/*.c or board/STM32N6570-DK/*.c, but aren't in this build:"; \
		echo ""; \
		sed 's/^/    /' $(BOARD_OBJ_DIR)/.bad_undefined_syms.txt; \
		echo ""; \
		echo "(checked against the real libc.a/libm.a/libgcc.a for this target multilib and"; \
		echo "every IMPORT-declared symbol under $(BSP_DIR) -- these are not libc/BSP calls)"; \
		echo ""; \
		echo "This means some file calls a function that no .c file in core/ or"; \
		echo "board/STM32N6570-DK/ actually defines -- a cross-file interface break that"; \
		echo "check-board-compile's per-file compile step cannot catch on its own."; \
		exit 1; \
	fi; \
	echo "check-board-link: real partial link of $$(echo $$objs | wc -w) objects clean --"; \
	echo "  every undefined reference is accounted for by libc/libm/libgcc or an"; \
	echo "  IMPORT-declared BSP symbol under $(BSP_DIR); no cross-file interface breaks."

clean:
	rm -f $(TARGET) $(LABELER) tron_snapshot_dataset.csv tron_snapshot_dataset_labeled.csv tron_telemetry.log
	rm -rf $(BOARD_OBJ_DIR) $(PICO_BSP_OBJ_DIR) $(PICO_BOARD_OBJ_DIR) $(SX126X_OBJ_DIR)
	rm -f $(TWO_BOARD_HARNESS_BIN) $(EVIDENCE_VECTOR_PARITY_SELFTEST_BIN) \
		$(FAULT_COMMS_RECEIVE_SELFTEST_BIN) $(FAULT_COMMS_BROADCAST_PACING_TEST_BIN) \
		$(FAULT_COMMS_ABORT_GATE_TEST_BIN) $(RECOVERY_PREDICTOR_STRESS_TEST_BIN) $(REALLOC_GATE_TEST_BIN) $(CONSENSUS_WEIGHTING_SELFTEST_BIN)

# --- Pico compile check --------------------------------------------------
# Mirrors check-board-compile/-link above, same class of check now that
# real project sources exist (board/Raspberry_Pi_Pico/{main_board,
# mission_link_task,debug,snapshot}.c, ported from board/STM32N6570-DK/'s
# equivalents) -- not just the BSP-only smoke check this used to be
# (check-pico-bsp, kept below as its own smaller target: still useful on
# its own to isolate "is the vendored BSP itself broken" from "did OUR
# port break something", since it only ever compiles the BSP's own
# sample, never our files).
#
# -mcpu=cortex-m0plus, NOT cortex-m55: RP2040 is dual Cortex-M0+, a
# different core family from the DK's Cortex-M55, not just a different
# chip in the same family -- see board/Raspberry_Pi_Pico/vendor/README.md.
# -D_PICO_RP2040_ is the one BSP-select flag (see that same README for
# why this, not a hand-set TARGET_DIR, is the real mechanism).
PICO_DIR = board/Raspberry_Pi_Pico
PICO_BSP_DIR ?= $(PICO_DIR)/vendor/mtk3_bsp
PICO_ARM_CC ?= arm-none-eabi-gcc
PICO_ARM_CFLAGS_BASE = -std=gnu11 -Wall -Wextra -mcpu=cortex-m0plus -mthumb \
	-ffreestanding -fno-builtin -D_PICO_RP2040_ -DTHIS_BOARD_ID=1 \
	-I$(CORE_DIR) -I$(PICO_DIR)
PICO_BSP_OBJ_DIR = build/pico-bsp-check
PICO_BOARD_OBJ_DIR = build/pico-check

ifneq ($(wildcard $(PICO_BSP_DIR)/include),)
  # -I$(SX126X_DIR): sx1262_hal.c #includes "sx126x.h"/"sx126x_hal.h", which
  # live in core/vendor/sx126x_driver/src, not board/Raspberry_Pi_Pico/.
  PICO_ARM_CFLAGS = $(PICO_ARM_CFLAGS_BASE) \
	-I$(PICO_BSP_DIR)/config -I$(PICO_BSP_DIR)/include -I$(PICO_BSP_DIR)/kernel/knlinc \
	-I$(SX126X_DIR)
  # sx126x.c (vendored driver core) + sx1262_hal.c (this board's HAL,
  # raw RP2040-register SPI0 + this BSP's own libbsp gpio_*() wrapper)
  # now fold into the real per-board build, so check-pico-link genuinely
  # link-checks the HAL instead of leaving those four symbols undefined.
  PICO_BOARD_SOURCES = $(CORE_SOURCES) $(PICO_DIR)/debug.c $(PICO_DIR)/snapshot.c $(PICO_DIR)/main_board.c $(PICO_DIR)/mission_link_task.c $(SX126X_DIR)/sx126x.c $(PICO_DIR)/sx1262_hal.c
  PICO_BSP_MODE = real BSP (v1) headers ($(PICO_BSP_DIR))
else
  PICO_ARM_CFLAGS = $(PICO_ARM_CFLAGS_BASE)
  # Same reasoning as check-board's fallback mode: board/Raspberry_Pi_Pico/
  # debug.c and main_board.c call real BSP APIs (tm_printf, tk_cre_tsk,
  # ...), so they genuinely can't compile without the BSP present -- not
  # a check-pico bug. Fallback checks core/ only.
  PICO_BOARD_SOURCES = $(CORE_SOURCES)
  PICO_BSP_MODE = plain freestanding, core/ only ($(PICO_BSP_DIR) not found -- see board/Raspberry_Pi_Pico/vendor/README.md; $(PICO_DIR)/*.c needs real BSP headers so it's skipped in this mode)
endif

check-pico: check-board-ids-distinct check-pico-compile check-pico-link
	@echo "check-pico: compile + link checks both passed ($(PICO_BSP_MODE))"

check-pico-compile:
	@echo "check-pico mode: $(PICO_BSP_MODE)"
	@mkdir -p $(PICO_BOARD_OBJ_DIR)
	@for f in $(PICO_BOARD_SOURCES); do \
		obj=$(PICO_BOARD_OBJ_DIR)/$$(basename $$f .c).o; \
		echo "  ARM-CC  $$f"; \
		$(PICO_ARM_CC) $(PICO_ARM_CFLAGS) -c $$f -o $$obj || exit 1; \
	done
	@echo "check-pico-compile: all sources compile clean for Cortex-M0+ ($(PICO_BSP_MODE))"

# Same undefined-cross-reference check as check-board-link -- see that
# target's own long comment above for the full 5-step reasoning; not
# repeated here since it's identical, only the target CPU/BSP differ.
PICO_ARM_TARGET_FLAGS = -mcpu=cortex-m0plus -mthumb

check-pico-link: check-pico-compile
	@echo "check-pico-link: partial-linking every $(PICO_BOARD_OBJ_DIR)/*.o together to check for undefined cross-file references"
	@objs="$$(ls $(PICO_BOARD_OBJ_DIR)/*.o 2>/dev/null)"; \
	if [ -z "$$objs" ]; then \
		echo "check-pico-link: no objects in $(PICO_BOARD_OBJ_DIR) -- check-pico-compile did not run?"; \
		exit 1; \
	fi; \
	$(ARM_NM) -o $$objs | awk -F'[: \t]+' '$$2=="U" {print $$3}' | sort -u > $(PICO_BOARD_OBJ_DIR)/.undefined_syms.txt; \
	$(ARM_NM) -o $$objs | awk -F'[: \t]+' '$$2!="U" && NF>=4 {print $$4}' | sort -u > $(PICO_BOARD_OBJ_DIR)/.defined_syms.txt; \
	comm -23 $(PICO_BOARD_OBJ_DIR)/.undefined_syms.txt $(PICO_BOARD_OBJ_DIR)/.defined_syms.txt > $(PICO_BOARD_OBJ_DIR)/.unresolved_syms.txt; \
	libc_a="$$($(PICO_ARM_CC) $(PICO_ARM_TARGET_FLAGS) -print-file-name=libc.a)"; \
	libm_a="$$($(PICO_ARM_CC) $(PICO_ARM_TARGET_FLAGS) -print-file-name=libm.a)"; \
	libgcc_a="$$($(PICO_ARM_CC) $(PICO_ARM_TARGET_FLAGS) -print-file-name=libgcc.a)"; \
	: > $(PICO_BOARD_OBJ_DIR)/.expected_external.txt; \
	for lib in "$$libc_a" "$$libm_a" "$$libgcc_a"; do \
		if [ -f "$$lib" ]; then \
			$(ARM_NM) "$$lib" 2>/dev/null | awk -F'[: \t]+' 'NF>=3 && $$2 ~ /^[A-Za-z]$$/ && $$2!="U" && $$2!="u" {print $$3}' >> $(PICO_BOARD_OBJ_DIR)/.expected_external.txt; \
		fi; \
	done; \
	if [ -d "$(PICO_BSP_DIR)" ]; then \
		grep -rhoE '^[[:space:]]*IMPORT[[:space:]]+[A-Za-z_][A-Za-z0-9_ ]*\*?[[:space:]]+[A-Za-z_][A-Za-z0-9_]*[[:space:]]*\(' $(PICO_BSP_DIR) 2>/dev/null \
			| sed -E 's/^.*[[:space:]\*]([A-Za-z_][A-Za-z0-9_]*)[[:space:]]*\($$/\1/' \
			>> $(PICO_BOARD_OBJ_DIR)/.expected_external.txt; \
		grep -rhoE '^[A-Za-z_][A-Za-z0-9_]*[[:space:]]+[A-Za-z_][A-Za-z0-9_]*\(' $(PICO_BSP_DIR)/include/bsp 2>/dev/null \
			| sed -E 's/^[A-Za-z_][A-Za-z0-9_]*[[:space:]]+([A-Za-z_][A-Za-z0-9_]*)\($$/\1/' \
			>> $(PICO_BOARD_OBJ_DIR)/.expected_external.txt; \
	fi; \
	sort -u $(PICO_BOARD_OBJ_DIR)/.expected_external.txt -o $(PICO_BOARD_OBJ_DIR)/.expected_external.txt; \
	comm -23 $(PICO_BOARD_OBJ_DIR)/.unresolved_syms.txt $(PICO_BOARD_OBJ_DIR)/.expected_external.txt > $(PICO_BOARD_OBJ_DIR)/.bad_undefined_syms.txt; \
	if [ -s $(PICO_BOARD_OBJ_DIR)/.bad_undefined_syms.txt ]; then \
		echo ""; \
		echo "check-pico-link: FAILED -- undefined reference(s) to symbol(s) that should be"; \
		echo "defined somewhere in core/*.c or $(PICO_DIR)/*.c, but aren't in this build:"; \
		echo ""; \
		sed 's/^/    /' $(PICO_BOARD_OBJ_DIR)/.bad_undefined_syms.txt; \
		echo ""; \
		echo "(checked against the real libc.a/libm.a/libgcc.a for this target multilib and"; \
		echo "every IMPORT-declared symbol under $(PICO_BSP_DIR) -- these are not libc/BSP calls)"; \
		echo ""; \
		echo "This means some file calls a function that no .c file in core/ or"; \
		echo "$(PICO_DIR)/ actually defines -- a cross-file interface break that"; \
		echo "check-pico-compile's per-file compile step cannot catch on its own."; \
		exit 1; \
	fi; \
	echo "check-pico-link: real partial link of $$(echo $$objs | wc -w) objects clean --"; \
	echo "  every undefined reference is accounted for by libc/libm/libgcc or an"; \
	echo "  IMPORT-declared BSP symbol under $(PICO_BSP_DIR); no cross-file interface breaks."

# --- Pico BSP smoke check ------------------------------------------------
# Smaller, narrower check kept alongside check-pico above: only ever
# compiles the vendored BSP's OWN bundled sample (app_program/app_main.c),
# never this project's files -- useful specifically to isolate "the
# vendored BSP itself is broken" from "our port broke something", which
# check-pico-compile alone can't distinguish (a BSP header problem and a
# project-file problem both show up as "check-pico-compile failed").
check-pico-bsp:
	@if [ ! -d "$(PICO_BSP_DIR)/include" ]; then \
		echo "check-pico-bsp: SKIPPED -- $(PICO_BSP_DIR) not found (see"; \
		echo "  board/Raspberry_Pi_Pico/vendor/README.md to fetch it)"; \
		exit 0; \
	fi
	@mkdir -p $(PICO_BSP_OBJ_DIR)
	@echo "  ARM-CC  $(PICO_BSP_DIR)/app_program/app_main.c"
	@$(PICO_ARM_CC) -std=gnu11 -Wall -Wextra -mcpu=cortex-m0plus -mthumb \
		-ffreestanding -fno-builtin -D_PICO_RP2040_ \
		-I$(PICO_BSP_DIR)/config -I$(PICO_BSP_DIR)/include -I$(PICO_BSP_DIR)/kernel/knlinc \
		-c $(PICO_BSP_DIR)/app_program/app_main.c -o $(PICO_BSP_OBJ_DIR)/app_main.o
	@echo "check-pico-bsp: vendored mtk3_bsp (pico_rp2040) compiles clean for Cortex-M0+"

# --- SX126X driver smoke check --------------------------------------------
# Compiles core/vendor/sx126x_driver/src/sx126x.c freestanding for BOTH
# real target CPUs -- unlike check-board/check-pico, this driver core
# doesn't need either board's BSP headers at all (it's genuinely
# standalone, calling out to sx126x_hal.h's four functions, which have
# no implementation anywhere yet -- see core/vendor/sx126x_driver/
# README.md's "What's still genuinely undecided" section). This only
# proves the vendored driver core itself compiles clean for both CPUs.
# It is now ALSO wired into check-board-compile/check-pico-compile's
# BOARD_SOURCES/PICO_BOARD_SOURCES (real per-board HALs exist now --
# board/STM32N6570-DK/sx1262_hal.c, board/Raspberry_Pi_Pico/sx1262_hal.c
# -- so check-board-link/check-pico-link genuinely link-check them
# instead of failing on "undefined reference to sx126x_hal_write" etc).
# This target stays anyway: it's a narrower, faster sanity check that
# isolates "is the vendored driver core itself sound" from "did one
# board's HAL/BSP integration break", the same way check-pico-bsp is
# kept alongside check-pico for an analogous reason.
SX126X_DIR = core/vendor/sx126x_driver/src
SX126X_OBJ_DIR = build/sx126x-driver-check

check-sx126x-driver:
	@if [ ! -f "$(SX126X_DIR)/sx126x.c" ]; then \
		echo "check-sx126x-driver: SKIPPED -- $(SX126X_DIR) not found"; \
		exit 0; \
	fi
	@mkdir -p $(SX126X_OBJ_DIR)
	@echo "  ARM-CC  $(SX126X_DIR)/sx126x.c (Cortex-M55, DK)"
	@$(ARM_CC) -std=gnu11 -Wall -Wextra -mcpu=cortex-m55 -mthumb -mfloat-abi=hard -mfpu=auto \
		-ffreestanding -fno-builtin -c $(SX126X_DIR)/sx126x.c -o $(SX126X_OBJ_DIR)/sx126x_m55.o
	@echo "  ARM-CC  $(SX126X_DIR)/sx126x.c (Cortex-M0+, Pico)"
	@$(PICO_ARM_CC) -std=gnu11 -Wall -Wextra -mcpu=cortex-m0plus -mthumb \
		-ffreestanding -fno-builtin -c $(SX126X_DIR)/sx126x.c -o $(SX126X_OBJ_DIR)/sx126x_m0.o
	@echo "check-sx126x-driver: vendored sx126x_driver core compiles clean for both Cortex-M55 and Cortex-M0+"

# --- Two-board mission-link harness check ---------------------------------
# Builds and runs laptop/tools/two_board_harness.c for real: two forked OS
# processes, a real Unix socket standing in for the LoRa link, real
# unmodified mission_link_task.c/comms_transport.c/mission_link_handshake.c
# proving a handshake + roster relay actually completes across processes.
# Mirrors check-sx126x-driver's shape (build + run, non-zero exit on
# failure) per that file's own history -- see CHANGELOG.md and this
# file's own top comment for the debugging history behind why it's built
# the way it is. NOT plumbed through BOARD_SOURCES/ARM_CC -- this is a
# laptop-only, host-gcc harness (real board firmware is out of scope; see
# the file's own "Does NOT prove" section), so it uses $(CC), not $(ARM_CC).
TWO_BOARD_HARNESS_BIN = build/two_board_harness

# --- Board-id distinctness check --------------------------------------
# core/lora_crypto.h's nonce construction (lora_crypto_build_nonce())
# only guarantees two boards can never produce the same (key, nonce)
# pair if their THIS_BOARD_ID values actually differ - that guarantee
# is a per-target compile-time constant (-DTHIS_BOARD_ID=..., set once
# each in ARM_CFLAGS above for the DK and PICO_ARM_CFLAGS below for the
# Pico), and NOTHING checks the two against each other at build time -
# they're baked into two separate binaries by two separate recipes, so
# no _Static_assert can span them. A provisioning slip (someone edits
# one recipe's -DTHIS_BOARD_ID and not the other, or a future third
# board reuses an existing value) would silently reintroduce the exact
# nonce-collision vulnerability that field was added to close, with
# nothing in the crypto layer itself able to catch it (see
# core/lora_crypto.h's own doc on why this can't be caught mesh-side
# either, once Channel 2 exists). This target is the cheapest place
# such a mistake CAN be caught: both values are visible in this one
# Makefile at once, even though the boards they belong to never are.
check-board-ids-distinct:
	@dk_id=$$($(ARM_CC) -dM -E -x c $(ARM_CFLAGS) /dev/null 2>/dev/null | grep -w THIS_BOARD_ID | awk '{print $$3}' | tr -d 'u'); \
	pico_id=$$($(PICO_ARM_CC) -dM -E -x c $(PICO_ARM_CFLAGS) /dev/null 2>/dev/null | grep -w THIS_BOARD_ID | awk '{print $$3}' | tr -d 'u'); \
	if [ -z "$$dk_id" ] || [ -z "$$pico_id" ]; then \
		echo "check-board-ids-distinct: SKIPPED -- THIS_BOARD_ID not defined for one or both targets in this build mode."; \
		echo "  (fallback/core-only mode does not compile board/*.c at all, so"; \
		echo "  THIS_BOARD_ID is moot there -- see BSP_MODE/PICO_BSP_MODE above."; \
		exit 0; \
	fi; \
	if [ "$$dk_id" = "$$pico_id" ]; then \
		echo "check-board-ids-distinct: FAILED -- DK and Pico THIS_BOARD_ID both resolve to $$dk_id."; \
		echo "  core/lora_crypto.h's per-board nonce partition (sender_board_id"; \
		echo "  byte in lora_crypto_build_nonce()) only holds if every board's"; \
		echo "  THIS_BOARD_ID is unique -- see this target's own comment in"; \
		echo "  the Makefile. Fix Makefile's -DTHIS_BOARD_ID=... overrides"; \
		echo "  (or board/*/main_board.c's #ifndef THIS_BOARD_ID fallbacks)"; \
		echo "  so DK and Pico do not both resolve to the same value."; \
		exit 1; \
	fi; \
	echo "check-board-ids-distinct: DK=$$dk_id, Pico=$$pico_id -- distinct, nonce partition assumption holds"

check-two-board-harness:
	@mkdir -p build
	@echo "  CC      $(TOOLS_DIR)/two_board_harness.c (+ real mission-link/comms_transport sources)"
	@$(CC) -std=gnu11 -Wall -Wextra -I$(CORE_DIR) -I$(BOARD_DIR) -I$(LAPTOP_DIR) \
		-I$(TOOLS_DIR)/harness_stubs \
		-o $(TWO_BOARD_HARNESS_BIN) $(TOOLS_DIR)/two_board_harness.c \
		$(BOARD_DIR)/mission_link_task.c $(LAPTOP_DIR)/debug.c \
		$(LAPTOP_DIR)/snapshot.c $(CORE_SOURCES) -lm
	@./$(TWO_BOARD_HARNESS_BIN)
	@echo "check-two-board-harness: real two-process handshake + LoRa-relayed roster delivery passed"

# --- Two-board CBBA consensus harness check --------------------------------
# CBBA-side counterpart to check-two-board-harness above: builds and runs
# laptop/tools/two_board_cbba_harness.c, same two-forked-process/real-socket
# shape, proving a real cbba_run() bid round resolves to the same winning
# team/incident pairing on BOTH processes' own view of the world.
#
# This delegates to laptop/tools/check-two-board-cbba-harness.sh rather
# than reimplementing its build+loop here: that script already runs the
# harness 40x (well above the 20-run flakiness bar this project holds
# every two-board harness to -- see check-two-board-harness's own history
# for why a single passing run was never good enough), and it's the
# thing that already found and cleared that bar for this harness. This
# target is just the mechanical `make check-two-board-cbba-harness`
# wiring the script's own top comment already anticipated adding later.
check-two-board-cbba-harness:
	@chmod +x $(TOOLS_DIR)/check-two-board-cbba-harness.sh
	@$(TOOLS_DIR)/check-two-board-cbba-harness.sh

# --- Consensus weighting selftest -----------------------------------------
# core/consensus_weighting.c's own standalone selftest (laptop/tools/
# consensus_weighting_selftest.c) - see that file's own build comment for
# why its dependency list is short (just fault_buffer.c/fault_config.c/
# mission_state.c for shared constants, not the whole simulator).
CONSENSUS_WEIGHTING_SELFTEST_BIN = build/consensus_weighting_selftest

check-consensus-weighting:
	@mkdir -p build
	@echo "  CC      $(TOOLS_DIR)/consensus_weighting_selftest.c"
	@$(CC) $(CFLAGS) -I$(CORE_DIR) \
		-o $(CONSENSUS_WEIGHTING_SELFTEST_BIN) \
		$(TOOLS_DIR)/consensus_weighting_selftest.c \
		$(CORE_DIR)/consensus_weighting.c $(CORE_DIR)/fault_buffer.c \
		$(CORE_DIR)/fault_config.c $(CORE_DIR)/mission_state.c $(LDFLAGS)
	@./$(CONSENSUS_WEIGHTING_SELFTEST_BIN)

# --- Evidence-vector parity selftest ---------------------------------------
# Confirms core/recovery_manager.c's evaluate_fault_predictor() and core/
# fault_comms.c's fault_comms_broadcast_local_teams() build byte-identical
# FAULT_BUFFER_REMOTE_EVIDENCE_FEATURES vectors for the same team/tick
# (audit #163) - see laptop/tools/evidence_vector_parity_selftest.c's own
# doc for why this needs the whole core/ directory linked, same pattern as
# laptop/tools/recovery_manager_persistence_selftest.c.
#
# -DCBBA_SIM_CLOCK_STEP_MS=120000u: fault_comms_broadcast_local_teams() is
# now paced against FAULT_COMMS_BROADCAST_INTERVAL_MS=120000 (core/
# fault_comms.c) via cbba_now_ms() - this test calls it exactly once and
# checks the vector it wrote, so cbba_now_ms()'s weak-default simulated
# clock (core/cbba.c, CBBA_SIM_CLOCK_STEP_MS=10 by default) needs to
# advance a full interval on that single call for the broadcast to
# actually fire, or the test would see nothing recorded at all - this
# override is scoped to this one selftest binary, not the real build.
EVIDENCE_VECTOR_PARITY_SELFTEST_BIN = build/evidence_vector_parity_selftest

check-evidence-vector-parity:
	@mkdir -p build
	@echo "  CC      $(TOOLS_DIR)/evidence_vector_parity_selftest.c"
	@$(CC) $(CFLAGS) $(LAPTOP_INCLUDES) -DCBBA_SIM_CLOCK_STEP_MS=120000u \
		-o $(EVIDENCE_VECTOR_PARITY_SELFTEST_BIN) \
		$(TOOLS_DIR)/evidence_vector_parity_selftest.c \
		$(CORE_SOURCES) $(LAPTOP_DIR)/debug.c $(LAPTOP_DIR)/snapshot.c $(LDFLAGS)
	@./$(EVIDENCE_VECTOR_PARITY_SELFTEST_BIN)

# --- fault_comms.c receive-path selftest ------------------------------------
# Drives core/fault_comms.c's fault_comms_ingest_remote_frames() with
# synthetic FaultVotePayload_t frames pushed through the real core/
# comms_transport.c inbox (comms_transport_deliver_received()), then
# inspects the real core/fault_buffer.c state it produced - see laptop/
# tools/fault_comms_receive_selftest.c's own header doc for why this
# needs the whole core/ directory linked (same reason check-evidence-
# vector-parity does) and for the explicit note that core/consensus_
# weighting.c is NOT wired into this call path today.
FAULT_COMMS_RECEIVE_SELFTEST_BIN = build/fault_comms_receive_selftest

check-fault-comms-receive:
	@mkdir -p build
	@echo "  CC      $(TOOLS_DIR)/fault_comms_receive_selftest.c"
	@$(CC) $(CFLAGS) $(LAPTOP_INCLUDES) \
		-o $(FAULT_COMMS_RECEIVE_SELFTEST_BIN) \
		$(TOOLS_DIR)/fault_comms_receive_selftest.c \
		$(CORE_SOURCES) $(LAPTOP_DIR)/debug.c $(LAPTOP_DIR)/snapshot.c $(LDFLAGS)
	@./$(FAULT_COMMS_RECEIVE_SELFTEST_BIN)

# --- fault_comms.c broadcast-pacing selftest ---------------------------------
# Confirms fault_comms_broadcast_local_teams()'s FAULT_COMMS_BROADCAST_
# INTERVAL_MS pacing gate produces real ~120s wall-clock cadence, not
# the invalidated TICK_TIME_SEC-based assumption - see laptop/tools/
# fault_comms_broadcast_pacing_test.c's own header doc. Overrides core/
# cbba.c's weak cbba_now_ms() with a test-controlled strong definition,
# so needs the whole core/ directory linked same as check-fault-comms-
# receive, but must NOT also link a second cbba_now_ms() - there isn't
# one, the override lives in the test file itself.
FAULT_COMMS_BROADCAST_PACING_TEST_BIN = build/fault_comms_broadcast_pacing_test

check-fault-comms-broadcast-pacing:
	@mkdir -p build
	@echo "  CC      $(TOOLS_DIR)/fault_comms_broadcast_pacing_test.c"
	@$(CC) $(CFLAGS) $(LAPTOP_INCLUDES) \
		-o $(FAULT_COMMS_BROADCAST_PACING_TEST_BIN) \
		$(TOOLS_DIR)/fault_comms_broadcast_pacing_test.c \
		$(CORE_SOURCES) $(LAPTOP_DIR)/debug.c $(LAPTOP_DIR)/snapshot.c $(LDFLAGS)
	@./$(FAULT_COMMS_BROADCAST_PACING_TEST_BIN)

# --- fault_comms.c ABORT_MISSION transmission-gate selftest -----------------
# Confirms fault_comms_broadcast_local_teams()'s per-team RECOVERY_
# ACTION_ABORT_MISSION check: an aborted team stops transmitting
# FAULT_BROADCAST frames while other, still-active teams are
# unaffected - see laptop/tools/fault_comms_abort_gate_test.c's own
# header doc. Excludes core/fault_predictor.c and core/watchdog.c from
# CORE_SOURCES (the test provides its own strong overrides so it can
# drive a real ABORT_MISSION recommendation with a synthetic
# confidence sequence - neither real function is declared weak, so
# linking both would be a duplicate-symbol error).
FAULT_COMMS_ABORT_GATE_TEST_BIN = build/fault_comms_abort_gate_test
FAULT_COMMS_ABORT_GATE_TEST_CORE_SOURCES = $(filter-out $(CORE_DIR)/fault_predictor.c $(CORE_DIR)/watchdog.c,$(CORE_SOURCES))

check-fault-comms-abort-gate:
	@mkdir -p build
	@echo "  CC      $(TOOLS_DIR)/fault_comms_abort_gate_test.c"
	@$(CC) $(CFLAGS) $(LAPTOP_INCLUDES) \
		-o $(FAULT_COMMS_ABORT_GATE_TEST_BIN) \
		$(TOOLS_DIR)/fault_comms_abort_gate_test.c \
		$(FAULT_COMMS_ABORT_GATE_TEST_CORE_SOURCES) \
		$(LAPTOP_DIR)/debug.c $(LAPTOP_DIR)/snapshot.c $(LDFLAGS)
	@./$(FAULT_COMMS_ABORT_GATE_TEST_BIN)

# --- recovery_manager.c fault-predictor path stress test --------------------
# Drives the REAL core/recovery_manager.c decision/tiering/hysteresis
# logic with synthetic model-confidence sequences (laptop/tools/recovery_
# predictor_stress_test_stubs.c replaces watchdog.c/fault_predictor.c) -
# confirms persistence-alone escalation, the dip-every-7th-tick
# persistence-streak exploit, and watchdog/model threshold-desync
# behavior, none of which need real trained weights.
RECOVERY_PREDICTOR_STRESS_TEST_BIN = build/recovery_predictor_stress_test

check-recovery-predictor-stress:
	@mkdir -p build
	@echo "  CC      $(TOOLS_DIR)/recovery_predictor_stress_test.c"
	@$(CC) $(CFLAGS) -I$(CORE_DIR) \
		-o $(RECOVERY_PREDICTOR_STRESS_TEST_BIN) \
		$(TOOLS_DIR)/recovery_predictor_stress_test.c \
		$(TOOLS_DIR)/recovery_predictor_stress_test_stubs.c \
		$(CORE_DIR)/recovery_manager.c $(CORE_DIR)/mission_state.c $(LDFLAGS)
	@./$(RECOVERY_PREDICTOR_STRESS_TEST_BIN)

# --- allocator.c REQUEST_TASK_REALLOCATION gate selftest ---------------------
# Confirms core/allocator.c's team_can_accept_incident() fix: a team
# recovery_manager.c has flagged REQUEST_TASK_REALLOCATION cannot be
# handed an unrelated new assignment (system task) in the same tick,
# even if it becomes STATUS_AVAILABLE - see laptop/tools/realloc_gate_
# test.c's own header doc, including its before/after regression control.
REALLOC_GATE_TEST_BIN = build/realloc_gate_test

check-realloc-gate:
	@mkdir -p build
	@echo "  CC      $(TOOLS_DIR)/realloc_gate_test.c"
	@$(CC) $(CFLAGS) -I$(CORE_DIR) \
		-o $(REALLOC_GATE_TEST_BIN) \
		$(TOOLS_DIR)/realloc_gate_test.c \
		$(TOOLS_DIR)/realloc_gate_test_stubs.c \
		$(CORE_DIR)/allocator.c $(CORE_DIR)/recovery_manager.c $(CORE_DIR)/mission_state.c $(LDFLAGS)
	@./$(REALLOC_GATE_TEST_BIN)

# --- Environment-class end-to-end escalation check ---------------------------
# Python, not C - reuses the Config A sklearn pipeline (same as this
# project's comparative-F1 experiments) to get real predicted confidence
# for real held-out environment-fault onset episodes, then runs each
# through a faithful port of recovery_manager.c's persistence/hysteresis
# state machine - laptop/tools/environment_class_e2e_stress.py's own
# header doc has the full method. Requires tron_snapshot_dataset.csv to
# exist with --fault-profile=combined fault injection (e.g. `./tron 1 30
# 400 --fault-profile=combined`) - not run automatically by this target
# since regenerating the dataset is slow and this is an analysis script,
# not a pass/fail gate.
check-environment-e2e-stress:
	python3 $(TOOLS_DIR)/environment_class_e2e_stress.py

