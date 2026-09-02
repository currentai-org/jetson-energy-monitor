/*
 * ina3221.h -- direct I2C driver for the TI INA3221, pure C.
 *
 * Mirrors src/ina3221.py's register map and behavior exactly (same
 * register addresses, same "fast single channel" config word, same
 * shunt-voltage-to-mA conversion), so results are comparable between the
 * Python and native implementations. See ina3221.py's module docstring
 * for full background on why this bypasses the kernel hwmon driver.
 */
#ifndef JEU_INA3221_H
#define JEU_INA3221_H

#include <stdint.h>

typedef struct {
    int fd;
    int address;
    double shunt_ohms;
    int orig_config_valid; /* 1 once orig_config holds a real snapshot */
    uint16_t orig_config;
} Ina3221;

/* Opens /dev/i2c-<bus_num> and returns 0 on success, -1 on failure (errno
 * set). Does NOT call ioctl(I2C_SLAVE, ...) -- see ina_bench.c's comment
 * on why (the kernel hwmon driver already owns that address; I2C_RDWR
 * carries the address per-message instead). */
int ina3221_open(Ina3221 *dev, int bus_num, int address, double shunt_ohms);

/* Reads the current CONFIG register into dev->orig_config (only the first
 * call has an effect; safe to call multiple times) so it can be restored
 * later via ina3221_restore_config(). Returns 0 on success, -1 on failure. */
int ina3221_save_config_for_restore(Ina3221 *dev);

/* Writes the "fast single channel" config for the given channel (1/2/3):
 * VBUSCT=VSHCT=140us, AVG=1 (no on-chip averaging), continuous shunt+bus
 * mode, matching ina3221.py's configure_fast_single_channel(). Returns 0
 * on success, -1 on failure. */
int ina3221_configure_fast_single_channel(Ina3221 *dev, int channel);

/* Restores the config register saved by ina3221_save_config_for_restore()
 * (no-op if never saved, or already restored -- idempotent like the
 * Python version). Returns 0 on success, -1 on failure. */
int ina3221_restore_config(Ina3221 *dev);

/* Reads instantaneous shunt current in mA for the given channel (1/2/3).
 * Returns 0 on success (out set), -1 on I2C failure (errno set, out
 * untouched). */
int ina3221_read_current_ma(Ina3221 *dev, int channel, double *out);

/* Reads instantaneous bus voltage in volts for the given channel.
 * Returns 0 on success (out set), -1 on I2C failure. */
int ina3221_read_bus_voltage_v(Ina3221 *dev, int channel, double *out);

/* Closes the underlying file descriptor. Safe to call once; dev->fd is
 * set to -1 afterwards. */
void ina3221_close(Ina3221 *dev);

/* Human-readable rail name for a channel number (1/2/3), mirroring
 * ina3221.py's CHANNEL_NAMES: "VDD_IN" / "VDD_CPU_GPU" / "VDD_SOC". */
const char *ina3221_channel_name(int channel);

/* Inverse of ina3221_channel_name(): returns the channel number (1/2/3)
 * for a name, or 0 if unrecognized. */
int ina3221_channel_from_name(const char *name);

#endif /* JEU_INA3221_H */
