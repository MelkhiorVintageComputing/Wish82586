// SPDX-License-Identifier: MIT
//
// Initialisation sequencer and System Control Block handler.
//
// This is the part of the chip that talks to the host through shared memory.
// After reset the first channel attention makes it walk the pointer chain the
// driver set up:
//
//   SCP  (at the address in CSR SCP_ADDR)  -> bus width, address of the ISCP
//   ISCP                                    -> SCB offset, control block base
//
// It then clears the ISCP busy flag, reports CX and CNA in the SCB status and
// raises the interrupt, which is what the drivers in doc/drivers wait for.
//
// Every later channel attention means "look at the SCB command word": the
// acknowledge bits are applied to the status, the command word is cleared, and
// the updated status is written back.  The command unit and receive unit
// control fields are read but not acted on yet.

module ie_core (
    input  logic        clk,
    input  logic        rst,          // bus reset
    input  logic        core_rst_i,   // CSR CTRL.RST, holds the core in reset
    input  logic        ca_i,         // channel attention, one cycle
    input  logic [31:0] scp_addr_i,

    output logic [2:0]  cus_o,
    output logic [2:0]  rus_o,
    output logic        busy_o,
    output logic        int_o,

    // ---- memory port (see wb_master) --------------------------------------
    output logic        bus_req_o,
    output logic        bus_we_o,
    output logic        bus_byte_o,
    output logic [23:0] bus_addr_o,
    output logic [15:0] bus_wdata_o,
    input  logic        bus_ack_i,
    input  logic [15:0] bus_rdata_i,
    input  logic        bus_err_i
);

  // ---- shared memory layout, byte offsets ---------------------------------
  localparam logic [23:0] SCP_ISCP_LO = 24'd6;   // 24-bit ISCP address
  localparam logic [23:0] SCP_ISCP_HI = 24'd8;
  localparam logic [23:0] ISCP_BUSY   = 24'd0;
  localparam logic [23:0] ISCP_SCB    = 24'd2;
  localparam logic [23:0] ISCP_CB_LO  = 24'd4;   // 24-bit control block base
  localparam logic [23:0] ISCP_CB_HI  = 24'd6;
  localparam logic [23:0] SCB_STATUS  = 24'd0;
  localparam logic [23:0] SCB_CMD     = 24'd2;

  typedef enum logic [3:0] {
    S_UNINIT,        // out of reset, waiting for the first channel attention
    S_SCP_SYSBUS,
    S_SCP_ISCP_LO,
    S_SCP_ISCP_HI,
    S_ISCP_SCB,
    S_ISCP_CB_LO,
    S_ISCP_CB_HI,
    S_ISCP_CLR_BUSY,
    S_INIT_WR_STATUS,
    S_IDLE,          // initialised, waiting for a channel attention
    S_SCB_RD_CMD,
    S_SCB_WR_STATUS,
    S_SCB_CLR_CMD
  } state_e;

  state_e      state;
  logic [23:0] iscp_addr;
  logic [23:0] cbbase;
  logic [15:0] scb_off;
  logic [23:0] scb_addr;
  logic [15:0] scb_cmd;
  logic [3:0]  flags;        // {CX, FR, CNA, RNR}
  logic        bus16;        // SCP bus width byte, 0 => 16-bit host bus
  logic        ca_pending;

  // The status word exactly as the host will read it.
  wire [15:0] status_word = {1'b0, cus_o, 1'b0, rus_o, flags, 4'b0000};

  assign scb_addr = cbbase + {8'h00, scb_off};

  // ---- request presented to the memory port -------------------------------
  always_comb begin
    bus_req_o   = 1'b0;
    bus_we_o    = 1'b0;
    bus_byte_o  = 1'b0;
    bus_addr_o  = 24'h0;
    bus_wdata_o = 16'h0;
    case (state)
      S_SCP_SYSBUS: begin
        bus_req_o  = 1'b1;
        bus_byte_o = 1'b1;
        bus_addr_o = scp_addr_i[23:0];
      end
      S_SCP_ISCP_LO: begin
        bus_req_o  = 1'b1;
        bus_addr_o = scp_addr_i[23:0] + SCP_ISCP_LO;
      end
      S_SCP_ISCP_HI: begin
        bus_req_o  = 1'b1;
        bus_addr_o = scp_addr_i[23:0] + SCP_ISCP_HI;
      end
      S_ISCP_SCB: begin
        bus_req_o  = 1'b1;
        bus_addr_o = iscp_addr + ISCP_SCB;
      end
      S_ISCP_CB_LO: begin
        bus_req_o  = 1'b1;
        bus_addr_o = iscp_addr + ISCP_CB_LO;
      end
      S_ISCP_CB_HI: begin
        bus_req_o  = 1'b1;
        bus_addr_o = iscp_addr + ISCP_CB_HI;
      end
      S_ISCP_CLR_BUSY: begin
        bus_req_o   = 1'b1;
        bus_we_o    = 1'b1;
        bus_byte_o  = 1'b1;
        bus_addr_o  = iscp_addr + ISCP_BUSY;
        bus_wdata_o = 16'h0000;
      end
      S_SCB_RD_CMD: begin
        bus_req_o  = 1'b1;
        bus_addr_o = scb_addr + SCB_CMD;
      end
      S_SCB_CLR_CMD: begin
        bus_req_o   = 1'b1;
        bus_we_o    = 1'b1;
        bus_addr_o  = scb_addr + SCB_CMD;
        bus_wdata_o = 16'h0000;
      end
      S_INIT_WR_STATUS, S_SCB_WR_STATUS: begin
        bus_req_o   = 1'b1;
        bus_we_o    = 1'b1;
        bus_addr_o  = scb_addr + SCB_STATUS;
        bus_wdata_o = status_word;
      end
      default: ;
    endcase
  end

  // ---- sequencer ----------------------------------------------------------
  always_ff @(posedge clk) begin
    if (rst || core_rst_i) begin
      state      <= S_UNINIT;
      iscp_addr  <= 24'h0;
      cbbase     <= 24'h0;
      scb_off    <= 16'h0;
      scb_cmd    <= 16'h0;
      flags      <= 4'h0;
      bus16      <= 1'b1;
      ca_pending <= 1'b0;
    end else begin
      if (ca_i) ca_pending <= 1'b1;

      case (state)
        S_UNINIT:
          if (ca_pending) begin
            ca_pending <= 1'b0;
            state      <= S_SCP_SYSBUS;
          end

        S_SCP_SYSBUS:
          if (bus_ack_i) begin
            bus16 <= ~bus_rdata_i[0];   // 0 => 16-bit bus, 1 => 8-bit bus
            state <= S_SCP_ISCP_LO;
          end

        S_SCP_ISCP_LO:
          if (bus_ack_i) begin
            iscp_addr[15:0] <= bus_rdata_i;
            state           <= S_SCP_ISCP_HI;
          end

        S_SCP_ISCP_HI:
          if (bus_ack_i) begin
            iscp_addr[23:16] <= bus_rdata_i[7:0];
            state            <= S_ISCP_SCB;
          end

        S_ISCP_SCB:
          if (bus_ack_i) begin
            scb_off <= bus_rdata_i;
            state   <= S_ISCP_CB_LO;
          end

        S_ISCP_CB_LO:
          if (bus_ack_i) begin
            cbbase[15:0] <= bus_rdata_i;
            state        <= S_ISCP_CB_HI;
          end

        S_ISCP_CB_HI:
          if (bus_ack_i) begin
            cbbase[23:16] <= bus_rdata_i[7:0];
            state         <= S_ISCP_CLR_BUSY;
          end

        S_ISCP_CLR_BUSY:
          if (bus_ack_i) begin
            // Initialisation done: report command executed and command unit
            // not active, which is what the drivers poll for.
            flags <= 4'b1010;           // CX, CNA
            state <= S_INIT_WR_STATUS;
          end

        S_INIT_WR_STATUS:
          if (bus_ack_i) state <= S_IDLE;

        S_IDLE:
          if (ca_pending) begin
            ca_pending <= 1'b0;
            state      <= S_SCB_RD_CMD;
          end

        S_SCB_RD_CMD:
          if (bus_ack_i) begin
            scb_cmd <= bus_rdata_i;
            if (bus_rdata_i[15]) begin
              // Software reset: behave as if RESET had been pulled.  Clear the
              // command word first so the host sees the command taken.
              flags <= 4'h0;
              state <= S_SCB_CLR_CMD;
            end else begin
              // Acknowledge bits clear the matching status bits.  The command
              // unit and receive unit control fields, bits [14:12] and [10:8],
              // are not acted on yet.
              flags <= flags & ~bus_rdata_i[7:4];
              state <= S_SCB_WR_STATUS;
            end
          end

        // The status is written back before the command word is cleared, so a
        // driver that polls the command word and then reads the status never
        // sees stale bits.
        S_SCB_WR_STATUS:
          if (bus_ack_i) state <= S_SCB_CLR_CMD;

        S_SCB_CLR_CMD:
          if (bus_ack_i) state <= scb_cmd[15] ? S_UNINIT : S_IDLE;

        default: state <= S_UNINIT;
      endcase
    end
  end

  // Both units are idle until they exist.
  assign cus_o  = 3'd0;
  assign rus_o  = 3'd0;
  assign busy_o = (state != S_IDLE) && (state != S_UNINIT);
  assign int_o  = |flags;

  // Consumed once the datapath needs them.
  // verilator lint_off UNUSED
  // Only the reset bit of the latched command survives to the next state; the
  // command unit and receive unit fields land here until those blocks exist.
  wire _unused = &{1'b0, bus_err_i, bus16, scb_cmd[14:0], scp_addr_i[31:24]};
  // verilator lint_on UNUSED

endmodule
