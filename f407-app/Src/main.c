#include "stm32f4xx.h"    /* CMSIS: peripheral pointers + bit macros */

int main(void)
{
    /* App lives at 0x08010000, so its vector table does too.
     * Point VTOR here or any interrupt vectors to the wrong table.  */
    SCB->VTOR = 0x08010000;

    /* Peripherals boot unclocked — writes to GPIOA do nothing until this. */
    RCC->AHB1ENR |= RCC_AHB1ENR_GPIOAEN;

    /* PA6, PA7 as outputs. Clear-then-set on multi-bit fields to avoid
     * leaving stale bits from any previous mode.                       */
    GPIOA->MODER &= ~(GPIO_MODER_MODER6 | GPIO_MODER_MODER7);
    GPIOA->MODER |=  (GPIO_MODER_MODER6_0 | GPIO_MODER_MODER7_0);

    while (1)
    {
        GPIOA->ODR |=  (1u << 6);    /* PA6 ON  */
        GPIOA->ODR &= ~(1u << 7);   /* PA7 OFF */
        for (volatile uint32_t i = 0; i < 2000000; i++) { }

        GPIOA->ODR &= ~(1u << 6);   /* PA6 OFF */
        GPIOA->ODR |=  (1u << 7);   /* PA7 ON  */
        for (volatile uint32_t i = 0; i < 2000000; i++) { }
    }
}
