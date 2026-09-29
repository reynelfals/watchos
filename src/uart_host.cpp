#include "uart_host.h"

#include <Arduino.h>
#include <string.h>

// USB CDC is Serial; pad UART uses UART1 so USB monitor stays free.
static HardwareSerial UartPad(1);
static bool gOpen = false;
static uint32_t gBaud = 0;

bool uartHostOpen(uint32_t baud) {
  if (baud < 300) baud = 300;
  if (baud > 921600) baud = 921600;
  if (gOpen && gBaud == baud) return true;
  if (gOpen) {
    UartPad.end();
    gOpen = false;
    gBaud = 0;
  }
  // RX=44 TX=43 (board UART0 pads; we drive them via UART1 peripheral).
  UartPad.setTimeout(20);
  UartPad.begin(baud, SERIAL_8N1, UART_PAD_RX, UART_PAD_TX);
  gOpen = true;
  gBaud = baud;
  Serial.printf("uart_pad: open baud=%u TX=%d RX=%d\n", (unsigned)baud,
                UART_PAD_TX, UART_PAD_RX);
  return true;
}

bool uartHostClose(void) {
  if (!gOpen) return true;
  UartPad.end();
  gOpen = false;
  gBaud = 0;
  Serial.println("uart_pad: closed");
  return true;
}

bool uartHostReady(void) { return gOpen; }

int uartHostAvailable(void) {
  if (!gOpen) return 0;
  return UartPad.available();
}

int uartHostWrite(const char* data, size_t len) {
  if (!gOpen || !data) return -1;
  if (len == 0) return 0;
  size_t n = UartPad.write((const uint8_t*)data, len);
  UartPad.flush();
  return (int)n;
}

int uartHostRead(char* buf, size_t maxBytes) {
  if (!gOpen || !buf || maxBytes == 0) return -1;
  int avail = UartPad.available();
  if (avail <= 0) return 0;
  size_t want = maxBytes;
  if ((size_t)avail < want) want = (size_t)avail;
  size_t n = UartPad.readBytes(buf, want);
  return (int)n;
}
