/**
  ******************************************************************************
  * @file    stm32_boot_xip.c
  * @author  MCD Application Team
  * @brief   This file manages booting in execute-in-place (XIP) mode.
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2022 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */

/* Includes ------------------------------------------------------------------*/
#include "stm32_boot_xip.h"
#include "stm32_extmem_conf.h"
#include <stdio.h>

extern UART_HandleTypeDef huart1;
#define DBG_PRINT(...) do { char dbg_buf[96]; int dbg_len = snprintf(dbg_buf, sizeof(dbg_buf), __VA_ARGS__); HAL_UART_Transmit(&huart1, (uint8_t *)dbg_buf, (uint16_t)dbg_len, HAL_MAX_DELAY); } while (0)

/** @addtogroup BOOT
  * @{
  */

/** @addtogroup BOOT_XIP
  * @{
  */

/* Private typedefs ----------------------------------------------------------*/
/* Private defines -----------------------------------------------------------*/

/* Offset of the image from the boot memory base */
#ifndef EXTMEM_XIP_IMAGE_OFFSET
#define EXTMEM_XIP_IMAGE_OFFSET 0
#endif /* EXTMEM_XIP_IMAGE_OFFSET */

/* Offset of the vector table from the start of the image */
#ifndef EXTMEM_HEADER_OFFSET
#define EXTMEM_HEADER_OFFSET 0
#endif /* EXTMEM_HEADER_OFFSET */

/* Private macros ------------------------------------------------------------*/
/* Private variables ---------------------------------------------------------*/
/* Private function prototypes -----------------------------------------------*/
BOOTStatus_TypeDef JumpToApplication(void);
BOOTStatus_TypeDef MapMemory(void);
BOOTStatus_TypeDef GetBaseAddress(uint32_t MemIndex, uint32_t *BaseAddress);

/**
  *  @addtogroup BOOT_XIP_Exported_Functions Boot XIP exported functions
  * @{
  */

/**
  * @brief Boots the application by mapping the memory and jumping to the application.
  * @retval BOOTStatus_TypeDef Status of the operation.
  */
BOOTStatus_TypeDef BOOT_Application(void)
{
  BOOTStatus_TypeDef retr;

  /* Mount the memory */
  DBG_PRINT("BOOT: calling MapMemory\r\n");
  retr = MapMemory();
  DBG_PRINT("BOOT: MapMemory returned %d\r\n", (int)retr);
  if (BOOT_OK == retr)
  {
    /* Jump on the application */
    retr = JumpToApplication();
  }
  return retr;
}

/**
  * @}
  */

/**
  *  @defgroup BOOT_XIP_Private_Functions Boot XIP private functions
  * @{
  */

/**
  * @brief  Maps the external memory.
  * @retval BOOTStatus_TypeDef Status of the operation.
  */
BOOTStatus_TypeDef MapMemory(void)
{
  BOOTStatus_TypeDef retr = BOOT_OK;

  /* Map all the memory */
  for (uint8_t index = 0; index < (sizeof(extmem_list_config) / sizeof(EXTMEM_DefinitionTypeDef)); index++)
  {
    EXTMEM_StatusTypeDef mapres = EXTMEM_MemoryMappedMode(index, EXTMEM_ENABLE);
    DBG_PRINT("BOOT: MemoryMappedMode(%d) = %d, hxspi2.ErrorCode=0x%08lX, State=%d\r\n",
              (int)index, (int)mapres, (unsigned long)hxspi2.ErrorCode, (int)hxspi2.State);
    switch (mapres)
    {
      case EXTMEM_ERROR_NOTSUPPORTED :
        if (EXTMEM_MEMORY_BOOTXIP ==  index)
        {
          retr = BOOT_ERROR_INCOMPATIBLEMEMORY;
        }
        else
        {
          /* We consider the memory will be not used any more */
          EXTMEM_DeInit(index);
        }
      case EXTMEM_OK:
        break;
      default :
        retr = BOOT_ERROR_MAPPEDMODEFAIL;
        break;
    }
  }
  return retr;
}

