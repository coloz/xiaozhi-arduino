#pragma once

// Sketch-local TFT_eSPI setup for the ESP32-S3/ST7789 test board.
#define USER_SETUP_INFO "ESP32-S3 ST7789 240x240 Xiaozhi emotion display"

#define ST7789_DRIVER
#define TFT_WIDTH  240
#define TFT_HEIGHT 240

// The display is write-only. GPIO 48 is a harmless input used as MISO because
// TFT_eSPI on Arduino-ESP32 3.x may alias MISO=-1 to MOSI on ESP32-S3.
#define TFT_MISO 48
#define TFT_MOSI 47
#define TFT_SCLK 21
#define TFT_CS   44
#define TFT_DC   43
#define TFT_RST  -1

// Select the ESP32-S3 FSPI compatibility path used by TFT_eSPI 2.5.x.
#define USE_FSPI_PORT

#define LOAD_GLCD

#define SPI_FREQUENCY      40000000
#define SPI_READ_FREQUENCY 20000000

#define DISABLE_ALL_LIBRARY_WARNINGS
