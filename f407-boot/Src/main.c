#include "stm32f4xx.h"

/* ---- Protocol constants ---- */
#define SYNC        0x7F
#define CMD_PING    0x01
#define CMD_ERASE   0x02
#define CMD_WRITE   0x03
#define CMD_VERIFY  0x04
#define CMD_JUMP    0x05
#define ACK         0x79
#define NACK        0x1F

#define APP_START   0x08010000u /* app lives here — sector 4 start */
#define APP_END     0x08080000u

/* 1 = run destructive flash self-test, 0 = normal boot */
#define RUN_FLASH_TEST   0

#define APP_SECTOR 4
#define SRAM_START 0x20000000u   /* used to sanity-check app's initial SP */
#define SRAM_END   0x20020000u

/* Busy-wait; volatile stops the compiler from deleting it */
static void delay(volatile uint32_t n) { while (n--) __asm__("nop"); }


/* ---- App detection + jump ---- */

/* Word 0 of the image is the initial SP. If it points into SRAM, image looks valid. */
static int app_valid(void) {
    uint32_t sp = *(volatile uint32_t *)APP_START;
    return (sp >= SRAM_START) && (sp <= SRAM_END);
}

/* Cortex-M handoff: point VTOR at app, load its SP, branch to its reset handler */
static void jump_to_app(void) {
    uint32_t app_sp    = *(volatile uint32_t *)(APP_START);
    uint32_t app_reset = *(volatile uint32_t *)(APP_START + 4);

    /* Reset peripherals to default state — app starts clean,
     * same as after a hardware reset. Without this, leftover
     * GPIO states cause visible bugs (alternate vs sync blink). */
    USART1->CR1 = 0;                          /* disable UART */
    RCC->APB2ENR &= ~RCC_APB2ENR_USART1EN;   /* USART1 clock off */
    GPIOA->MODER = 0xA8000000;                /* PA default (JTAG pins only) */
    GPIOA->ODR = 0;                           /* all outputs LOW */
    SysTick->CTRL = 0;                        /* stop SysTick if running */

    SCB->VTOR = APP_START;
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
        flash_program_word(APP_START + i * 4, test_words[i]);
    }
    flash_lock();

    int ok = 1;
    for (int i = 0; i < 4; i++) {
        if (*(volatile uint32_t *)(APP_START + i * 4) != test_words[i]) {
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
//UART initialization and send/receive functions
static void uart_init(void) {
    /* USART1 clock */
    RCC->APB2ENR |= RCC_APB2ENR_USART1EN;

    /* PA9 (TX), PA10 (RX) -> AF7 */
    GPIOA->MODER &= ~(GPIO_MODER_MODER9 | GPIO_MODER_MODER10);
    GPIOA->MODER |=  (GPIO_MODER_MODER9_1 | GPIO_MODER_MODER10_1);
    GPIOA->AFR[1] &= ~((0xFu << 4) | (0xFu << 8));
    GPIOA->AFR[1] |=  ((7u << 4) | (7u << 8));

    /* 115200 baud @ 16 MHz HSI */
    USART1->BRR = (8u << 4) | 11u;
    USART1->CR1 = USART_CR1_TE | USART_CR1_RE | USART_CR1_UE;
}

static void uart_send(uint8_t b) {
    while (!(USART1->SR & USART_SR_TXE)) { }
    USART1->DR = b;
}

static uint8_t uart_recv(void) {
    uint32_t sr;
    while (1) {
        sr = USART1->SR;
        if (sr & USART_SR_ORE) {
            (void)USART1->DR;   /* clear overrun by reading DR */
            continue;            /* byte was lost, wait for next */
        }
        if (sr & USART_SR_RXNE) {
            return (uint8_t)USART1->DR;
        }
    }
}

static uint32_t crc32(const uint8_t *data, uint32_t len) {
    uint32_t crc = 0xFFFFFFFF;
    for (uint32_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int j = 0; j < 8; j++) {
            crc = (crc >> 1) ^ (0xEDB88320 & -(crc & 1));
        }
    }
    return ~crc;
}

static void uart_send_u32(uint32_t v) {
    uart_send((v >> 24) & 0xFF);
    uart_send((v >> 16) & 0xFF);
    uart_send((v >>  8) & 0xFF);
    uart_send( v        & 0xFF);
}

static uint32_t uart_recv_u32(void) {
    uint32_t v  = (uint32_t)uart_recv() << 24;
    v |= (uint32_t)uart_recv() << 16;
    v |= (uint32_t)uart_recv() << 8;
    v |= (uint32_t)uart_recv();
    return v;
}



/* ---- Main ---- */

int main(void)
{
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
    uart_init();
    /* Let host's serial port settle after opening */
       delay(100000);


    /* Send a greeting so you know it's alive */
    const char *msg = "BOOT> Ready\r\n";
    while (*msg) uart_send(*msg++);


    /* CRITICAL: clear overrun before entering protocol loop */
      (void)USART1->SR;
      (void)USART1->DR;


    static uint8_t rx_buff[1024];
    while (1) {
    	/* 1. Sync hunt */
    	    while (uart_recv() != SYNC) { }
    	    GPIOA->ODR ^= (1u << 6);    /* LED toggles = SYNC received */

    	    /* 2. Read header */
    	    uint8_t cmd  = uart_recv();
    	    uint16_t len = (uint16_t)uart_recv() << 8;
    	    len         |= uart_recv();

    	    /* 3. Size guard */
    	    if (len > 1024) {
    	        uart_send(NACK);
    	        uart_send(0x01);
    	        continue;
    	    }

    	    /* 4. Read payload */
    	    for (uint16_t i = 0; i < len; i++) {
    	        rx_buff[i] = uart_recv();
    	    }

    	    /* 5. CRC check */
    	    uint32_t rx_crc = uart_recv_u32();
    	    uint8_t crc_buf[3 + 1024];
    	    crc_buf[0] = cmd;
    	    crc_buf[1] = (uint8_t)(len >> 8);
    	    crc_buf[2] = (uint8_t)len;
    	    for (uint16_t i = 0; i < len; i++) {
    	        crc_buf[3 + i] = rx_buff[i];
    	    }
    	    if (crc32(crc_buf, 3 + len) != rx_crc) {
    	        uart_send(NACK);
    	        uart_send(0x02);
    	        continue;
    	    }

    	    /* 6. Dispatch */
    	    switch (cmd) {
    	    case CMD_PING:
    	        uart_send(ACK);
    	        break;

    	    case CMD_ERASE:
    	    	/* Wipe the entire app region: sectors 4-7 (448KB)
    	    	         * Sectors 0-3 (bootloader) are never touched — hardcoded protection.
    	    	         * Erase must complete before any WRITE — flash can only flip 1→0,
    	    	         * erase is the only way to get bits back to 1 (0xFF).
    	    	*/
    	    	flash_unlock();
    	    	flash_erase_sector(4);   /* 64KB  @ 0x08010000 */
				flash_erase_sector(5);   /* 128KB @ 0x08020000 */
				flash_erase_sector(6);   /* 128KB @ 0x08040000 */
				flash_erase_sector(7);   /* 128KB @ 0x08060000 */
				flash_lock();            /* re-lock prevents accidental writes */
				uart_send(ACK);
				break;

    	    case CMD_WRITE: {
    	            /* Payload format: [4-byte address big-endian] [data bytes...]
    	             * Minimum 5 bytes: 4 for address + at least 1 data byte.
    	             * Max 1028 bytes: 4 for address + 1024 data (one chunk). */
    	            if (len < 5) { uart_send(NACK); uart_send(0x03); break; }

    	            /* Extract target address from first 4 payload bytes */
    	            uint32_t addr = ((uint32_t)rx_buff[0] << 24)
    	                          | ((uint32_t)rx_buff[1] << 16)
    	                          | ((uint32_t)rx_buff[2] << 8)
    	                          |  (uint32_t)rx_buff[3];
    	            uint16_t data_len = len - 4;  /* remaining = actual firmware data */

    	            /* Safety fence: reject writes outside app region.
    	             * This makes it impossible to overwrite the bootloader (sectors 0-3)
    	             * even if the host tool has a bug. */
    	            if (addr < APP_START || (addr + data_len) > APP_END) {
    	                uart_send(NACK);
    	                uart_send(0x04);   /* error: address out of range */
    	                break;
    	            }

    	            /* Program word-by-word (STM32F4 flash writes in 32-bit units).
    	             * If data_len isn't a multiple of 4, the last partial word
    	             * is padded with 0xFF (which is the erased state, so harmless). */
    	            flash_unlock();
    	            for (uint16_t i = 0; i < data_len; i += 4) {
    	                uint32_t word = 0xFFFFFFFF;  /* start with erased pattern */
    	                for (int b = 0; b < 4 && (i + b) < data_len; b++) {
    	                    word &= ~(0xFFu << (b * 8));       /* clear target byte */
    	                    word |= (uint32_t)rx_buff[4 + i + b] << (b * 8);  /* insert data */
    	                }
    	                flash_program_word(addr + i, word);
    	            }
    	            flash_lock();
    	            uart_send(ACK);
    	            break;
    	        }
    	    case CMD_VERIFY: {
    	        /* Payload: 4-byte address + 4-byte length (both big-endian)
    	         * The bootloader computes CRC32 over that flash range and
    	         * sends it back. The host compares against its own CRC of
    	         * the original .bin file — match = firmware programmed correctly. */
    	        if (len != 8) { uart_send(NACK); uart_send(0x05); break; }

    	        /* Extract address and length from payload */
    	        uint32_t addr = ((uint32_t)rx_buff[0] << 24)
    	                      | ((uint32_t)rx_buff[1] << 16)
    	                      | ((uint32_t)rx_buff[2] << 8)
    	                      |  (uint32_t)rx_buff[3];
    	        uint32_t vlen = ((uint32_t)rx_buff[4] << 24)
    	                      | ((uint32_t)rx_buff[5] << 16)
    	                      | ((uint32_t)rx_buff[6] << 8)
    	                      |  (uint32_t)rx_buff[7];

    	        /* Same safety fence as WRITE — stay inside app region */
    	        if (addr < APP_START || (addr + vlen) > APP_END) {
    	            uart_send(NACK);
    	            uart_send(0x04);   /* error: address out of range */
    	            break;
    	        }

    	        /* CRC32 directly over flash memory.
    	         * STM32 flash is memory-mapped, so we just cast the address
    	         * to a pointer — no need to copy data into a buffer first.
    	         * This is the same crc32() function used for frame verification,
    	         * now serving a second purpose: flash integrity checking. */
    	        uint32_t flash_crc = crc32((const uint8_t *)addr, vlen);
    	        uart_send(ACK);
    	        uart_send_u32(flash_crc);   /* 4 bytes back to host for comparison */
    	        break;
    	    }

    	    case CMD_JUMP:
    	        /* Send ACK immediately so the host knows the command was received.
    	         * The delay lets the ACK byte fully leave the UART shift register
    	         * before we relocate VTOR and abandon bootloader code. */
    	        uart_send(ACK);
    	        delay(10000);

    	        /* Only jump if a valid app exists — check initial SP in SRAM range.
    	         * If no valid app (erased flash, test pattern, corrupted image),
    	         * send NACK so the host knows the jump didn't happen. */
    	        if (app_valid()) {
    	            jump_to_app();   /* never returns — app owns the CPU now */
    	        } else {
    	            uart_send(NACK);
    	            uart_send(0x06);   /* error: no valid app at APP_START */
    	        }
    	        break;
    	    default:
    	        uart_send(NACK);
    	        uart_send(0xFF);
    	        break;
    	    }
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
