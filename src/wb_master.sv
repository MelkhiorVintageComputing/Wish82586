// SPDX-License-Identifier: MIT
//
// Wishbone B4 classic master.
//
// The MAC sees memory the way the 82586 does: a 24-bit byte address space
// holding little-endian 8- and 16-bit fields.  This block turns one such
// access at a time into a bus cycle on the 32-bit Wishbone port, picking the
// byte lanes and shifting the data into and out of the right half of the word.
//
// Protocol on the internal port: hold req_i with the address and data until
// ack_o pulses for one cycle.  Reads present their data with that pulse.
// 16-bit accesses are expected to be even-aligned, as they are on the real
// part; an odd address is treated as if bit 0 were zero.

module wb_master #(
    parameter int WB_ADDR_W = 32,
    parameter int WB_DATA_W = 32
) (
    input  logic                   clk,
    input  logic                   rst,

    // ---- internal port ----------------------------------------------------
    input  logic                   req_i,
    input  logic                   we_i,
    input  logic                   byte_i,     // 1 = 8-bit access, 0 = 16-bit
    input  logic [23:0]            addr_i,
    input  logic [15:0]            wdata_i,
    output logic                   ack_o,
    output logic [15:0]            rdata_o,
    output logic                   err_o,

    // ---- Wishbone B4 classic master --------------------------------------
    output logic                   wbm_cyc_o,
    output logic                   wbm_stb_o,
    output logic                   wbm_we_o,
    output logic [WB_DATA_W/8-1:0] wbm_sel_o,
    output logic [WB_ADDR_W-1:0]   wbm_adr_o,
    output logic [WB_DATA_W-1:0]   wbm_dat_o,
    input  logic [WB_DATA_W-1:0]   wbm_dat_i,
    input  logic                   wbm_ack_i,
    input  logic                   wbm_err_i
);

  initial begin
    if (WB_DATA_W != 32) $fatal(1, "wb_master: only WB_DATA_W = 32 is wired up");
  end

  // Byte lanes and data placement for the access being presented.
  logic [3:0]  sel_next;
  logic [31:0] dat_next;

  always_comb begin
    if (byte_i) begin
      sel_next = 4'b0001 << addr_i[1:0];
      dat_next = {4{wdata_i[7:0]}};
    end else begin
      sel_next = addr_i[1] ? 4'b1100 : 4'b0011;
      dat_next = {2{wdata_i}};
    end
  end

  typedef enum logic [0:0] { ST_IDLE, ST_ACTIVE } state_e;
  state_e      state;
  logic [1:0]  lsb_q;
  logic        byte_q;

  // Extract the addressed field from the word the slave returned.
  logic [15:0] rdata_next;
  always_comb begin
    if (byte_q) rdata_next = {8'h00, wbm_dat_i[8*lsb_q +: 8]};
    else        rdata_next = lsb_q[1] ? wbm_dat_i[31:16] : wbm_dat_i[15:0];
  end

  always_ff @(posedge clk) begin
    if (rst) begin
      state     <= ST_IDLE;
      wbm_cyc_o <= 1'b0;
      wbm_stb_o <= 1'b0;
      wbm_we_o  <= 1'b0;
      wbm_sel_o <= '0;
      wbm_adr_o <= '0;
      wbm_dat_o <= '0;
      ack_o     <= 1'b0;
      err_o     <= 1'b0;
      rdata_o   <= '0;
      lsb_q     <= 2'b00;
      byte_q    <= 1'b0;
    end else begin
      ack_o <= 1'b0;
      err_o <= 1'b0;
      case (state)
        ST_IDLE: begin
          // ack_o still high means the previous access is only just finishing;
          // the requester has not had a chance to drop req_i yet.
          if (req_i && !ack_o) begin
            wbm_adr_o <= {{(WB_ADDR_W-24){1'b0}}, addr_i[23:2], 2'b00};
            wbm_we_o  <= we_i;
            wbm_sel_o <= sel_next;
            wbm_dat_o <= dat_next;
            wbm_cyc_o <= 1'b1;
            wbm_stb_o <= 1'b1;
            lsb_q     <= addr_i[1:0];
            byte_q    <= byte_i;
            state     <= ST_ACTIVE;
          end
        end
        ST_ACTIVE: begin
          if (wbm_ack_i || wbm_err_i) begin
            wbm_cyc_o <= 1'b0;
            wbm_stb_o <= 1'b0;
            rdata_o   <= rdata_next;
            ack_o     <= 1'b1;
            err_o     <= wbm_err_i;
            state     <= ST_IDLE;
          end
        end
        default: state <= ST_IDLE;
      endcase
    end
  end

endmodule
