#include "common.hpp"
#include "i2ckbd.hpp"

#include <hardware/irq.h>
#include <hardware/structs/timer.h>
#include <hardware/i2c.h>

static absolute_time_t now;
uint8_t keycheck = 0;
uint8_t keyread = 0;
uint8_t frame_buff[FRAME_BUFF_STRIDE * FRAME_BUFF_HEIGHT];
//0 == released, 1 = pressed
int input_pins[] = {
        PIN_PAD_A, PIN_PAD_B, PIN_PAD_SELECT, PIN_PAD_START,
        PIN_PAD_UP, PIN_PAD_DOWN, PIN_PAD_LEFT, PIN_PAD_RIGHT
};

int wait_key() {
    // wait any button pushed
    int i = -1;
    for (;;) {
        sleep_ms(10);
        for (i = 0; i < 8; i++) {
            if (input_pins[i] == 1) {
                input_pins[i] = 0;
                return i;
            }
        }
    }
    return i;
}

void draw_string(int x, int y, const char *str) {
    mono8x16::draw_string_rgb565(
            frame_buff, FRAME_BUFF_STRIDE, FRAME_BUFF_WIDTH, FRAME_BUFF_HEIGHT,
            x, y, str, 0xffff);
}

void clear_frame_buff() {
    for (int i = 0; i < FRAME_BUFF_STRIDE * FRAME_BUFF_HEIGHT; i++) {
        frame_buff[i] = 0;
    }
}

void update_lcd() {
    picocalc::start_write_data((picocalc::WIDTH - FRAME_BUFF_WIDTH) / 2, (picocalc::HEIGHT - FRAME_BUFF_HEIGHT) / 2,
                               FRAME_BUFF_WIDTH, FRAME_BUFF_HEIGHT, frame_buff);
    picocalc::finish_write_data();
}

void setBacklight(int level){//STM32: i2c reg is REG_ID_BKL(0x05)
    //level is 0-100%
    level*=255;
    level/=100;
    i2ckbd::I2C_Send_RegData(i2ckbd::I2C_KBD_ADDR,0x05,(uint8_t)level);
}

//keyboard key status to input_pins map
void set_kdb_key(uint8_t pin_offset, uint8_t key_status) {

    if (key_status == 1) {
        input_pins[pin_offset] = 1;
    } else if (key_status == 3) {
        input_pins[pin_offset] = 0;
    }
}

void kbd_interrupt() {
    int kbd_ret;
    int c;
    static int ctrlheld = 0;
    uint8_t key_stat = 0;//press,release, or hold
    if (keycheck == 0) {
        if (keyread == 0) {
            kbd_ret = i2ckbd::write_i2c_kbd();
            keyread = 1;
        } else {
            kbd_ret = i2ckbd::read_i2c_kbd();
            keyread = 0;
        }
        keycheck=KEYCHECKTIME;
    }
    if (kbd_ret < 0) {
        if (i2ckbd::check_if_failed() > 0) {
            printf("try to reset i2c\n");
            i2ckbd::reset_failed();
            i2ckbd::init_i2c_kbd();//re-init
        }
    }

    if (kbd_ret) {
        if (kbd_ret == 0xA503)ctrlheld = 0;
        else if (kbd_ret == 0xA502) {
            ctrlheld = 1;
        } else if ((kbd_ret & 0xff) == 1) {//pressed
            key_stat = 1;
        } else if ((kbd_ret & 0xff) == 3) {
            key_stat = 3;
        }

        c = kbd_ret >> 8;
        int realc = -1;
        switch (c) {
            case 0xA1:
            case 0xA2:
            case 0xA3:
            case 0xA4:
            case 0xA5:
                realc = -1;//skip shift alt ctrl keys
                break;
            default:
                realc = c;
                break;
        }

        c = realc;
        if (c >= 'a' && c <= 'z' && ctrlheld)c = c - 'a' + 1;

        switch (c) {
            case 0xb5://UP
                set_kdb_key(4, key_stat);
                break;
            case 0xb6://DOWN
                set_kdb_key(5, key_stat);
                break;
            case 0xb4://LEFT
                set_kdb_key(6, key_stat);
                break;
            case 0xb7://RIGHT
                set_kdb_key(7, key_stat);
                break;
            case '-':// select
                set_kdb_key(2, key_stat);
                break;
            case '=':// start
                set_kdb_key(3, key_stat);
                break;
            case '['://B
                set_kdb_key(1, key_stat);
                break;
            case ']'://A
                set_kdb_key(0, key_stat);
                break;
            default:
                break;
        }
    }
}

