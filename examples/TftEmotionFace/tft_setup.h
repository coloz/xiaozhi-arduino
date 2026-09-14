#pragma once

// Local setup for the COM18 ESP32-S3 board.
#define USER_SETUP_INFO "ESP32-S3 ST7789 240x240 robot eyes"
#define ST7789_DRIVER
#define TFT_WIDTH 240
#define TFT_HEIGHT 240
#define TFT_MOSI 47
#define TFT_SCLK 21
#define TFT_CS 44
#define TFT_DC 43
#define TFT_RST -1
#define TFT_MISO 48
#define USE_FSPI_PORT
#define LOAD_GLCD
#define SPI_FREQUENCY 40000000
#define SPI_READ_FREQUENCY 20000000
#define DISABLE_ALL_LIBRARY_WARNINGS
