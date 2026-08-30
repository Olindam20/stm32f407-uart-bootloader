#include "stm32f4xx.h"    /* CMSIS: peripheral pointers + bit macros */


#define MAGIC_ADDR   ((volatile uint32_t *)0x2001FFF0)
#define MAGIC_VALUE  0xB007B007u

static void enter_bootloader(void) {
    *MAGIC_ADDR = MAGIC_VALUE;
    NVIC_SystemReset();
    while (1) { }   /* never reached — reset happens immediately */
}

/* Minimal UART init — just enough to receive the trigger byte */
static void uart_init(void) {
    RCC->APB2ENR |= RCC_APB2ENR_USART1EN;

    GPIOA->MODER &= ~(GPIO_MODER_MODER9 | GPIO_MODER_MODER10);
    GPIOA->MODER |=  (GPIO_MODER_MODER9_1 | GPIO_MODER_MODER10_1);
    GPIOA->AFR[1] &= ~((0xFu << 4) | (0xFu << 8));
    GPIOA->AFR[1] |=  ((7u << 4) | (7u << 8));

    USART1->BRR = (8u << 4) | 11u;
    USART1->CR1 = USART_CR1_TE | USART_CR1_RE | USART_CR1_UE;
}

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

    uart_init();

    while (1) {
            GPIOA->ODR ^= (1u << 6) | (1u << 7);
            for (volatile uint32_t i = 0; i < 2000000; i++) { }

            /* NEW: check for reprogram request while blinking */
            if (USART1->SR & USART_SR_RXNE) {
                uint8_t b = (uint8_t)USART1->DR;
                if (b == 0x7F) {
                    enter_bootloader();
                }
            }
        }
}
