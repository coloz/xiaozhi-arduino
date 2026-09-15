#pragma once

// Force-include in ALL translation units, including TFT_eSPI and grokEyes.
// Requires the TFT_eSPI fork with CH13613 QSPI support (see build_ojoy.ps1).
#define USER_SETUP_LOADED
#define USER_SETUP_INFO "OJoy CH13613 QSPI 480x480"
#define CH13613_DRIVER
#define TFT_WIDTH 480
#define TFT_HEIGHT 480
#define TFT_MISO -1
#define TFT_MOSI -1
#define TFT_SCLK 17
#define TFT_CS 45
#define TFT_DC -1
#define TFT_RST 7
#define TFT_BL -1
#define TFT_D0 8
#define TFT_D1 16
#define TFT_D2 15
#define TFT_D3 18
#define TFT_TE 38
#define TFT_RGB_ORDER TFT_RGB
#define SPI_FREQUENCY 40000000
#define LOAD_GLCD
#define LOAD_FONT2
#define LOAD_FONT4
#define LOAD_FONT6
#define LOAD_FONT7
#define LOAD_FONT8
#define LOAD_GFXFF
#define SMOOTH_FONT
