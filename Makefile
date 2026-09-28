CC ?= gcc
# Python for the laptop/tools scripts. Uses the project's .venv when it
# exists (python3.11+ -m venv .venv && .venv/bin/pip install -r
# requirements.txt), else python3. The analysis gates need numpy, pandas and
# scikit-learn on a Python whose scipy imports. Override with PYTHON=...
# Absolute and quoted because some recipes cd into laptop/tools first and
# the repo path may contain spaces.
ifeq ($(origin PYTHON),undefined)
  ifneq ($(wildcard .venv/bin/python),)
    PYTHON = "$(CURDIR)/.venv/bin/python"
  else
    PYTHON = python3
  endif
endif
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
	$(LAPTOP_DIR)/snapshot.c \
	$(LAPTOP_DIR)/mission_feed.c \
	$(LAPTOP_DIR)/mission_mirror.c

LAPTOP_INCLUDES = -I$(CORE_DIR) -I$(LAPTOP_DIR)

LABELER = label_snapshots
LABELER_SOURCES = $(TOOLS_DIR)/label_snapshots.c

TARGET = tron

.PHONY: all clean smoke label_dataset check-board check-board-compile check-board-link check-pico check-pico-compile check-pico-link check-pico-bsp check-sx126x-driver check-two-board-harness check-two-board-cbba-harness check-board-ids-distinct check-mission-link-status-gate check-recovery-predictor-stress check-realloc-gate check-recovery-manager-persistence check-persistence-bounded-forgiveness check-environment-e2e-stress check-selftests check-handshake-selftest check-lora-frame-selftest check-mission-link-selftest check-override-selftest check-cbba-board-selftest check-sx1262-rx-selftest-dk check-sx126x-rx-selftest-pico check-comms-transport-selftest check-mission-link-roster-selftest check-mission-mirror-selftest check-soft-spi-selftest check-sx1262-setup-selftest check-two-board-reboot-harness check-two-board-mission-harness check-bridge-selftests check-virtual-bus check-env-raw-travel-cost-ablation check-consensus-selftest check-consensus-scenarios check-consensus-airtime check-consensus-shadow-regression check-consensus-multiboard-untouched build-consensus-scenario check-fault-predictor-features-selftest check-zone-parity check-zone-refresh check-feature-pack-parity check-feature-spec-fresh check-feature-spec-leak-guard check-board-single check-board-single-compile check-board-single-link check-board-single-symbols check-single-board-dk-harness

all: $(TARGET) $(LABELER)

# Peer consensus (CONSENSUS_DESIGN.md): the laptop `tron` build turns on the
# virtual bus and enforcement (decision D1). Deliberately NOT part of CFLAGS:
# every board build, Pico build, two-board harness and selftest keeps
# VIRTUAL_BUS_ENABLED=0 and compiles none of it. Override with
# LAPTOP_CONSENSUS_FLAGS='' or CONSENSUS_ENFORCE=0 for a shadow-mode build.
CONSENSUS_ENFORCE ?= 1
LAPTOP_CONSENSUS_FLAGS ?= -DVIRTUAL_BUS_ENABLED=1 -DCONSENSUS_ENFORCE=$(CONSENSUS_ENFORCE)

# Single-board STM32N6570-DK build (the submission target): all 10 teams
# local to board 0 (COMMS_TRANSPORT_TEAMS_PER_BOARD=NUM_TEAMS, same override
# the laptop CFLAGS above already apply unconditionally), plus the same
# consensus flags LAPTOP_CONSENSUS_FLAGS uses (fast path stays on via
# consensus_config.h's own default - not repeated here). Deliberately NOT
# folded into ARM_CFLAGS/PICO_ARM_CFLAGS below: those stay exactly as they
# are (VIRTUAL_BUS_ENABLED=0, default 5-teams-per-board split) so every
# existing multi-board board/Pico/two-board-harness target keeps building
# and testing unchanged - see check-board-single-* targets and
# check-single-board-dk-harness further down for what actually uses this.
DK_SINGLE_BOARD_FLAGS ?= -DCOMMS_TRANSPORT_TEAMS_PER_BOARD=NUM_TEAMS -DVIRTUAL_BUS_ENABLED=1 -DCONSENSUS_ENFORCE=$(CONSENSUS_ENFORCE)

$(TARGET): $(LAPTOP_SOURCES)
	$(CC) $(CFLAGS) $(LAPTOP_CONSENSUS_FLAGS) $(LAPTOP_INCLUDES) -o $(TARGET) $(LAPTOP_SOURCES) $(LDFLAGS)

$(LABELER): $(LABELER_SOURCES)
	$(CC) $(CFLAGS) -I$(CORE_DIR) -o $(LABELER) $(LABELER_SOURCES) $(LDFLAGS)

# Runs from build/smoke so its 200-tick clean-profile snapshot CSV does not
# overwrite the repo-root tron_snapshot_dataset.csv label_dataset's own
# default INPUT reads (see that target below).
smoke: $(TARGET)
	@mkdir -p build/smoke
	cd build/smoke && ../../$(TARGET) 1 1 200

# label_dataset's own convenience default - not read by any automated
# check (each of those generates and reads its own private copy under
# build/, see the three dataset-analysis targets below) - just whatever
# `./tron` run happens to be sitting at repo root when label_dataset is
# run manually.
tron_snapshot_dataset.csv: $(TARGET)
	./$(TARGET) 1 5 3000 --fault-profile=combined

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
ARM_NM ?= arm-none-eabi-nm
BSP_DIR ?= board/STM32N6570-DK/vendor/mtk3_bsp2
# -mcmse: the real Appli build passes it, and it is what makes the CMSIS
# header define CPU_IN_SECURE_STATE, without which the secure-alias
# peripheral macros (GPIOA_S, SPI5_S, ...) sx1262_hal.c uses don't exist.
ARM_CFLAGS_BASE = -std=gnu11 -Wall -Wextra -mcpu=cortex-m55 -mthumb -mcmse \
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
  BOARD_SOURCES = $(CORE_SOURCES) $(BOARD_DIR)/dk_debug.c $(BOARD_DIR)/snapshot.c $(BOARD_DIR)/main_board.c $(BOARD_DIR)/mission_link_task.c $(SX126X_DIR)/sx126x.c $(BOARD_DIR)/sx1262_hal.c $(BOARD_DIR)/usart1_driver.c
  BSP_MODE = real BSP2 headers ($(BSP_DIR))
else
  ARM_CFLAGS = $(ARM_CFLAGS_BASE)
  # board/STM32N6570-DK/dk_debug.c and main_board.c call real BSP APIs
  # (tm_printf, tk_cre_tsk, ...) now, so they genuinely can't compile
  # without board/STM32N6570-DK/vendor/mtk3_bsp2 -- that's correct,
  # not a check-board bug. Fallback mode checks core/ only: still a
  # real, useful check (catches libc/OS dependency creep in the
  # portable simulator), just weaker than the real-BSP mode above.
  BOARD_SOURCES = $(CORE_SOURCES)
  BSP_MODE = plain freestanding, core/ only (board/STM32N6570-DK/vendor/mtk3_bsp2 not found -- see that folder's README.md; board/STM32N6570-DK/*.c needs real BSP headers so it's skipped in this mode)
endif

# SX1262_SOFT_SPI=1 builds the DK radio HAL with bit-banged SPI instead
# of the SPI5 peripheral (see board/STM32N6570-DK/sx1262_hal.c and
# core/soft_spi.h). Off by default. The real STM32CubeIDE Appli project
# needs -DSX1262_SOFT_SPI added to its own C symbols to match.
ifeq ($(SX1262_SOFT_SPI),1)
  ARM_CFLAGS += -DSX1262_SOFT_SPI
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

