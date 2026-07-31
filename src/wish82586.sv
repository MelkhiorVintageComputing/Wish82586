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
  // Memory port, shared by the SCB handler and the command unit.
  logic        bus_req, bus_we, bus_byte, bus_ack, bus_err;
  logic [23:0] bus_addr;
  logic [15:0] bus_wdata, bus_rdata, bus_rdata_raw;

  logic        scb_req, scb_we, scb_byte, scb_ack, scb_err;
  logic [23:0] scb_addr;
  logic [15:0] scb_wdata;

  logic        cu_req, cu_we, cu_byte, cu_ack, cu_err;
  logic [23:0] cu_addr;
  logic [15:0] cu_wdata;

  // Command unit control and the parameters it captures.
  logic [23:0] cbbase;
  logic        cu_start, cu_resume, cu_suspend, cu_abort;
  logic [15:0] cu_cbl;
  logic [2:0]  cu_cus;
  logic        ev_cx, ev_cna;
  logic [47:0] ia_addr;
  logic [95:0] cfg_bytes;

  ie_core u_core (
      .clk          (clk),
      .rst          (rst),
      .core_rst_i   (core_rst),
      .ca_i         (ca),
      .scp_addr_i   (scp_addr),
      .cus_o        (cus),
      .rus_o        (rus),
      .busy_o       (busy),
      .int_o        (core_int),
      .cbbase_o     (cbbase),
      .cu_start_o   (cu_start),
      .cu_cbl_o     (cu_cbl),
      .cu_resume_o  (cu_resume),
      .cu_suspend_o (cu_suspend),
      .cu_abort_o   (cu_abort),
      .cus_i        (cu_cus),
      .ev_cx_i      (ev_cx),
      .ev_cna_i     (ev_cna),
      .bus_req_o    (scb_req),
      .bus_we_o     (scb_we),
      .bus_byte_o   (scb_byte),
      .bus_addr_o   (scb_addr),
      .bus_wdata_o  (scb_wdata),
      .bus_ack_i    (scb_ack),
      .bus_rdata_i  (bus_rdata),
      .bus_err_i    (scb_err)
  );

  ie_cu u_cu (
      .clk          (clk),
      .rst          (rst),
      .core_rst_i   (core_rst),
      .cbbase_i     (cbbase),
      .start_i      (cu_start),
      .start_cbl_i  (cu_cbl),
      .resume_i     (cu_resume),
      .suspend_i    (cu_suspend),
      .abort_i      (cu_abort),
      .cus_o        (cu_cus),
      .ev_cx_o      (ev_cx),
      .ev_cna_o     (ev_cna),
      .ia_addr_o    (ia_addr),
      .cfg_bytes_o  (cfg_bytes),
      .bus_req_o    (cu_req),
      .bus_we_o     (cu_we),
      .bus_byte_o   (cu_byte),
      .bus_addr_o   (cu_addr),
      .bus_wdata_o  (cu_wdata),
      .bus_ack_i    (cu_ack),
      .bus_rdata_i  (bus_rdata),
      .bus_err_i    (cu_err)
  );

  wb_arb u_arb (
      .clk        (clk),
      .rst        (rst),
      .p0_req_i   (scb_req),
      .p0_we_i    (scb_we),
      .p0_byte_i  (scb_byte),
      .p0_addr_i  (scb_addr),
      .p0_wdata_i (scb_wdata),
      .p0_ack_o   (scb_ack),
      .p0_err_o   (scb_err),
      .p1_req_i   (cu_req),
      .p1_we_i    (cu_we),
      .p1_byte_i  (cu_byte),
      .p1_addr_i  (cu_addr),
      .p1_wdata_i (cu_wdata),
      .p1_ack_o   (cu_ack),
      .p1_err_o   (cu_err),
      .rdata_o    (bus_rdata),
      .req_o      (bus_req),
      .we_o       (bus_we),
      .byte_o     (bus_byte),
      .addr_o     (bus_addr),
      .wdata_o    (bus_wdata),
      .ack_i      (bus_ack),
      .err_i      (bus_err),
      .rdata_i    (bus_rdata_raw)
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
      .rdata_o   (bus_rdata_raw),
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
  // ia_addr and cfg_bytes are already captured from the IA-SETUP and CONFIGURE
  // commands and are waiting for the transmit and receive paths to use them.
  // verilator lint_off UNUSED
  wire _unused = &{1'b0, mii_tx_clk, mii_rx_clk, mii_rxd, mii_rx_dv, mii_rx_er,
                   mii_crs, mii_col, mdio_i, ia_addr, cfg_bytes};
  // verilator lint_on UNUSED

endmodule
