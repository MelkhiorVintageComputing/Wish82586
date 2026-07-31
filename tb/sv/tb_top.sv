// SPDX-License-Identifier: MIT
//
// Simulation top level for the whole regression.
//
// It bundles the DUT with the leaf modules that have their own unit tests, so
// the entire testbench builds into a single binary.  Everything here is wiring
// only - no behaviour.  Port names are what the C++ testbench references.

module tb_top (
    input  logic        clk,
    input  logic        rst,

    // ---- DUT: Wishbone slave (CSR) ----------------------------------------
    input  logic        dut_wbs_cyc_i,
    input  logic        dut_wbs_stb_i,
    input  logic        dut_wbs_we_i,
    input  logic [3:0]  dut_wbs_sel_i,
    /* verilator lint_off UNUSEDSIGNAL */
    input  logic [31:0] dut_wbs_adr_i,   // only [7:0] reach the DUT
    /* verilator lint_on UNUSEDSIGNAL */
    input  logic [31:0] dut_wbs_dat_i,
    output logic [31:0] dut_wbs_dat_o,
    output logic        dut_wbs_ack_o,
    output logic        dut_wbs_err_o,

    // ---- DUT: Wishbone master (shared memory) -----------------------------
    output logic        dut_wbm_cyc_o,
    output logic        dut_wbm_stb_o,
    output logic        dut_wbm_we_o,
    output logic [3:0]  dut_wbm_sel_o,
    output logic [31:0] dut_wbm_adr_o,
    output logic [31:0] dut_wbm_dat_o,
    input  logic [31:0] dut_wbm_dat_i,
    input  logic        dut_wbm_ack_i,
    input  logic        dut_wbm_err_i,

    output logic        dut_irq_o,

    // ---- DUT: MII ---------------------------------------------------------
    input  logic        dut_mii_tx_clk,
    output logic [3:0]  dut_mii_txd,
    output logic        dut_mii_tx_en,
    output logic        dut_mii_tx_er,
    input  logic        dut_mii_rx_clk,
    input  logic [3:0]  dut_mii_rxd,
    input  logic        dut_mii_rx_dv,
    input  logic        dut_mii_rx_er,
    input  logic        dut_mii_crs,
    input  logic        dut_mii_col,

    output logic        dut_mdc,
    output logic        dut_mdio_o,
    output logic        dut_mdio_oe,
    input  logic        dut_mdio_i,

    // ---- unit test: nibble-wide CRC ---------------------------------------
    input  logic        crc4_init,
    input  logic        crc4_en,
    input  logic [3:0]  crc4_data,
    output logic [31:0] crc4_crc,
    output logic [31:0] crc4_fcs,
    output logic        crc4_ok,

    // ---- unit test: byte-wide CRC -----------------------------------------
    input  logic        crc8_init,
    input  logic        crc8_en,
    input  logic [7:0]  crc8_data,
    output logic [31:0] crc8_crc,
    output logic [31:0] crc8_fcs,
    output logic        crc8_ok,

    // ---- unit test: MII receive front end ----------------------------------
    // Fed by the same pins the DUT sees, so it observes the same stimulus.
    output logic        rxfe_wr,
    output logic [11:0] rxfe_data,
    input  logic        rxfe_full,
    output logic        rxfe_active,
    output logic [15:0] rxfe_bytes,

    // ---- unit test: dual clock FIFO, written from the PHY clock ------------
    input  logic        afifo_wr_en,
    input  logic [11:0] afifo_wr_data,
    output logic        afifo_wfull,
    input  logic        afifo_rd_en,
    output logic [11:0] afifo_rd_data,
    output logic        afifo_rempty,

    // ---- unit test: synchronous FIFO --------------------------------------
    input  logic        fifo_flush,
    input  logic        fifo_wr_en,
    input  logic [7:0]  fifo_wr_data,
    output logic        fifo_full,
    input  logic        fifo_rd_en,
    output logic [7:0]  fifo_rd_data,
    output logic        fifo_empty,
    output logic [4:0]  fifo_level
);

  wish82586 u_dut (
      .clk        (clk),
      .rst        (rst),
      .wbs_cyc_i  (dut_wbs_cyc_i),
      .wbs_stb_i  (dut_wbs_stb_i),
      .wbs_we_i   (dut_wbs_we_i),
      .wbs_sel_i  (dut_wbs_sel_i),
      .wbs_adr_i  (dut_wbs_adr_i[7:0]),
      .wbs_dat_i  (dut_wbs_dat_i),
      .wbs_dat_o  (dut_wbs_dat_o),
      .wbs_ack_o  (dut_wbs_ack_o),
      .wbs_err_o  (dut_wbs_err_o),
      .wbm_cyc_o  (dut_wbm_cyc_o),
      .wbm_stb_o  (dut_wbm_stb_o),
      .wbm_we_o   (dut_wbm_we_o),
      .wbm_sel_o  (dut_wbm_sel_o),
      .wbm_adr_o  (dut_wbm_adr_o),
      .wbm_dat_o  (dut_wbm_dat_o),
      .wbm_dat_i  (dut_wbm_dat_i),
      .wbm_ack_i  (dut_wbm_ack_i),
      .wbm_err_i  (dut_wbm_err_i),
      .irq_o      (dut_irq_o),
      .mii_tx_clk (dut_mii_tx_clk),
      .mii_txd    (dut_mii_txd),
      .mii_tx_en  (dut_mii_tx_en),
      .mii_tx_er  (dut_mii_tx_er),
      .mii_rx_clk (dut_mii_rx_clk),
      .mii_rxd    (dut_mii_rxd),
      .mii_rx_dv  (dut_mii_rx_dv),
      .mii_rx_er  (dut_mii_rx_er),
      .mii_crs    (dut_mii_crs),
      .mii_col    (dut_mii_col),
      .mdc        (dut_mdc),
      .mdio_o     (dut_mdio_o),
      .mdio_oe    (dut_mdio_oe),
      .mdio_i     (dut_mdio_i)
  );

  crc32_eth #(.DATA_W(4)) u_crc4 (
      .clk      (clk),
      .rst      (rst),
      .init     (crc4_init),
      .en       (crc4_en),
      .data_i   (crc4_data),
      .crc_o    (crc4_crc),
      .fcs_o    (crc4_fcs),
      .crc_ok_o (crc4_ok)
  );

  crc32_eth #(.DATA_W(8)) u_crc8 (
      .clk      (clk),
      .rst      (rst),
      .init     (crc8_init),
      .en       (crc8_en),
      .data_i   (crc8_data),
      .crc_o    (crc8_crc),
      .fcs_o    (crc8_fcs),
      .crc_ok_o (crc8_ok)
  );

  mii_rx u_rxfe (
      .rx_clk       (dut_mii_rx_clk),
      .rst          (rst),
      .rxd          (dut_mii_rxd),
      .rx_dv        (dut_mii_rx_dv),
      .rx_er        (dut_mii_rx_er),
      .fifo_wr_o    (rxfe_wr),
      .fifo_data_o  (rxfe_data),
      .fifo_full_i  (rxfe_full),
      .active_o     (rxfe_active),
      .byte_count_o (rxfe_bytes)
  );

  async_fifo #(.WIDTH(12), .DEPTH(16)) u_afifo (
      .wclk    (dut_mii_rx_clk),
      .wrst    (rst),
      .wr_en   (afifo_wr_en),
      .wr_data (afifo_wr_data),
      .wfull   (afifo_wfull),
      .rclk    (clk),
      .rrst    (rst),
      .rd_en   (afifo_rd_en),
      .rd_data (afifo_rd_data),
      .rempty  (afifo_rempty)
  );

  sync_fifo #(.WIDTH(8), .DEPTH(16)) u_fifo (
      .clk     (clk),
      .rst     (rst),
      .flush   (fifo_flush),
      .wr_en   (fifo_wr_en),
      .wr_data (fifo_wr_data),
      .full    (fifo_full),
      .rd_en   (fifo_rd_en),
      .rd_data (fifo_rd_data),
      .empty   (fifo_empty),
      .level   (fifo_level)
  );

endmodule
