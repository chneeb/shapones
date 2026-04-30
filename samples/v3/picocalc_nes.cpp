#include "hardware/gpio.h"
#include "hardware/clocks.h"
#include "hardware/vreg.h"
#include "hardware/watchdog.h"
#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "pico/util/queue.h"
#include "pico/time.h"

#include "i2ckbd.hpp"
#include "picocalc.hpp"

#include "pwm_audio.hpp"

#include "shapones/shapones.hpp"

#include "common.hpp"
#include "boot_menu.hpp"

// monitor pin for debugging
static constexpr int PIN_MONITOR = 1;

// speaker PWM out
static constexpr int PIN_SPEAKER = 26;

// APU configuration
static constexpr int SPK_LATENCY = 256;
static constexpr int SPK_PWM_FREQ = 22050;

/*
// NES color table ,rgb444
static const uint16_t COLOR_TABLE[] = {
    0x555, 0x027, 0x019, 0x308, 0x406, 0x603, 0x500, 0x410, 
    0x230, 0x040, 0x040, 0x040, 0x034, 0x000, 0x000, 0x000, 
    0x999, 0x05C, 0x33F, 0x62E, 0x81B, 0xA16, 0x922, 0x740, 
    0x560, 0x270, 0x080, 0x072, 0x067, 0x000, 0x000, 0x000, 
    0xFFF, 0x5AF, 0x78F, 0xB6F, 0xE5F, 0xF5B, 0xF76, 0xD82, 
    0xAB0, 0x7C0, 0x5D2, 0x3D7, 0x3BD, 0x444, 0x000, 0x000, 
    0xFFF, 0xADF, 0xCCF, 0xDBF, 0xFBF, 0xFBD, 0xFBB, 0xEC9, 
    0xDD7, 0xBE7, 0xAE9, 0x9EB, 0xADE, 0xAAA, 0x000, 0x000,
};
*/

// NES color table ,rgb565
static const uint16_t COLOR_TABLE[] = {
0x52aa, 0x010e, 0x0093, 0x3011, 0x400c, 0x6006, 0x6000, 0x4080,
0x2180, 0x0220, 0x0220, 0x0220, 0x0188, 0x0000, 0x0000, 0x0000,
0x9cd3, 0x02b9, 0x319f, 0x611d, 0x8897, 0xa88c, 0xa904, 0x7220,
0x5320, 0x23a0, 0x0440, 0x03a4, 0x032e, 0x0000, 0x0000, 0x0000,
0xffff, 0x555f, 0x745f, 0xbb3f, 0xeabf, 0xfab7, 0xfbac, 0xdc44,
0xadc0, 0x7660, 0x56e4, 0x36ee, 0x35db, 0x4228, 0x0000, 0x0000,
0xffff, 0xaeff, 0xce7f, 0xdddf, 0xfddf, 0xfddb, 0xfdd7, 0xee73,
0xdeee, 0xbf6e, 0xaf73, 0x9f77, 0xaefd, 0xad55, 0x0000, 0x0000
};

/*
static const uint16_t COLOR_TABLE[] = {
        0x52AA, 0x0117, 0x00D2, 0x1861, 0x20C2, 0x30A0, 0x2840, 0x2108,
        0x10C2, 0x0020, 0x0020, 0x0020, 0x018A, 0x0000, 0x0000, 0x0000,
        0x9CD3, 0x02BF, 0x1CFF, 0x4E3F, 0x733F, 0xA89F, 0x9105, 0x82E0,
        0x5B00, 0x2120, 0x0010, 0x0019, 0x0053, 0x0000, 0x0000, 0x0000,
        0xFFFF, 0x52FF, 0x7BFF, 0xB7FF, 0xE7FF, 0xF79E, 0xFFDC, 0xD740,
        0xAC00, 0x8300, 0x5A40, 0x3AA0, 0x3B7C, 0x5294, 0x0000, 0x0000,
        0xFFFF, 0xBFFF, 0xDDFF, 0xEFFC, 0xFFFF, 0xFFFB, 0xFFEA, 0xD749,
        0xCE79, 0xC6B8, 0xAD8C, 0x94EF, 0xC678, 0xAD55, 0x0000, 0x0000,
};
 */
// line buffer FIFO between core1 --> core0
// <------------- STRIDE ------------>
// +---------+-----------------------+    A
// | y coord | color numbers (256px) |    |
// | 1 Byte  | 256 Byte              |    |
// +---------+-----------------------+  DEPTH
// |    :    |           :           |    |
// |    :    |           :           |    |
// +---------+-----------------------+    V
static constexpr int LINE_FIFO_DEPTH = 8;
static constexpr int LINE_FIFO_STRIDE = shapones::SCREEN_WIDTH + 1;
static uint8_t line_buff[LINE_FIFO_DEPTH * LINE_FIFO_STRIDE];
static volatile int line_fifo_wptr = 0;
static volatile int line_fifo_rptr = 0;

// sound buffer for DMA
static uint8_t spk_buff[SPK_LATENCY];

// start game
static void boot_nes();

// core0 main loop
static void cpu_loop();

// core1 main loop
static void ppu_loop();

// APU DMA finish IRQ handler
static void apu_dma_handler();

// let APU fill the sound buffer
static void apu_fill_buffer(PwmAudio::sample_t *buff);

// PWM audio driver
PwmAudio speaker(PIN_SPEAKER, SPK_LATENCY, 8, (float)SYS_CLK_FREQ / SPK_PWM_FREQ, apu_dma_handler);

