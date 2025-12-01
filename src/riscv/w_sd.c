
#include "w_sd.h"

#include <neorv32.h>
#include <string.h>

//
// GLOBALS
//

#define BAUD_RATE 19200
#define BLOCK_SIZE 512

// SD command definitions
#define CMD0_GO_IDLE_STATE 0
#define CMD8_SEND_IF_COND 8
#define CMD16_SET_BLOCKLEN 16
#define CMD17_READ_BLOCK 17
#define CMD55_APP_CMD 55
#define CMD58_READ_OCR 58
#define ACMD41_SD_SEND_OP_COND 41

// SD response types
#define SD_IDLE_STATE 0x01
#define SD_READY 0x00
#define SD_ILLEGAL_CMD 0x05
#define SD_DATA_TOKEN 0xFE

// SD command arguments
#define CMD8_ARG 0x1AA
#define ACMD41_HCS 0x40000000

// SD CRC values
#define CMD0_CRC 0x95
#define CMD8_CRC 0x87
#define DEFAULT_CRC 0xFF

// SPI configuration
#define SPI_PRESCALER 0
#define SPI_DIVIDER 0
#define SPI_MODE 0
#define SPI_CS0 0

// Clock and timing
#define INIT_CLOCK_CYCLES 10
#define CMD_RESPONSE_RETRIES 8
#define ACMD41_TIMEOUT 50000
#define DATA_TOKEN_TIMEOUT 1000000
#define EXTRA_CLOCKS 10

uint32_t spi_configured;

void spi_setup(void) {
    neorv32_spi_setup(SPI_PRESCALER, SPI_DIVIDER, SPI_MODE, 0);
    spi_configured = 1;
    neorv32_uart0_printf("SPI configured: prescaler=%d, divider=%d, mode=%d\n",
                         SPI_PRESCALER, SPI_DIVIDER, SPI_MODE);
}

void spi_cs(uint8_t type) {
    if (!spi_configured) {
        neorv32_uart0_printf("SPI not configured!\n");
        return;
    }
    if (type)
        neorv32_spi_cs_en(SPI_CS0);
    else
        neorv32_spi_cs_dis();
}

void sd_send_command(uint8_t cmd, uint32_t arg, uint8_t crc, uint8_t* response, uint8_t resp_len) {
    uint8_t tmp;

    spi_cs(1);

    spi_transfer_byte(0x40 | cmd);
    spi_transfer_byte((arg >> 24) & 0xFF);
    spi_transfer_byte((arg >> 16) & 0xFF);
    spi_transfer_byte((arg >> 8) & 0xFF);
    spi_transfer_byte(arg & 0xFF);
    spi_transfer_byte(crc);

    for (uint8_t i = 0; i < CMD_RESPONSE_RETRIES; i++) {
        tmp = spi_transfer_byte(0xFF);
        if ((tmp & 0x80) == 0) break;
    }
    response[0] = tmp;

    for (uint8_t i = 1; i < resp_len; i++) response[i] = spi_transfer_byte(0xFF);

    spi_cs(0);
    spi_transfer_byte(0xFF);
}

void sd_send_command_nocs(uint8_t cmd, uint32_t arg, uint8_t crc, uint8_t* response, uint8_t resp_len) {
    uint8_t tmp;

    spi_transfer_byte(0x40 | cmd);
    spi_transfer_byte((arg >> 24) & 0xFF);
    spi_transfer_byte((arg >> 16) & 0xFF);
    spi_transfer_byte((arg >> 8) & 0xFF);
    spi_transfer_byte(arg & 0xFF);
    spi_transfer_byte(crc);

    for (uint8_t i = 0; i < CMD_RESPONSE_RETRIES; i++) {
        tmp = spi_transfer_byte(0xFF);
        if ((tmp & 0x80) == 0) break;
    }
    response[0] = tmp;

    for (uint8_t i = 1; i < resp_len; i++) response[i] = spi_transfer_byte(0xFF);
}

