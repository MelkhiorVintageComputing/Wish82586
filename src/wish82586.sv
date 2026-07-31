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
    parameter int WB_DATA_W = 32,   // fixed at 32, see doc/interface.md
    parameter int WB_ADDR_W = 30   // word address lines, see doc/interface.md
) (
    input  logic                   clk,
    input  logic                   rst,          // Wishbone RST_I: synchronous, active high

    // ---- Wishbone B4 classic slave: control registers ---------------------
    input  logic                   wbs_cyc_i,
    input  logic                   wbs_stb_i,
    input  logic                   wbs_we_i,
    input  logic [3:0]             wbs_sel_i,
    input  logic [5:0]             wbs_adr_i,   // word address
    input  logic [31:0]            wbs_dat_i,
    output logic [31:0]            wbs_dat_o,
    output logic                   wbs_ack_o,
    output logic                   wbs_err_o,

    // ---- Wishbone B4 classic master: shared memory ------------------------
    output logic                   wbm_cyc_o,
    output logic                   wbm_stb_o,
    output logic                   wbm_we_o,
    output logic [WB_DATA_W/8-1:0] wbm_sel_o,
    output logic [WB_ADDR_W-1:0]   wbm_adr_o,   // word address
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

  logic        ru_req, ru_we, ru_byte, ru_ack, ru_err;
  logic [23:0] ru_addr;
  logic [15:0] ru_wdata;

  // Receive unit control and the receive FIFO between the two clock domains.
  logic [23:0] scb_base;      // absolute address of the SCB
  logic        ru_start, ru_resume, ru_suspend, ru_abort;
  logic [15:0] ru_rfa;
  logic [2:0]  ru_rus;
  logic        ev_fr, ev_rnr;
  logic        rxf_wr, rxf_full, rxf_rd, rxf_empty;
  logic [11:0] rxf_wdata, rxf_rdata;
  logic        rx_active;     // observation only
  logic [15:0] rx_bytes;

  // Command unit control and the parameters it captures.
  logic [23:0] cbbase;
  logic        cu_start, cu_resume, cu_suspend, cu_abort;
  logic [15:0] cu_cbl;
  logic [2:0]  cu_cus;
  logic        ev_cx, ev_cna;
  logic [47:0] ia_addr;
  logic [95:0] cfg_bytes;

  // Transmit staging and the transmitter handshake.
  logic        tx_ram_we;
  logic [10:0] tx_ram_waddr, tx_ram_raddr;
  logic [7:0]  tx_ram_wdata, tx_ram_rdata;
  logic        tx_go, tx_done, tx_ok, tx_xcoll, tx_defer, tx_no_crs;
  logic [3:0]  tx_ncoll;
  logic [15:0] tx_len;

  // CONFIGURE parameters the transmitter needs, by byte offset in the block.
  wire [7:0]  cfg_ifs      = cfg_bytes[47:40];
  wire [10:0] cfg_slot     = {cfg_bytes[58:56], cfg_bytes[55:48]};
  wire [3:0]  cfg_retry    = cfg_bytes[63:60];
  wire        cfg_no_crc   = cfg_bytes[68];
  wire [7:0]  cfg_min_len  = cfg_bytes[87:80];
  wire        cfg_int_lb   = cfg_bytes[30];   // CONFIGURE byte 3, bit 6

  // Internal loopback: the transmit side hands frames straight to the receive
  // unit through this FIFO instead of putting them on the wire.
  logic        mc_clear, mc_wr, mc_all;
  logic [47:0] mc_addr;
  logic        lb_wr, lb_full, lb_rd, lb_empty;
  logic [11:0] lb_wdata, lb_rdata;
  logic [6:0]  lb_level;   // observation only

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
      .scb_addr_o   (scb_base),
      .ru_start_o   (ru_start),
      .ru_rfa_o     (ru_rfa),
      .ru_resume_o  (ru_resume),
      .ru_suspend_o (ru_suspend),
      .ru_abort_o   (ru_abort),
      .rus_i        (ru_rus),
      .ev_fr_i      (ev_fr),
      .ev_rnr_i     (ev_rnr),
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
      .ia_addr_o     (ia_addr),
      .cfg_bytes_o   (cfg_bytes),
      .tx_ram_we_o   (tx_ram_we),
      .tx_ram_addr_o (tx_ram_waddr),
      .tx_ram_data_o (tx_ram_wdata),
      .tx_go_o       (tx_go),
      .tx_len_o      (tx_len),
      .tx_done_i     (tx_done),
      .tx_ok_i       (tx_ok),
      .tx_ncoll_i    (tx_ncoll),
      .tx_xcoll_i    (tx_xcoll),
      .tx_defer_i    (tx_defer),
      .tx_no_crs_i   (tx_no_crs),
      .lb_enable_i   (cfg_int_lb),
      .lb_wr_o       (lb_wr),
      .lb_data_o     (lb_wdata),
      .lb_full_i     (lb_full),
      .mc_clear_o    (mc_clear),
      .mc_wr_o       (mc_wr),
      .mc_addr_o     (mc_addr),
      .mc_all_o      (mc_all),
      .bus_req_o    (cu_req),
      .bus_we_o     (cu_we),
      .bus_byte_o   (cu_byte),
      .bus_addr_o   (cu_addr),
      .bus_wdata_o  (cu_wdata),
      .bus_ack_i    (cu_ack),
      .bus_rdata_i  (bus_rdata),
      .bus_err_i    (cu_err)
  );

  // Receive path: front end in the PHY clock domain, FIFO across, receive unit
  // on the system clock.
  mii_rx u_mii_rx (
      .rx_clk       (mii_rx_clk),
      .rst          (rst),
      .rxd          (mii_rxd),
      .rx_dv        (mii_rx_dv),
      .rx_er        (mii_rx_er),
      .fifo_wr_o    (rxf_wr),
      .fifo_data_o  (rxf_wdata),
      .fifo_full_i  (rxf_full),
      .active_o     (rx_active),
      .byte_count_o (rx_bytes)
  );

  async_fifo #(.WIDTH(12), .DEPTH(64)) u_rx_fifo (
      .wclk    (mii_rx_clk),
      .wrst    (rst),
      .wr_en   (rxf_wr),
      .wr_data (rxf_wdata),
      .wfull   (rxf_full),
      .rclk    (clk),
      .rrst    (rst),
      .rd_en   (rxf_rd),
      .rd_data (rxf_rdata),
      .rempty  (rxf_empty)
  );

  sync_fifo #(.WIDTH(12), .DEPTH(64)) u_lb_fifo (
      .clk     (clk),
      .rst     (rst),
      .flush   (1'b0),
      .wr_en   (lb_wr),
      .wr_data (lb_wdata),
      .full    (lb_full),
      .rd_en   (lb_rd),
      .rd_data (lb_rdata),
      .empty   (lb_empty),
      .level   (lb_level)
  );

  // The receive unit takes frames from the wire, or from the loopback path
  // when the chip is configured that way.
  wire        ru_src_empty = cfg_int_lb ? lb_empty : rxf_empty;
  wire [11:0] ru_src_data  = cfg_int_lb ? lb_rdata : rxf_rdata;
  logic       ru_src_rd;
  assign lb_rd   = ru_src_rd &&  cfg_int_lb;
  assign rxf_rd  = ru_src_rd && !cfg_int_lb;

  ie_ru u_ru (
      .clk             (clk),
      .rst             (rst),
      .core_rst_i      (core_rst),
      .cbbase_i        (cbbase),
      .scb_addr_i      (scb_base),
      .start_i         (ru_start),
      .start_rfa_i     (ru_rfa),
      .resume_i        (ru_resume),
      .suspend_i       (ru_suspend),
      .abort_i         (ru_abort),
      .rus_o           (ru_rus),
      .ev_fr_o         (ev_fr),
      .ev_rnr_o        (ev_rnr),
      .ia_addr_i       (ia_addr),
      .promisc_i       (cfg_bytes[64]),
      .no_bcast_i      (cfg_bytes[65]),
      .mc_clear_i      (mc_clear),
      .mc_wr_i         (mc_wr),
      .mc_addr_i       (mc_addr),
      .mc_all_i        (mc_all),
      .save_bad_i      (cfg_bytes[23]),
      .min_frame_len_i (cfg_bytes[87:80]),
      .rx_empty_i      (ru_src_empty),
      .rx_data_i       (ru_src_data),
      .rx_rd_o         (ru_src_rd),
      .bus_req_o       (ru_req),
      .bus_we_o        (ru_we),
      .bus_byte_o      (ru_byte),
      .bus_addr_o      (ru_addr),
      .bus_wdata_o     (ru_wdata),
      .bus_ack_i       (ru_ack),
      .bus_rdata_i     (bus_rdata),
      .bus_err_i       (ru_err)
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
      .p1_req_i   (ru_req),
      .p1_we_i    (ru_we),
      .p1_byte_i  (ru_byte),
      .p1_addr_i  (ru_addr),
      .p1_wdata_i (ru_wdata),
      .p1_ack_o   (ru_ack),
      .p1_err_o   (ru_err),
      .p2_req_i   (cu_req),
      .p2_we_i    (cu_we),
      .p2_byte_i  (cu_byte),
      .p2_addr_i  (cu_addr),
      .p2_wdata_i (cu_wdata),
      .p2_ack_o   (cu_ack),
      .p2_err_o   (cu_err),
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

  // Transmit path: the command unit stages a frame here, mii_tx clocks it out.
  dp_ram #(.WIDTH(8), .DEPTH(2048)) u_tx_ram (
      .wclk    (clk),
      .wr_en   (tx_ram_we),
      .wr_addr (tx_ram_waddr),
      .wr_data (tx_ram_wdata),
      .rclk    (mii_tx_clk),
      .rd_addr (tx_ram_raddr),
      .rd_data (tx_ram_rdata)
  );

  mii_tx u_mii_tx (
      .tx_clk        (mii_tx_clk),
      .rst           (rst),
      .go_i          (tx_go),
      .len_i         (tx_len),
      .done_o        (tx_done),
      .ok_o          (tx_ok),
      .ncoll_o       (tx_ncoll),
      .xcoll_o       (tx_xcoll),
      .defer_o       (tx_defer),
      .no_crs_o      (tx_no_crs),
      .retry_limit_i (cfg_retry),
      .ifs_i         (cfg_ifs),
      .slot_time_i   (cfg_slot),
      .min_len_i     (cfg_min_len),
      .no_crc_i      (cfg_no_crc),
      .ram_addr_o    (tx_ram_raddr),
      .ram_data_i    (tx_ram_rdata),
      .txd           (mii_txd),
      .tx_en         (mii_tx_en),
      .tx_er         (mii_tx_er),
      .crs           (mii_crs),
      .col           (mii_col)
  );

  assign mdc       = 1'b0;
  assign mdio_o    = 1'b0;
  assign mdio_oe   = 1'b0;

  // Signals consumed once the datapath exists; keep the linter quiet for now.
  // ia_addr and cfg_bytes are already captured from the IA-SETUP and CONFIGURE
  // commands and are waiting for the transmit and receive paths to use them.
  // verilator lint_off UNUSED
  wire _unused = &{1'b0, mdio_i, cfg_bytes, rx_active, rx_bytes, lb_level};
  // verilator lint_on UNUSED

endmodule
