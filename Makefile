# SPDX-License-Identifier: MIT
#
# Wish82586 build and regression.
#
#   make            build the testbench
#   make test       build and run the whole regression
#   make test T=crc run the tests whose name contains "crc"
#   make wave T=... run those tests with waveform tracing (build/waves/*.vcd)
#   make lint       Verilator lint of the RTL
#   make lint-icarus  second opinion from Icarus Verilog
#   make synth      quick Yosys elaboration check
#   make clean

VERILATOR ?= verilator
IVERILOG  ?= iverilog
YOSYS     ?= yosys

TOP       := tb_top
BUILD     := build

# PHY selects the interface the whole build is configured for:
#   make test            MII, four bits, the default
#   make test PHY=gmii   GMII, eight bits
# The two are separate builds because the width is an elaboration parameter.
PHY       ?= mii
ifeq ($(PHY),gmii)
PHY_W     := 8
else ifeq ($(PHY),mii)
PHY_W     := 4
else
$(error PHY must be mii or gmii)
endif

OBJDIR    := $(BUILD)/obj_dir_$(PHY)
BIN       := $(OBJDIR)/wish82586_tb

RTL_DIR   := src
TB_SV_DIR := tb/sv
TB_CPP    := tb/cpp

RTL       := $(RTL_DIR)/wish82586_pkg.sv \
             $(RTL_DIR)/crc32_eth.sv \
             $(RTL_DIR)/sync_fifo.sv \
             $(RTL_DIR)/async_fifo.sv \
             $(RTL_DIR)/mii_rx.sv \
             $(RTL_DIR)/mii_tx.sv \
             $(RTL_DIR)/dp_ram.sv \
             $(RTL_DIR)/wb_csr.sv \
             $(RTL_DIR)/wb_mdio.sv \
             $(RTL_DIR)/mdio_prog.sv \
             $(RTL_DIR)/wb_master.sv \
             $(RTL_DIR)/wb_arb.sv \
             $(RTL_DIR)/ie_core.sv \
             $(RTL_DIR)/ie_cu.sv \
             $(RTL_DIR)/ie_ru.sv \
             $(RTL_DIR)/wish82586.sv
TB_SV     := $(TB_SV_DIR)/tb_top.sv
CPP_SRCS  := $(wildcard $(TB_CPP)/*.cpp) $(wildcard $(TB_CPP)/tests/*.cpp)
CPP_HDRS  := $(wildcard $(TB_CPP)/*.h)

# Tests to run, empty means all.  Pass extra runner flags in FLAGS.
T     ?=
FLAGS ?=

VFLAGS := --cc --exe --build --trace -Wall \
          --top-module $(TOP) \
          -GPHY_DATA_W=$(PHY_W) \
          -Mdir $(OBJDIR) \
          -o wish82586_tb \
          -CFLAGS "-I$(CURDIR)/$(TB_CPP) -DPHY_DATA_W=$(PHY_W) -O2 -Wall -Wno-unused-parameter"

.PHONY: all test test-all wave lint lint-icarus synth list clean

all: $(BIN)

$(BIN): $(RTL) $(TB_SV) $(CPP_SRCS) $(CPP_HDRS) Makefile
	@mkdir -p $(BUILD)
	$(VERILATOR) $(VFLAGS) $(RTL) $(TB_SV) $(addprefix $(CURDIR)/,$(CPP_SRCS))

test: $(BIN)
	$(BIN) $(FLAGS) $(T)

# Both interfaces, which is what CI runs.
test-all:
	$(MAKE) test PHY=mii
	$(MAKE) test PHY=gmii

wave: $(BIN)
	$(BIN) --trace $(FLAGS) $(T)
	@echo "waveforms in $(BUILD)/waves/"

list: $(BIN)
	@$(BIN) --list

lint:
	$(VERILATOR) --lint-only -Wall -GPHY_DATA_W=4 --top-module wish82586 $(RTL)
	$(VERILATOR) --lint-only -Wall -GPHY_DATA_W=8 --top-module wish82586 $(RTL)
	$(VERILATOR) --lint-only -Wall -GPHY_DATA_W=4 --top-module $(TOP) $(RTL) $(TB_SV)
	$(VERILATOR) --lint-only -Wall -GPHY_DATA_W=8 --top-module $(TOP) $(RTL) $(TB_SV)

lint-icarus:
	$(IVERILOG) -g2012 -t null -o /dev/null $(RTL) $(TB_SV)

synth:
	@for top in wish82586 crc32_eth sync_fifo async_fifo mii_rx mii_tx dp_ram wb_csr wb_mdio mdio_prog wb_master wb_arb ie_core ie_cu ie_ru; do \
	  printf '%-12s ' "$$top"; \
	  $(YOSYS) -q -p "read_verilog -sv $(RTL); hierarchy -check -top $$top; proc; opt" \
	    && echo "elaborates" || exit 1; \
	done

clean:
	rm -rf $(BUILD)
