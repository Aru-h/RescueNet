################################################################################
# Automatically-generated file. Do not edit!
# Toolchain: GNU Tools for STM32 (14.3.rel1)
################################################################################

# Add inputs and outputs from these tool invocations to the build variables 
C_SRCS += \
/Users/harushsgtemkar/projects/Tron\ project/Tron\ project\ Code/tron_merged-board-2/board/STM32N6570-DK/vendor/mtk3_bsp2/mtkernel/lib/libtm/libtm.c \
/Users/harushsgtemkar/projects/Tron\ project/Tron\ project\ Code/tron_merged-board-2/board/STM32N6570-DK/vendor/mtk3_bsp2/mtkernel/lib/libtm/libtm_printf.c 

OBJS += \
./board_src/vendor/mtk3_bsp2/mtkernel/lib/libtm/libtm.o \
./board_src/vendor/mtk3_bsp2/mtkernel/lib/libtm/libtm_printf.o 

C_DEPS += \
./board_src/vendor/mtk3_bsp2/mtkernel/lib/libtm/libtm.d \
./board_src/vendor/mtk3_bsp2/mtkernel/lib/libtm/libtm_printf.d 


# Each subdirectory must supply rules for building sources it contributes
board_src/vendor/mtk3_bsp2/mtkernel/lib/libtm/libtm.o: /Users/harushsgtemkar/projects/Tron\ project/Tron\ project\ Code/tron_merged-board-2/board/STM32N6570-DK/vendor/mtk3_bsp2/mtkernel/lib/libtm/libtm.c board_src/vendor/mtk3_bsp2/mtkernel/lib/libtm/subdir.mk
	arm-none-eabi-gcc "$<" -mcpu=cortex-m55 -std=gnu11 -g3 -DDEBUG -DUSE_HAL_DRIVER -DSTM32N657xx -D_STM32CUBE_DISCOVERY_N657_ -c -I../Core/Inc -I"/Users/harushsgtemkar/projects/Tron project/Tron project Code/tron_merged-board-2/tron_board/Appli/../../core/vendor/sx126x_driver/src" -I../../Secure_nsclib -I../../Drivers/STM32N6xx_HAL_Driver/Inc -I../../Drivers/CMSIS/Device/ST/STM32N6xx/Include -I../../Drivers/STM32N6xx_HAL_Driver/Inc/Legacy -I../../Drivers/CMSIS/Include -I"/Users/harushsgtemkar/projects/Tron project/Tron project Code/tron_merged-board-2/tron_board/Appli/../../core" -I"/Users/harushsgtemkar/projects/Tron project/Tron project Code/tron_merged-board-2/tron_board/Appli/../../board/STM32N6570-DK" -I"/Users/harushsgtemkar/projects/Tron project/Tron project Code/tron_merged-board-2/tron_board/Appli/../../board/STM32N6570-DK/vendor/mtk3_bsp2/include" -I"/Users/harushsgtemkar/projects/Tron project/Tron project Code/tron_merged-board-2/tron_board/Appli/../../board/STM32N6570-DK/vendor/mtk3_bsp2/config" -I"/Users/harushsgtemkar/projects/Tron project/Tron project Code/tron_merged-board-2/tron_board/Appli/../../board/STM32N6570-DK/vendor/mtk3_bsp2/mtkernel" -I"/Users/harushsgtemkar/projects/Tron project/Tron project Code/tron_merged-board-2/tron_board/Appli/../../board/STM32N6570-DK/vendor/mtk3_bsp2" -I"/Users/harushsgtemkar/projects/Tron project/Tron project Code/tron_merged-board-2/tron_board/Appli/../../board/STM32N6570-DK/vendor/mtk3_bsp2/mtkernel/kernel/knlinc" -O0 -ffunction-sections -fdata-sections -Wall -fstack-usage -fcyclomatic-complexity -mcmse -MMD -MP -MF"$(@:%.o=%.d)" -MT"$@" --specs=nano.specs -mfpu=fpv5-d16 -mfloat-abi=hard -mthumb -o "$@"