/**
  * @brief  Jumps to the application using its vector table.
  * @retval BOOTStatus_TypeDef Status of the operation.
  */
BOOTStatus_TypeDef JumpToApplication(void)
{
  uint32_t primask_bit;
  typedef  void (*pFunction)(void);
  static pFunction JumpToApp;
  uint32_t Application_vector;

  DBG_PRINT("BOOT: entered JumpToApplication\r\n");
  if (EXTMEM_OK != EXTMEM_GetMapAddress(EXTMEM_MEMORY_BOOTXIP, &Application_vector))
  {
    DBG_PRINT("BOOT: GetMapAddress FAILED\r\n");
    return BOOT_ERROR_INCOMPATIBLEMEMORY;
  }
  DBG_PRINT("BOOT: base=0x%08lX offset=0x%08lX+0x%08lX\r\n",
            (unsigned long)Application_vector, (unsigned long)EXTMEM_XIP_IMAGE_OFFSET, (unsigned long)EXTMEM_HEADER_OFFSET);
  DBG_PRINT("BOOT: SP=0x%08lX PC=0x%08lX\r\n",
            (unsigned long)(*(__IO uint32_t *)(Application_vector + EXTMEM_XIP_IMAGE_OFFSET + EXTMEM_HEADER_OFFSET)),
            (unsigned long)(*(__IO uint32_t *)(Application_vector + EXTMEM_XIP_IMAGE_OFFSET + EXTMEM_HEADER_OFFSET + 4u)));

  /* Suspend SysTick */
  HAL_SuspendTick();

#if defined(__ICACHE_PRESENT) && (__ICACHE_PRESENT == 1U)
  /* if I-Cache is enabled, disable I-Cache-----------------------------------*/
  if (SCB->CCR & SCB_CCR_IC_Msk)
  {
    SCB_DisableICache();
  }
#endif /* __ICACHE_PRESENT */

#if defined(__DCACHE_PRESENT) && (__DCACHE_PRESENT == 1U)
  /* if D-Cache is enabled, disable D-Cache-----------------------------------*/
  if (SCB->CCR & SCB_CCR_DC_Msk)
  {
    SCB_DisableDCache();
  }
#endif /* __DCACHE_PRESENT */

  /* Initialize user application's Stack Pointer & Jump to user application  */
  primask_bit = __get_PRIMASK();
  __disable_irq();

  /* Apply offsets for image location and vector table offset */
  Application_vector += EXTMEM_XIP_IMAGE_OFFSET + EXTMEM_HEADER_OFFSET;

  SCB->VTOR = (uint32_t)Application_vector;
  JumpToApp = (pFunction)(*(__IO uint32_t *)(Application_vector + 4u));

#if (defined(__ARM_ARCH_8M_MAIN__ ) && (__ARM_ARCH_8M_MAIN__ == 1))
  /* on ARM v8m, set MSPLIM before setting MSP to avoid unwanted stack overflow faults */
  __set_MSPLIM(0x00000000);
#elif (defined(__ARM_ARCH_8_1M_MAIN__ ) && (__ARM_ARCH_8_1M_MAIN__ == 1))
  /* on ARM v8m, set MSPLIM before setting MSP to avoid unwanted stack overflow faults */
  __set_MSPLIM(0x00000000);
#elif (defined(__ARM_ARCH_8M_BASE__ ) && (__ARM_ARCH_8M_BASE__ == 1))
  /* on ARM v8m, set MSPLIM before setting MSP to avoid unwanted stack overflow faults */
  __set_MSPLIM(0x00000000);
#endif  /* __ARM_ARCH_8M_MAIN__ or __ARM_ARCH_8_1M_MAIN__ or __ARM_ARCH_8M_BASE__ */

  __set_MSP(*(__IO uint32_t *) Application_vector);

  /* Re-enable the interrupts */
  __set_PRIMASK(primask_bit);

  DBG_PRINT("BOOT: jumping now to 0x%08lX\r\n", (unsigned long)JumpToApp);
  JumpToApp();
  return BOOT_OK;
}

/**
  * @}
  */

/**
  * @}
 */

/**
  * @}
  */