# --- Single-board DK compile/link check --------------------------------
# Mirrors check-board-compile/check-board-link exactly (same BOARD_SOURCES,
# same BSP_MODE fallback), just with DK_SINGLE_BOARD_FLAGS added and a
# separate object directory, so this never touches build/board-check --
# check-consensus-multiboard-untouched's nm scan of that directory (and of
# build/pico-check) must keep seeing zero vbus_/consensus_ symbols, and this
# target's whole point is the opposite assertion (see check-board-single-
# symbols below). Two independent object sets from the same sources, one
# with the flag on and one with it off, is what actually proves the flag -
# not board identity - is what turns the virtual bus on.
ARM_CFLAGS_DK_SINGLE = $(ARM_CFLAGS) $(DK_SINGLE_BOARD_FLAGS)
BOARD_OBJ_DIR_SINGLE = build/board-single-check

check-board-single: check-board-ids-distinct check-board-single-compile check-board-single-link check-board-single-symbols
	@echo "check-board-single: single-board DK compile + link + symbol checks all passed ($(BSP_MODE))"

check-board-single-compile:
	@echo "check-board-single mode: $(BSP_MODE)"
	@mkdir -p $(BOARD_OBJ_DIR_SINGLE)
	@for f in $(BOARD_SOURCES); do \
		obj=$(BOARD_OBJ_DIR_SINGLE)/$$(basename $$f .c).o; \
		echo "  ARM-CC  $$f (single-board DK flags)"; \
		$(ARM_CC) $(ARM_CFLAGS_DK_SINGLE) -c $$f -o $$obj || exit 1; \
	done
	@echo "check-board-single-compile: all sources compile clean for Cortex-M55 with vbus+consensus enabled ($(BSP_MODE))"

check-board-single-link: check-board-single-compile
	@echo "check-board-single-link: partial-linking every $(BOARD_OBJ_DIR_SINGLE)/*.o together to check for undefined cross-file references"
	@objs="$$(ls $(BOARD_OBJ_DIR_SINGLE)/*.o 2>/dev/null)"; \
	if [ -z "$$objs" ]; then \
		echo "check-board-single-link: no objects in $(BOARD_OBJ_DIR_SINGLE) -- check-board-single-compile did not run?"; \
		exit 1; \
	fi; \
	$(ARM_NM) -o $$objs | awk -F'[: \t]+' '$$2=="U" {print $$3}' | sort -u > $(BOARD_OBJ_DIR_SINGLE)/.undefined_syms.txt; \
	$(ARM_NM) -o $$objs | awk -F'[: \t]+' '$$2!="U" && NF>=4 {print $$4}' | sort -u > $(BOARD_OBJ_DIR_SINGLE)/.defined_syms.txt; \
	comm -23 $(BOARD_OBJ_DIR_SINGLE)/.undefined_syms.txt $(BOARD_OBJ_DIR_SINGLE)/.defined_syms.txt > $(BOARD_OBJ_DIR_SINGLE)/.unresolved_syms.txt; \
	libc_a="$$($(ARM_CC) $(ARM_TARGET_FLAGS) -print-file-name=libc.a)"; \
	libm_a="$$($(ARM_CC) $(ARM_TARGET_FLAGS) -print-file-name=libm.a)"; \
	libgcc_a="$$($(ARM_CC) $(ARM_TARGET_FLAGS) -print-file-name=libgcc.a)"; \
	: > $(BOARD_OBJ_DIR_SINGLE)/.expected_external.txt; \
	for lib in "$$libc_a" "$$libm_a" "$$libgcc_a"; do \
		if [ -f "$$lib" ]; then \
			$(ARM_NM) "$$lib" 2>/dev/null | awk -F'[: \t]+' 'NF>=3 && $$2 ~ /^[A-Za-z]$$/ && $$2!="U" && $$2!="u" {print $$3}' >> $(BOARD_OBJ_DIR_SINGLE)/.expected_external.txt; \
		fi; \
	done; \
	if [ -d "$(BSP_DIR)" ]; then \
		grep -rhoE '^[[:space:]]*IMPORT[[:space:]]+[A-Za-z_][A-Za-z0-9_ ]*\*?[[:space:]]+[A-Za-z_][A-Za-z0-9_]*[[:space:]]*\(' $(BSP_DIR) 2>/dev/null \
			| sed -E 's/^.*[[:space:]\*]([A-Za-z_][A-Za-z0-9_]*)[[:space:]]*\($$/\1/' \
			>> $(BOARD_OBJ_DIR_SINGLE)/.expected_external.txt; \
	fi; \
	sort -u $(BOARD_OBJ_DIR_SINGLE)/.expected_external.txt -o $(BOARD_OBJ_DIR_SINGLE)/.expected_external.txt; \
	comm -23 $(BOARD_OBJ_DIR_SINGLE)/.unresolved_syms.txt $(BOARD_OBJ_DIR_SINGLE)/.expected_external.txt > $(BOARD_OBJ_DIR_SINGLE)/.bad_undefined_syms.txt; \
	if [ -s $(BOARD_OBJ_DIR_SINGLE)/.bad_undefined_syms.txt ]; then \
		echo ""; \
		echo "check-board-single-link: FAILED -- undefined reference(s) to symbol(s) that should be"; \
		echo "defined somewhere in core/*.c or board/STM32N6570-DK/*.c, but aren't in this build:"; \
		echo ""; \
		sed 's/^/    /' $(BOARD_OBJ_DIR_SINGLE)/.bad_undefined_syms.txt; \
		echo ""; \
		exit 1; \
	fi; \
	echo "check-board-single-link: real partial link of $$(echo $$objs | wc -w) objects clean"

# Positive counterpart to check-consensus-multiboard-untouched's negative
# assertion: the single-board DK build MUST contain vbus_*/consensus_*
# symbols (that's the entire point of DK_SINGLE_BOARD_FLAGS), so a build
# that silently loses -DVIRTUAL_BUS_ENABLED=1 (a Makefile edit, a typo in
# the flag variable, ...) fails loudly here instead of quietly shipping a
# single-board image with no peer consensus at all. In fallback (no-BSP)
# mode this only has core/*.o to check (board/*.c isn't compiled at all -
# same limitation check-board-compile's own comment documents), which is
# still a real check: core/vbus.c and core/consensus.c only define these
# symbols when actually compiled with VIRTUAL_BUS_ENABLED=1.
check-board-single-symbols: check-board-single-compile
	@objs="$$(ls $(BOARD_OBJ_DIR_SINGLE)/*.o 2>/dev/null)"; \
	if [ -z "$$objs" ]; then \
		echo "check-board-single-symbols: no objects in $(BOARD_OBJ_DIR_SINGLE) -- check-board-single-compile did not run?"; \
		exit 1; \
	fi; \
	if $(ARM_NM) $$objs 2>/dev/null | grep -Eq ' [A-Za-z] (vbus_init|vbus_step|consensus_step|consensus_init)'; then \
		echo "check-board-single-symbols: vbus_init/vbus_step/consensus_init/consensus_step all found defined in $(BOARD_OBJ_DIR_SINGLE) -- single-board DK build carries the virtual bus and consensus"; \
	else \
		echo "check-board-single-symbols: FAILED -- no vbus_*/consensus_* symbols defined in $(BOARD_OBJ_DIR_SINGLE)"; \
		echo "  DK_SINGLE_BOARD_FLAGS's -DVIRTUAL_BUS_ENABLED=1 is not taking effect for this build."; \
		exit 1; \
	fi