uint8_t sd_init(void) {
    uint8_t resp[5];
    int watchdog;

    neorv32_uart0_printf("Starting SD card initialization...\n");

    neorv32_uart0_printf("Sending clock cycles for SD init...\n");
    spi_cs(0);
    for (int i = 0; i < INIT_CLOCK_CYCLES; i++) spi_transfer_byte(0xFF);

    neorv32_uart0_printf("CMD0 (GO_IDLE_STATE)...\n");
    sd_send_command(CMD0_GO_IDLE_STATE, 0, CMD0_CRC, resp, 1);
    if (resp[0] != SD_IDLE_STATE) {
        neorv32_uart0_printf(
            "ERROR: SD card did not enter idle state. "
            "Check wiring, power, and card presence.\n");
        return 0;
    }

    neorv32_uart0_printf("CMD8 (SEND_IF_COND)...\n");
    sd_send_command(CMD8_SEND_IF_COND, CMD8_ARG, CMD8_CRC, resp, 5);

    if (resp[0] == SD_ILLEGAL_CMD) {
        neorv32_uart0_printf(
            "ERROR: Illegal command. This card is likely NOT SDHC/SDXC "
            "(older SDSC card). Initialization method differs.\n");
        return 0;
    }
    if (resp[0] != SD_IDLE_STATE) {
        neorv32_uart0_printf("ERROR: CMD8 failed. SD card may not support required voltage.\n");
        return 0;
    }

    if (resp[3] != 0x01 || resp[4] != 0xAA) {
        neorv32_uart0_printf(
            "ERROR: CMD8 echo-back check failed. "
            "Card may not support 2.7–3.6V or may be incompatible.\n");
        return 0;
    }

    neorv32_uart0_printf("ACMD41 (SD_SEND_OP_COND)...\n");
    watchdog = ACMD41_TIMEOUT;
    do {
        sd_send_command(CMD55_APP_CMD, 0, DEFAULT_CRC, resp, 1);
        if (resp[0] > 1) {
            neorv32_uart0_printf("ERROR: CMD55 failed (response 0x");
            aux_print_hex_byte(resp[0]);
            neorv32_uart0_printf("). Cannot send ACMD41.\n");
            return 0;
        }

        sd_send_command(ACMD41_SD_SEND_OP_COND, ACMD41_HCS, DEFAULT_CRC, resp, 1);
        if (--watchdog == 0) {
            neorv32_uart0_printf(
                "ERROR: ACMD41 timeout. "
                "Card did not leave idle state. "
                "Card may be incompatible or slow to start.\n");
            return 0;
        }
    } while (resp[0] != SD_READY);

    neorv32_uart0_printf("Card initialized successfully.\n");

    neorv32_uart0_printf("CMD58 (READ_OCR)...\n");
    sd_send_command(CMD58_READ_OCR, 0, DEFAULT_CRC, resp, 5);
    if (resp[0] != SD_READY) {
        neorv32_uart0_printf("ERROR: CMD58 failed. Cannot read OCR register.\n");
        return 0;
    }

    neorv32_uart0_printf("OCR: 0x");
    for (int i = 1; i < 5; i++) aux_print_hex_byte(resp[i]);
    neorv32_uart0_printf("\n");

    neorv32_uart0_printf("CMD16 (SET_BLOCKLEN=%d)...\n", BLOCK_SIZE);
    sd_send_command(CMD16_SET_BLOCKLEN, BLOCK_SIZE, DEFAULT_CRC, resp, 1);

    if (resp[0] != SD_READY) {
        neorv32_uart0_printf("ERROR: CMD16 failed. Card did not accept %d-byte block size.\n", BLOCK_SIZE);
        return 0;
    }

    neorv32_uart0_printf("SD card block length set. Initialization complete.\n");
    return 1;
}

uint8_t spi_transfer_byte(uint8_t tx) {
    return (uint8_t)neorv32_spi_transfer(tx);
}

void sd_read_block(uint32_t blockIndex, uint8_t* buffer) {
    uint8_t resp, token;
    uint32_t timeout;

    if (!spi_configured) {
        neorv32_uart0_printf("SPI not configured!\n");
        return;
    }

    spi_cs(0);
    for (int i = 0; i < EXTRA_CLOCKS; i++) spi_transfer_byte(0xFF);

    spi_cs(1);

    sd_send_command_nocs(CMD17_READ_BLOCK, blockIndex, DEFAULT_CRC, &resp, 1);
    if (resp != SD_READY) {
        neorv32_uart0_printf("CMD17 failed: 0x");
        aux_print_hex_byte(resp);
        neorv32_uart0_printf("\n");
        spi_cs(0);
        return;
    }

    timeout = DATA_TOKEN_TIMEOUT;
    do {
        token = spi_transfer_byte(0xFF);
        if (--timeout == 0) {
            neorv32_uart0_printf("Timeout waiting for data token!\n");
            spi_cs(0);
            return;
        }
    } while (token != SD_DATA_TOKEN);

    for (int i = 0; i < BLOCK_SIZE; i++) buffer[i] = spi_transfer_byte(0xFF);

    spi_transfer_byte(0xFF);
    spi_transfer_byte(0xFF);

    spi_cs(0);
    spi_transfer_byte(0xFF);
}

void wad_read_bytes(uint8_t* dest, uint32_t wad_start_block, uint32_t lump_offset, uint32_t lump_size) {
    uint8_t block[BLOCK_SIZE];

    uint32_t abs_byte = wad_start_block * BLOCK_SIZE + lump_offset;

    uint32_t block_index = abs_byte / BLOCK_SIZE;
    uint32_t block_offset = abs_byte % BLOCK_SIZE;

    uint32_t bytes_remaining = lump_size;
    uint8_t* out = dest;

    while (bytes_remaining > 0) {
        sd_read_block(block_index, block);

        uint32_t chunk = BLOCK_SIZE - block_offset;
        if (chunk > bytes_remaining)
            chunk = bytes_remaining;

        for (uint32_t i = 0; i < chunk; i++)
            out[i] = block[block_offset + i];

        out += chunk;
        bytes_remaining -= chunk;
        block_index++;
        block_offset = 0;
    }
}
