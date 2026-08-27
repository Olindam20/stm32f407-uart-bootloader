#include "stm32f4xx.h"

#define APP_BASE   0x08010000u
#define SRAM_START 0x20000000u
#define SRAM_END   0x20020000u


static void delay(volatile uint32_t n)
{
	while(n--)
  {
    __asm__("nop");
  }
}

static int app_valid(void)
{
  /* Word 0 of the app image is its initial stack pointer.
     * If it points into SRAM, we treat the image as valid.       */
    uint32_t sp = *(volatile uint32_t *)APP_BASE;
    return (sp >= SRAM_START) && (sp <= SRAM_END);
}


static void jump_to_app(void) {
    uint32_t app_sp    = *(volatile uint32_t *)(APP_BASE);
    uint32_t app_reset = *(volatile uint32_t *)(APP_BASE + 4);

    /* Point the interrupt vector table at the app's table */
    SCB->VTOR = APP_BASE;

    /* Set the main stack pointer to the app's initial SP */
    __asm__ volatile ("msr msp, %0" : : "r" (app_sp));

    /* Jump to the app's reset handler */
    void (*app_entry)(void) = (void (*)(void)) app_reset;
    app_entry();

    while (1) { }  /* unreachable */
}

int main(void)
{
    RCC->AHB1ENR |= RCC_AHB1ENR_GPIOAEN;

    /* PA6 as output — bootloader owns only D2 */
    GPIOA->MODER &= ~GPIO_MODER_MODER6;
    GPIOA->MODER |=  GPIO_MODER_MODER6_0;

    /* Blink PA6 a few times to visibly announce "bootloader running" */
    for (int i = 0; i < 6; i++) {
        GPIOA->ODR ^= (1u << 6);
        delay(500000);
    }

    /* Then hand over to the app, if one is present */
    if (app_valid()) {
        jump_to_app();
    }

    /* No valid app: stay here, blink fast to show "no app found" */
    while (1) {
        GPIOA->ODR ^= (1u << 6);
        delay(100000);
    }
}
