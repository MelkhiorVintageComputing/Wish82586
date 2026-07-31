// SPDX-License-Identifier: MIT
//
// MII transmitter.  Runs in the PHY's transmit clock domain.
//
// The command unit stages a whole frame in the buffer RAM and raises go; this
// block defers until the medium is quiet, sends preamble, delimiter, the
// frame, whatever padding the minimum length needs and the FCS, then reports
// what happened.  Because the frame is already staged, a collision only means
// starting the RAM again - nothing has to be re-read from host memory - which
// is what makes the retry loop simple.
//
// The handshake with the system side is four phase: go rises, done rises when
// the attempt is over, go falls, done falls.  len and the configuration inputs
// must be stable before go rises; they only change when a CONFIGURE command
// runs, which cannot happen while a transmit is in flight.

module mii_tx (
    input  logic        tx_clk,
    input  logic        rst,

    // ---- control, from the system clock domain ------------------------------
    input  logic        go_i,
    input  logic [15:0] len_i,
    output logic        done_o,
    output logic        ok_o,
    output logic [3:0]  ncoll_o,
    output logic        xcoll_o,
    output logic        defer_o,
    output logic        no_crs_o,

    // ---- configuration ------------------------------------------------------
    input  logic [3:0]  retry_limit_i,
    input  logic [7:0]  ifs_i,          // interframe spacing, bit times
    input  logic [10:0] slot_time_i,    // slot time, bit times
    input  logic [7:0]  min_len_i,      // minimum frame length, FCS included
    input  logic        no_crc_i,

    // ---- staged frame -------------------------------------------------------
    output logic [10:0] ram_addr_o,
    input  logic [7:0]  ram_data_i,

    // ---- MII ----------------------------------------------------------------
    output logic [3:0]  txd,
    output logic        tx_en,
    output logic        tx_er,
    input  logic        crs,
    input  logic        col
);

  typedef enum logic [3:0] {
    T_IDLE,
    T_DEFER,
    T_PREAMBLE,
    T_DATA,
    T_FCS,
    T_END,
    T_JAM,
    T_BACKOFF,
    T_DONE
  } state_e;

  state_e      state;
  logic        go_s1, go_s2;
  logic [15:0] idx;              // byte being sent
  logic        phase;            // 0 = low nibble, 1 = high nibble
  logic [4:0]  pre_cnt;
  logic [2:0]  fcs_cnt;
  logic [15:0] gap_cnt;
  logic [4:0]  jam_cnt;
  logic [3:0]  attempt;
  logic        deferred;
  logic [15:0] lfsr;

  // The frame goes out at least min_len bytes long, FCS included.
  wire [15:0] pad_len  = ({8'h00, min_len_i} > 16'd4) ?
                         ({8'h00, min_len_i} - 16'd4) : 16'd0;
  wire [15:0] send_len = (len_i > pad_len) ? len_i : pad_len;
  wire        padding  = (idx >= len_i);
  wire [7:0]  cur_byte = padding ? 8'h00 : ram_data_i;

  // Bit times to nibble times.
  wire [15:0] ifs_nibbles  = {8'h00, ifs_i} >> 2;
  wire [7:0]  slot_nibbles = slot_time_i[9:2];

  // Truncated binary exponential backoff, with an LFSR standing in for the
  // random number.  TODO: the standard masks at ten attempts and counts whole
  // slot times; this is a bounded approximation, enough to get off the wire
  // and try again but not the real distribution.
  wire [3:0]  shift         = (attempt > 4'd7) ? 4'd0 : (4'd7 - attempt);
  wire [7:0]  backoff_slots = lfsr[7:0] >> shift;
  wire [15:0] backoff_ticks = 16'(backoff_slots * slot_nibbles);

  // ---- FCS ----------------------------------------------------------------
  // The accumulator is fed combinationally with the nibble being handed to the
  // output register this cycle, so by the time the FCS itself is read the last
  // data nibble is already in it.
  wire [3:0] data_nibble = phase ? cur_byte[7:4] : cur_byte[3:0];
  wire       crc_init    = (state != T_DATA) && (state != T_FCS);
  wire       crc_en      = (state == T_DATA) && !col;

  logic [31:0] fcs;
  logic [31:0] crc_unused;
  logic        crc_ok_unused;

  crc32_eth #(.DATA_W(4)) u_crc (
      .clk      (tx_clk),
      .rst      (rst),
      .init     (crc_init),
      .en       (crc_en),
      .data_i   (data_nibble),
      .crc_o    (crc_unused),
      .fcs_o    (fcs),
      .crc_ok_o (crc_ok_unused)
  );

  wire [4:0] fcs_base = {fcs_cnt[1:0], 3'b000};

  // Address the byte after the one being sent, so the registered RAM read has
  // landed by the time it is needed.
  always_comb begin
    if (state == T_DATA && phase) ram_addr_o = idx[10:0] + 11'd1;
    else                          ram_addr_o = idx[10:0];
  end

  always_ff @(posedge tx_clk) begin
    if (rst) begin
      state    <= T_IDLE;
      go_s1    <= 1'b0;
      go_s2    <= 1'b0;
      idx      <= 16'h0;
      phase    <= 1'b0;
      pre_cnt  <= 5'h0;
      fcs_cnt  <= 3'h0;
      gap_cnt  <= 16'h0;
      jam_cnt  <= 5'h0;
      attempt  <= 4'h0;
      deferred <= 1'b0;
      lfsr     <= 16'hace1;
      txd      <= 4'h0;
      tx_en    <= 1'b0;
      tx_er    <= 1'b0;
      done_o   <= 1'b0;
      ok_o     <= 1'b0;
      ncoll_o  <= 4'h0;
      xcoll_o  <= 1'b0;
      defer_o  <= 1'b0;
      no_crs_o <= 1'b0;
    end else begin
      go_s1 <= go_i;
      go_s2 <= go_s1;
      lfsr  <= {lfsr[14:0], lfsr[15] ^ lfsr[13] ^ lfsr[12] ^ lfsr[10]};

      case (state)
        T_IDLE: begin
          tx_en <= 1'b0;
          txd   <= 4'h0;
          if (go_s2 && !done_o) begin
            attempt  <= 4'h0;
            deferred <= 1'b0;
            ncoll_o  <= 4'h0;
            xcoll_o  <= 1'b0;
            no_crs_o <= 1'b0;
            gap_cnt  <= ifs_nibbles;
            state    <= T_DEFER;
          end
        end

        // Wait for the medium to go quiet, then let the interframe gap pass.
        T_DEFER: begin
          if (crs) begin
            deferred <= 1'b1;
            gap_cnt  <= ifs_nibbles;
          end else if (gap_cnt != 16'h0) begin
            gap_cnt <= gap_cnt - 16'd1;
          end else begin
            idx     <= 16'h0;
            phase   <= 1'b0;
            pre_cnt <= 5'd16;
            state   <= T_PREAMBLE;
          end
        end

        T_PREAMBLE: begin
          tx_en   <= 1'b1;
          txd     <= (pre_cnt == 5'd1) ? 4'hd : 4'h5;
          pre_cnt <= pre_cnt - 5'd1;
          if (col) begin
            state <= T_JAM;
          end else if (pre_cnt == 5'd1) begin
            state <= T_DATA;
            phase <= 1'b0;
          end
        end

        T_DATA: begin
          tx_en <= 1'b1;
          txd   <= data_nibble;
          if (col) begin
            state <= T_JAM;
          end else begin
            phase <= ~phase;
            if (phase) begin
              if (idx + 16'd1 >= send_len) begin
                fcs_cnt <= 3'd0;
                phase   <= 1'b0;
                state <= no_crc_i ? T_END : T_FCS;
              end else begin
                idx <= idx + 16'd1;
              end
            end
          end
        end

        // The FCS goes out least significant byte first, low nibble first.
        T_FCS: begin
          tx_en <= 1'b1;
          txd   <= phase ? fcs[fcs_base + 5'd4 +: 4] : fcs[fcs_base +: 4];
          if (col) begin
            state <= T_JAM;
          end else begin
            phase <= ~phase;
            if (phase) begin
              if (fcs_cnt == 3'd3) begin
                state <= T_END;
              end else begin
                fcs_cnt <= fcs_cnt + 3'd1;
              end
            end
          end
        end

        // The nibble assigned in the last data or FCS cycle only reaches the
        // wire on the next one, so transmit enable has to outlive it by a
        // cycle or the frame goes out a nibble short.
        T_END: begin
          tx_en <= 1'b0;
          state <= T_DONE;
        end

        // Keep transmitting for a moment so everyone sees the collision.
        T_JAM: begin
          tx_en   <= 1'b1;
          txd     <= 4'h5;
          jam_cnt <= jam_cnt + 5'd1;
          if (jam_cnt == 5'd7) begin
            tx_en   <= 1'b0;
            jam_cnt <= 5'h0;
            if (ncoll_o != 4'hf) ncoll_o <= ncoll_o + 4'd1;
            if (attempt >= retry_limit_i) begin
              xcoll_o <= 1'b1;
              state   <= T_DONE;
            end else begin
              attempt <= attempt + 4'd1;
              gap_cnt <= backoff_ticks;
              state   <= T_BACKOFF;
            end
          end
        end

        T_BACKOFF: begin
          tx_en <= 1'b0;
          if (gap_cnt != 16'h0) begin
            gap_cnt <= gap_cnt - 16'd1;
          end else begin
            gap_cnt <= ifs_nibbles;
            state   <= T_DEFER;
          end
        end

        T_DONE: begin
          tx_en   <= 1'b0;
          defer_o <= deferred;
          if (!go_s2) begin
            done_o <= 1'b0;
            state  <= T_IDLE;
          end else begin
            done_o <= 1'b1;
            ok_o   <= ~xcoll_o;
          end
        end

        default: state <= T_IDLE;
      endcase
    end
  end

  // verilator lint_off UNUSED
  wire _unused = &{1'b0, crc_unused, crc_ok_unused, slot_time_i[10],
                   slot_time_i[1:0], fcs_cnt[2]};
  // verilator lint_on UNUSED

endmodule
