#ifndef _BOARD_CONFIG_H_
#define _BOARD_CONFIG_H_

#include <driver/gpio.h>
#include "sdkconfig.h"

// Wall-E on a Seeed XIAO ESP32-S3 Sense. Pins follow the Huy Vector wiring diagram.
// Every XIAO user pin (D0-D10) is in use.

// Spoken name of the configured wake word (build option --wake-word), for screen hints.
#if CONFIG_SR_WN_WN9_JARVIS_TTS
#define WAKE_WORD_NAME "Jarvis"
#elif CONFIG_SR_WN_WN9_COMPUTER_TTS
#define WAKE_WORD_NAME "Computer"
#elif CONFIG_SR_WN_WN9_HIWALLE_TTS2
#define WAKE_WORD_NAME "Hi Wall-E"
#else
#define WAKE_WORD_NAME "the wake word"
#endif

// Audio: PDM microphone on the Sense board, MAX98357A I2S amplifier (powered from 3V3)
#define AUDIO_INPUT_SAMPLE_RATE  16000
#define AUDIO_OUTPUT_SAMPLE_RATE 24000
#define AUDIO_MIC_PDM_CLK  GPIO_NUM_42
#define AUDIO_MIC_PDM_DATA GPIO_NUM_41
#define AUDIO_SPK_DOUT     GPIO_NUM_5   // D4 -> amp DIN
#define AUDIO_SPK_BCLK     GPIO_NUM_6   // D5 -> amp BCLK
#define AUDIO_SPK_WS       GPIO_NUM_43  // D6 -> amp LRC (UART0 TX pad: console must be USB)

// Display: 1.54" ST7789 240x240 on SPI. CS is tied to GND, the backlight to 3V3.
#define DISPLAY_SPI_HOST     SPI3_HOST
#define DISPLAY_SCLK_PIN     GPIO_NUM_9   // D10, display pin "SCL"
#define DISPLAY_MOSI_PIN     GPIO_NUM_8   // D9, display pin "SDA"
#define DISPLAY_RST_PIN      GPIO_NUM_7   // D8
#define DISPLAY_DC_PIN       GPIO_NUM_44  // D7 (UART0 RX pad)
#define DISPLAY_CS_PIN       GPIO_NUM_NC
#define DISPLAY_WIDTH        240
#define DISPLAY_HEIGHT       240
#define DISPLAY_SPI_MODE     3            // CS held low needs mode 3 on ST7789 (verify with !pattern)
#define DISPLAY_SPI_CLOCK_HZ (40 * 1000 * 1000)
#define DISPLAY_INVERT_COLOR true
#define DISPLAY_RGB_ORDER    LCD_RGB_ELEMENT_ORDER_RGB
#define DISPLAY_MIRROR_X     false
#define DISPLAY_MIRROR_Y     false
#define DISPLAY_SWAP_XY      false
#define DISPLAY_OFFSET_X     0
#define DISPLAY_OFFSET_Y     0

// Motor driver: Mini L298N (MX1508 style, IN1-IN4, no enable pins), powered from 5V.
// Per the diagram, motor A (IN1/IN2) drives the right wheel and motor B (IN3/IN4) the left.
#define MOTOR_IN1_PIN GPIO_NUM_4  // D3
#define MOTOR_IN2_PIN GPIO_NUM_3  // D2
#define MOTOR_IN3_PIN GPIO_NUM_2  // D1
#define MOTOR_IN4_PIN GPIO_NUM_1  // D0

// Buttons and on-board parts
#define BOOT_BUTTON_GPIO GPIO_NUM_0
// microSD chip select on the Sense board, shared with the orange user LED (active low).
// Held high: the card slot shares GPIO7/8/9 with the display and must stay deselected.
#define SD_CS_USER_LED_GPIO GPIO_NUM_21

// Camera: OV3660 on the Sense board (fixed wiring)
#define CAMERA_PIN_PWDN  GPIO_NUM_NC
#define CAMERA_PIN_RESET GPIO_NUM_NC
#define CAMERA_PIN_XCLK  GPIO_NUM_10
#define CAMERA_PIN_SIOD  GPIO_NUM_40
#define CAMERA_PIN_SIOC  GPIO_NUM_39
#define CAMERA_PIN_D0    GPIO_NUM_15  // Y2
#define CAMERA_PIN_D1    GPIO_NUM_17  // Y3
#define CAMERA_PIN_D2    GPIO_NUM_18  // Y4
#define CAMERA_PIN_D3    GPIO_NUM_16  // Y5
#define CAMERA_PIN_D4    GPIO_NUM_14  // Y6
#define CAMERA_PIN_D5    GPIO_NUM_12  // Y7
#define CAMERA_PIN_D6    GPIO_NUM_11  // Y8
#define CAMERA_PIN_D7    GPIO_NUM_48  // Y9
#define CAMERA_PIN_VSYNC GPIO_NUM_38
#define CAMERA_PIN_HREF  GPIO_NUM_47
#define CAMERA_PIN_PCLK  GPIO_NUM_13
#define XCLK_FREQ_HZ     20000000
#define CAMERA_VFLIP     true   // OV3660 modules are usually upside down; verify with a photo
#define CAMERA_HMIRROR   false

#endif  // _BOARD_CONFIG_H_
