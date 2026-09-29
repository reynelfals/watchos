#pragma once

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** UART pad pins on Waveshare ESP32-S3-Touch-AMOLED-2.06 (SenseCAP / LoRa path). */
#ifndef UART_PAD_TX
#define UART_PAD_TX 43
#endif
#ifndef UART_PAD_RX
#define UART_PAD_RX 44
#endif

/** Open hardware UART1 on TX=43 RX=44. Idempotent if already open at same baud. */
bool uartHostOpen(uint32_t baud);

/** Close pad UART. Safe if already closed. */
bool uartHostClose(void);

/** True while pad UART is open. */
bool uartHostReady(void);

/** Bytes waiting in RX buffer (0 if closed). */
int uartHostAvailable(void);

/**
 * Write raw bytes. Returns bytes written, or -1 if closed / error.
 * Partial writes possible; caller may retry.
 */
int uartHostWrite(const char* data, size_t len);

/**
 * Read up to maxBytes into buf (not necessarily NUL-terminated).
 * Returns bytes read, 0 if none, or -1 if closed / error.
 */
int uartHostRead(char* buf, size_t maxBytes);

#ifdef __cplusplus
}
#endif
