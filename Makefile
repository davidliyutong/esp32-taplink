.PHONY: all setup update init \
        build flash monitor flash-monitor menuconfig clean fullclean \
        format lint help

.DEFAULT_GOAL := build

# ---------- ESP-IDF local installation ----------
IDF_VERSION    ?= v5.5.3
IDF_TARGET     ?= esp32s3
IDF_PATH       := $(CURDIR)/.esp-idf
IDF_TOOLS_PATH := $(CURDIR)/.espressif

export IDF_PATH
export IDF_TOOLS_PATH

SHELL := /bin/bash

IDF_ACTIVATE := . $(IDF_PATH)/export.sh > /dev/null 2>&1 &&

# ---------- Setup ----------

setup:
	@if [ ! -d "$(IDF_PATH)" ]; then \
		echo "Cloning ESP-IDF $(IDF_VERSION)..."; \
		git clone -b $(IDF_VERSION) --recursive https://github.com/espressif/esp-idf.git $(IDF_PATH); \
	else \
		echo "ESP-IDF already present at $(IDF_PATH)"; \
	fi
	@echo "Installing toolchain for $(IDF_TARGET)..."
	@$(IDF_PATH)/install.sh $(IDF_TARGET)

update:
	cd $(IDF_PATH) && git fetch && git checkout $(IDF_VERSION) && git submodule update --init --recursive
	@$(IDF_PATH)/install.sh $(IDF_TARGET)

init:
	$(IDF_ACTIVATE) idf.py set-target $(IDF_TARGET)

# ---------- Firmware build ----------

all: build

build:
	$(IDF_ACTIVATE) idf.py build

flash:
	$(IDF_ACTIVATE) idf.py flash

monitor:
	$(IDF_ACTIVATE) idf.py monitor

flash-monitor:
	$(IDF_ACTIVATE) idf.py flash monitor

menuconfig:
	$(IDF_ACTIVATE) idf.py menuconfig

clean:
	$(IDF_ACTIVATE) idf.py clean

fullclean:
	$(IDF_ACTIVATE) idf.py fullclean

# ---------- Dev tooling ----------

format:
	@command -v clang-format >/dev/null 2>&1 || { echo "clang-format not found"; exit 1; }
	@find main -type f \( -name '*.hpp' -o -name '*.cpp' -o -name '*.h' -o -name '*.c' \) \
	    -exec clang-format -i {} +

lint:
	@command -v clang-tidy >/dev/null 2>&1 || { echo "clang-tidy not found"; exit 1; }
	@echo "Running clang-tidy (requires compile_commands.json — run 'make build' first)"
	@find main -type f \( -name '*.cpp' -o -name '*.hpp' -o -name '*.c' -o -name '*.h' \) \
	    -exec clang-tidy -p build {} +

# ---------- Help ----------

help:
	@echo "esp32-netlink — build targets"
	@echo ""
	@echo "  Setup (run once):"
	@echo "    make setup                 Clone + install ESP-IDF locally"
	@echo "    make update                Update local ESP-IDF to IDF_VERSION"
	@echo "    make init                  Set IDF target (run after setup)"
	@echo ""
	@echo "  Firmware:"
	@echo "    make build                 Compile firmware"
	@echo "    make flash                 Flash to device"
	@echo "    make monitor               Open serial monitor"
	@echo "    make flash-monitor         Flash + monitor"
	@echo "    make menuconfig            Interactive Kconfig menu"
	@echo "    make clean                 Remove build objects"
	@echo "    make fullclean             Remove entire build directory"
	@echo ""
	@echo "  Dev:"
	@echo "    make format                clang-format in-place"
	@echo "    make lint                  clang-tidy (needs prior build)"
	@echo ""
	@echo "  Environment:"
	@echo "    IDF_VERSION    = $(IDF_VERSION)"
	@echo "    IDF_TARGET     = $(IDF_TARGET)"
	@echo "    IDF_PATH       = $(IDF_PATH)"
	@echo "    IDF_TOOLS_PATH = $(IDF_TOOLS_PATH)"
