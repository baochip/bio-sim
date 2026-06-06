# bio-sim Makefile -- standalone build against a preinstalled Verilator.
# Dockerization later just calls these same targets inside the image.

VERILATOR ?= verilator
TOP        = bio_bdma_wrapper
BUILD      = build
EXE        = $(BUILD)/bio_sim
JSON       = sim/json.hpp
JSON_URL   = https://raw.githubusercontent.com/nlohmann/json/v3.11.3/single_include/nlohmann/json.hpp

# Warnings are demoted for first bring-up: this is vendor RTL, not our code,
# and we want elaboration to proceed so we can iterate. Tighten later.
VFLAGS = --cc --exe --build -j 0 \
         --trace-fst --assert \
         --timescale-override 1ps/1ps \
         --top-module $(TOP) \
         +define+SIM \
         +define+USE_OSS_BRIDGE \
         -Wno-fatal \
         -Wno-BLKANDNBLK \
         -Wno-WIDTH \
         -Wno-COMBDLY \
         -Wno-CASEINCOMPLETE \
         --no-timing \
         -Wno-UNOPTFLAT -Wno-TIMESCALEMOD \
         -Wno-STMTDLY

CFLAGS_EXTRA = -std=c++17 -pthread -I$(CURDIR)/sim
LDFLAGS_EXTRA = -pthread

.PHONY: all build run clean json

all: build

$(JSON):
	@echo ">> fetching nlohmann/json single header"
	@curl -fsSL $(JSON_URL) -o $(JSON) || \
	  (echo "!! could not fetch json.hpp -- download it manually to $(JSON)"; exit 1)

json: $(JSON)

build: $(JSON)
	$(VERILATOR) $(VFLAGS) \
	  -f rtl.f \
	  $(CURDIR)/sim/sim_main.cpp \
	  --CFLAGS "$(CFLAGS_EXTRA)" \
	  --LDFLAGS "$(LDFLAGS_EXTRA)" \
	  -Mdir $(BUILD) -o bio_sim

# usage: make run CONFIG=configs/smoke.json
CONFIG ?= configs/smoke.json
run: build
	$(EXE) $(CONFIG)

clean:
	rm -rf $(BUILD)

VERSION ?= $(shell git describe --tags --abbrev=0 2>/dev/null || echo dev)

release: ## Export image as a tar.gz for CDN hosting
	podman save bio-sim:latest | gzip > bio-sim-$(VERSION)-$(shell uname -m).tar.gz
	@echo "Upload bio-sim-$(VERSION)-$(shell uname -m).tar.gz to your CDN"
	@echo "Users install with: curl -fsSL https://baochip.com/cdn/bio-sim-$(VERSION)-linux-amd64.tar.gz | podman load"