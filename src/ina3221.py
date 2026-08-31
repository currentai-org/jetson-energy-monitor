"""
Direct I2C driver for the TI INA3221 triple-channel current/voltage monitor,
as found on the NVIDIA Jetson Orin Nano Developer Kit (I2C addr 0x40, bus 1).

Bypasses the Linux kernel hwmon driver entirely -- talks to the chip's
registers directly over /dev/i2c-1 via smbus2. This gives us full control
over conversion time / averaging, which the stock hwmon config does not
expose at a usable sample rate (default on this board multiplexes all three
channels with a slow ~4.156ms bus-voltage conversion time -> ~77Hz aggregate).

No sudo required: the `ubuntu` user is already a member of the `i2c` group,
which owns /dev/i2c-1 (crw-rw---- root:i2c).
"""
from __future__ import annotations

import atexit
import time
from dataclasses import dataclass

import smbus2

# --- Register map (TI INA3221 datasheet, SBOS689) ---
REG_CONFIG = 0x00
REG_SHUNT = {1: 0x01, 2: 0x03, 3: 0x05}
REG_BUS = {1: 0x02, 2: 0x04, 3: 0x06}
REG_MASK_ENABLE = 0x0F

# Conversion time codes (VBUSCT / VSHCT), microseconds
CONV_TIME_US = {
    0b000: 140,
    0b001: 204,
    0b010: 332,
    0b011: 588,
    0b100: 1100,
    0b101: 2116,
    0b110: 4156,
    0b111: 8244,
}

# Averaging mode codes -> number of samples averaged on-chip
AVG_SAMPLES = {
    0b000: 1,
    0b001: 4,
    0b010: 16,
    0b011: 64,
    0b100: 128,
    0b101: 256,
    0b110: 512,
    0b111: 1024,
}

MODE_CONTINUOUS_SHUNT_BUS = 0b111
MODE_POWER_DOWN = 0b000

SHUNT_LSB_UV = 40.0  # microvolts per LSB, 13-bit signed shunt voltage register

# Human-readable names for the three INA3221 channels as wired on the Jetson
# Orin Nano Developer Kit carrier board. Channel numbers (1/2/3) remain the
# internal representation everywhere (register maps, Sampler, etc.) since
# they map directly onto the datasheet's REG_SHUNT/REG_BUS tables -- these
# mappings exist purely for CLI args and UI display.
CHANNEL_NAMES: dict[int, str] = {
    1: "VDD_IN",  # total board input power
    2: "VDD_CPU_GPU",  # combined CPU+GPU+CV rail (labeled VDD_CPU_GPU_CV on schematic)
    3: "VDD_SOC",  # SoC rail
}
NAME_TO_CHANNEL: dict[str, int] = {name: ch for ch, name in CHANNEL_NAMES.items()}


def channel_name(channel: int) -> str:
    """Human-readable name for a channel number (1/2/3), e.g. 'VDD_IN'."""
    return CHANNEL_NAMES.get(channel, f"channel {channel}")


def channel_from_name(name: str) -> int:
    """Inverse of channel_name(); raises ValueError on an unrecognized name."""
    try:
        return NAME_TO_CHANNEL[name]
    except KeyError:
        valid = ", ".join(NAME_TO_CHANNEL)
        raise ValueError(f"Unknown INA3221 channel name {name!r}; expected one of: {valid}")


@dataclass
class Ina3221Config:
    ch1_enable: bool = True
    ch2_enable: bool = False
    ch3_enable: bool = False
    vbusct: int = 0b000  # 140us
    vshct: int = 0b000  # 140us
    avg: int = 0b000  # 1 sample, no on-chip averaging
    mode: int = MODE_CONTINUOUS_SHUNT_BUS

    def to_word(self) -> int:
        word = 0
        word |= (1 if self.ch1_enable else 0) << 14
        word |= (1 if self.ch2_enable else 0) << 13
        word |= (1 if self.ch3_enable else 0) << 12
        word |= (self.vbusct & 0b111) << 9
        word |= (self.vshct & 0b111) << 6
        word |= (self.avg & 0b111) << 3
        word |= self.mode & 0b111
        return word

    @classmethod
    def from_word(cls, word: int) -> "Ina3221Config":
        return cls(
            ch1_enable=bool(word & (1 << 14)),
            ch2_enable=bool(word & (1 << 13)),
            ch3_enable=bool(word & (1 << 12)),
            vbusct=(word >> 9) & 0b111,
            vshct=(word >> 6) & 0b111,
            avg=(word >> 3) & 0b111,
            mode=word & 0b111,
        )

    @property
    def conversion_period_s(self) -> float:
        """Time for one full shunt+bus conversion cycle across enabled channels."""
        n_channels = sum([self.ch1_enable, self.ch2_enable, self.ch3_enable]) or 1
        per_channel_us = CONV_TIME_US[self.vshct] + CONV_TIME_US[self.vbusct]
        avg_mult = AVG_SAMPLES[self.avg]
        return (per_channel_us * avg_mult * n_channels) / 1e6

    @property
    def max_rate_hz(self) -> float:
        return 1.0 / self.conversion_period_s


