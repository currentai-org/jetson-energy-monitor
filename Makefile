# Makefile for the Jetson Energy Usage CLI/TUI (jeu), pure C, no external
# dependencies beyond libc/libm/libpthread. See README.md for usage and
# docs/DEVLOG.md for the design history.
#
# Source lives in src/; build artifacts go to build/; the jeu/ina_bench
# binaries themselves land at the repo root so `./jeu ...` keeps working
# exactly as documented in README.md.

CC ?= gcc
CFLAGS ?= -O2 -Wall -Wextra -std=gnu11
LDFLAGS = -lm -lpthread
PREFIX ?= /usr/local

SRC_DIR = src
BUILD_DIR = build

SOURCES = main.c common.c ina3221.c sampler.c sysinfo.c sysmon.c csv_log.c jsonl_log.c tests.c \
          term.c screen.c sparkline.c tui.c
OBJECTS = $(addprefix $(BUILD_DIR)/,$(SOURCES:.c=.o))
TARGET = jeu

.PHONY: all clean bench install uninstall

all: $(TARGET)

$(TARGET): $(OBJECTS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

# Also build the standalone ina_bench.c reference-point benchmark from the
# early performance investigation (see docs/DEVLOG.md), kept alongside for
# comparison against jeu's own footprint.
bench: ina_bench

ina_bench: $(SRC_DIR)/ina_bench.c
	$(CC) $(CFLAGS) -o $@ $< -lm

install: $(TARGET)
	install -d $(DESTDIR)$(PREFIX)/bin
	install -m 755 $(TARGET) $(DESTDIR)$(PREFIX)/bin/$(TARGET)

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/$(TARGET)

clean:
	rm -rf $(BUILD_DIR) $(TARGET) ina_bench *.csv
