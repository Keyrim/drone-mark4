/* Minimal Cortex-M4F (STM32F405) startup.
 * Vector table reduced to the 16 system exceptions: enough to link, not to
 * serve peripheral IRQs yet. */

#include <stdint.h>

extern uint32_t _sidata; /* start of the .data image in FLASH */
extern uint32_t _sdata;  /* start of .data in RAM */
extern uint32_t _edata;
extern uint32_t _sbss;
extern uint32_t _ebss;
extern uint32_t _estack;

extern int main(void);
extern void __libc_init_array(void);

/* Required by cxa_atexit when a static has a destructor (no crtbegin with
 * -nostartfiles). */
void *__dso_handle __attribute__((weak));

/* newlib's __libc_init_array calls _init; crti.o is not linked
 * (-nostartfiles), so provide empty stubs. */
void _init(void)
{
}
void _fini(void)
{
}

void Default_Handler(void)
{
    for (;;)
    {
    }
}

void Reset_Handler(void)
{
    /* FPU: enable CP10/CP11 before the first float instruction
     * (-mfloat-abi=hard). */
    volatile uint32_t *const cpacr = (volatile uint32_t *)0xE000ED88u;
    *cpacr |= (0xFu << 20u);

    const uint32_t *src = &_sidata;
    uint32_t *dst = &_sdata;
    while (dst < &_edata)
    {
        *dst++ = *src++;
    }
    for (dst = &_sbss; dst < &_ebss;)
    {
        *dst++ = 0u;
    }

    __libc_init_array();
    (void)main();
    for (;;)
    {
    }
}

__attribute__((section(".isr_vector"), used)) void *const g_vector_table[16] = {
    (void *)&_estack,        /* MSP initial */
    (void *)Reset_Handler,   /* Reset */
    (void *)Default_Handler, /* NMI */
    (void *)Default_Handler, /* HardFault */
    (void *)Default_Handler, /* MemManage */
    (void *)Default_Handler, /* BusFault */
    (void *)Default_Handler, /* UsageFault */
    0,
    0,
    0,
    0,                       /* reserves */
    (void *)Default_Handler, /* SVCall */
    (void *)Default_Handler, /* DebugMonitor */
    0,                       /* reserve */
    (void *)Default_Handler, /* PendSV */
    (void *)Default_Handler, /* SysTick */
};
