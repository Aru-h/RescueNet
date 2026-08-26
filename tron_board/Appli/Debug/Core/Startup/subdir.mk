################################################################################
# Automatically-generated file. Do not edit!
# Toolchain: GNU Tools for STM32 (14.3.rel1)
################################################################################

# Add inputs and outputs from these tool invocations to the build variables 
S_SRCS += \
../Core/Startup/startup_stm32n657x0hxq.s 

OBJS += \
./Core/Startup/startup_stm32n657x0hxq.o 

S_DEPS += \
./Core/Startup/startup_stm32n657x0hxq.d 


# Each subdirectory must supply rules for building sources it contributes
Core/Startup/%.o: ../Core/Startup/%.s Core/Startup/subdir.mk
	arm-none-eabi-gcc -mcpu=cortex-m55 -g3 -DDEBUG -D_STM32CUBE_DISCOVERY_N657_ -c -I../Core/Inc -I"/Users/harushsgtemkar/projects/Tron project/Tron project Code/tron_merged-board-2/tron_board/Appli/../../core" -I"/Users/harushsgtemkar/projects/Tron project/Tron project Code/tron_merged-board-2/tron_board/Appli/../../platform/board" -I"/Users/harushsgtemkar/projects/Tron project/Tron project Code/tron_merged-board-2/tron_board/Appli/../../vendor/mtk3_bsp2/include" -I"/Users/harushsgtemkar/projects/Tron project/Tron project Code/tron_merged-board-2/tron_board/Appli/../../vendor/mtk3_bsp2/config" -I"/Users/harushsgtemkar/projects/Tron project/Tron project Code/tron_merged-board-2/tron_board/Appli/../../vendor/mtk3_bsp2/mtkernel" -I"/Users/harushsgtemkar/projects/Tron project/Tron project Code/tron_merged-board-2/tron_board/Appli/../../vendor/mtk3_bsp2" -I"/Users/harushsgtemkar/projects/Tron project/Tron project Code/tron_merged-board-2/tron_board/Appli/../../vendor/mtk3_bsp2/mtkernel/kernel/knlinc" -I../../Drivers/CMSIS/Include -I../../Drivers/STM32N6xx_HAL_Driver/Inc/Legacy -I../../Drivers/CMSIS/Device/ST/STM32N6xx/Include -I../../Drivers/STM32N6xx_HAL_Driver/Inc -I../../Secure_nsclib -x assembler-with-cpp -MMD -MP -MF"$(@:%.o=%.d)" -MT"$@" --specs=nano.specs -mfpu=fpv5-d16 -mfloat-abi=hard -mthumb -o "$@" "$<"

clean: clean-Core-2f-Startup

clean-Core-2f-Startup:
	-$(RM) ./Core/Startup/startup_stm32n657x0hxq.d ./Core/Startup/startup_stm32n657x0hxq.o

.PHONY: clean-Core-2f-Startup