clean:
	rm -f $(TARGET) $(LABELER) tron_snapshot_dataset.csv tron_snapshot_dataset_labeled.csv tron_telemetry.log
	rm -rf $(BOARD_OBJ_DIR) $(BOARD_OBJ_DIR_SINGLE) $(PICO_BSP_OBJ_DIR) $(PICO_BOARD_OBJ_DIR) $(SX126X_OBJ_DIR)
	rm -f $(TWO_BOARD_HARNESS_BIN) $(MISSION_LINK_STATUS_GATE_TEST_BIN) \
		$(RECOVERY_PREDICTOR_STRESS_TEST_BIN) $(REALLOC_GATE_TEST_BIN) \
		$(RECOVERY_MANAGER_PERSISTENCE_SELFTEST_BIN)
	rm -rf $(SELFTEST_BIN_DIR)

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
	@# -Wno-unused-parameter: the vendored sample task_1(INT stacd, void *exinf)
	@# ignores both µT-Kernel task arguments; vendored code is not edited here.
	@$(PICO_ARM_CC) -std=gnu11 -Wall -Wextra -Wno-unused-parameter -mcpu=cortex-m0plus -mthumb \
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
	@echo "check-two-board-harness: real two-process handshake + LoRa-relayed roster and override delivery passed"

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
	@$(TOOLS_DIR)/check-two-board-cbba-harness.sh

# --- mission_link.c status-reporting under ABORT_MISSION regression test ---
# core/fault_predictor.c and core/watchdog.c are excluded from CORE_SOURCES
# below (this test provides its own strong overrides so it can drive a
# real ABORT_MISSION recommendation with a synthetic confidence sequence -
# neither real function is declared weak, so linking both would be a
# duplicate-symbol error).
FAULT_COMMS_ABORT_GATE_TEST_CORE_SOURCES = $(filter-out $(CORE_DIR)/fault_predictor.c $(CORE_DIR)/watchdog.c,$(CORE_SOURCES))

# Confirms board/STM32N6570-DK/mission_link_task.c's mission_link_
# status_tx_step() keeps reporting a team whose recovery recommendation
# is RECOVERY_ACTION_ABORT_MISSION (it used to skip such teams, hiding
# the abort-to-rest status/location change from the laptop). REAL frame
# sequencing: links the actual mission_link_task.c, calls its real
# public mission_link_task_step() twice (once to anchor every team, once
# after team 0 aborts and moves), and decodes the real wire bytes it
# writes (captured via this test's own tm_putchar() stub,
# laptop/tools/mission_link_gate_test_stubs/) with the real core/
# mission_link.c mission_link_parse_status() codec - see laptop/tools/
# mission_link_status_gate_test.c's own header doc. Excludes core/
# fault_predictor.c and core/watchdog.c from CORE_SOURCES as described
# above.
MISSION_LINK_STATUS_GATE_TEST_BIN = build/mission_link_status_gate_test

check-mission-link-status-gate:
	@mkdir -p build
	@echo "  CC      $(TOOLS_DIR)/mission_link_status_gate_test.c"
	@$(CC) $(CFLAGS) -I$(TOOLS_DIR)/mission_link_gate_test_stubs \
		-I$(CORE_DIR) -I$(BOARD_DIR) -I$(LAPTOP_DIR) \
		-o $(MISSION_LINK_STATUS_GATE_TEST_BIN) \
		$(TOOLS_DIR)/mission_link_status_gate_test.c \
		$(BOARD_DIR)/mission_link_task.c \
		$(FAULT_COMMS_ABORT_GATE_TEST_CORE_SOURCES) \
		$(LAPTOP_DIR)/debug.c $(LAPTOP_DIR)/snapshot.c $(LDFLAGS)
	@./$(MISSION_LINK_STATUS_GATE_TEST_BIN)

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

# --- recovery_manager.c persistence-streak selftest -------------------------
# core/recovery_manager.c's recovery_manager_persistence_streak_step()
# (hard-consecutive-tick mechanism) AND its EXPERIMENTAL, not-adopted
# bounded-forgiveness sibling - see laptop/tools/recovery_manager_
# persistence_selftest.c's own header doc. Pre-existing file that had
# no Makefile target at all until now - a real, unrelated gap, filled
# here rather than left as a second loose end alongside item 8's work.
RECOVERY_MANAGER_PERSISTENCE_SELFTEST_BIN = build/recovery_manager_persistence_selftest

check-recovery-manager-persistence:
	@mkdir -p build
	@echo "  CC      $(TOOLS_DIR)/recovery_manager_persistence_selftest.c"
	@$(CC) $(CFLAGS) $(LAPTOP_INCLUDES) \
		-o $(RECOVERY_MANAGER_PERSISTENCE_SELFTEST_BIN) \
		$(TOOLS_DIR)/recovery_manager_persistence_selftest.c \
		$(CORE_SOURCES) $(LAPTOP_DIR)/debug.c $(LAPTOP_DIR)/snapshot.c $(LDFLAGS)
	@./$(RECOVERY_MANAGER_PERSISTENCE_SELFTEST_BIN)

# --- Persistence bounded-forgiveness ablation --------------------------------
# Python, not C - real false-reject/false-accept validation of the
# bounded-forgiveness variant against the SAME methodology that tuned
# FAULT_PERSISTENCE_TRIP_TICKS=8 (simulator ground truth + evidence_
# channels.py, no trained model) - see laptop/tools/persistence_
# bounded_forgiveness_ablation.py's own header doc for the measured
# verdict. Not run automatically by this target for the same reason as
# check-environment-e2e-stress below (slow dataset regen, analysis
# script not a pass/fail gate).
#
# Each of these three dataset-analysis targets generates and reads its
# OWN --fault-profile=combined dataset under a private build/ directory,
# rather than sharing one repo-root tron_snapshot_dataset.csv - three
# targets racing to generate/read the same shared file (make -j, or one
# running while another is manually invoked) corrupted it for whichever
# read it mid-write once. TRON_SNAPSHOT_CSV tells each script which copy
# to read (laptop/tools/*.py's own CSV_PATH line).
PERSISTENCE_ABLATION_DIR = build/persistence-bounded-forgiveness
ENV_STRESS_DIR = build/environment-e2e-stress
TRAVEL_COST_ABLATION_DIR = build/env-raw-travel-cost-ablation

$(PERSISTENCE_ABLATION_DIR)/tron_snapshot_dataset.csv: $(TARGET)
	@mkdir -p $(PERSISTENCE_ABLATION_DIR)
	cd $(PERSISTENCE_ABLATION_DIR) && ../../$(TARGET) 1 5 3000 --fault-profile=combined

check-persistence-bounded-forgiveness: $(PERSISTENCE_ABLATION_DIR)/tron_snapshot_dataset.csv
	TRON_SNAPSHOT_CSV=$(PERSISTENCE_ABLATION_DIR)/tron_snapshot_dataset.csv \
		$(PYTHON) $(TOOLS_DIR)/persistence_bounded_forgiveness_ablation.py

# --- Environment-class end-to-end escalation check ---------------------------
# Python, not C - reuses the Config A sklearn pipeline (same as this
# project's comparative-F1 experiments) to get real predicted confidence
# for real held-out environment-fault onset episodes, then runs each
# through a faithful port of recovery_manager.c's persistence/hysteresis
# state machine - laptop/tools/environment_class_e2e_stress.py's own
# header doc has the full method. Not run automatically by this target
# since regenerating the dataset is slow and this is an analysis script,
# not a pass/fail gate.
$(ENV_STRESS_DIR)/tron_snapshot_dataset.csv: $(TARGET)
	@mkdir -p $(ENV_STRESS_DIR)
	cd $(ENV_STRESS_DIR) && ../../$(TARGET) 1 5 3000 --fault-profile=combined

check-environment-e2e-stress: $(ENV_STRESS_DIR)/tron_snapshot_dataset.csv
	TRON_SNAPSHOT_CSV=$(ENV_STRESS_DIR)/tron_snapshot_dataset.csv \
		$(PYTHON) $(TOOLS_DIR)/environment_class_e2e_stress.py

# --- env_raw_travel_cost before/after ablation --------------------------
# Environment-class F1 with and without the env_raw_travel_cost CSV
# column (laptop/snapshot.c; environment_cell_travel_cost() at the team's
# own cell). Same sklearn pipeline as the other ablations. Not run
# automatically, same reason as check-environment-e2e-stress.
$(TRAVEL_COST_ABLATION_DIR)/tron_snapshot_dataset.csv: $(TARGET)
	@mkdir -p $(TRAVEL_COST_ABLATION_DIR)
	cd $(TRAVEL_COST_ABLATION_DIR) && ../../$(TARGET) 1 5 3000 --fault-profile=combined

