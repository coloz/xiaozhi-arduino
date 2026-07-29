#pragma once

// Change this file to adapt the example to another ESP32-S3 board. The sample
// values describe one 240x240 ST7789 display plus an ES8311 audio codec; they
// are not tied to a board vendor.
#define BOARD_TYPE "esp32s3-tft-es8311"
#define BOARD_NAME "ESP32-S3 TFT voice terminal"

// Display wiring and behavior.
#define BOARD_LCD_WIDTH 240
#define BOARD_LCD_HEIGHT 240
#define BOARD_LCD_ROTATION 1
#define BOARD_LCD_SCLK 21
#define BOARD_LCD_MOSI 47
#define BOARD_LCD_MISO -1
#define BOARD_LCD_DC 43
#define BOARD_LCD_CS 44
#define BOARD_LCD_RST -1
#define BOARD_LCD_SPI_FREQUENCY 40000000
#define BOARD_LCD_SPI_READ_FREQUENCY 20000000

// Conversation button. It is active-low and uses the internal pull-up.
#define BOARD_CHAT_BUTTON 0
#define BOARD_CHAT_BUTTON_ACTIVE_LEVEL 0

// Audio control bus.
#define BOARD_AUDIO_I2C_SDA 41
#define BOARD_AUDIO_I2C_SCL 42
#define BOARD_AUDIO_I2C_FREQUENCY 400000
#define BOARD_AUDIO_CODEC_ADDRESS 0x18

// Full-duplex I2S wiring. DIN/DOUT are named from the controller's point of
// view: DATA_OUT connects to the codec input, DATA_IN to the codec output.
#define BOARD_AUDIO_I2S_PORT 0
#define BOARD_AUDIO_MCLK 46
#define BOARD_AUDIO_BCLK 39
#define BOARD_AUDIO_WS 2
#define BOARD_AUDIO_DATA_OUT 38
#define BOARD_AUDIO_DATA_IN 40
#define BOARD_AUDIO_MCLK_INVERTED false
#define BOARD_AUDIO_BCLK_INVERTED false
#define BOARD_AUDIO_WS_INVERTED false

// Audio format and analog tuning. Adjust gains for the microphone, amplifier,
// speaker, and power rails used by the target board.
#define BOARD_AUDIO_HARDWARE_SAMPLE_RATE 24000
#define BOARD_AUDIO_MCLK_MULTIPLE 256
#define BOARD_AUDIO_PA_SUPPLY_VOLTAGE 5.0f
#define BOARD_AUDIO_CODEC_DAC_VOLTAGE 3.3f
#define BOARD_AUDIO_PA_GAIN_DB 0.0f
#define BOARD_AUDIO_MIC_GAIN_DB 30.0f
#define BOARD_AUDIO_OUTPUT_VOLUME_DB -12.0f
