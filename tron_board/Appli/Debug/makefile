################################################################################
# Automatically-generated file. Do not edit!
# Toolchain: GNU Tools for STM32 (14.3.rel1)
################################################################################

-include ../makefile.init

RM := rm -rf

# All of the sources participating in the build are defined here
-include sources.mk
-include core_src/vendor/sx126x_driver/src/subdir.mk
-include core_src/subdir.mk
-include board_src/vendor/mtk3_bsp2/sysdepend/stm32_cube/lib/libtm/discovery_stm32n657/subdir.mk
-include board_src/vendor/mtk3_bsp2/sysdepend/stm32_cube/lib/libtk/cpu/stm32n6/subdir.mk
-include board_src/vendor/mtk3_bsp2/sysdepend/stm32_cube/lib/libtk/cpu/core/armv8m/subdir.mk
-include board_src/vendor/mtk3_bsp2/sysdepend/stm32_cube/lib/libtk/subdir.mk
-include board_src/vendor/mtk3_bsp2/sysdepend/stm32_cube/device/hal_i2c/subdir.mk
-include board_src/vendor/mtk3_bsp2/sysdepend/stm32_cube/device/hal_adc/sysdepend/subdir.mk
-include board_src/vendor/mtk3_bsp2/sysdepend/stm32_cube/device/hal_adc/subdir.mk
-include board_src/vendor/mtk3_bsp2/sysdepend/stm32_cube/cpu/core/armv8m/subdir.mk
-include board_src/vendor/mtk3_bsp2/sysdepend/stm32_cube/subdir.mk
-include board_src/vendor/mtk3_bsp2/mtkernel/lib/libtm/subdir.mk
-include board_src/vendor/mtk3_bsp2/mtkernel/lib/libtk/subdir.mk
-include board_src/vendor/mtk3_bsp2/mtkernel/kernel/usermain/subdir.mk
-include board_src/vendor/mtk3_bsp2/mtkernel/kernel/tstdlib/subdir.mk
-include board_src/vendor/mtk3_bsp2/mtkernel/kernel/tkernel/subdir.mk
-include board_src/vendor/mtk3_bsp2/mtkernel/kernel/sysinit/subdir.mk
-include board_src/vendor/mtk3_bsp2/mtkernel/kernel/inittask/subdir.mk
-include board_src/vendor/mtk3_bsp2/mtkernel/device/ser/subdir.mk
-include board_src/vendor/mtk3_bsp2/mtkernel/device/i2c/subdir.mk
-include board_src/vendor/mtk3_bsp2/mtkernel/device/common/drvif/subdir.mk
-include board_src/vendor/mtk3_bsp2/mtkernel/device/adc/subdir.mk
-include board_src/subdir.mk
-include Drivers/STM32N6xx_HAL_Driver/subdir.mk
-include Core/Startup/subdir.mk
-include Core/Src/subdir.mk
-include objects.mk

ifneq ($(MAKECMDGOALS),clean)
ifneq ($(strip $(S_DEPS)),)
-include $(S_DEPS)
endif
ifneq ($(strip $(S_UPPER_DEPS)),)
-include $(S_UPPER_DEPS)
endif
ifneq ($(strip $(C_DEPS)),)
-include $(C_DEPS)
endif
endif

-include ../makefile.defs

OPTIONAL_TOOL_DEPS := \
$(wildcard ../makefile.defs) \
$(wildcard ../makefile.init) \
$(wildcard ../makefile.targets) \


BUILD_ARTIFACT_NAME := tron_board_Appli
BUILD_ARTIFACT_EXTENSION := elf
BUILD_ARTIFACT_PREFIX :=
BUILD_ARTIFACT := $(BUILD_ARTIFACT_PREFIX)$(BUILD_ARTIFACT_NAME)$(if $(BUILD_ARTIFACT_EXTENSION),.$(BUILD_ARTIFACT_EXTENSION),)

# Add inputs and outputs from these tool invocations to the build variables 
EXECUTABLES += \
tron_board_Appli.elf \

MAP_FILES += \
tron_board_Appli.map \

SIZE_OUTPUT += \
default.size.stdout \

OBJDUMP_LIST += \
tron_board_Appli.list \

OBJCOPY_BIN += \
tron_board_Appli.bin \


# All Target
all: main-build

# Main-build Target
main-build: tron_board_Appli.elf secondary-outputs

# Tool invocations
tron_board_Appli.elf tron_board_Appli.map: $(OBJS) $(USER_OBJS) /Users/harushsgtemkar/projects/Tron\ project/Tron\ project\ Code/tron_merged-board-2/tron_board/Appli/STM32N657X0HXQ_LRUN.ld makefile objects.list $(OPTIONAL_TOOL_DEPS)
	arm-none-eabi-gcc -o "tron_board_Appli.elf" @"objects.list" $(USER_OBJS) $(LIBS) -mcpu=cortex-m55 -T"/Users/harushsgtemkar/projects/Tron project/Tron project Code/tron_merged-board-2/tron_board/Appli/STM32N657X0HXQ_LRUN.ld" --specs=nosys.specs -Wl,-Map="tron_board_Appli.map" -Wl,--gc-sections -static -Wl,--cmse-implib -Wl,--out-implib=./secure_nsclib.o --specs=nano.specs -mfpu=fpv5-d16 -mfloat-abi=hard -mthumb -Wl,--start-group -lc -lm -Wl,--end-group
	@echo 'Finished building target: $@'
	@echo ' '

default.size.stdout: $(EXECUTABLES) makefile objects.list $(OPTIONAL_TOOL_DEPS)
	arm-none-eabi-size  $(EXECUTABLES)
	@echo 'Finished building: $@'
	@echo ' '

tron_board_Appli.list: $(EXECUTABLES) makefile objects.list $(OPTIONAL_TOOL_DEPS)
	arm-none-eabi-objdump -h -S $(EXECUTABLES) > "tron_board_Appli.list"
	@echo 'Finished building: $@'
	@echo ' '

tron_board_Appli.bin: $(EXECUTABLES) makefile objects.list $(OPTIONAL_TOOL_DEPS)
	arm-none-eabi-objcopy  -O binary $(EXECUTABLES) "tron_board_Appli.bin"
	@echo 'Finished building: $@'
	@echo ' '

# Other Targets
clean:
	-$(RM) default.size.stdout tron_board_Appli.bin tron_board_Appli.elf tron_board_Appli.list tron_board_Appli.map
	-@echo ' '

secondary-outputs: $(SIZE_OUTPUT) $(OBJDUMP_LIST) $(OBJCOPY_BIN)

fail-specified-linker-script-missing:
	@echo 'Error: Cannot find the specified linker script. Check the linker settings in the build configuration.'
	@exit 2

warn-no-linker-script-specified:
	@echo 'Warning: No linker script specified. Check the linker settings in the build configuration.'

.PHONY: all clean dependents main-build fail-specified-linker-script-missing warn-no-linker-script-specified

-include ../makefile.targets