check-env-raw-travel-cost-ablation: $(TRAVEL_COST_ABLATION_DIR)/tron_snapshot_dataset.csv
	TRON_SNAPSHOT_CSV=$(TRAVEL_COST_ABLATION_DIR)/tron_snapshot_dataset.csv \
		$(PYTHON) $(TOOLS_DIR)/env_raw_travel_cost_ablation.py

# Feature contract (single source of truth): core/feature_spec.h and
# feature_spec.py are GENERATED from the committed feature_spec.json by
# generate_feature_spec.py - never hand-edited. This regenerates both in
# place and fails if that changes anything relative to what's committed,
# so a feature_spec.json edit that forgot to re-run the generator (or a
# hand-edit to either generated file) is caught, not silently merged.
check-feature-spec-fresh:
	@test -f feature_spec.json || (echo "check-feature-spec-fresh: feature_spec.json missing - run data_prep.py first" && exit 1)
	@$(PYTHON) generate_feature_spec.py feature_spec.json
	@git diff --quiet -- core/feature_spec.h feature_spec.py || \
		(echo "check-feature-spec-fresh: core/feature_spec.h / feature_spec.py are stale relative to the committed feature_spec.json:" && \
		 git diff -- core/feature_spec.h feature_spec.py && exit 1)
	@echo "check-feature-spec-fresh: core/feature_spec.h and feature_spec.py match feature_spec.json"

# Fast leak guard on the committed feature_spec.json alone (no CSV, no
# simulator run) - data_prep.py asserts the same thing every time it runs;
# this is the always-cheap version for CI/a plain `make check-feature-
# spec-leak-guard` with nothing else built.
check-feature-spec-leak-guard:
	@test -f feature_spec.json || (echo "check-feature-spec-leak-guard: feature_spec.json missing - run data_prep.py first" && exit 1)
	@$(PYTHON) $(TOOLS_DIR)/feature_spec_leak_check.py feature_spec.json

# --- Standalone laptop/tools selftests ---------------------------------------
# These seven selftests existed with no Makefile target at all, so nothing
# ever ran them (each file's own "Build:" comment was the only way in, and
# three of those comments had drifted out of linking as core/ grew). Each
# returns nonzero on any failed check, so these are real pass/fail gates.
#
# SELFTEST_CFLAGS deliberately does NOT use $(CFLAGS): that adds
# -DCOMMS_TRANSPORT_TEAMS_PER_BOARD=NUM_TEAMS (every team on board 0),
# and cbba_board_selftest.c's board-locality check depends on the real
# default 5-teams-per-board split. Wide selftests link $(CORE_SOURCES)
# rather than a hand-kept file list, so a new core/ file can't break them
# the way it broke those Build: comments.
#
# Binaries run from $(SELFTEST_BIN_DIR), not the repo root:
# override_selftest writes tron_snapshot_dataset.csv into its working
# directory, and would otherwise overwrite the real dataset.
SELFTEST_CFLAGS = -std=gnu11 -Wall -Wextra -Wpedantic -I$(CORE_DIR) -I$(LAPTOP_DIR)
SELFTEST_BIN_DIR = build/selftests
LORA_CRYPTO_SOURCES = $(CORE_DIR)/lora_frame.c $(CORE_DIR)/lora_crypto.c \
	$(CORE_DIR)/aead_chacha20poly1305.c $(CORE_DIR)/chacha20.c $(CORE_DIR)/poly1305.c
# cbba_board_selftest.c defines its own snapshot_record()/g_simulation_
# trace_enabled stand-ins, which collide with simulation.c; sim_runner.c
# needs simulation.c's generate_world().
CBBA_BOARD_SELFTEST_CORE_SOURCES = $(filter-out $(CORE_DIR)/simulation.c $(CORE_DIR)/sim_runner.c,$(CORE_SOURCES))

check-selftests: check-handshake-selftest check-lora-frame-selftest \
	check-mission-link-selftest check-override-selftest \
	check-cbba-board-selftest check-sx1262-rx-selftest-dk \
	check-sx126x-rx-selftest-pico check-comms-transport-selftest \
	check-mission-link-roster-selftest check-mission-mirror-selftest \
	check-soft-spi-selftest check-sx1262-setup-selftest
	@echo "check-selftests: all twelve laptop/tools selftests passed"

check-handshake-selftest:
	@mkdir -p $(SELFTEST_BIN_DIR)
	@echo "  CC      $(TOOLS_DIR)/handshake_selftest.c"
	@$(CC) $(SELFTEST_CFLAGS) -o $(SELFTEST_BIN_DIR)/handshake_selftest \
		$(TOOLS_DIR)/handshake_selftest.c $(CORE_DIR)/mission_link_handshake.c $(LDFLAGS)
	@cd $(SELFTEST_BIN_DIR) && ./handshake_selftest

check-lora-frame-selftest:
	@mkdir -p $(SELFTEST_BIN_DIR)
	@echo "  CC      $(TOOLS_DIR)/lora_frame_selftest.c"
	@$(CC) $(SELFTEST_CFLAGS) -o $(SELFTEST_BIN_DIR)/lora_frame_selftest \
		$(TOOLS_DIR)/lora_frame_selftest.c $(LORA_CRYPTO_SOURCES) $(LDFLAGS)
	@cd $(SELFTEST_BIN_DIR) && ./lora_frame_selftest

check-mission-link-selftest:
	@mkdir -p $(SELFTEST_BIN_DIR)
	@echo "  CC      $(TOOLS_DIR)/mission_link_selftest.c"
	@$(CC) $(SELFTEST_CFLAGS) -o $(SELFTEST_BIN_DIR)/mission_link_selftest \
		$(TOOLS_DIR)/mission_link_selftest.c $(CORE_SOURCES) $(LAPTOP_DIR)/snapshot.c $(LDFLAGS)
	@cd $(SELFTEST_BIN_DIR) && ./mission_link_selftest

check-override-selftest:
	@mkdir -p $(SELFTEST_BIN_DIR)
	@echo "  CC      $(TOOLS_DIR)/override_selftest.c"
	@$(CC) $(SELFTEST_CFLAGS) -o $(SELFTEST_BIN_DIR)/override_selftest \
		$(TOOLS_DIR)/override_selftest.c $(CORE_SOURCES) \
		$(LAPTOP_DIR)/debug.c $(LAPTOP_DIR)/snapshot.c $(LDFLAGS)
	@cd $(SELFTEST_BIN_DIR) && ./override_selftest

check-cbba-board-selftest:
	@mkdir -p $(SELFTEST_BIN_DIR)
	@echo "  CC      $(TOOLS_DIR)/cbba_board_selftest.c"
	@$(CC) $(SELFTEST_CFLAGS) -o $(SELFTEST_BIN_DIR)/cbba_board_selftest \
		$(TOOLS_DIR)/cbba_board_selftest.c $(CBBA_BOARD_SELFTEST_CORE_SOURCES) $(LDFLAGS)
	@cd $(SELFTEST_BIN_DIR) && ./cbba_board_selftest

check-sx1262-rx-selftest-dk:
	@mkdir -p $(SELFTEST_BIN_DIR)
	@echo "  CC      $(TOOLS_DIR)/sx1262_rx_selftest_dk.c"
	@$(CC) $(SELFTEST_CFLAGS) -I$(SX126X_DIR) -o $(SELFTEST_BIN_DIR)/sx1262_rx_selftest_dk \
		$(TOOLS_DIR)/sx1262_rx_selftest_dk.c $(CORE_SOURCES) \
		$(LAPTOP_DIR)/debug.c $(LAPTOP_DIR)/snapshot.c $(SX126X_DIR)/sx126x.c $(LDFLAGS)
	@cd $(SELFTEST_BIN_DIR) && ./sx1262_rx_selftest_dk

