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

extern UART_HandleTypeDef huart1;

/* Bug fix: DBG_PRINT used to route every debug line through snprintf()
   (newlib's reentrant _svfprintf_r/__ssputs_r under the hood - confirmed
   by a live debugger call stack after a real crash on real hardware
   inside one of these calls, in JumpToApplication() - see that
   function's own comment). That machinery isn't reliably safe this
   early in a bare-metal boot: the JumpToApplication() call landed in
   unmapped code after a stack/VTOR switch, and a completely separate
   one - this file's very first DBG_PRINT, before ANY snprintf call had
   run yet this boot, in BOOT_Application() - was confirmed on real
   hardware to hang forever in Flash Boot despite working every single
   time under a debugger-attached Dev_boot session. Replaced every
   DBG_PRINT call below with these three tiny helpers - direct
   HAL_UART_Transmit of a literal, or of a manually hex/decimal-
   formatted value - the exact same mechanism Core/Src/main.c's own
   FSBL_UART_PUTS() already uses successfully at every earlier boot
   stage in both boot modes. No snprintf, no newlib reentrancy, nothing
   routes through libc's stdio subsystem at all. */
static void dbg_puts(const char *s)
{
  uint16_t len = 0u;
  while (s[len] != '\0')
  {
    len++;
  }
  HAL_UART_Transmit(&huart1, (uint8_t *)s, len, HAL_MAX_DELAY);
}

static void dbg_put_hex32(uint32_t v)
{
  static const char hexd[16] = "0123456789ABCDEF";
  char buf[8];
  int i;

  for (i = 7; i >= 0; i--)
  {
    buf[i] = hexd[v & 0xFu];
    v >>= 4;
  }
  HAL_UART_Transmit(&huart1, (uint8_t *)buf, 8u, HAL_MAX_DELAY);
}

static void dbg_put_dec(int v)
{
  char buf[12];
  int i = (int)sizeof(buf);
  int neg = (v < 0);
  unsigned int uv = neg ? (unsigned int)(-v) : (unsigned int)v;

  buf[--i] = '\0';
  do
  {
    buf[--i] = (char)('0' + (uv % 10u));
    uv /= 10u;
  } while (uv != 0u);
  if (neg)
  {
    buf[--i] = '-';
  }
  dbg_puts(&buf[i]);
}

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
  dbg_puts("BOOT: calling MapMemory\r\n");
  retr = MapMemory();
  dbg_puts("BOOT: MapMemory returned ");
  dbg_put_dec((int)retr);
  dbg_puts("\r\n");
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
    dbg_puts("BOOT: MemoryMappedMode(");
    dbg_put_dec((int)index);
    dbg_puts(") = ");
    dbg_put_dec((int)mapres);
    dbg_puts(", hxspi2.ErrorCode=0x");
    dbg_put_hex32((uint32_t)hxspi2.ErrorCode);
    dbg_puts(", State=");
    dbg_put_dec((int)hxspi2.State);
    dbg_puts("\r\n");
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

  dbg_puts("BOOT: entered JumpToApplication\r\n");
  if (EXTMEM_OK != EXTMEM_GetMapAddress(EXTMEM_MEMORY_BOOTXIP, &Application_vector))
  {
    dbg_puts("BOOT: GetMapAddress FAILED\r\n");
    return BOOT_ERROR_INCOMPATIBLEMEMORY;
  }
  dbg_puts("BOOT: base=0x");
  dbg_put_hex32((uint32_t)Application_vector);
  dbg_puts(" offset=0x");
  dbg_put_hex32((uint32_t)EXTMEM_XIP_IMAGE_OFFSET);
  dbg_puts("+0x");
  dbg_put_hex32((uint32_t)EXTMEM_HEADER_OFFSET);
  dbg_puts("\r\n");
  dbg_puts("BOOT: SP=0x");
  dbg_put_hex32(*(__IO uint32_t *)(Application_vector + EXTMEM_XIP_IMAGE_OFFSET + EXTMEM_HEADER_OFFSET));
  dbg_puts(" PC=0x");
  dbg_put_hex32(*(__IO uint32_t *)(Application_vector + EXTMEM_XIP_IMAGE_OFFSET + EXTMEM_HEADER_OFFSET + 4u));
  dbg_puts("\r\n");

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

  /* Bug fix: a DBG_PRINT() used to sit here, after SCB->VTOR (line ~174)
     and the stack pointer (line 188 above) were already switched to the
     Appli's own (not-yet-running) context. DBG_PRINT's snprintf() needs
     a working stack, and any fault during it - even a small UART timing
     hiccup - would vector through the Appli's exception table instead
     of the FSBL's, landing in unmapped/untracked code (confirmed on
     real hardware: a SIGTRAP into an address with "no debug information
     available", inside snprintf/_svfprintf_r/__ssputs_r, right at this
     line). Removed rather than moved: JumpToApp is the exact same
     address already printed two lines above the MapMemory print block
     ("BOOT: SP=... PC=..." - see that DBG_PRINT), so this told us
     nothing new anyway. */
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
