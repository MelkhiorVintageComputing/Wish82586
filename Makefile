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
OBJDIR    := $(BUILD)/obj_dir
BIN       := $(OBJDIR)/wish82586_tb

RTL_DIR   := src
TB_SV_DIR := tb/sv
TB_CPP    := tb/cpp

RTL       := $(RTL_DIR)/wish82586_pkg.sv \
             $(RTL_DIR)/crc32_eth.sv \
             $(RTL_DIR)/sync_fifo.sv \
             $(RTL_DIR)/async_fifo.sv \
             $(RTL_DIR)/mii_rx.sv \
             $(RTL_DIR)/wb_csr.sv \
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
          -Mdir $(OBJDIR) \
          -o wish82586_tb \
          -CFLAGS "-I$(CURDIR)/$(TB_CPP) -O2 -Wall -Wno-unused-parameter"

.PHONY: all test wave lint lint-icarus synth list clean

all: $(BIN)

$(BIN): $(RTL) $(TB_SV) $(CPP_SRCS) $(CPP_HDRS) Makefile
	@mkdir -p $(BUILD)
	$(VERILATOR) $(VFLAGS) $(RTL) $(TB_SV) $(addprefix $(CURDIR)/,$(CPP_SRCS))

test: $(BIN)
	$(BIN) $(FLAGS) $(T)

wave: $(BIN)
	$(BIN) --trace $(FLAGS) $(T)
	@echo "waveforms in $(BUILD)/waves/"

list: $(BIN)
	@$(BIN) --list

lint:
	$(VERILATOR) --lint-only -Wall --top-module wish82586 $(RTL)
	$(VERILATOR) --lint-only -Wall --top-module $(TOP) $(RTL) $(TB_SV)

lint-icarus:
	$(IVERILOG) -g2012 -t null -o /dev/null $(RTL) $(TB_SV)

synth:
	@for top in wish82586 crc32_eth sync_fifo async_fifo mii_rx wb_csr wb_master wb_arb ie_core ie_cu ie_ru; do \
	  printf '%-12s ' "$$top"; \
	  $(YOSYS) -q -p "read_verilog -sv $(RTL); hierarchy -check -top $$top; proc; opt" \
	    && echo "elaborates" || exit 1; \
	done

clean:
	rm -rf $(BUILD)