board_src/vendor/mtk3_bsp2/mtkernel/lib/libtm/libtm_printf.o: /Users/harushsgtemkar/projects/Tron\ project/Tron\ project\ Code/tron_merged-board-2/board/STM32N6570-DK/vendor/mtk3_bsp2/mtkernel/lib/libtm/libtm_printf.c board_src/vendor/mtk3_bsp2/mtkernel/lib/libtm/subdir.mk
	arm-none-eabi-gcc "$<" -mcpu=cortex-m55 -std=gnu11 -g3 -DDEBUG -DUSE_HAL_DRIVER -DSTM32N657xx -D_STM32CUBE_DISCOVERY_N657_ -c -I../Core/Inc -I"/Users/harushsgtemkar/projects/Tron project/Tron project Code/tron_merged-board-2/tron_board/Appli/../../core/vendor/sx126x_driver/src" -I../../Secure_nsclib -I../../Drivers/STM32N6xx_HAL_Driver/Inc -I../../Drivers/CMSIS/Device/ST/STM32N6xx/Include -I../../Drivers/STM32N6xx_HAL_Driver/Inc/Legacy -I../../Drivers/CMSIS/Include -I"/Users/harushsgtemkar/projects/Tron project/Tron project Code/tron_merged-board-2/tron_board/Appli/../../core" -I"/Users/harushsgtemkar/projects/Tron project/Tron project Code/tron_merged-board-2/tron_board/Appli/../../board/STM32N6570-DK" -I"/Users/harushsgtemkar/projects/Tron project/Tron project Code/tron_merged-board-2/tron_board/Appli/../../board/STM32N6570-DK/vendor/mtk3_bsp2/include" -I"/Users/harushsgtemkar/projects/Tron project/Tron project Code/tron_merged-board-2/tron_board/Appli/../../board/STM32N6570-DK/vendor/mtk3_bsp2/config" -I"/Users/harushsgtemkar/projects/Tron project/Tron project Code/tron_merged-board-2/tron_board/Appli/../../board/STM32N6570-DK/vendor/mtk3_bsp2/mtkernel" -I"/Users/harushsgtemkar/projects/Tron project/Tron project Code/tron_merged-board-2/tron_board/Appli/../../board/STM32N6570-DK/vendor/mtk3_bsp2" -I"/Users/harushsgtemkar/projects/Tron project/Tron project Code/tron_merged-board-2/tron_board/Appli/../../board/STM32N6570-DK/vendor/mtk3_bsp2/mtkernel/kernel/knlinc" -O0 -ffunction-sections -fdata-sections -Wall -fstack-usage -fcyclomatic-complexity -mcmse -MMD -MP -MF"$(@:%.o=%.d)" -MT"$@" --specs=nano.specs -mfpu=fpv5-d16 -mfloat-abi=hard -mthumb -o "$@"

clean: clean-board_src-2f-vendor-2f-mtk3_bsp2-2f-mtkernel-2f-lib-2f-libtm

clean-board_src-2f-vendor-2f-mtk3_bsp2-2f-mtkernel-2f-lib-2f-libtm:
	-$(RM) ./board_src/vendor/mtk3_bsp2/mtkernel/lib/libtm/libtm.cyclo ./board_src/vendor/mtk3_bsp2/mtkernel/lib/libtm/libtm.d ./board_src/vendor/mtk3_bsp2/mtkernel/lib/libtm/libtm.o ./board_src/vendor/mtk3_bsp2/mtkernel/lib/libtm/libtm.su ./board_src/vendor/mtk3_bsp2/mtkernel/lib/libtm/libtm_printf.cyclo ./board_src/vendor/mtk3_bsp2/mtkernel/lib/libtm/libtm_printf.d ./board_src/vendor/mtk3_bsp2/mtkernel/lib/libtm/libtm_printf.o ./board_src/vendor/mtk3_bsp2/mtkernel/lib/libtm/libtm_printf.su

.PHONY: clean-board_src-2f-vendor-2f-mtk3_bsp2-2f-mtkernel-2f-lib-2f-libtm