check-sx1262-setup-selftest:
	@mkdir -p $(SELFTEST_BIN_DIR)
	@echo "  CC      $(TOOLS_DIR)/sx1262_setup_selftest.c"
	@$(CC) $(SELFTEST_CFLAGS) -I$(SX126X_DIR) -o $(SELFTEST_BIN_DIR)/sx1262_setup_selftest \
		$(TOOLS_DIR)/sx1262_setup_selftest.c $(SX126X_DIR)/sx126x.c $(LDFLAGS)
	@cd $(SELFTEST_BIN_DIR) && ./sx1262_setup_selftest

# core/vbus.c compiles to nothing unless VIRTUAL_BUS_ENABLED is set, so
# the selftest builds it explicitly. random.c is linked only to prove the
# bus leaves its streams untouched.
check-virtual-bus:
	@mkdir -p $(SELFTEST_BIN_DIR)
	@echo "  CC      $(TOOLS_DIR)/virtual_bus_selftest.c"
	@$(CC) $(SELFTEST_CFLAGS) -DVIRTUAL_BUS_ENABLED=1 -o $(SELFTEST_BIN_DIR)/virtual_bus_selftest \
		$(TOOLS_DIR)/virtual_bus_selftest.c $(CORE_DIR)/vbus.c $(CORE_DIR)/random.c $(LDFLAGS)
	@cd $(SELFTEST_BIN_DIR) && ./virtual_bus_selftest

# Fault-predictor history features (stall fractions over the last 5 and
# 10 ticks, incident priority / victim-count deltas): a hand-worked
# 17-tick sequence checked against data_prep's rolling/baseline logic.
check-fault-predictor-features-selftest:
	@mkdir -p $(SELFTEST_BIN_DIR)
	@echo "  CC      $(TOOLS_DIR)/fault_predictor_features_selftest.c"
	@$(CC) $(SELFTEST_CFLAGS) -o $(SELFTEST_BIN_DIR)/fault_predictor_features_selftest \
		$(TOOLS_DIR)/fault_predictor_features_selftest.c $(CORE_DIR)/fault_predictor_features.c \
		$(CORE_DIR)/mission_state.c $(LDFLAGS)
	@cd $(SELFTEST_BIN_DIR) && ./fault_predictor_features_selftest

# Zone parity (feature-contract Decision D): a board fed only the district-
# map/zone-trust frames (mission_link.h 5e/5f) must compute the exact same
# zone_id_for_location()/search_zone_trust() the laptop does. One source
# file, two binaries - "gen" (normal build, generates ground truth + the
# frame stream from a real seeded mission) and "check" (built with
# ENVIRONMENT_HAS_WORLD_STORAGE=0, the real board port, applies only the
# frames and compares). See zone_parity_test.c's own top-of-file comment.
ZONE_PARITY_DIR = build/zone-parity

check-zone-parity:
	@mkdir -p $(ZONE_PARITY_DIR)
	@echo "  CC      $(TOOLS_DIR)/zone_parity_test.c (gen)"
	@$(CC) $(SELFTEST_CFLAGS) -o $(ZONE_PARITY_DIR)/zone_parity_gen \
		$(TOOLS_DIR)/zone_parity_test.c $(CORE_SOURCES) \
		$(LAPTOP_DIR)/debug.c $(LAPTOP_DIR)/snapshot.c $(LDFLAGS)
	@echo "  CC      $(TOOLS_DIR)/zone_parity_test.c (check, ENVIRONMENT_HAS_WORLD_STORAGE=0)"
	@$(CC) $(SELFTEST_CFLAGS) -DENVIRONMENT_HAS_WORLD_STORAGE=0 -o $(ZONE_PARITY_DIR)/zone_parity_check \
		$(TOOLS_DIR)/zone_parity_test.c $(CORE_SOURCES) \
		$(LAPTOP_DIR)/snapshot.c $(LDFLAGS)
	@cd $(ZONE_PARITY_DIR) && ./zone_parity_gen frames.txt truth.csv
	@cd $(ZONE_PARITY_DIR) && ./zone_parity_check frames.txt truth.csv

# Zone refresh convergence (feature-contract Decision D follow-up):
# check-zone-parity only covers the initial district-map/zone-trust burst
# decoding correctly - it never exercises mission_feed_tick()'s periodic
# full resend, so that code could be dead and still pass it (it briefly
# was). This drops every tick-0 frame (simulating a board that missed the
# burst entirely) and confirms zone_id_for_location()/search_zone_trust()
# still converge to the laptop's answers within one MISSION_LINK_ZONE_
# TRUST_REFRESH_TICKS period. Same two-binary design as check-zone-parity;
# see zone_refresh_test.c's own top-of-file comment.
ZONE_REFRESH_DIR = build/zone-refresh

check-zone-refresh:
	@mkdir -p $(ZONE_REFRESH_DIR)
	@echo "  CC      $(TOOLS_DIR)/zone_refresh_test.c (gen)"
	@$(CC) $(SELFTEST_CFLAGS) -o $(ZONE_REFRESH_DIR)/zone_refresh_gen \
		$(TOOLS_DIR)/zone_refresh_test.c $(CORE_SOURCES) \
		$(LAPTOP_DIR)/debug.c $(LAPTOP_DIR)/snapshot.c $(LAPTOP_DIR)/mission_feed.c $(LDFLAGS)
	@echo "  CC      $(TOOLS_DIR)/zone_refresh_test.c (check, ENVIRONMENT_HAS_WORLD_STORAGE=0)"
	@$(CC) $(SELFTEST_CFLAGS) -DENVIRONMENT_HAS_WORLD_STORAGE=0 -o $(ZONE_REFRESH_DIR)/zone_refresh_check \
		$(TOOLS_DIR)/zone_refresh_test.c $(CORE_SOURCES) \
		$(LAPTOP_DIR)/snapshot.c $(LDFLAGS)
	@cd $(ZONE_REFRESH_DIR) && ./zone_refresh_gen frames.txt truth.csv
	@cd $(ZONE_REFRESH_DIR) && ./zone_refresh_check frames.txt truth.csv

# Feature-pack C-vs-Python parity (FEATURE_CONTRACT_DESIGN.md section 5:
# "C vs Python feature parity: <=1e-5 scaled"): a real 5-seed run through
# feature_pack_dump (core/feature_pack.c's own feature_pack_build_
# vector(), same tick/same state snapshot.c just recorded from) compared
# against data_prep.py's --dump-raw-features reconstruction of the same
# rows from the training CSV it wrote, both scaled with the same
# committed feature_spec.py constants. See feature_pack_dump.c's and
# feature_pack_parity_test.py's own top-of-file comments.
FEATURE_PACK_PARITY_DIR = build/feature-pack-parity

check-feature-pack-parity:
	@mkdir -p $(FEATURE_PACK_PARITY_DIR)
	@echo "  CC      $(TOOLS_DIR)/feature_pack_dump.c"
	@$(CC) $(SELFTEST_CFLAGS) -o $(FEATURE_PACK_PARITY_DIR)/feature_pack_dump \
		$(TOOLS_DIR)/feature_pack_dump.c $(CORE_SOURCES) \
		$(LAPTOP_DIR)/debug.c $(LAPTOP_DIR)/snapshot.c $(LDFLAGS)
	@cd $(FEATURE_PACK_PARITY_DIR) && ./feature_pack_dump 5 300 9001 features_c.csv
	@$(PYTHON) data_prep.py $(FEATURE_PACK_PARITY_DIR)/tron_snapshot_dataset.csv \
		--dump-raw-features $(FEATURE_PACK_PARITY_DIR)/features_py_raw.csv > /dev/null
	@$(PYTHON) $(TOOLS_DIR)/feature_pack_parity_test.py \
		$(FEATURE_PACK_PARITY_DIR)/features_c.csv $(FEATURE_PACK_PARITY_DIR)/features_py_raw.csv

