#include "stm32f4xx.h"

/* 1 = run destructive flash self-test, 0 = normal boot */
#define RUN_FLASH_TEST   0

#define APP_BASE   0x08010000u   /* app lives here — sector 4 start */
#define APP_SECTOR 4
#define SRAM_START 0x20000000u   /* used to sanity-check app's initial SP */
#define SRAM_END   0x20020000u

/* Busy-wait; volatile stops the compiler from deleting it */
static void delay(volatile uint32_t n) { while (n--) __asm__("nop"); }


/* ---- App detection + jump ---- */

/* Word 0 of the image is the initial SP. If it points into SRAM, image looks valid. */
static int app_valid(void) {
    uint32_t sp = *(volatile uint32_t *)APP_BASE;
    return (sp >= SRAM_START) && (sp <= SRAM_END);
}

/* Cortex-M handoff: point VTOR at app, load its SP, branch to its reset handler */
static void jump_to_app(void) {
    uint32_t app_sp    = *(volatile uint32_t *)(APP_BASE);
    uint32_t app_reset = *(volatile uint32_t *)(APP_BASE + 4);

    SCB->VTOR = APP_BASE;
    __asm__ volatile ("msr msp, %0" : : "r" (app_sp));

    void (*app_entry)(void) = (void (*)(void)) app_reset;
    app_entry();
    while (1) { }
}


/* ---- Flash driver ---- */

/* Two magic keys in this order unlock FLASH->CR; wrong order hard-locks until reset */
static void flash_unlock(void) {
    if (FLASH->CR & FLASH_CR_LOCK) {
        FLASH->KEYR = 0x45670123;
        FLASH->KEYR = 0xCDEF89AB;
    }
}

static void flash_lock(void) {
    FLASH->CR |= FLASH_CR_LOCK;
}

/* BSY = 1 while an erase/program is in progress; never touch CR or start new op during this */
static void flash_wait_busy(void) {
    while (FLASH->SR & FLASH_SR_BSY) { }
}

/* Erase one sector. Erased flash reads 0xFF. Program can only flip 1->0, so erase is
 * the only way to get a bit back to 1. */
static void flash_erase_sector(uint32_t sector) {
    flash_wait_busy();

    /* Clear old error flags (rc_w1: write 1 to clear) */
    FLASH->SR = FLASH_SR_EOP | FLASH_SR_WRPERR
              | FLASH_SR_PGAERR | FLASH_SR_PGPERR | FLASH_SR_PGSERR;

    FLASH->CR &= ~FLASH_CR_PSIZE;
    FLASH->CR |=  FLASH_CR_PSIZE_1;             /* PSIZE = 32-bit (safe at 3.3V) */

    FLASH->CR &= ~FLASH_CR_SNB;
    FLASH->CR |= (sector << FLASH_CR_SNB_Pos) | FLASH_CR_SER;

    FLASH->CR |= FLASH_CR_STRT;                 /* GO */
    flash_wait_busy();

    FLASH->CR &= ~FLASH_CR_SER;
}

/* Program one 32-bit word. Target must currently be 0xFFFFFFFF and 4-byte aligned.
 * The store below is intercepted by the flash controller because PG=1. */
static void flash_program_word(uint32_t addr, uint32_t data) {
    flash_wait_busy();
    FLASH->CR &= ~FLASH_CR_PSIZE;
    FLASH->CR |=  FLASH_CR_PSIZE_1;
    FLASH->CR |=  FLASH_CR_PG;

    *(volatile uint32_t *)addr = data;

    flash_wait_busy();
    FLASH->CR &= ~FLASH_CR_PG;
}

/* Erase sector 4, write 4 known words, read back and verify. D2 solid = pass. */
static void run_flash_self_test(void) {
    const uint32_t test_words[4] = {
        0xDEADBEEF, 0xCAFEBABE, 0x12345678, 0xA5A5A5A5
    };

    flash_unlock();
    flash_erase_sector(APP_SECTOR);
    for (int i = 0; i < 4; i++) {
        flash_program_word(APP_BASE + i * 4, test_words[i]);
    }
    flash_lock();

    int ok = 1;
    for (int i = 0; i < 4; i++) {
        if (*(volatile uint32_t *)(APP_BASE + i * 4) != test_words[i]) {
            ok = 0;
            break;
        }
    }

    if (ok) {
        GPIOA->ODR &= ~(1u << 6);      /* D2 solid on (active low) */
        while (1) { }
    } else {
        while (1) {                     /* fast blink = fail */
            GPIOA->ODR ^= (1u << 6);
            delay(100000);
        }
    }
}


/* ---- Main ---- */

int main(void) {
    RCC->AHB1ENR |= RCC_AHB1ENR_GPIOAEN;
    GPIOA->MODER &= ~GPIO_MODER_MODER6;
    GPIOA->MODER |=  GPIO_MODER_MODER6_0;

    /* Intro blinks */
    for (int i = 0; i < 6; i++) {
        GPIOA->ODR ^= (1u << 6);
        delay(500000);
    }

#if RUN_FLASH_TEST
    run_flash_self_test();
#else
    /* Read K0 button - PA0*/
    int enter_update_mode=0;
    for(int i=0;i<50;i++)
    {
        int button_pressed = (GPIOA->IDR & (1u << 0)) !=0;
        if (button_pressed) {
            enter_update_mode=1;
            break;
        }
        delay(200000);
    }

     if (enter_update_mode) {
        /* Update mode — fast blink forever (later: UART receive + flash write) */
        while (1) {
            GPIOA->ODR ^= (1u << 6);
            delay(100000);
        }
    }
    
    /* Window expired without button press → normal boot */
    if (app_valid()) {
        jump_to_app();
    }

    while (1) {                              /* no app → fast blink */
        GPIOA->ODR ^= (1u << 6);
        delay(100000);
    }
#endif
}