int main() {
    // setup clocks
    vreg_set_voltage(VREG_VOLTAGE_1_30);
    sleep_ms(100);
    stdio_init_all();
    set_sys_clock_khz(SYS_CLK_FREQ / 1000, true);
    setup_default_uart();
    
    // setup monitor pin
    gpio_init(PIN_MONITOR);
    gpio_set_dir(PIN_MONITOR, GPIO_OUT);
    gpio_put(PIN_MONITOR, 0);

    i2ckbd::init_i2c_kbd();
    device_init();
    nunchuck_init();

    picocalc::init(SYS_CLK_FREQ);

    // initialize shapones emulator core
    auto nes_cfg = shapones::get_default_config();
    nes_cfg.apu_sampling_rate = SPK_PWM_FREQ;
    shapones::init(nes_cfg);

    // show boot menu
    if ( ! boot_menu()) {
        for(;;) sleep_ms(100);
    }

    // boot game
    boot_nes();

    return 0;
}

static void boot_nes() {
    // reset
    shapones::reset();

    // start APU loop
    apu_fill_buffer(speaker.get_buffer(0));
    apu_fill_buffer(speaker.get_buffer(1));
    speaker.play();
    
    // start PPU loop
    multicore_launch_core1(ppu_loop);

    // start CPU loop
    cpu_loop();
}


static void cpu_loop() {
    auto t_last_frame = get_absolute_time();
    int frame_count = 0;
    char fps_str[16];
    for(;;) {
        // run CPU
        shapones::cpu::service();

        // update input status: keyboard (via ISR) OR nunchuck (polled per frame)
        shapones::input::status_t input_status;
        input_status.raw = 0;
        for(int i = 0; i < 8; i++) {
            if (input_pins[i] == 1 || nunchuck_pins[i] == 1) {
                input_status.raw |= (1 << i);
            }
        }
        if( (input_status.raw & 0x0C) == 0x0C) {
            watchdog_enable(1, 1);
            watchdog_reboot(0, 0, 0);
        }
        shapones::input::set_status(0, input_status);

        // check line buffer FIFO state
        int fifo_rptr = line_fifo_rptr;
        if (line_fifo_wptr != fifo_rptr) {
            // convert palette indices to RGB565 with 5/4 scale (256x240 -> 320x300)
            int y = line_buff[fifo_rptr * LINE_FIFO_STRIDE];
            int out_y = (y * 5) / 4;
            uint8_t *rd_ptr = line_buff + (fifo_rptr * LINE_FIFO_STRIDE + 1);
            uint8_t *wr_ptr = frame_buff + (out_y * FRAME_BUFF_STRIDE);

            // horizontal scale 256->320: Bresenham, no division in inner loop
            int src_x = 0, err = 0;
            for (int x = 0; x < FRAME_BUFF_WIDTH; x++) {
                uint16_t c0 = COLOR_TABLE[rd_ptr[src_x] & 0x3f];
                *(wr_ptr++) = (c0 >> 8) & 0xff;
                *(wr_ptr++) = c0 & 0xff;
                err += shapones::SCREEN_WIDTH;
                if (err >= FRAME_BUFF_WIDTH) {
                    err -= FRAME_BUFF_WIDTH;
                    src_x++;
                }
            }

            // vertical scale 240->300: every 4th input line is doubled (y%4==3)
            if (y % 4 == 3) {
                memcpy(frame_buff + (out_y + 1) * FRAME_BUFF_STRIDE,
                       frame_buff + out_y * FRAME_BUFF_STRIDE,
                       FRAME_BUFF_STRIDE);
            }

            line_fifo_rptr = (fifo_rptr + 1) % LINE_FIFO_DEPTH;

            if (y == shapones::SCREEN_HEIGHT - 1) {
                nunchuck_poll();
                // fps measurement
                auto t_now = get_absolute_time();
                if (frame_count < 60-1) {
                    frame_count++;
                }
                else {
                    auto t_diff = absolute_time_diff_us(t_last_frame, t_now);
                    float fps = (60.0f * 1000000) / t_diff;
                    sprintf(fps_str, "%5.2ffps", fps);
                    t_last_frame = t_now;
                    frame_count = 0;
                }

                // DMA transfer
                picocalc::finish_write_data();
                //draw_string(0, 0, fps_str);
                picocalc::start_write_data((picocalc::WIDTH - FRAME_BUFF_WIDTH) / 2, (picocalc::HEIGHT - FRAME_BUFF_HEIGHT) / 2, FRAME_BUFF_WIDTH, FRAME_BUFF_HEIGHT, frame_buff);
                static int tmp = 0;
                //gpio_put(PIN_MONITOR, tmp);
                tmp ^= 1;
            }
        }
    }
}


static void ppu_loop() {
    constexpr int FRAME_DELAY_US = 16666;
    absolute_time_t next_time = delayed_by_us(get_absolute_time(), FRAME_DELAY_US);
    
    for(;;) {
        int wptr = line_fifo_wptr;
        shapones::ppu::status_t ppu_status;
        shapones::ppu::service(&line_buff[wptr * LINE_FIFO_STRIDE + 1], false, &ppu_status);
        int y = ppu_status.focus_y;
        if (ppu_status.is_end_of_visible_line() && y < shapones::SCREEN_HEIGHT) {
            // Vsync
            if (y == 0) {
                busy_wait_until(next_time);
                next_time = delayed_by_us(get_absolute_time(), FRAME_DELAY_US);
            }

            // push new line
            line_buff[wptr * LINE_FIFO_STRIDE] = y;
            line_fifo_wptr = (wptr + 1) % LINE_FIFO_DEPTH;
        }
    }
}

static void apu_dma_handler() {
    speaker.flip_buffer();
    apu_fill_buffer(speaker.get_next_buffer());
}

static void apu_fill_buffer(PwmAudio::sample_t *buff) {
    shapones::apu::service(spk_buff, speaker.LATENCY);
    for (int i = 0; i < speaker.LATENCY; i++) {
        buff[i] = spk_buff[i];
    }
}