# Peer consensus unit tests (CONSENSUS_DESIGN.md 7.9, 7.11): real vbus,
# synthetic teams, no simulation. Needs the virtual bus and the test hooks.
CONSENSUS_TEST_DEFS = -DVIRTUAL_BUS_ENABLED=1 -DCONSENSUS_TEST_HOOKS=1

check-consensus-selftest:
	@mkdir -p $(SELFTEST_BIN_DIR)
	@echo "  CC      $(TOOLS_DIR)/consensus_selftest.c"
	# The selftest exercises the fast path, which is off by default (decision D7).
	@$(CC) $(SELFTEST_CFLAGS) $(CONSENSUS_TEST_DEFS) -DCONSENSUS_FAST_PATH=1 -o $(SELFTEST_BIN_DIR)/consensus_selftest \
		$(TOOLS_DIR)/consensus_selftest.c $(CORE_DIR)/consensus.c $(CORE_DIR)/vbus.c \
		$(CORE_DIR)/mission_state.c $(LDFLAGS)
	@cd $(SELFTEST_BIN_DIR) && ./consensus_selftest

# Whole-simulation scenario tests and the measurement modes (design 7.1-7.5,
# 7.11). The variant flags are compile-time, so each variant is its own
# binary: make build-consensus-scenario SCEN_BIN=path SCEN_DEFS='-D...'.
# The binary links the laptop sim like `tron` does (same
# COMMS_TRANSPORT_TEAMS_PER_BOARD override), plus the test hooks.
SCEN_DEFS ?= -DCONSENSUS_ENFORCE=1
SCEN_BIN ?= $(SELFTEST_BIN_DIR)/consensus_scenario_test

build-consensus-scenario:
	@mkdir -p $(dir $(SCEN_BIN))
	@$(CC) $(SELFTEST_CFLAGS) -DCOMMS_TRANSPORT_TEAMS_PER_BOARD=NUM_TEAMS $(CONSENSUS_TEST_DEFS) $(SCEN_DEFS) \
		-o $(SCEN_BIN) $(TOOLS_DIR)/consensus_scenario_test.c $(CORE_SOURCES) \
		$(LAPTOP_DIR)/debug.c $(LAPTOP_DIR)/snapshot.c $(LDFLAGS)

# Asserted scenarios: the shipped configuration (enforcing, cap off, fast
# path on) and the shadow-mode build, then the random-emission variant, whose
# detection is reported rather than asserted.
check-consensus-scenarios:
	@echo "  consensus scenarios: enforcing, fast path on (shipped), periodic emission"
	@$(MAKE) --no-print-directory build-consensus-scenario SCEN_DEFS='-DCONSENSUS_ENFORCE=1 -DCONSENSUS_FAST_PATH=1'
	@cd $(SELFTEST_BIN_DIR) && ./consensus_scenario_test assert
	@echo "  consensus scenarios: vote path only (fast path off)"
	@$(MAKE) --no-print-directory build-consensus-scenario SCEN_DEFS='-DCONSENSUS_ENFORCE=1 -DCONSENSUS_FAST_PATH=0' \
		SCEN_BIN=$(SELFTEST_BIN_DIR)/consensus_scenario_test_fast
	@cd $(SELFTEST_BIN_DIR) && ./consensus_scenario_test_fast assert quick
	@echo "  consensus scenarios: shadow mode (CONSENSUS_ENFORCE=0)"
	@$(MAKE) --no-print-directory build-consensus-scenario SCEN_DEFS='-DCONSENSUS_ENFORCE=0 -DCONSENSUS_FAST_PATH=0' \
		SCEN_BIN=$(SELFTEST_BIN_DIR)/consensus_scenario_test_shadow
	@cd $(SELFTEST_BIN_DIR) && ./consensus_scenario_test_shadow assert quick
	@echo "  consensus scenarios: random emission variant"
	@$(MAKE) --no-print-directory build-consensus-scenario SCEN_DEFS='-DCONSENSUS_ENFORCE=1 -DCONSENSUS_FAST_PATH=0 -DTEAM_TEST_RANDOM_EMISSION=1' \
		SCEN_BIN=$(SELFTEST_BIN_DIR)/consensus_scenario_test_random
	@cd $(SELFTEST_BIN_DIR) && ./consensus_scenario_test_random assert quick

check-consensus-airtime:
	@$(MAKE) --no-print-directory build-consensus-scenario SCEN_DEFS='-DCONSENSUS_ENFORCE=1'
	@cd $(SELFTEST_BIN_DIR) && ./consensus_scenario_test airtime

# Shadow-mode regression (design 7.7): with CONSENSUS_ENFORCE=0 every column
# the base branch writes must be identical. The base is built from
# CONSENSUS_BASE_REF in a scratch tree. run_id is excluded outright;
# incident_priority/victim_count are compared after mapping the base's old
# no-task sentinel (-1) to the candidate's new one (0), so a genuine
# regression in either still fails (see consensus_shadow_compare.py's
# EXCLUDED_COLUMNS/REMAPPED_COLUMNS and module docstring). The base is
# pinned to e37181a, the last submission commit before consensus was
# merged: the branch tip now contains consensus itself.
CONSENSUS_BASE_REF ?= e37181a
SHADOW_DIR = build/consensus-shadow

check-consensus-shadow-regression:
	@rm -rf $(SHADOW_DIR)
	@mkdir -p $(SHADOW_DIR)/base
	@git archive $(CONSENSUS_BASE_REF) | tar -x -C $(SHADOW_DIR)/base
	@$(MAKE) --no-print-directory -C $(SHADOW_DIR)/base tron >/dev/null
	@$(CC) $(CFLAGS) -DVIRTUAL_BUS_ENABLED=1 -DCONSENSUS_ENFORCE=0 $(LAPTOP_INCLUDES) \
		-o $(SHADOW_DIR)/tron_shadow $(LAPTOP_SOURCES) $(LDFLAGS)
	@$(PYTHON) $(TOOLS_DIR)/consensus_shadow_compare.py $(SHADOW_DIR)/base/$(TARGET) $(SHADOW_DIR)/tron_shadow

# Multi-board builds must contain none of the virtual bus or consensus code
# (design 7.8): the board, Pico and two-board targets build and pass as
# before, and no consensus_* / vbus_* symbol appears in any board object.
# Needs arm-none-eabi-gcc; without it the object scan is skipped and the
# board targets say so themselves.
# Without the vendor BSPs (board/*/vendor/mtk3_bsp*, not in this repo) the
# whole-image link checks fail on the base branch too, for unrelated missing
# snapshot and debug backends; the per-file compile checks still run, and so
# does the link check whenever the BSP is present.
CONSENSUS_MULTIBOARD_BOARD_CHECKS = check-board-compile check-pico-compile
ifneq ($(wildcard $(BSP_DIR)/include),)
  CONSENSUS_MULTIBOARD_BOARD_CHECKS = check-board check-pico
endif

