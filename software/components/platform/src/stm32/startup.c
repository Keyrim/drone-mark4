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

/* Peripheral handlers referenced by the vector table default to the trap
 * above; a strong definition next to the driver that owns the IRQ takes
 * over at link time. */
void TIM3_IRQHandler(void) __attribute__((weak, alias("Default_Handler")));
void USART1_IRQHandler(void) __attribute__((weak, alias("Default_Handler")));

/* Defined at the bottom of this file; Reset_Handler needs its address to
 * point VTOR at it. */
extern void *const g_vector_table[];

void Reset_Handler(void)
{
    /* VTOR first, before anything can fault or interrupt: the linker placed
     * this image's table wherever its flash window starts (0x08000000 for
     * drone_boot, slot base + 512 for a firmware slot), so every image
     * points the core at its own vectors and none of them depends on being
     * the image the reset vector fetched. The bootloader sets VTOR again
     * before it jumps; belt and suspenders, and either one alone is enough. */
    volatile uint32_t *const vtor = (volatile uint32_t *)0xE000ED08u;
    *vtor = (uint32_t)&g_vector_table[0];
    __asm volatile("dsb" ::: "memory");
    __asm volatile("isb" ::: "memory");

    /* PRIMASK next: a cold reset arrives with interrupts enabled, but the
     * bootloader masks them (cpsid i) for its own jump sequence and a
     * handed-over image must not inherit that. Without this, every
     * interrupt pends forever, WFI wakes and nothing runs the handlers -
     * the flight loop spins on a tick counter no one increments. Safe this
     * early: the NVIC has nothing enabled until the services init. */
    __asm volatile("cpsie i" ::: "memory");

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

/* 16 system exceptions plus the peripheral IRQs up to the highest one in
 * use (USART1, position 37 in RM0090 table 61). Unused positions trap in
 * Default_Handler; extend the table when a driver needs a higher IRQ. */
__attribute__((section(".isr_vector"), used)) void *const g_vector_table[16 + 38] = {
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
    0,                         /* reserved */
    (void *)Default_Handler,   /* SVCall */
    (void *)Default_Handler,   /* DebugMonitor */
    0,                         /* reserved */
    (void *)Default_Handler,   /* PendSV */
    (void *)Default_Handler,   /* SysTick */
    (void *)Default_Handler,   /* 0: WWDG */
    (void *)Default_Handler,   /* 1: PVD */
    (void *)Default_Handler,   /* 2: TAMP_STAMP */
    (void *)Default_Handler,   /* 3: RTC_WKUP */
    (void *)Default_Handler,   /* 4: FLASH */
    (void *)Default_Handler,   /* 5: RCC */
    (void *)Default_Handler,   /* 6: EXTI0 */
    (void *)Default_Handler,   /* 7: EXTI1 */
    (void *)Default_Handler,   /* 8: EXTI2 */
    (void *)Default_Handler,   /* 9: EXTI3 */
    (void *)Default_Handler,   /* 10: EXTI4 */
    (void *)Default_Handler,   /* 11: DMA1_Stream0 */
    (void *)Default_Handler,   /* 12: DMA1_Stream1 */
    (void *)Default_Handler,   /* 13: DMA1_Stream2 */
    (void *)Default_Handler,   /* 14: DMA1_Stream3 */
    (void *)Default_Handler,   /* 15: DMA1_Stream4 */
    (void *)Default_Handler,   /* 16: DMA1_Stream5 */
    (void *)Default_Handler,   /* 17: DMA1_Stream6 */
    (void *)Default_Handler,   /* 18: ADC */
    (void *)Default_Handler,   /* 19: CAN1_TX */
    (void *)Default_Handler,   /* 20: CAN1_RX0 */
    (void *)Default_Handler,   /* 21: CAN1_RX1 */
    (void *)Default_Handler,   /* 22: CAN1_SCE */
    (void *)Default_Handler,   /* 23: EXTI9_5 */
    (void *)Default_Handler,   /* 24: TIM1_BRK_TIM9 */
    (void *)Default_Handler,   /* 25: TIM1_UP_TIM10 */
    (void *)Default_Handler,   /* 26: TIM1_TRG_COM_TIM11 */
    (void *)Default_Handler,   /* 27: TIM1_CC */
    (void *)Default_Handler,   /* 28: TIM2 */
    (void *)TIM3_IRQHandler,   /* 29: TIM3, paces the sensor loop */
    (void *)Default_Handler,   /* 30: TIM4 */
    (void *)Default_Handler,   /* 31: I2C1_EV */
    (void *)Default_Handler,   /* 32: I2C1_ER */
    (void *)Default_Handler,   /* 33: I2C2_EV */
    (void *)Default_Handler,   /* 34: I2C2_ER */
    (void *)Default_Handler,   /* 35: SPI1 */
    (void *)Default_Handler,   /* 36: SPI2 */
    (void *)USART1_IRQHandler, /* 37: USART1, telemetry TX ring */
};
