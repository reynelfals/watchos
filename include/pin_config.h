#pragma once

// Required by XPowersLib before including XPowersLib.h (Waveshare Mylibrary/pin_config.h)
#define XPOWERS_CHIP_AXP2101

// Waveshare ESP32-S3-Touch-AMOLED-2.06 pin map
// Sources:
//   examples/arduino/libraries/Mylibrary/pin_config.h
//   https://www.waveshare.com/wiki/ESP32-S3-Touch-AMOLED-2.06
//   espressif/arduino-esp32 variants/waveshare_esp32_s3_touch_amoled_206/pins_arduino.h

// AMOLED CO5300 (QSPI)
#define LCD_SDIO0 4
#define LCD_SDIO1 5
#define LCD_SDIO2 6
#define LCD_SDIO3 7
#define LCD_SCLK  11
#define LCD_CS    12
#define LCD_RESET 8
#define LCD_TE    13
#define LCD_WIDTH  410
#define LCD_HEIGHT 502

// FT3168 capacitive touch (I2C 0x38), shared bus SCL=14 SDA=15
#define IIC_SDA  15
#define IIC_SCL  14
#define TP_INT   38
#define TP_RESET 9
#define FT3168_I2C_ADDR 0x38

// TF / microSD — 1-bit SDMMC (Waveshare 07_LVGL_SD_Test)
// Wiki also lists the same pins as SPI: CS=17 MOSI=1 MISO=3 SCK=2
#define SDMMC_CLK  2
#define SDMMC_CMD  1
#define SDMMC_DATA 3
#define SDMMC_CS   17

// PCF85063 RTC (I2C 0x51) — shared bus with touch / AXP2101 / QMI8658
#define PCF85063_I2C_ADDR 0x51
#define RTC_INT 39

// AXP2101 PMIC (I2C 0x34) — shared bus with touch / RTC
#define AXP2101_I2C_ADDR 0x34

// Vibration motor (schematic MOTOR → GPIO18)
#define VIBRATE_PIN 18

// QMI8658 six-axis IMU (I2C 0x6B) — shared bus; INT1 → GPIO21
#define QMI8658_I2C_ADDR 0x6B
#define QMI8658_INT1 21

// Hardware UART pads (SenseCAP / LoRa path) — TX=GPIO43 RX=GPIO44
#define UART_PAD_TX 43
#define UART_PAD_RX 44

// I2S audio (Waveshare wiki / docs.waveshare.com peripheral table)
// Playback: ES8311; capture: ES7210 dual mics (DIN = I2S_ASDOUT)
#define I2S_MCLK   16
#define I2S_SCLK   41  // BCLK
#define I2S_LRCK   45  // WS
#define I2S_DSDIN  40  // ESP → ES8311 (playback)
#define I2S_ASDOUT 42  // ES7210 → ESP (mic)
#define PA_CTRL    46  // NS4150B amp enable (keep LOW when not playing)

#define ES8311_I2C_ADDR 0x18
#define ES7210_I2C_ADDR 0x40
