// SPDX-License-Identifier: MIT
//
// Wish82586 - Intel 82586 software-compatible Ethernet MAC.
//
//   host side : Wishbone B4 classic slave  (CSR: reset, channel attention, ...)
//               Wishbone B4 classic master (DMA into the shared host memory)
//   PHY side  : MII (GMII to follow)
//
// Status: the CSR block is functional; the command unit, receive unit and MAC
// datapath are not implemented yet.  The instantiation points below are the
// contract the testbench in tb/ already drives - see doc/interface.md.

module wish82586 #(
    parameter int WB_DATA_W = 32,   // 32 today; 16 planned
    parameter int WB_ADDR_W = 32
) (
    input  logic                   clk,
    input  logic                   rst,          // Wishbone RST_I: synchronous, active high

    // ---- Wishbone B4 classic slave: control registers ---------------------
    input  logic                   wbs_cyc_i,
    input  logic                   wbs_stb_i,
    input  logic                   wbs_we_i,
    input  logic [3:0]             wbs_sel_i,
    input  logic [7:0]             wbs_adr_i,
    input  logic [31:0]            wbs_dat_i,
    output logic [31:0]            wbs_dat_o,
    output logic                   wbs_ack_o,
    output logic                   wbs_err_o,

    // ---- Wishbone B4 classic master: shared memory ------------------------
    output logic                   wbm_cyc_o,
    output logic                   wbm_stb_o,
    output logic                   wbm_we_o,
    output logic [WB_DATA_W/8-1:0] wbm_sel_o,
    output logic [WB_ADDR_W-1:0]   wbm_adr_o,
    output logic [WB_DATA_W-1:0]   wbm_dat_o,
    input  logic [WB_DATA_W-1:0]   wbm_dat_i,
    input  logic                   wbm_ack_i,
    input  logic                   wbm_err_i,

    // ---- interrupt --------------------------------------------------------
    output logic                   irq_o,

    // ---- MII --------------------------------------------------------------
    input  logic                   mii_tx_clk,
    output logic [3:0]             mii_txd,
    output logic                   mii_tx_en,
    output logic                   mii_tx_er,
    input  logic                   mii_rx_clk,
    input  logic [3:0]             mii_rxd,
    input  logic                   mii_rx_dv,
    input  logic                   mii_rx_er,
    input  logic                   mii_crs,
    input  logic                   mii_col,

    // ---- PHY management (not driven yet) ----------------------------------
    output logic                   mdc,
    output logic                   mdio_o,
    output logic                   mdio_oe,
    input  logic                   mdio_i
);

  // ---------------------------------------------------------------------------
  // Control registers
  // ---------------------------------------------------------------------------
  logic        core_rst;
  logic        ca;
  logic [31:0] scp_addr;

  // Driven by the command / receive units once they exist.
  logic [2:0]  cus;
  logic [2:0]  rus;
  logic        busy;
  logic        core_int;

  wb_csr u_csr (
      .clk        (clk),
      .rst        (rst),
      .wbs_cyc_i  (wbs_cyc_i),
      .wbs_stb_i  (wbs_stb_i),
      .wbs_we_i   (wbs_we_i),
      .wbs_sel_i  (wbs_sel_i),
      .wbs_adr_i  (wbs_adr_i),
      .wbs_dat_i  (wbs_dat_i),
      .wbs_dat_o  (wbs_dat_o),
      .wbs_ack_o  (wbs_ack_o),
      .wbs_err_o  (wbs_err_o),
      .core_rst_o (core_rst),
      .ca_o       (ca),
      .scp_addr_o (scp_addr),
      .cus_i      (cus),
      .rus_i      (rus),
      .busy_i     (busy),
      .int_i      (core_int),
      .irq_o      (irq_o)
  );

  // ---------------------------------------------------------------------------
  // Initialisation sequencer and SCB handler, and the memory port it drives.
  //
  // TODO: command unit, receive unit, transmit/receive MAC datapath, and an
  // arbiter in front of wb_master once more than one block wants the bus.
  // ---------------------------------------------------------------------------
  logic        bus_req, bus_we, bus_byte, bus_ack, bus_err;
  logic [23:0] bus_addr;
  logic [15:0] bus_wdata, bus_rdata;

  ie_core u_core (
      .clk         (clk),
      .rst         (rst),
      .core_rst_i  (core_rst),
      .ca_i        (ca),
      .scp_addr_i  (scp_addr),
      .cus_o       (cus),
      .rus_o       (rus),
      .busy_o      (busy),
      .int_o       (core_int),
      .bus_req_o   (bus_req),
      .bus_we_o    (bus_we),
      .bus_byte_o  (bus_byte),
      .bus_addr_o  (bus_addr),
      .bus_wdata_o (bus_wdata),
      .bus_ack_i   (bus_ack),
      .bus_rdata_i (bus_rdata),
      .bus_err_i   (bus_err)
  );

  wb_master #(
      .WB_ADDR_W (WB_ADDR_W),
      .WB_DATA_W (WB_DATA_W)
  ) u_wbm (
      .clk       (clk),
      .rst       (rst),
      .req_i     (bus_req),
      .we_i      (bus_we),
      .byte_i    (bus_byte),
      .addr_i    (bus_addr),
      .wdata_i   (bus_wdata),
      .ack_o     (bus_ack),
      .rdata_o   (bus_rdata),
      .err_o     (bus_err),
      .wbm_cyc_o (wbm_cyc_o),
      .wbm_stb_o (wbm_stb_o),
      .wbm_we_o  (wbm_we_o),
      .wbm_sel_o (wbm_sel_o),
      .wbm_adr_o (wbm_adr_o),
      .wbm_dat_o (wbm_dat_o),
      .wbm_dat_i (wbm_dat_i),
      .wbm_ack_i (wbm_ack_i),
      .wbm_err_i (wbm_err_i)
  );

  // ---------------------------------------------------------------------------
  // The MAC datapath is still to be written, so the PHY side stays quiet.
  // ---------------------------------------------------------------------------
  assign mii_txd   = '0;
  assign mii_tx_en = 1'b0;
  assign mii_tx_er = 1'b0;

  assign mdc       = 1'b0;
  assign mdio_o    = 1'b0;
  assign mdio_oe   = 1'b0;

  // Signals consumed once the datapath exists; keep the linter quiet for now.
  // verilator lint_off UNUSED
  wire _unused = &{1'b0, mii_tx_clk, mii_rx_clk, mii_rxd, mii_rx_dv, mii_rx_er,
                   mii_crs, mii_col, mdio_i};
  // verilator lint_on UNUSED

endmodule
