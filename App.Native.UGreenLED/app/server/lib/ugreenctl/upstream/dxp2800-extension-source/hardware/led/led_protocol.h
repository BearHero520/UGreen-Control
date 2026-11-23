#ifndef UGREENCTL_LED_PROTOCOL_H
#define UGREENCTL_LED_PROTOCOL_H

#include "led/led.h"

#define UGREENCTL_LED_FRAME_SIZE 11U
#define UGREENCTL_LED_COMMAND_SIZE 12U
#define UGREENCTL_LED_I2C_ADDRESS 0x3a
#define UGREENCTL_LED_CHIP_ID_REGISTER 0x5a
#define UGREENCTL_LED_CHIP_ID_C5B2 0xc5b2

int ugreenctl_led_decode(const uint8_t *frame, size_t length,
                        struct ugreenctl_led_value *value);
int ugreenctl_led_encode(uint8_t id, uint8_t command,
                        const uint8_t parameters[4],
                        uint8_t frame[UGREENCTL_LED_COMMAND_SIZE]);
int ugreenctl_led_encode_smbus(uint8_t command, const uint8_t parameters[4],
                              bool little_endian_checksum, uint8_t frame[12]);

#endif
