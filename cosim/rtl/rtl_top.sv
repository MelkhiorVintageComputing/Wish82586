// SPDX-License-Identifier: MIT
//
// Simulation top level for the co-simulation library.
//
// The MAC with this project's own control registers on it - wish82586_wb -
// and nothing else.  It exists only to widen the narrow ports to the widths
// the C++ bus models expect to find a `uint32_t' behind.  No behaviour here.

module rtl_top #(
    parameter int PHY_DATA_W = 4     // 4 = MII, 8 = GMII
) (
    input  logic        clk,
    input  logic        rst,

    // ---- Wishbone slave: the card's control registers ---------------------
    input  logic        wbs_cyc_i,
    input  logic        wbs_stb_i,
    input  logic        wbs_we_i,
    input  logic [3:0]  wbs_sel_i,
    /* verilator lint_off UNUSEDSIGNAL */
    input  logic [31:0] wbs_adr_i,   // only [5:0] reach the DUT
    /* verilator lint_on UNUSEDSIGNAL */
    input  logic [31:0] wbs_dat_i,
    output logic [31:0] wbs_dat_o,
    output logic        wbs_ack_o,
    output logic        wbs_err_o,

    // ---- Wishbone master: the card's memory window ------------------------
    output logic        wbm_cyc_o,
    output logic        wbm_stb_o,
    output logic        wbm_we_o,
    output logic [3:0]  wbm_sel_o,
    output logic [29:0] wbm_adr_o,
    output logic [31:0] wbm_dat_o,
    input  logic [31:0] wbm_dat_i,
    input  logic        wbm_ack_i,
    input  logic        wbm_err_i,

    output logic        irq_o,

    // ---- MII --------------------------------------------------------------
    input  logic        mii_tx_clk,
    output logic [PHY_DATA_W-1:0] mii_txd,
    output logic        mii_tx_en,
    output logic        mii_tx_er,
    input  logic        mii_rx_clk,
    input  logic [PHY_DATA_W-1:0] mii_rxd,
    input  logic        mii_rx_dv,
    input  logic        mii_rx_er,
    input  logic        mii_crs,
    input  logic        mii_col
);

  wish82586_wb #(.PHY_DATA_W(PHY_DATA_W)) u_dut (
      .clk        (clk),
      .rst        (rst),
      .wbs_cyc_i  (wbs_cyc_i),
      .wbs_stb_i  (wbs_stb_i),
      .wbs_we_i   (wbs_we_i),
      .wbs_sel_i  (wbs_sel_i),
      .wbs_adr_i  (wbs_adr_i[5:0]),
      .wbs_dat_i  (wbs_dat_i),
      .wbs_dat_o  (wbs_dat_o),
      .wbs_ack_o  (wbs_ack_o),
      .wbs_err_o  (wbs_err_o),
      .wbm_cyc_o  (wbm_cyc_o),
      .wbm_stb_o  (wbm_stb_o),
      .wbm_we_o   (wbm_we_o),
      .wbm_sel_o  (wbm_sel_o),
      .wbm_adr_o  (wbm_adr_o),
      .wbm_dat_o  (wbm_dat_o),
      .wbm_dat_i  (wbm_dat_i),
      .wbm_ack_i  (wbm_ack_i),
      .wbm_err_i  (wbm_err_i),
      .irq_o      (irq_o),
      .mii_tx_clk (mii_tx_clk),
      .mii_txd    (mii_txd),
      .mii_tx_en  (mii_tx_en),
      .mii_tx_er  (mii_tx_er),
      .mii_rx_clk (mii_rx_clk),
      .mii_rxd    (mii_rxd),
      .mii_rx_dv  (mii_rx_dv),
      .mii_rx_er  (mii_rx_er),
      .mii_crs    (mii_crs),
      .mii_col    (mii_col)
  );

endmodule
