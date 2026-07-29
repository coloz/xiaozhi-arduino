#pragma once

#include "BoardConfig.h"

// Sketch-local TFT_eSPI setup. Keeping it beside the sketch makes the example
// self-contained and leaves all board-specific values in BoardConfig.h.
#define USER_SETUP_INFO BOARD_NAME

#define ST7789_DRIVER
#define TFT_WIDTH BOARD_LCD_WIDTH
#define TFT_HEIGHT BOARD_LCD_HEIGHT

// The LCD is write-only. RST and backlight are controlled by the carrier board.
#define TFT_MISO BOARD_LCD_MISO
#define TFT_MOSI BOARD_LCD_MOSI
#define TFT_SCLK BOARD_LCD_SCLK
#define TFT_CS BOARD_LCD_CS
#define TFT_DC BOARD_LCD_DC
#define TFT_RST BOARD_LCD_RST

// Arduino-ESP32 3.3.x numbers FSPI differently from the low-level register
// indices used by TFT_eSPI 2.5.x on ESP32-S3. HSPI maps consistently to SPI3
// and can still be routed to the board's SCLK/MOSI pins through GPIO Matrix.
#define USE_HSPI_PORT

#define LOAD_GLCD

#define SPI_FREQUENCY BOARD_LCD_SPI_FREQUENCY
#define SPI_READ_FREQUENCY BOARD_LCD_SPI_READ_FREQUENCY
