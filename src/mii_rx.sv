// SPDX-License-Identifier: MIT
//
// MII receive front end.  Runs entirely in the PHY's receive clock domain and
// hands bytes to the rest of the chip through an asynchronous FIFO.
//
// It strips the preamble and start frame delimiter, assembles nibbles into
// bytes low nibble first, checks the FCS, and holds the last four bytes back
// so the frame handed on never includes the FCS - the 82586 does not store it.
//
// FIFO word: {end, err[2:0], data[7:0]}.  Data words carry a byte of frame;
// the single end word closes the frame and carries, in err, whether the FCS
// was wrong, whether the frame ended on a nibble boundary, and whether the
// FIFO overflowed while the frame was being received.

module mii_rx (
    input  logic        rx_clk,
    input  logic        rst,

    input  logic [3:0]  rxd,
    input  logic        rx_dv,
    input  logic        rx_er,

    output logic        fifo_wr_o,
    output logic [11:0] fifo_data_o,
    input  logic        fifo_full_i,

    // For the unit tests and for waveform reading.
    output logic        active_o,
    output logic [15:0] byte_count_o
);

  localparam logic [3:0] PREAMBLE_NIBBLE = 4'h5;
  localparam logic [3:0] SFD_NIBBLE      = 4'hd;

  typedef enum logic [1:0] { RX_IDLE, RX_PREAMBLE, RX_DATA, RX_END } state_e;
  state_e state;

  logic [3:0]  lo_nibble;
  logic        odd;            // a nibble is waiting for its partner
  logic [15:0] nbytes;
  logic        crc_err_q, dribble_q, overrun_q, rx_er_q;

  // The last four bytes of a frame are its FCS, which is not passed on.  Bytes
  // move into the FIFO only once four more have arrived behind them.
  logic [7:0] dl0, dl1, dl2, dl3;
  logic [2:0] dl_cnt;

  // ---- FCS ----------------------------------------------------------------
  logic crc_init, crc_en;
  logic crc_ok;
  logic [31:0] crc_unused, fcs_unused;
  crc32_eth #(.DATA_W(4)) u_crc (
      .clk      (rx_clk),
      .rst      (rst),
      .init     (crc_init),
      .en       (crc_en),
      .data_i   (rxd),
      .crc_o    (crc_unused),
      .fcs_o    (fcs_unused),
      .crc_ok_o (crc_ok)
  );

  wire [7:0] new_byte = {rxd, lo_nibble};

  assign crc_init = (state == RX_IDLE) || (state == RX_PREAMBLE && rx_dv &&
                                           rxd == SFD_NIBBLE);
  assign crc_en   = (state == RX_DATA) && rx_dv;

  assign active_o     = (state != RX_IDLE);
  assign byte_count_o = nbytes;

  always_ff @(posedge rx_clk) begin
    if (rst) begin
      state       <= RX_IDLE;
      lo_nibble   <= 4'h0;
      odd         <= 1'b0;
      nbytes      <= 16'h0;
      crc_err_q   <= 1'b0;
      dribble_q   <= 1'b0;
      overrun_q   <= 1'b0;
      rx_er_q     <= 1'b0;
      dl0         <= 8'h0;
      dl1         <= 8'h0;
      dl2         <= 8'h0;
      dl3         <= 8'h0;
      dl_cnt      <= 3'h0;
      fifo_wr_o   <= 1'b0;
      fifo_data_o <= 12'h0;
    end else begin
      fifo_wr_o <= 1'b0;

      case (state)
        RX_IDLE: begin
          odd     <= 1'b0;
          nbytes  <= 16'h0;
          dl_cnt  <= 3'h0;
          rx_er_q <= 1'b0;
          if (rx_dv) begin
            overrun_q <= 1'b0;
            dribble_q <= 1'b0;
            crc_err_q <= 1'b0;
            // A frame starts with preamble; anything else is noise but is
            // followed anyway, so a missing SFD shows up as a bad frame.
            state <= RX_PREAMBLE;
          end
        end

        RX_PREAMBLE: begin
          if (!rx_dv) begin
            state <= RX_IDLE;                    // carrier event, no frame
          end else if (rxd == SFD_NIBBLE) begin
            state <= RX_DATA;
            odd   <= 1'b0;
          end else if (rxd != PREAMBLE_NIBBLE) begin
            // Not preamble and not the delimiter: treat what follows as data
            // so the frame is reported rather than silently dropped.
            state     <= RX_DATA;
            odd       <= 1'b1;
            lo_nibble <= rxd;
          end
        end

        RX_DATA: begin
          if (rx_er) rx_er_q <= 1'b1;
          if (rx_dv) begin
            odd <= ~odd;
            if (!odd) begin
              lo_nibble <= rxd;
            end else begin
              nbytes <= nbytes + 16'd1;
              if (dl_cnt == 3'd4) begin
                fifo_wr_o   <= 1'b1;
                fifo_data_o <= {4'h0, dl0};
                if (fifo_full_i) overrun_q <= 1'b1;
                dl0 <= dl1;
                dl1 <= dl2;
                dl2 <= dl3;
                dl3 <= new_byte;
              end else begin
                case (dl_cnt)
                  3'd0: dl0 <= new_byte;
                  3'd1: dl1 <= new_byte;
                  3'd2: dl2 <= new_byte;
                  default: dl3 <= new_byte;
                endcase
                dl_cnt <= dl_cnt + 3'd1;
              end
            end
          end else begin
            dribble_q <= odd;
            crc_err_q <= ~crc_ok;
            state     <= RX_END;
          end
        end

        RX_END: begin
          // One end word closes the frame.  It cannot be dropped, so wait for
          // room rather than losing the frame boundary.
          if (!fifo_full_i) begin
            fifo_wr_o   <= 1'b1;
            fifo_data_o <= {1'b1, crc_err_q | rx_er_q, dribble_q, overrun_q, 8'h00};
            state       <= RX_IDLE;
          end
        end

        default: state <= RX_IDLE;
      endcase
    end
  end

  // verilator lint_off UNUSED
  wire _unused = &{1'b0, crc_unused, fcs_unused};
  // verilator lint_on UNUSED

endmodule
