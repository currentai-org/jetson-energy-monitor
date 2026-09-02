/* ina3221.c -- see ina3221.h. Register map/values verbatim from
 * src/ina3221.py (TI INA3221 datasheet SBOS689). */
#include "ina3221.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/i2c-dev.h>
#include <linux/i2c.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define REG_CONFIG 0x00
#define SHUNT_LSB_UV 40.0

static uint8_t shunt_reg(int channel) {
    switch (channel) {
        case 1: return 0x01;
        case 2: return 0x03;
        case 3: return 0x05;
        default: return 0x01;
    }
}

static uint8_t bus_reg(int channel) {
    switch (channel) {
        case 1: return 0x02;
        case 2: return 0x04;
        case 3: return 0x06;
        default: return 0x02;
    }
}

static int i2c_read_word(int fd, int address, uint8_t reg, uint16_t *out) {
    uint8_t reg_buf[1] = {reg};
    uint8_t data_buf[2] = {0, 0};
    struct i2c_msg msgs[2] = {
        {.addr = (uint16_t)address, .flags = 0, .len = 1, .buf = reg_buf},
        {.addr = (uint16_t)address, .flags = I2C_M_RD, .len = 2, .buf = data_buf},
    };
    struct i2c_rdwr_ioctl_data rdwr = {.msgs = msgs, .nmsgs = 2};
    if (ioctl(fd, I2C_RDWR, &rdwr) < 0) return -1;
    *out = ((uint16_t)data_buf[0] << 8) | data_buf[1];
    return 0;
}

static int i2c_write_word(int fd, int address, uint8_t reg, uint16_t value) {
    uint8_t buf[3] = {reg, (uint8_t)(value >> 8), (uint8_t)(value & 0xFF)};
    struct i2c_msg msg = {.addr = (uint16_t)address, .flags = 0, .len = 3, .buf = buf};
    struct i2c_rdwr_ioctl_data rdwr = {.msgs = &msg, .nmsgs = 1};
    return ioctl(fd, I2C_RDWR, &rdwr) < 0 ? -1 : 0;
}

int ina3221_open(Ina3221 *dev, int bus_num, int address, double shunt_ohms) {
    char path[64];
    snprintf(path, sizeof(path), "/dev/i2c-%d", bus_num);
    dev->fd = open(path, O_RDWR);
    if (dev->fd < 0) return -1;
    dev->address = address;
    dev->shunt_ohms = shunt_ohms;
    dev->orig_config_valid = 0;
    dev->orig_config = 0;
    return 0;
}

int ina3221_save_config_for_restore(Ina3221 *dev) {
    if (dev->orig_config_valid) return 0;
    uint16_t word;
    if (i2c_read_word(dev->fd, dev->address, REG_CONFIG, &word) < 0) return -1;
    dev->orig_config = word;
    dev->orig_config_valid = 1;
    return 0;
}

int ina3221_configure_fast_single_channel(Ina3221 *dev, int channel) {
    if (ina3221_save_config_for_restore(dev) < 0) return -1;
    uint16_t word = 0;
    if (channel == 1) word |= (1 << 14);
    if (channel == 2) word |= (1 << 13);
    if (channel == 3) word |= (1 << 12);
    /* vbusct = 0b000 (140us), vshct = 0b000 (140us), avg = 0b000 (1 sample)
     * -- all already zero, left explicit here only in comments to mirror
     * ina3221.py's Ina3221Config fields for easy comparison. */
    word |= 0b111; /* mode: continuous shunt + bus */
    return i2c_write_word(dev->fd, dev->address, REG_CONFIG, word);
}

int ina3221_restore_config(Ina3221 *dev) {
    if (!dev->orig_config_valid) return 0;
    uint16_t word = dev->orig_config;
    dev->orig_config_valid = 0;
    return i2c_write_word(dev->fd, dev->address, REG_CONFIG, word);
}

int ina3221_read_current_ma(Ina3221 *dev, int channel, double *out) {
    uint16_t raw;
    if (i2c_read_word(dev->fd, dev->address, shunt_reg(channel), &raw) < 0) return -1;
    int16_t signed_raw = (int16_t)raw;
    int32_t signed_val = ((int32_t)signed_raw) >> 3; /* 13-bit, left-justified */
    double uv = signed_val * SHUNT_LSB_UV;
    double amps = (uv * 1e-6) / dev->shunt_ohms;
    *out = amps * 1000.0;
    return 0;
}

int ina3221_read_bus_voltage_v(Ina3221 *dev, int channel, double *out) {
    uint16_t raw;
    if (i2c_read_word(dev->fd, dev->address, bus_reg(channel), &raw) < 0) return -1;
    int16_t signed_raw = (int16_t)raw;
    int32_t signed_val = ((int32_t)signed_raw) >> 3;
    *out = signed_val * 0.008; /* 8mV/LSB */
    return 0;
}

void ina3221_close(Ina3221 *dev) {
    if (dev->fd >= 0) {
        close(dev->fd);
        dev->fd = -1;
    }
}

const char *ina3221_channel_name(int channel) {
    switch (channel) {
        case 1: return "VDD_IN";
        case 2: return "VDD_CPU_GPU";
        case 3: return "VDD_SOC";
        default: return "unknown";
    }
}

int ina3221_channel_from_name(const char *name) {
    if (strcmp(name, "VDD_IN") == 0) return 1;
    if (strcmp(name, "VDD_CPU_GPU") == 0) return 2;
    if (strcmp(name, "VDD_SOC") == 0) return 3;
    return 0;
}
