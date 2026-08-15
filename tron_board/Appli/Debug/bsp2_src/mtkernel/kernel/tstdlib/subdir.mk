################################################################################
# Automatically-generated file. Do not edit!
# Toolchain: GNU Tools for STM32 (14.3.rel1)
################################################################################

# Add inputs and outputs from these tool invocations to the build variables 
C_SRCS += \
/Users/harushsgtemkar/Downloads/tron-4/vendor/mtk3_bsp2/mtkernel/kernel/tstdlib/bitop.c \
/Users/harushsgtemkar/Downloads/tron-4/vendor/mtk3_bsp2/mtkernel/kernel/tstdlib/string.c 

OBJS += \
./bsp2_src/mtkernel/kernel/tstdlib/bitop.o \
./bsp2_src/mtkernel/kernel/tstdlib/string.o 

C_DEPS += \
./bsp2_src/mtkernel/kernel/tstdlib/bitop.d \
./bsp2_src/mtkernel/kernel/tstdlib/string.d 


# Each subdirectory must supply rules for building sources it contributes
bsp2_src/mtkernel/kernel/tstdlib/bitop.o: /Users/harushsgtemkar/Downloads/tron-4/vendor/mtk3_bsp2/mtkernel/kernel/tstdlib/bitop.c bsp2_src/mtkernel/kernel/tstdlib/subdir.mk
	arm-none-eabi-gcc "$<" -mcpu=cortex-m55 -std=gnu11 -g3 -DDEBUG -DUSE_HAL_DRIVER -DSTM32N657xx -D_STM32CUBE_DISCOVERY_N657_ -c -I../Core/Inc -I../../Secure_nsclib -I../../Drivers/STM32N6xx_HAL_Driver/Inc -I../../Drivers/CMSIS/Device/ST/STM32N6xx/Include -I../../Drivers/STM32N6xx_HAL_Driver/Inc/Legacy -I../../Drivers/CMSIS/Include -I/Users/harushsgtemkar/Downloads/tron-4/tron_board/Appli/../../core -I/Users/harushsgtemkar/Downloads/tron-4/tron_board/Appli/../../platform/board -I/Users/harushsgtemkar/Downloads/tron-4/tron_board/Appli/../../vendor/mtk3_bsp2/include -I/Users/harushsgtemkar/Downloads/tron-4/tron_board/Appli/../../vendor/mtk3_bsp2/config -I/Users/harushsgtemkar/Downloads/tron-4/tron_board/Appli/../../vendor/mtk3_bsp2/mtkernel -I/Users/harushsgtemkar/Downloads/tron-4/tron_board/Appli/../../vendor/mtk3_bsp2 -I/Users/harushsgtemkar/Downloads/tron-4/tron_board/Appli/../../vendor/mtk3_bsp2/mtkernel/kernel/knlinc -O0 -ffunction-sections -fdata-sections -Wall -fstack-usage -fcyclomatic-complexity -mcmse -MMD -MP -MF"$(@:%.o=%.d)" -MT"$@" --specs=nano.specs -mfpu=fpv5-d16 -mfloat-abi=hard -mthumb -o "$@"
bsp2_src/mtkernel/kernel/tstdlib/string.o: /Users/harushsgtemkar/Downloads/tron-4/vendor/mtk3_bsp2/mtkernel/kernel/tstdlib/string.c bsp2_src/mtkernel/kernel/tstdlib/subdir.mk
	arm-none-eabi-gcc "$<" -mcpu=cortex-m55 -std=gnu11 -g3 -DDEBUG -DUSE_HAL_DRIVER -DSTM32N657xx -D_STM32CUBE_DISCOVERY_N657_ -c -I../Core/Inc -I../../Secure_nsclib -I../../Drivers/STM32N6xx_HAL_Driver/Inc -I../../Drivers/CMSIS/Device/ST/STM32N6xx/Include -I../../Drivers/STM32N6xx_HAL_Driver/Inc/Legacy -I../../Drivers/CMSIS/Include -I/Users/harushsgtemkar/Downloads/tron-4/tron_board/Appli/../../core -I/Users/harushsgtemkar/Downloads/tron-4/tron_board/Appli/../../platform/board -I/Users/harushsgtemkar/Downloads/tron-4/tron_board/Appli/../../vendor/mtk3_bsp2/include -I/Users/harushsgtemkar/Downloads/tron-4/tron_board/Appli/../../vendor/mtk3_bsp2/config -I/Users/harushsgtemkar/Downloads/tron-4/tron_board/Appli/../../vendor/mtk3_bsp2/mtkernel -I/Users/harushsgtemkar/Downloads/tron-4/tron_board/Appli/../../vendor/mtk3_bsp2 -I/Users/harushsgtemkar/Downloads/tron-4/tron_board/Appli/../../vendor/mtk3_bsp2/mtkernel/kernel/knlinc -O0 -ffunction-sections -fdata-sections -Wall -fstack-usage -fcyclomatic-complexity -mcmse -MMD -MP -MF"$(@:%.o=%.d)" -MT"$@" --specs=nano.specs -mfpu=fpv5-d16 -mfloat-abi=hard -mthumb -o "$@"

clean: clean-bsp2_src-2f-mtkernel-2f-kernel-2f-tstdlib

clean-bsp2_src-2f-mtkernel-2f-kernel-2f-tstdlib:
	-$(RM) ./bsp2_src/mtkernel/kernel/tstdlib/bitop.cyclo ./bsp2_src/mtkernel/kernel/tstdlib/bitop.d ./bsp2_src/mtkernel/kernel/tstdlib/bitop.o ./bsp2_src/mtkernel/kernel/tstdlib/bitop.su ./bsp2_src/mtkernel/kernel/tstdlib/string.cyclo ./bsp2_src/mtkernel/kernel/tstdlib/string.d ./bsp2_src/mtkernel/kernel/tstdlib/string.o ./bsp2_src/mtkernel/kernel/tstdlib/string.su

.PHONY: clean-bsp2_src-2f-mtkernel-2f-kernel-2f-tstdlib

