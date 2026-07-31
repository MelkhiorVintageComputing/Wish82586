// SPDX-License-Identifier: MIT
//
// Command unit.
//
// Walks the linked list of command blocks the host built and hung off
// SCB.CBL.  For each block: mark it busy, read the opcode and the link, do
// what it asks, write back the completion status, then follow the link unless
// the block says to stop (EL) or to suspend (S).
//
// A block's command word carries EL at bit 15, S at bit 14, I at bit 13 and
// the opcode in bits [2:0].  Its status word gets C at bit 15, B at bit 14 and
// OK at bit 13, which is what the drivers poll.
//
// Implemented: NOP, IA-SETUP, CONFIGURE.  MC-SETUP is accepted but its address
// list is not stored yet, and TRANSMIT, TDR, DUMP and DIAGNOSE complete
// without OK until the datapath exists.

module ie_cu (
    input  logic        clk,
    input  logic        rst,
    input  logic        core_rst_i,

    input  logic [23:0] cbbase_i,

    // ---- control from the SCB handler --------------------------------------
    input  logic        start_i,        // one cycle, with the CBL offset
    input  logic [15:0] start_cbl_i,
    input  logic        resume_i,
    input  logic        suspend_i,
    input  logic        abort_i,

    output logic [2:0]  cus_o,
    output logic        ev_cx_o,        // a block with I completed
    output logic        ev_cna_o,       // the unit stopped being active

    // ---- parameters the datapath will need ---------------------------------
    output logic [47:0] ia_addr_o,      // individual address, first octet in [7:0]
    output logic [95:0] cfg_bytes_o,    // CONFIGURE bytes, byte 0 in [7:0]

    // ---- memory port -------------------------------------------------------
    output logic        bus_req_o,
    output logic        bus_we_o,
    output logic        bus_byte_o,
    output logic [23:0] bus_addr_o,
    output logic [15:0] bus_wdata_o,
    input  logic        bus_ack_i,
    input  logic [15:0] bus_rdata_i,
    input  logic        bus_err_i
);

  localparam logic [15:0] CB_ST_BUSY = 16'h4000;              // B
  localparam logic [15:0] CB_ST_DONE = 16'h8000;              // C
  localparam logic [15:0] CB_ST_OK   = 16'h2000;              // OK

  typedef enum logic [3:0] {
    CU_IDLE,
    CU_FETCH_CMD,
    CU_FETCH_LINK,
    CU_WR_BUSY,
    CU_IA,
    CU_CFG,
    CU_FINISH,
    CU_NEXT
  } state_e;

  state_e      state;
  logic [15:0] cb_off;
  logic [15:0] cmd;
  logic [15:0] link;
  logic [2:0]  idx;
  logic [2:0]  cfg_words;
  logic        ok;
  logic        suspended;
  logic        suspend_pending;
  logic        abort_pending;

  wire [23:0] cb_addr = cbbase_i + {8'h00, cb_off};
  wire [2:0]  opcode  = cmd[2:0];

  // Words of CONFIGURE parameters to fetch, from the byte count in byte 0.
  // The part accepts 4 to 12 bytes; anything else is clamped.
  wire [3:0] cfg_count = bus_rdata_i[3:0];
  wire [2:0] cfg_words_next = (cfg_count < 4'd2)   ? 3'd1 :
                              (cfg_count >= 4'd12) ? 3'd6 :
                              (cfg_count[3:1] + {2'b00, cfg_count[0]});

  // ---- request presented to the memory port -------------------------------
  always_comb begin
    bus_req_o   = 1'b0;
    bus_we_o    = 1'b0;
    bus_byte_o  = 1'b0;
    bus_addr_o  = 24'h0;
    bus_wdata_o = 16'h0;
    case (state)
      CU_FETCH_CMD: begin
        bus_req_o  = 1'b1;
        bus_addr_o = cb_addr + 24'd2;
      end
      CU_FETCH_LINK: begin
        bus_req_o  = 1'b1;
        bus_addr_o = cb_addr + 24'd4;
      end
      CU_WR_BUSY: begin
        bus_req_o   = 1'b1;
        bus_we_o    = 1'b1;
        bus_addr_o  = cb_addr;
        bus_wdata_o = CB_ST_BUSY;
      end
      CU_IA, CU_CFG: begin
        bus_req_o  = 1'b1;
        bus_addr_o = cb_addr + 24'd6 + {19'h0, idx, 1'b0};
      end
      CU_FINISH: begin
        bus_req_o   = 1'b1;
        bus_we_o    = 1'b1;
        bus_addr_o  = cb_addr;
        bus_wdata_o = CB_ST_DONE | (ok ? CB_ST_OK : 16'h0);
      end
      default: ;
    endcase
  end

  // ---- sequencer ----------------------------------------------------------
  always_ff @(posedge clk) begin
    if (rst || core_rst_i) begin
      state           <= CU_IDLE;
      cb_off          <= 16'h0;
      cmd             <= 16'h0;
      link            <= 16'h0;
      idx             <= 3'd0;
      cfg_words       <= 3'd1;
      ok              <= 1'b0;
      suspended       <= 1'b0;
      suspend_pending <= 1'b0;
      abort_pending   <= 1'b0;
      ev_cx_o         <= 1'b0;
      ev_cna_o        <= 1'b0;
      ia_addr_o       <= 48'h0;
      cfg_bytes_o     <= 96'h0;
    end else begin
      ev_cx_o  <= 1'b0;
      ev_cna_o <= 1'b0;

      if (suspend_i) suspend_pending <= 1'b1;
      if (abort_i)   abort_pending   <= 1'b1;

      case (state)
        CU_IDLE: begin
          if (start_i) begin
            cb_off          <= start_cbl_i;
            suspended       <= 1'b0;
            suspend_pending <= 1'b0;
            abort_pending   <= 1'b0;
            state           <= CU_FETCH_CMD;
          end else if (resume_i && suspended) begin
            // cb_off already points at the block the list stopped on.
            suspended       <= 1'b0;
            suspend_pending <= 1'b0;
            state           <= CU_FETCH_CMD;
          end else if (abort_i) begin
            suspended <= 1'b0;
          end
        end

        CU_FETCH_CMD:
          if (bus_ack_i) begin
            cmd   <= bus_rdata_i;
            state <= CU_FETCH_LINK;
          end

        CU_FETCH_LINK:
          if (bus_ack_i) begin
            link  <= bus_rdata_i;
            state <= CU_WR_BUSY;
          end

        CU_WR_BUSY:
          if (bus_ack_i) begin
            idx <= 3'd0;
            ok  <= 1'b1;
            case (opcode)
              wish82586_pkg::CMD_NOP:       state <= CU_FINISH;
              wish82586_pkg::CMD_IA_SETUP:  state <= CU_IA;
              wish82586_pkg::CMD_CONFIGURE: state <= CU_CFG;
              // TODO: store the multicast address list once there is a filter
              // to store it in.
              wish82586_pkg::CMD_MC_SETUP:  state <= CU_FINISH;
              // TODO: TRANSMIT, TDR, DUMP and DIAGNOSE need the datapath.
              default: begin
                ok    <= 1'b0;
                state <= CU_FINISH;
              end
            endcase
          end

        CU_IA:
          if (bus_ack_i) begin
            ia_addr_o[{idx[1:0], 4'h0} +: 16] <= bus_rdata_i;
            if (idx == 3'd2) state <= CU_FINISH;
            else             idx   <= idx + 3'd1;
          end

        CU_CFG:
          if (bus_ack_i) begin
            cfg_bytes_o[{idx, 4'h0} +: 16] <= bus_rdata_i;
            if (idx == 3'd0) begin
              // Byte 0 of the parameters says how many bytes to configure, so
              // how many more words there are to fetch only becomes known once
              // the first one has arrived.
              cfg_words <= cfg_words_next;
              if (cfg_words_next <= 3'd1) state <= CU_FINISH;
              else                        idx   <= 3'd1;
            end else if (idx + 3'd1 >= cfg_words) begin
              state <= CU_FINISH;
            end else begin
              idx <= idx + 3'd1;
            end
          end

        CU_FINISH:
          if (bus_ack_i) begin
            if (cmd[13]) ev_cx_o <= 1'b1;   // I: interrupt on completion
            state <= CU_NEXT;
          end

        CU_NEXT: begin
          if (abort_pending || cmd[15]) begin          // aborted, or end of list
            abort_pending <= 1'b0;
            suspended     <= 1'b0;
            ev_cna_o      <= 1'b1;
            state         <= CU_IDLE;
          end else if (cmd[14] || suspend_pending) begin   // suspend after this
            cb_off          <= link;
            suspend_pending <= 1'b0;
            suspended       <= 1'b1;
            ev_cna_o        <= 1'b1;
            state           <= CU_IDLE;
          end else begin
            cb_off <= link;
            state  <= CU_FETCH_CMD;
          end
        end

        default: state <= CU_IDLE;
      endcase
    end
  end

  always_comb begin
    if (state != CU_IDLE)   cus_o = wish82586_pkg::CUS_ACTIVE;
    else if (suspended)     cus_o = wish82586_pkg::CUS_SUSPENDED;
    else                    cus_o = wish82586_pkg::CUS_IDLE;
  end

  // verilator lint_off UNUSED
  wire _unused = &{1'b0, bus_err_i, cmd[12:3]};
  // verilator lint_on UNUSED

endmodule
