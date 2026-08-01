# SPDX-License-Identifier: MIT
#
# Builds the Verilated wish82586 into a shared library the emulated card in
# QEMU loads at run time.  Kept out of the top level Makefile on purpose: the
# regression must not grow a dependency on the co-simulation.
#
#   make            build work/lib/libwish82586rtl.so
#   make PHY=gmii   the same against a GMII build of the core
#   make clean

VERILATOR ?= verilator

ROOT      := $(CURDIR)/../..
WORK      ?= $(ROOT)/work
BUILD     := $(WORK)/rtl-build
LIBDIR    := $(WORK)/lib
LIB       := $(LIBDIR)/libwish82586rtl.so
BINDIR    := $(WORK)/bin
SELFTEST  := $(BINDIR)/rtl-selftest

PHY       ?= mii
ifeq ($(PHY),gmii)
PHY_W     := 8
else ifeq ($(PHY),mii)
PHY_W     := 4
else
$(error PHY must be mii or gmii)
endif

RTL_DIR   := $(ROOT)/src
TB_CPP    := $(ROOT)/tb/cpp

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

# Only the testbench pieces that model hardware; nothing that knows about
# tests, and nothing that builds the shared memory image.
SHIM_SRCS := wish_rtl.cpp \
             $(TB_CPP)/sim.cpp \
             $(TB_CPP)/wb.cpp \
             $(TB_CPP)/mii_phy.cpp \
             $(TB_CPP)/eth.cpp \
             $(TB_CPP)/ie_driver.cpp \
             $(TB_CPP)/i82586.cpp

VINC      := $(shell $(VERILATOR) --getenv VERILATOR_ROOT)/include

CXXFLAGS  := -O2 -fPIC -faligned-new -std=c++14 -Wall -Wno-unused-parameter \
             -I$(CURDIR) -I$(TB_CPP) -I$(BUILD) -I$(VINC) -I$(VINC)/vltstd \
             -DPHY_DATA_W=$(PHY_W)

.PHONY: all clean lint check

all: $(LIB) $(SELFTEST)

# Drives the library the way the driver does, with no QEMU in the way.
check: $(SELFTEST)
	$(SELFTEST)

$(BUILD)/Vrtl_top__ALL.a: rtl_top.sv $(RTL)
	@mkdir -p $(BUILD)
	$(VERILATOR) --cc --build -Wall -Wno-fatal \
	    --top-module rtl_top -GPHY_DATA_W=$(PHY_W) \
	    -CFLAGS "-fPIC -O2" \
	    -Mdir $(BUILD) $(RTL) rtl_top.sv

$(LIB): $(BUILD)/Vrtl_top__ALL.a $(SHIM_SRCS) wish_rtl.h
	@mkdir -p $(LIBDIR)
	$(CXX) $(CXXFLAGS) -shared -o $@ $(SHIM_SRCS) \
	    $(VINC)/verilated.cpp $(VINC)/verilated_threads.cpp $(VINC)/verilated_vcd_c.cpp \
	    $(BUILD)/Vrtl_top__ALL.a
	@echo "built $@"

$(SELFTEST): selftest.cpp $(LIB) wish_rtl.h
	@mkdir -p $(BINDIR)
	$(CXX) $(CXXFLAGS) -o $@ selftest.cpp -L$(LIBDIR) -lwish82586rtl \
	    -Wl,-rpath,$(LIBDIR)

lint:
	$(VERILATOR) --lint-only -Wall -GPHY_DATA_W=$(PHY_W) \
	    --top-module rtl_top $(RTL) rtl_top.sv

clean:
	rm -rf $(BUILD) $(LIB) $(SELFTEST)
