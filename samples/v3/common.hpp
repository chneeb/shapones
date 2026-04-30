#ifndef COMMON_HPP
#define COMMON_HPP

#include "stdint.h"
#include "pico/stdlib.h"
#include "hardware/clocks.h"
#include "hardware/gpio.h"
#include "shapones/shapones.hpp"
#include "picocalc.hpp"
#include "mono8x16.hpp"

// system clock frequency
static constexpr uint32_t SYS_CLK_FREQ = 250 * MHZ;

// frame buffer for DMA (RGB565)
// 5/4 scale of NES 256x240: fills 320x300 of the 320x320 LCD (10px border top/bottom)
constexpr int FRAME_BUFF_WIDTH  = 320;
constexpr int FRAME_BUFF_HEIGHT = 300;
constexpr int FRAME_BUFF_STRIDE = FRAME_BUFF_WIDTH * 2;
extern uint8_t frame_buff[FRAME_BUFF_STRIDE * FRAME_BUFF_HEIGHT];

// pad pins
static constexpr int PIN_PAD_A      = 0;
static constexpr int PIN_PAD_B      = 0;
static constexpr int PIN_PAD_START  = 0;
static constexpr int PIN_PAD_SELECT = 0;
static constexpr int PIN_PAD_RIGHT  = 0;
static constexpr int PIN_PAD_DOWN   = 0;
static constexpr int PIN_PAD_LEFT   = 0;
static constexpr int PIN_PAD_UP     = 0;
extern int input_pins[];

static constexpr uint TICKSPERSEC = 1000;   /* Ticks per second */
static constexpr uint8_t KEYCHECKTIME=16;

// wait until some key is pressed
int wait_key();

// clear frame buffer with black color
void clear_frame_buff();

// draw string to frame buffer
void draw_string(int x, int y, const char *str);

// transfer image from frame buffer to LCD
void update_lcd();

void device_init();

void setBacklight(int);

// NES mini controller on i2c0, GP4 (SDA) / GP5 (SCL)
// nunchuck_pins[] mirrors input_pins[] order; ORed at input read time
extern int nunchuck_pins[8];
void nunchuck_init();
void nunchuck_poll();

#endif
