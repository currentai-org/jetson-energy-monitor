# Makefile for the pure-C Jetson Energy Usage CLI (native/ directory).
# See README.md for context: this is a from-scratch C reimplementation of
# src/app.py's --headless mode, built on the ina_bench.c benchmark's
# techniques (raw I2C ioctl, clock_nanosleep pacing, batched I/O).

CC ?= gcc
CFLAGS ?= -O2 -Wall -Wextra -std=gnu11
LDFLAGS = -lm -lpthread

SOURCES = main.c common.c ina3221.c sampler.c sysinfo.c sysmon.c csv_log.c jsonl_log.c tests.c
OBJECTS = $(SOURCES:.c=.o)
TARGET = jeu

.PHONY: all clean bench

all: $(TARGET)

$(TARGET): $(OBJECTS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

# Also build the standalone ina_bench.c reference-point benchmark from the
# earlier spike, kept alongside for comparison.
bench: ina_bench

ina_bench: ina_bench.c
	$(CC) $(CFLAGS) -o $@ $< -lm

clean:
	rm -f $(OBJECTS) $(TARGET) ina_bench *.csv
