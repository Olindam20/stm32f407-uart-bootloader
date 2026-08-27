#include "stm32f4xx.h"

int main(void)
{
    SCB->VTOR = 0x08010000;                 /* own our vector table */

    RCC->AHB1ENR |= RCC_AHB1ENR_GPIOAEN;    /* clock to GPIOA */

    /* PA6, PA7 as outputs */
    GPIOA->MODER &= ~(GPIO_MODER_MODER6 | GPIO_MODER_MODER7);
    GPIOA->MODER |=  (GPIO_MODER_MODER6_0 | GPIO_MODER_MODER7_0);

    while (1) {
        GPIOA->ODR ^= (1u << 6) | (1u << 7);   /* toggle both LEDs */
        for (volatile uint32_t i = 0; i < 2000000; i++) { }
    }
}
