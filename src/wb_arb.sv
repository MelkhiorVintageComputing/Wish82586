// SPDX-License-Identifier: MIT
//
// Two-way arbiter in front of the memory port.
//
// Port 0 has priority.  It belongs to the SCB handler, whose accesses come in
// short bursts after a channel attention, so the command and receive units on
// the other port cannot be starved for long.
//
// A grant is taken when a request first appears and held until the access is
// acknowledged, which is what wb_master expects: the requester keeps req
// asserted for the whole transaction.

module wb_arb (
    input  logic        clk,
    input  logic        rst,

    // ---- port 0 (priority) --------------------------------------------------
    input  logic        p0_req_i,
    input  logic        p0_we_i,
    input  logic        p0_byte_i,
    input  logic [23:0] p0_addr_i,
    input  logic [15:0] p0_wdata_i,
    output logic        p0_ack_o,
    output logic        p0_err_o,

    // ---- port 1 -------------------------------------------------------------
    input  logic        p1_req_i,
    input  logic        p1_we_i,
    input  logic        p1_byte_i,
    input  logic [23:0] p1_addr_i,
    input  logic [15:0] p1_wdata_i,
    output logic        p1_ack_o,
    output logic        p1_err_o,

    // Read data is shared; each port latches it on its own ack.
    output logic [15:0] rdata_o,

    // ---- to wb_master -------------------------------------------------------
    output logic        req_o,
    output logic        we_o,
    output logic        byte_o,
    output logic [23:0] addr_o,
    output logic [15:0] wdata_o,
    input  logic        ack_i,
    input  logic        err_i,
    input  logic [15:0] rdata_i
);

  logic lock;   // a grant is in progress
  logic sel;    // which port holds it

  // Before the grant is locked in, priority decides; afterwards it stays put.
  wire sel_now = lock ? sel : (p0_req_i ? 1'b0 : 1'b1);

  always_ff @(posedge clk) begin
    if (rst) begin
      lock <= 1'b0;
      sel  <= 1'b0;
    end else if (!lock) begin
      if (p0_req_i) begin
        sel  <= 1'b0;
        lock <= 1'b1;
      end else if (p1_req_i) begin
        sel  <= 1'b1;
        lock <= 1'b1;
      end
    end else if (ack_i) begin
      lock <= 1'b0;
    end
  end

  always_comb begin
    if (sel_now) begin
      req_o   = p1_req_i;
      we_o    = p1_we_i;
      byte_o  = p1_byte_i;
      addr_o  = p1_addr_i;
      wdata_o = p1_wdata_i;
    end else begin
      req_o   = p0_req_i;
      we_o    = p0_we_i;
      byte_o  = p0_byte_i;
      addr_o  = p0_addr_i;
      wdata_o = p0_wdata_i;
    end
  end

  assign p0_ack_o = ack_i && !sel_now;
  assign p0_err_o = err_i && !sel_now;
  assign p1_ack_o = ack_i && sel_now;
  assign p1_err_o = err_i && sel_now;
  assign rdata_o  = rdata_i;

endmodule
