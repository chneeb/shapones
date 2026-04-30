
#include "picocalc.hpp"
#include "picocalc_c.h"
#include "picocalc.pio.h"

namespace picocalc {

static int sys_clock_hz;
static direction_t curr_dir = EMPTY;
static int curr_speed = 10 * MHZ;

static PIO spi_pio;
static uint spi_sm;
static uint spi_offset;
static uint dma_tx;

// SPI push byte
static void pio_push(uint8_t data);

// SPI pop byte
static uint8_t pio_pop();

// wait for SPI to idle
static void pio_wait_idle();

// setup SPI direction and speed
static void setup_pio(direction_t new_dir, int new_speed);

// set SPI direction
static void set_spi_direction(direction_t new_dir);

void init(int sys_clk_hz) {
    sys_clock_hz = sys_clk_hz;

    spi_pio = pio0;
    spi_sm = 0;

    spi_offset = pio_add_program(spi_pio, &picocalc_program );
    pio_gpio_init(spi_pio, PIN_MISO);
    pio_gpio_init(spi_pio, PIN_MOSI);
    pio_gpio_init(spi_pio, PIN_SCK);
    pio_sm_set_consecutive_pindirs(spi_pio, spi_sm, PIN_MISO, 1, false);
    pio_sm_set_consecutive_pindirs(spi_pio, spi_sm, PIN_MOSI, 1, true);
    pio_sm_set_consecutive_pindirs(spi_pio, spi_sm, PIN_SCK, 1, true);

    dma_tx = dma_claim_unused_channel(true);
    
    gpio_init(PIN_RST);
    gpio_init(PIN_DC);
    gpio_init(PIN_LCD_CS);
    gpio_init(PIN_RAM_CS);
    gpio_set_dir(PIN_RST, GPIO_OUT);
    gpio_set_dir(PIN_DC, GPIO_OUT);
    gpio_set_dir(PIN_RAM_CS,GPIO_OUT);
    gpio_set_dir(PIN_LCD_CS, GPIO_OUT);

    gpio_put(PIN_RST, 1);
    gpio_put(PIN_LCD_CS, 1);
    gpio_put(PIN_RAM_CS,1);
    
    // hardware reset
    gpio_put(PIN_RST, 1);
    sleep_ms(1);
    gpio_put(PIN_RST, 0);
    sleep_ms(10);
    gpio_put(PIN_RST, 1);
    sleep_ms(10);

    {
        uint8_t data[] = {0xc3};
        write_command(0xF0,data,sizeof(data));
    }
    {
        uint8_t data[] = {0x96};
        write_command(0xF0,data,sizeof(data));
    }

    {
        uint8_t data[] = {0x48};
        write_command(0x36,data,sizeof(data));
    }

    {
        uint8_t data[] = {0x65};
        write_command(0x3A,data,sizeof(data));
    }
    {
        // Frame Rate Control
        uint8_t data[] = {0xA0};
        write_command(0xB1,data,sizeof(data));
    }
    {
        uint8_t data[] = {0x00};
        write_command(0xB4,data,sizeof(data));
    }
    {
        uint8_t data[] = {0xc6};
        write_command(0xB7,data,sizeof(data));
    }
    {
        uint8_t data[] = {0x02,0xE0};
        write_command(0xB9,data,sizeof(data));
    }

    {
        uint8_t data[] = {0x80,0x06};
        write_command(0xC0,data,sizeof(data));
    }

    {
        uint8_t data[] = {0x15};
        write_command(0xC1,data,sizeof(data));
    }

    {
        uint8_t data[] = {0xA7};
        write_command(0xC2,data,sizeof(data));
    }
    {
        uint8_t data[] = {0x04};
        write_command(0xC5,data,sizeof(data));
    }

    {
        uint8_t data[] = {0x40,0x8A,0x00,0x00,0x29,0x19,0xAA,0x33};
        write_command(0xE8,data,sizeof(data));
    }

    {
        uint8_t data[] = {0xF0,0x06,0x0F,0x05,0x04,0x20,0x37,0x33,0x4C,0x37,0x13,0x14,0x2B,0x31};
        write_command(0xE0,data,sizeof(data));
    }

    {
        uint8_t data[] = {0xF0,0x11,0x1B,0x11,0x0F,0x0A,0x37,0x43,0x4C,0x37,0x13,0x13,0x2C,0x32};
        write_command(0xE1,data,sizeof(data));
    }

    {
        uint8_t data[] = {0x3C};
        write_command(0xF0,data,sizeof(data));
    }

    {
        uint8_t data[] = {0x69};
        write_command(0xF0,data,sizeof(data));
    }

    {
        uint8_t data[] = {0x00};
        write_command(0x35,data,sizeof(data));
    }
    write_command(0x11);//TFT_SLPOUT
    sleep_ms(120);
    //TFT_INVON
    write_command(0x21);

    clear(0);

    write_command(0x29);//TFT_DISPON
    sleep_ms(120);

    {
        uint8_t data[] = {0x00,0x00,0x01,0x3F};
        write_command(0x2A,data,sizeof(data));
    }

    {
        uint8_t data[] = {0x00,0x00,0x01,0x3F};
        write_command(0x2B,data,sizeof(data));
    }
    write_command(0x2C);

/*
    {
        //// Positive Gamma Control
        uint8_t data[] = {0x00,0x03,0x09,0x08,0x16,0x0a,0x3f,0x78,0x4c,0x09,0x0a,0x08,0x16,0x1a,0x0f};
        write_command(0xE0,data,sizeof(data));
    }
    {
        //// Negative Gamma Control
        uint8_t data[] = {0x00,0x16,0x19,0x03,0x0f,0x05,0x32,0x45,0x46,0x04,0x0e,0x0d,0x35,0x37,0x0f};
        write_command(0xE1,data,sizeof(data));
    }
    {
        // Power Control 1
        uint8_t data[] = {0x17,0x15};
        write_command(0xC0,data,sizeof(data));
    }
    {
        // Power Control 2
        uint8_t data[] = {0x41};
        write_command(0xC1,data,sizeof(data));
    }
    {
        //// VCOM Control
        uint8_t data[] = {0x00,0x12,0x80};
        write_command(0xC5,data,sizeof(data));
    }
    {
        // Memory Access Control
        //MX,MV, RGB mode
        uint8_t data[] = {0x48}; // (0x40 | 0x20) or 0x48
        write_command(0x36,data,sizeof(data));
    }

    {
        // Pixel Interface Format
        uint8_t data[] = {0x65}; //0x65=16 bit colour for SPI,0x66=18bits
        write_command(0x3A,data,sizeof(data));
    }

    {
        // Interface Mode Control
        uint8_t data[] = {0x00};
        write_command(0xB0,data,sizeof(data));
    }
    {
        // Frame Rate Control
        uint8_t data[] = {0xA0};
        write_command(0xB1,data,sizeof(data));
    }

    {
        // Display Inversion Control
        uint8_t data[] = {0x02};
        write_command(0xB4,data,sizeof(data));
    }

    {
        // Display Function Control
        uint8_t data[] = {0x02,0x02,0x3B};
        write_command(0xB6,data,sizeof(data));
    }

    {
        // Entry Mode Set
        uint8_t data[] = {0xC6,0xE9,0x00};
        write_command(0xB7,data,sizeof(data));
    }

    {
        // Adjust Control 3
        uint8_t data[] = {0xA9,0x51,0x2C,0x82};
        write_command(0xF7,data,sizeof(data));
    }

        write_command(0x11);//TFT_SLPOUT
        sleep_ms(120);

        //TFT_INVON
        write_command(0x21);

        clear(0);

        write_command(0x29);//TFT_DISPON
        sleep_ms(120);
*/
}

void setup_pio(direction_t new_dir, int new_speed) {
    if (new_dir == curr_dir && new_speed == curr_speed) return;

    float div = sys_clock_hz / 2.f / new_speed;
    
    pio_sm_set_enabled(spi_pio, spi_sm, false);

    // load new PIO
    if (new_dir == TX) {
        pio_sm_config pio_cfg = picocalc_program_get_default_config(spi_offset);
        sm_config_set_out_pins(&pio_cfg, PIN_MOSI, 1);
        sm_config_set_sideset_pins(&pio_cfg, PIN_SCK);
        sm_config_set_fifo_join(&pio_cfg, PIO_FIFO_JOIN_TX);
        sm_config_set_out_shift(&pio_cfg, false, true, 8);
        sm_config_set_clkdiv(&pio_cfg, div);
        pio_sm_init(spi_pio, spi_sm, spi_offset, &pio_cfg);
    }
    else if (new_dir == RX) {
        pio_sm_config pio_cfg = picocalc_program_get_default_config(spi_offset);
        sm_config_set_out_pins(&pio_cfg, PIN_MOSI, 1);
        sm_config_set_in_pins(&pio_cfg, PIN_MISO);
        sm_config_set_sideset_pins(&pio_cfg, PIN_SCK);
        sm_config_set_out_shift(&pio_cfg, false, true, 8);
        sm_config_set_in_shift(&pio_cfg, false, true, 8);
        sm_config_set_clkdiv(&pio_cfg, div);
        pio_sm_init(spi_pio, spi_sm, spi_offset, &pio_cfg);
    }

    hw_set_bits(&spi_pio->input_sync_bypass, 1u << PIN_MISO);
    pio_sm_set_enabled(spi_pio, spi_sm, true);

    curr_dir = new_dir;
    curr_speed = new_speed;
}

void set_spi_direction(direction_t new_dir) {
    setup_pio(new_dir, curr_speed);
}

void set_spi_speed(int new_speed) {
    setup_pio(curr_dir, new_speed);
}

void clear(uint16_t color) {
    uint8_t data[WIDTH * 2];
    for (int x = 0; x < WIDTH * 2; x++) {
        data[x] = color;
    }
    for (int y = 0; y < HEIGHT; y++) {
        start_write_data(0, y, WIDTH, 1, data);
        finish_write_data();
    }
}

void start_write_data(int x0, int y0, int w, int h, uint8_t *data) {
    int x1 = x0 + w - 1;
    int y1 = y0 + h - 1;
    {
        uint8_t xcoord[] = {
            (uint8_t)(x0 >> 8), 
            (uint8_t)(x0 & 0xff), 
            (uint8_t)(x1 >> 8), 
            (uint8_t)(x1 & 0xff)
        };
        write_command(0x2a, xcoord, sizeof(xcoord));
    }
    {
        uint8_t ycoord[] = {
            (uint8_t)(y0 >> 8), 
            (uint8_t)(y0 & 0xff), 
            (uint8_t)(y1 >> 8), 
            (uint8_t)(y1 & 0xff)
        };
        write_command(0x2b, ycoord, sizeof(ycoord));
    }
    //write_command(0x2c, data, w * h * 3 / 2);

    {
        gpio_put(PIN_DC, 0);
        gpio_put(PIN_LCD_CS, 0);
        uint8_t cmd = 0x2c;
        write_blocking(&cmd, 1);
        gpio_put(PIN_DC, 1);
        
        {
            dma_channel_config dma_cfg = dma_channel_get_default_config(dma_tx);
            channel_config_set_transfer_data_size(&dma_cfg, DMA_SIZE_8);
            channel_config_set_dreq(&dma_cfg, pio_get_dreq(spi_pio, spi_sm, true));
            dma_channel_configure(dma_tx, &dma_cfg,
                                    &spi_pio->txf[spi_sm], // write address
                                    data, // read address
                                    w * h * 2, // element count (each element is of size transfer_data_size)
                                    false); // don't start yet
        }
        
        dma_start_channel_mask(1u << dma_tx);
    }

}

void finish_write_data() {
    dma_channel_wait_for_finish_blocking(dma_tx);
    pio_wait_idle();
    gpio_put(PIN_LCD_CS, 1);
}

void write_command(uint8_t cmd, const uint8_t *data, int len) {
    set_spi_direction(TX);
    gpio_put(PIN_DC, 0); // command mode
    gpio_put(PIN_LCD_CS, 0);
    write_blocking(&cmd, 1);
    if (data) {
        gpio_put(PIN_DC, 1); // data mode
        write_blocking(data, len);
    }
    gpio_put(PIN_LCD_CS, 1);
}

void write_command(uint8_t cmd) {
    set_spi_direction(TX);
    write_command(cmd, nullptr, 0);
}

void write_blocking(const uint8_t *data, int len) {
    set_spi_direction(TX);
    for(int i = 0; i < len; i++) {
        pio_push(data[i]);
    }
    pio_wait_idle();
}

void read_blocking(uint8_t tx_repeat, uint8_t *buff, int len) {
    set_spi_direction(RX);
    int tx_remain = len, rx_remain = len;
    io_rw_8 *txfifo = (io_rw_8 *) &spi_pio->txf[spi_sm];
    io_rw_8 *rxfifo = (io_rw_8 *) &spi_pio->rxf[spi_sm];
    while (tx_remain || rx_remain) {
        if (tx_remain && !pio_sm_is_tx_fifo_full(spi_pio, spi_sm)) {
            *txfifo = tx_repeat;
            --tx_remain;
        }
        if (rx_remain && !pio_sm_is_rx_fifo_empty(spi_pio, spi_sm)) {
            *buff++ = *rxfifo;
            --rx_remain;
        }
    }
}

static void pio_push(uint8_t data) {
    while (pio_sm_is_tx_fifo_full(spi_pio, spi_sm)) { }
    *(volatile uint8_t*)&spi_pio->txf[spi_sm] = data;
}

static uint8_t pio_pop() {
    while (pio_sm_is_rx_fifo_empty(spi_pio, spi_sm)) { }
    return *(volatile uint8_t*)&spi_pio->rxf[spi_sm];
}

static void pio_wait_idle() {
    uint32_t stall_mask = 1u << (spi_sm + PIO_FDEBUG_TXSTALL_LSB);
    spi_pio->fdebug = stall_mask;
    while (!(spi_pio->fdebug & stall_mask)) { }
}

extern "C" {

void ws19804_write_blocking(const uint8_t *buff, int len) {
    picocalc::write_blocking(buff, len);
}
void ws19804_read_blocking(uint8_t tx_repeat, uint8_t *buff, int len) {
    picocalc::read_blocking(tx_repeat, buff, len);
}

}

}