check-consensus-multiboard-untouched: $(CONSENSUS_MULTIBOARD_BOARD_CHECKS) check-two-board-harness check-two-board-cbba-harness check-two-board-mission-harness
	@found=0; \
	for d in build/board-check build/pico-check; do \
		[ -d $$d ] || continue; \
		for o in $$d/*.o; do \
			if $(ARM_NM) $$o 2>/dev/null | grep -Eq ' [A-Za-z] (consensus_|vbus_|g_diag_consensus|g_consensus|lora_time_on_air_ms_x1000)'; then \
				echo "check-consensus-multiboard-untouched: consensus/vbus symbol in $$o"; found=1; \
			fi; \
		done; \
	done; \
	if [ $$found -ne 0 ]; then exit 1; fi; \
	echo "check-consensus-multiboard-untouched: no consensus or vbus symbols in board or Pico objects"

check-soft-spi-selftest:
	@mkdir -p $(SELFTEST_BIN_DIR)
	@echo "  CC      $(TOOLS_DIR)/soft_spi_selftest.c"
	@$(CC) $(SELFTEST_CFLAGS) -o $(SELFTEST_BIN_DIR)/soft_spi_selftest \
		$(TOOLS_DIR)/soft_spi_selftest.c $(LDFLAGS)
	@cd $(SELFTEST_BIN_DIR) && ./soft_spi_selftest

check-sx126x-rx-selftest-pico:
	@mkdir -p $(SELFTEST_BIN_DIR)
	@echo "  CC      $(TOOLS_DIR)/sx126x_rx_selftest_pico.c"
	@$(CC) $(SELFTEST_CFLAGS) -I$(SX126X_DIR) -o $(SELFTEST_BIN_DIR)/sx126x_rx_selftest_pico \
		$(TOOLS_DIR)/sx126x_rx_selftest_pico.c $(SX126X_DIR)/sx126x.c $(LORA_CRYPTO_SOURCES) \
		$(CORE_DIR)/comms_transport.c $(CORE_DIR)/mission_state.c $(LDFLAGS)
	@cd $(SELFTEST_BIN_DIR) && ./sx126x_rx_selftest_pico

check-mission-link-roster-selftest:
	@mkdir -p $(SELFTEST_BIN_DIR)
	@echo "  CC      $(TOOLS_DIR)/mission_link_roster_selftest.c"
	@$(CC) $(SELFTEST_CFLAGS) -I$(TOOLS_DIR)/harness_stubs -I$(BOARD_DIR) \
		-o $(SELFTEST_BIN_DIR)/mission_link_roster_selftest \
		$(TOOLS_DIR)/mission_link_roster_selftest.c $(BOARD_DIR)/mission_link_task.c \
		$(CORE_SOURCES) $(LAPTOP_DIR)/debug.c $(LAPTOP_DIR)/snapshot.c $(LDFLAGS)
	@cd $(SELFTEST_BIN_DIR) && ./mission_link_roster_selftest

# laptop/mission_mirror.c: the laptop's board-mission picture built from
# board status reports - see laptop/tools/mission_mirror_selftest.c.
check-mission-mirror-selftest:
	@mkdir -p $(SELFTEST_BIN_DIR)
	@echo "  CC      $(TOOLS_DIR)/mission_mirror_selftest.c"
	@$(CC) $(SELFTEST_CFLAGS) -DCOMMS_TRANSPORT_TEAMS_PER_BOARD=NUM_TEAMS \
		-o $(SELFTEST_BIN_DIR)/mission_mirror_selftest \
		$(TOOLS_DIR)/mission_mirror_selftest.c $(LAPTOP_DIR)/mission_mirror.c \
		$(CORE_SOURCES) $(LAPTOP_DIR)/debug.c $(LAPTOP_DIR)/snapshot.c $(LDFLAGS)
	@cd $(SELFTEST_BIN_DIR) && ./mission_mirror_selftest

check-comms-transport-selftest:
	@mkdir -p $(SELFTEST_BIN_DIR)
	@echo "  CC      $(TOOLS_DIR)/comms_transport_selftest.c"
	@$(CC) $(SELFTEST_CFLAGS) -o $(SELFTEST_BIN_DIR)/comms_transport_selftest \
		$(TOOLS_DIR)/comms_transport_selftest.c $(LORA_CRYPTO_SOURCES) \
		$(CORE_DIR)/comms_transport.c $(CORE_DIR)/mission_state.c $(LDFLAGS)
	@cd $(SELFTEST_BIN_DIR) && ./comms_transport_selftest

# --- Laptop bridge (Python) selftests ----------------------------------------
# Handshake coordinator, telemetry frame parser, override acks, team status
# reports and the mission feed (parsers, trackers, full USART1 scanner chain,
# a real `tron --mission-feed` run, and app.py's routes when Flask is
# installed). The mission feed test needs ./tron built (make all). No serial port or board needed; run from laptop/tools
# because the tests import those modules directly.
check-bridge-selftests:
	@cd $(TOOLS_DIR) && $(PYTHON) handshake_selftest.py > /dev/null && echo "  handshake_selftest.py passed"
	@cd $(TOOLS_DIR) && $(PYTHON) telemetry_bridge_selftest.py > /dev/null && echo "  telemetry_bridge_selftest.py passed"
	@cd $(TOOLS_DIR) && $(PYTHON) override_ack_selftest.py
	@cd $(TOOLS_DIR) && $(PYTHON) team_status_selftest.py
	@cd $(TOOLS_DIR) && $(PYTHON) mission_feed_selftest.py

# --- Two-board reboot recovery -----------------------------------------------
# One board reboots while the other stays up; the link must recover by
# itself. Every combination of {board 1 reboots, board 0 reboots} x
# {quiet, CBBA traffic}, built against BOTH boards' mission_link_task.c,
# REBOOT_HARNESS_RUNS times each (the two processes aren't lockstep, so
# one pass proves little - use REBOOT_HARNESS_RUNS=25 before trusting a
# change to comms/crypto/handshake code). See laptop/tools/two_board_
# reboot_harness.c for what each scenario asserts.
REBOOT_HARNESS_RUNS ?= 3
REBOOT_HARNESS_BIN_DIR = build/reboot-harness

check-two-board-reboot-harness:
	@mkdir -p $(REBOOT_HARNESS_BIN_DIR)
	@set -e; for board in STM32N6570-DK Raspberry_Pi_Pico; do \
		echo "  CC      $(TOOLS_DIR)/two_board_reboot_harness.c (board/$$board)"; \
		$(CC) -std=gnu11 -Wall -Wextra -Wpedantic -I$(CORE_DIR) -Iboard/$$board -I$(LAPTOP_DIR) \
			-I$(TOOLS_DIR)/harness_stubs -o $(REBOOT_HARNESS_BIN_DIR)/$$board \
			$(TOOLS_DIR)/two_board_reboot_harness.c board/$$board/mission_link_task.c \
			$(LAPTOP_DIR)/debug.c $(LAPTOP_DIR)/snapshot.c $(CORE_SOURCES) -lm; \
	done
	@fail=0; for board in STM32N6570-DK Raspberry_Pi_Pico; do \
		for mode in board1 board0; do for traffic in quiet cbba; do \
			pass=0; i=0; while [ $$i -lt $(REBOOT_HARNESS_RUNS) ]; do \
				if ./$(REBOOT_HARNESS_BIN_DIR)/$$board $$mode $$traffic > $(REBOOT_HARNESS_BIN_DIR)/last.log 2>&1; \
				then pass=$$((pass+1)); else cp $(REBOOT_HARNESS_BIN_DIR)/last.log \
					$(REBOOT_HARNESS_BIN_DIR)/fail-$$board-$$mode-$$traffic-$$i.log; fi; \
				i=$$((i+1)); done; \
			echo "  $$board: $$mode reboot, $$traffic traffic: $$pass/$(REBOOT_HARNESS_RUNS)"; \
			[ $$pass -eq $(REBOOT_HARNESS_RUNS) ] || fail=1; \
		done; done; \
	done; \
	if [ $$fail -ne 0 ]; then echo "check-two-board-reboot-harness: FAILED - logs in $(REBOOT_HARNESS_BIN_DIR)/fail-*.log"; exit 1; fi
	@echo "check-two-board-reboot-harness: every reboot scenario recovered on both boards"

# Laptop as clock master, through the real board code on two processes: a
# clock tick from USART1 reaches both boards (latest wins), and a team
# board 1 moves shows up in the v3 status board 0 writes for the laptop.
# See laptop/tools/two_board_mission_harness.c.
MISSION_HARNESS_BIN_DIR = build/mission-harness

check-two-board-mission-harness:
	@mkdir -p $(MISSION_HARNESS_BIN_DIR)
	@set -e; for board in STM32N6570-DK Raspberry_Pi_Pico; do \
		echo "  CC      $(TOOLS_DIR)/two_board_mission_harness.c (board/$$board)"; \
		$(CC) -std=gnu11 -Wall -Wextra -Wpedantic -I$(CORE_DIR) -Iboard/$$board -I$(LAPTOP_DIR) \
			-I$(TOOLS_DIR)/harness_stubs -o $(MISSION_HARNESS_BIN_DIR)/$$board \
			$(TOOLS_DIR)/two_board_mission_harness.c board/$$board/mission_link_task.c \
			$(LAPTOP_DIR)/debug.c $(LAPTOP_DIR)/snapshot.c $(CORE_SOURCES) -lm; \
		./$(MISSION_HARNESS_BIN_DIR)/$$board; \
	done
	@echo "check-two-board-mission-harness: clock, map, sites and positions crossed both boards"

# --- Single-board DK harness ---------------------------------------------
# The submission target, proven end to end host-side: real board/STM32N6570-
# DK/mission_link_task.c + core/*.c (unmodified), built with
# DK_SINGLE_BOARD_FLAGS (all 10 teams local, vbus + consensus enabled). See
# laptop/tools/single_board_dk_harness.c's own top comment for exactly what
# it asserts and why lora_spi_transmit() (not a counter or a mock) is the
# right seam to prove "no relay send is ever attempted" - unlike every
# two-board harness, there is deliberately no second process and no socket
# here, since single-board-dk has no peer board and no radio to simulate.
SINGLE_BOARD_HARNESS_BIN = build/single_board_dk_harness

check-single-board-dk-harness:
	@mkdir -p build
	@echo "  CC      $(TOOLS_DIR)/single_board_dk_harness.c (+ real board/STM32N6570-DK/mission_link_task.c)"
	@$(CC) -std=gnu11 -Wall -Wextra -Wpedantic $(DK_SINGLE_BOARD_FLAGS) -DTHIS_BOARD_ID=0 \
		-I$(CORE_DIR) -I$(BOARD_DIR) -I$(LAPTOP_DIR) -I$(TOOLS_DIR)/harness_stubs \
		-o $(SINGLE_BOARD_HARNESS_BIN) $(TOOLS_DIR)/single_board_dk_harness.c \
		$(BOARD_DIR)/mission_link_task.c $(LAPTOP_DIR)/debug.c $(LAPTOP_DIR)/snapshot.c \
		$(CORE_SOURCES) -lm
	@./$(SINGLE_BOARD_HARNESS_BIN)
	@echo "check-single-board-dk-harness: 10 local teams + vbus + consensus + mission link passed, zero relay sends"


# =============================================================================
# Agent C review-fix tests (board code, dashboard and tooling)
# =============================================================================

# app.py's cross-site request forgery guard, through Flask's test client:
# JSON same-origin/tooling POSTs work; text/plain and form POSTs (CORS-
# "simple", sent cross-site with no preflight) are 415 with no side effect;
# a foreign Origin/Referer is 403 even as JSON. Skips if Flask is missing.
# See laptop/tools/dashboard_csrf_selftest.py.
.PHONY: check-dashboard-csrf-selftest
check-dashboard-csrf-selftest:
	@cd $(TOOLS_DIR) && $(PYTHON) dashboard_csrf_selftest.py

# ---------------------------------------------------------------------
# Board-to-board protocol review fixes (H2 H3 H4 H5) - selftests.
# Each is built twice, plainly and under ASan/UBSan: several of these
# tests feed deliberately malformed frames whose only failure mode on
# the old code was an out-of-bounds access, which a CHECK() alone can
# miss.
# ---------------------------------------------------------------------
AGENT_B_SANITIZE = -g -O1 -fsanitize=address,undefined -fno-sanitize-recover=undefined

.PHONY: check-mission-link-validation-selftest
check-mission-link-validation-selftest:
	@mkdir -p $(SELFTEST_BIN_DIR)
	@echo "  CC      $(TOOLS_DIR)/mission_link_validation_selftest.c"
	@$(CC) $(SELFTEST_CFLAGS) -o $(SELFTEST_BIN_DIR)/mission_link_validation_selftest \
		$(TOOLS_DIR)/mission_link_validation_selftest.c $(CORE_SOURCES) \
		$(LAPTOP_DIR)/debug.c $(LAPTOP_DIR)/snapshot.c $(LDFLAGS)
	@$(CC) $(SELFTEST_CFLAGS) $(AGENT_B_SANITIZE) -o $(SELFTEST_BIN_DIR)/mission_link_validation_selftest_asan \
		$(TOOLS_DIR)/mission_link_validation_selftest.c $(CORE_SOURCES) \
		$(LAPTOP_DIR)/debug.c $(LAPTOP_DIR)/snapshot.c $(LDFLAGS)
	@cd $(SELFTEST_BIN_DIR) && ./mission_link_validation_selftest > /dev/null 2>&1 && \
		./mission_link_validation_selftest_asan

.PHONY: check-cbba-wire-validation
check-cbba-wire-validation:
	@mkdir -p $(SELFTEST_BIN_DIR)
	@echo "  CC      $(TOOLS_DIR)/cbba_board_selftest.c (ASan/UBSan)"
	@$(CC) $(SELFTEST_CFLAGS) $(AGENT_B_SANITIZE) -o $(SELFTEST_BIN_DIR)/cbba_board_selftest_asan \
		$(TOOLS_DIR)/cbba_board_selftest.c $(CBBA_BOARD_SELFTEST_CORE_SOURCES) $(LDFLAGS)
	@cd $(SELFTEST_BIN_DIR) && ./cbba_board_selftest_asan

# ---------------------------------------------------------------------
# Consensus/AI evaluation harness (docs/evaluation/README.md). Measures
# whether peer consensus (and later the AI fault predictor) actually
# improves fault detection and mission outcome over a rules-only baseline,
# on the real ./tron simulator - see laptop/tools/evaluate.py and
# laptop/tools/eval_metrics.py's module docstrings for the full
# methodology, metric definitions, and known limitations.
# ---------------------------------------------------------------------
.PHONY: evaluate check-evaluate-determinism check-eval-metrics-unittest

# Full evaluation: exam/benchmark seeds 1001-1020, 3000 ticks each,
# rules-only vs. rules+consensus, fault-profile=random. Override any of
# EVAL_* to sweep other seeds/ticks/profiles/variants; see evaluate.py
# --help for the rest of the flags (e.g. --profiles random,combined).
EVAL_VARIANTS ?= rules,rules+consensus
EVAL_SEEDS_START ?= 1001
EVAL_SEEDS_COUNT ?= 20
EVAL_TICKS ?= 3000
EVAL_PROFILES ?= random
EVAL_OUT_DIR ?= build/evaluate

evaluate:
	@$(PYTHON) $(TOOLS_DIR)/evaluate.py \
		--variants $(EVAL_VARIANTS) \
		--seeds-start $(EVAL_SEEDS_START) --seeds-count $(EVAL_SEEDS_COUNT) \
		--ticks $(EVAL_TICKS) --profiles $(EVAL_PROFILES) \
		--out-dir $(EVAL_OUT_DIR)

# Metric-computation unit tests (hand-built ground-truth/detection
# sequences with known answers) - no simulator invocation, runs in
# milliseconds. See laptop/tools/test_eval_metrics.py.
check-eval-metrics-unittest:
	@$(PYTHON) $(TOOLS_DIR)/test_eval_metrics.py -v

# Tiny (2 seeds x 300 ticks) evaluate.py run executed once with
# --workers 1 and once with --workers 4, asserting byte-identical
# summary_metrics.csv/paired_diff.csv/report.md - see
# laptop/tools/check-evaluate-determinism.sh.
check-evaluate-determinism:
	@PYTHON=$(PYTHON) $(TOOLS_DIR)/check-evaluate-determinism.sh