static void __attribute__ ((optimize("-Os"))) __not_in_flash_func(timer_tick_cb)(unsigned alarm) {

    absolute_time_t next;
    update_us_since_boot(&next, to_us_since_boot(now) + ( TICKSPERSEC));
    if (hardware_alarm_set_target(0, next)) {
        update_us_since_boot(&next, time_us_64() + ( TICKSPERSEC));
        hardware_alarm_set_target(0, next);
    }

    kbd_interrupt();
    if(keycheck){
        keycheck--;
    }

}

static constexpr uint8_t NUNCHUCK_ADDR = 0x52;
static constexpr int NUNCHUCK_SDA = 4;
static constexpr int NUNCHUCK_SCL = 5;

int nunchuck_pins[8] = {};

void nunchuck_init() {
#ifndef DISABLE_NUNCHUCK
    i2c_init(i2c0, 400 * 1000);
    gpio_set_function(NUNCHUCK_SDA, GPIO_FUNC_I2C);
    gpio_set_function(NUNCHUCK_SCL, GPIO_FUNC_I2C);
    gpio_pull_up(NUNCHUCK_SDA);
    gpio_pull_up(NUNCHUCK_SCL);

    uint8_t cmd[2];
    cmd[0] = 0xF0; cmd[1] = 0x55;
    i2c_write_blocking(i2c0, NUNCHUCK_ADDR, cmd, 2, false);
    sleep_ms(1);
    cmd[0] = 0xFB; cmd[1] = 0x00;
    i2c_write_blocking(i2c0, NUNCHUCK_ADDR, cmd, 2, false);
    sleep_ms(1);
    cmd[0] = 0xFE; cmd[1] = 0x03;
    i2c_write_blocking(i2c0, NUNCHUCK_ADDR, cmd, 2, false);
    sleep_ms(1);
#endif
}

void nunchuck_poll() {
#ifndef DISABLE_NUNCHUCK
    uint8_t reg = 0x00;
    if (i2c_write_blocking(i2c0, NUNCHUCK_ADDR, &reg, 1, false) < 0)
        return;
    sleep_us(200);
    uint8_t nc[8];
    if (i2c_read_blocking(i2c0, NUNCHUCK_ADDR, nc, 8, false) != 8)
        return;

    // Write full state each poll — both press (1) and release (0) — active-low
    uint8_t b6 = nc[6], b7 = nc[7];
    nunchuck_pins[0] = !(b7 & 0x10);  // A
    nunchuck_pins[1] = !(b7 & 0x40);  // B
    nunchuck_pins[2] = !(b6 & 0x10);  // Select
    nunchuck_pins[3] = !(b6 & 0x04);  // Start
    nunchuck_pins[4] = !(b7 & 0x01);  // Up
    nunchuck_pins[5] = !(b6 & 0x40);  // Down
    nunchuck_pins[6] = !(b7 & 0x02);  // Left
    nunchuck_pins[7] = !(b6 & 0x80);  // Right
#endif
}

void device_init() {

    hardware_alarm_claim(0);
    update_us_since_boot(&now, time_us_64());
    hardware_alarm_set_callback(0, timer_tick_cb);
    hardware_alarm_force_irq(0);

    //setBacklight(80);
}