class INA3221:
    """Raw-register INA3221 access over smbus2, no kernel hwmon involvement."""

    def __init__(self, bus_num: int = 1, address: int = 0x40, shunt_ohms: float = 0.005):
        self.address = address
        self.shunt_ohms = shunt_ohms
        self.bus = smbus2.SMBus(bus_num)
        self._orig_config_word: int | None = None

    # -- low level register access --
    def _read_word(self, reg: int) -> int:
        w = smbus2.i2c_msg.write(self.address, [reg])
        r = smbus2.i2c_msg.read(self.address, 2)
        self.bus.i2c_rdwr(w, r)
        data = list(r)
        return (data[0] << 8) | data[1]

    def _write_word(self, reg: int, value: int) -> None:
        hi = (value >> 8) & 0xFF
        lo = value & 0xFF
        w = smbus2.i2c_msg.write(self.address, [reg, hi, lo])
        self.bus.i2c_rdwr(w)

    # -- configuration --
    def read_config(self) -> Ina3221Config:
        return Ina3221Config.from_word(self._read_word(REG_CONFIG))

    def write_config(self, cfg: Ina3221Config) -> None:
        self._write_word(REG_CONFIG, cfg.to_word())

    def save_config_for_restore(self) -> None:
        """Snapshot the current config so it can be restored on exit (best-effort;
        does not fully undo kernel-driver-managed alert/mask registers, only the
        main config register that we mutate)."""
        if self._orig_config_word is None:
            self._orig_config_word = self._read_word(REG_CONFIG)
            self._atexit_registered = True
            atexit.register(self.restore_config)

    def restore_config(self) -> None:
        """Idempotent: safe to call explicitly and let the atexit hook fire
        again afterwards (e.g. once explicitly on app shutdown, once more via
        atexit) -- second call is a no-op once the word is cleared, and any
        stray OSError/ValueError from an already-closed bus fd is swallowed."""
        if self._orig_config_word is not None:
            word = self._orig_config_word
            self._orig_config_word = None
            try:
                self._write_word(REG_CONFIG, word)
            except (OSError, ValueError, TypeError):
                pass

    def configure_fast_single_channel(self, channel: int = 1) -> Ina3221Config:
        """Configure for maximum sample rate on a single channel (default:
        channel 1 / VDD_IN, total board input power). See CHANNEL_NAMES for
        the full channel-number-to-rail-name mapping."""
        self.save_config_for_restore()
        cfg = Ina3221Config(
            ch1_enable=(channel == 1),
            ch2_enable=(channel == 2),
            ch3_enable=(channel == 3),
            vbusct=0b000,
            vshct=0b000,
            avg=0b000,
            mode=MODE_CONTINUOUS_SHUNT_BUS,
        )
        self.write_config(cfg)
        return cfg

    # -- measurement --
    def read_shunt_voltage_uv(self, channel: int) -> float:
        raw = self._read_word(REG_SHUNT[channel])
        # 13-bit signed value, left-justified in bits 15:3
        signed = raw if raw < 0x8000 else raw - 0x10000
        signed >>= 3
        return signed * SHUNT_LSB_UV

    def read_bus_voltage_v(self, channel: int) -> float:
        raw = self._read_word(REG_BUS[channel])
        signed = raw if raw < 0x8000 else raw - 0x10000
        signed >>= 3
        return signed * 0.008  # 8mV/LSB

    def read_current_ma(self, channel: int = 1) -> float:
        uv = self.read_shunt_voltage_uv(channel)
        amps = (uv * 1e-6) / self.shunt_ohms
        return amps * 1000.0

    def close(self) -> None:
        try:
            self.bus.close()
        except Exception:
            pass


if __name__ == "__main__":
    dev = INA3221()
    print("Original config:", dev.read_config())
    cfg = dev.configure_fast_single_channel(channel=1)
    print("Fast config:", cfg, "max rate ~%.0f Hz" % cfg.max_rate_hz)
    t0 = time.perf_counter()
    for _ in range(20):
        ma = dev.read_current_ma(1)
        v = dev.read_bus_voltage_v(1)
        print(f"{ma:8.1f} mA  {v:5.3f} V")
        time.sleep(0.001)
    dt = time.perf_counter() - t0
    print(f"20 reads in {dt*1000:.1f} ms -> {20/dt:.0f} Hz achieved")
    dev.restore_config()
