/*
 *----------------------------------------------------------------------
 *    micro T-Kernel 3.0 BSP
 *
 *    Copyright (C) 2021-2022 by Ken Sakamura.
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 *
 *    Released by TRON Forum(http://www.tron.org) at 2022/11.
 *
 *----------------------------------------------------------------------
 */

/*
 * Eclipse-CDT-only workaround (Track 2 / Raspberry Pi Pico build):
 * this project's "Cross GCC Assembler -> General -> Other assembler
 * flags" field unconditionally wraps whatever's typed there in
 * -Wa,..., which only reaches the real `as` assembler AFTER the C
 * preprocessor pass has already run - so a plain -D_PICO_RP2040_
 * from that field never actually defines this macro in time for the
 * #include below, and the whole rest of this file (gated on
 * CPU_CORE_ARMV6M, which sys/machine.h only defines when
 * _PICO_RP2040_ is already set) silently compiles to nothing,
 * dropping knl_dispatch_entry and breaking the final link.
 *
 * The terminal build (build_pico_elf.sh) passes -D_PICO_RP2040_
 * correctly and doesn't need this - the #ifndef below is a no-op
 * there. This is purely a workaround for a real limitation in this
 * specific toolchain definition's Eclipse UI, not a change to what
 * this file actually needs to build correctly.
 */
#ifndef _PICO_RP2040_
#define _PICO_RP2040_
#define THIS_BOARD_ID 1
#endif

#include <sys/machine.h>
#ifdef CPU_CORE_ARMV6M
/*
 *	dispatch.S (ARMv6-M)
 *	Dispatcher
 */

#define	_in_asm_source_

#include <sys/machine.h>
#include <tk/errno.h>
#include <sys/sysdef.h>
#include <sys/knldef.h>

#include "offset.h"

	.code 16
	.syntax unified
	.thumb

/* ------------------------------------------------------------------------ */
/*
 * Dispatcher
 *
 *	Contexts to save
 *	Save registers except for ssp to a stack. Save 'ssp' to TCB.
 *
 *	   High Address +---------------+
 *			| xpsr		|
 *			| pc		| Return address
 *			| lr		|
 *			| R12		|
 *			| R0-R3		|
 *			+---------------+ Save by Exception entry process.
 *			| R4 - R11	|
 *		ssp ->	| lr		|
 *	    Low Address +---------------+
 *
 */

	.text
	.align 2
	.thumb
	.thumb_func
	.globl Csym(knl_dispatch_entry)

Csym(knl_dispatch_entry):	
/*----------------- Start dispatch processing. -----------------*/
	ldr	r0, =Csym(knl_dispatch_disabled)
	ldr	r1, =1
	str	r1, [r0]			// Dispatch disable

	ldr	r0, =Csym(knl_ctxtsk)
	ldr	r1, [r0]			// R1 = ctxtsk
	cmp	r1, #0
	bne	l_dispatch_000

	ldr	r2, =(Csym(knl_tmp_stack) + TMP_STACK_SIZE)	// Set temporal stack
	mov	sp, r2
	b	l_dispatch_100

/*----------------- Save "ctxtsk" context. -----------------*/
l_dispatch_000: 
	push	{r4-r7}
	mov	r0, r8
	mov	r1, r9
	mov	r2, r10
	mov	r3, r11
	push	{r0-r3}
	push	{lr}

	ldr	r0, =Csym(knl_ctxtsk)
	ldr	r1, [r0]			// R1 = ctxtsk
	mov	r2, sp
	str	r2, [r1, #TCB_tskctxb + CTXB_ssp]	// Save 'ssp' to TCB

	ldr	r2, =0
	str	r2, [r0]			// ctxtsk = NULL


/*----------------- Dispatch from "ctxtsk" to "schedtsk" -----------------*/
l_dispatch_100:
	ldr	r5, =Csym(knl_schedtsk)		// R5 = &schedtsk
	ldr	r6, =Csym(knl_lowpow_discnt)	// R6 = &lowpow_discnt

l_dispatch_110:
	ldr	r2, =1				// Disable interruput
	msr	primask, r2

	ldr	r7, [r5]			// R7 = schedtsk
	cmp	r7, #0				// Is there 'schedtsk'?
	bne	l_dispatch_130

	/* Moves to power saving mode because there are no tasks that can be run. */
	ldr	r2, [r6]			// Is 'low_pow' disabled?
	cmp	r2, #0
	beq	l_dispatch_120
	bl	Csym(low_pow)			// call low_pow()

l_dispatch_120:
	ldr	r2, =0
	msr	primask, r2			// Enable interruput

	b	l_dispatch_110

l_dispatch_130:					// Switch to 'schedtsk'
	str	r7, [r0]			// ctxtsk = schedtsk
	ldr	r2, [r7, #TCB_tskctxb + CTXB_ssp]	// Restore 'ssp' from TCB
	mov	sp, r2

/*----------------- Restore "schedtsk" context. -----------------*/

	pop	{r0}
	mov	lr, r0
	pop	{r0-r3}
	mov	r11, r3
	mov	r10, r2
	mov	r9, r1
	mov	r8, r0
	pop	{r4-r7}

	ldr	r0, =Csym(knl_dispatch_disabled)
	ldr	r1, =0
	str	r1, [r0]			// Dispatch enable

	msr	primask, r1			// Enable inperrupt

	bx	lr

#endif /* CPU_CORE_ARMV6M */