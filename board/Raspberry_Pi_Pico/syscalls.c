/*
 * board/Raspberry_Pi_Pico/syscalls.c
 *
 * Minimal newlib syscall stubs for a bare-metal (no OS underneath)
 * link. Needed because linking against arm-none-eabi's libc/libm
 * (pulled in transitively by things like fabsf()/log1pf() in
 * core/scoring.c and core/watchdog.c, or by abort() inside libc
 * itself) requires these low-level "reentrant" syscalls to exist
 * somewhere, even on a target with no real filesystem or process
 * model. Confirmed none of these existed anywhere in this vendored
 * BSP for any board - the only syscall stub in the whole repo is
 * tron_board/Appli/Core/Src/sysmem.c's _sbrk(), which is an
 * STM32CubeIDE-generated file for a completely different toolchain
 * setup (that board's own managed CubeIDE project) and isn't
 * reachable from this board's plain-Makefile/terminal build.
 *
 * Only _sbrk() has a real implementation (a simple bump allocator
 * bounded by the linker script's __end symbol and RAM's top, same
 * shape as the DK's own CubeIDE-generated sysmem.c). Everything else
 * returns an error, which is correct and sufficient for this target:
 * this firmware doesn't use a filesystem or spawn processes, these
 * stubs exist purely to satisfy the linker for library code paths
 * that reference them but are never actually exercised at runtime
 * (e.g. an unused branch inside a libc function).
 */

#include <sys/machine.h>
#ifdef CPU_RP2040

#include <errno.h>
#include <stdint.h>
#include <sys/stat.h>
#include <sys/types.h>

/* Provided by the linker script (etc/linker/pico_rp2040/tkernel_map.ld):
 * end of .bss / start of free RAM. */
extern uint8_t __end;

/* RAM (xrw) : ORIGIN = 0x20000000, LENGTH = 256K, per the same linker
 * script - used only as an upper bound so a runaway allocation fails
 * cleanly instead of the heap silently colliding with the stack. */
#define RP2040_RAM_END ((uint8_t *)(0x20000000u + 256u * 1024u))

static uint8_t *heap_end = 0;

void *_sbrk(int incr)
{
    uint8_t *prev_heap_end;

    if (heap_end == 0)
    {
        heap_end = &__end;
    }
    prev_heap_end = heap_end;

    if (heap_end + incr > RP2040_RAM_END)
    {
        errno = ENOMEM;
        return (void *)-1;
    }

    heap_end += incr;
    return (void *)prev_heap_end;
}

int _write(int file, const char *ptr, int len)
{
    (void)file;
    (void)ptr;
    (void)len;
    errno = ENOSYS;
    return -1;
}

int _read(int file, char *ptr, int len)
{
    (void)file;
    (void)ptr;
    (void)len;
    errno = ENOSYS;
    return -1;
}

int _close(int file)
{
    (void)file;
    errno = ENOSYS;
    return -1;
}

int _fstat(int file, struct stat *st)
{
    (void)file;
    st->st_mode = S_IFCHR;
    return 0;
}

int _isatty(int file)
{
    (void)file;
    return 1;
}

int _lseek(int file, int ptr, int dir)
{
    (void)file;
    (void)ptr;
    (void)dir;
    return 0;
}

void _exit(int status)
{
    (void)status;
    while (1)
    {
        /* No OS to return to on this target - park here. */
    }
}

int _kill(int pid, int sig)
{
    (void)pid;
    (void)sig;
    errno = EINVAL;
    return -1;
}

int _getpid(void)
{
    return 1;
}

#endif /* CPU_RP2040 */
