// SPDX-License-Identifier: MIT
//
// Drives the co-simulation library the way the NetBSD driver drives the card,
// without QEMU in the way.
//
// The point is to be able to tell a broken shim from a broken device model
// from a broken guest.  Everything below writes the same structures at the
// same offsets that ef_attach() and i82586_init() write, using nothing but the
// C interface the emulated card uses, so if this passes and the guest still
// does not work the fault is on the QEMU side.
//
// Built by the Makefile here; run it after building the library.

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "wish_rtl.h"

namespace {

constexpr uint32_t WINDOW = 64 * 1024;
constexpr uint32_t BASE = (1u << 24) - WINDOW;   // where the chip sees it

// Offsets inside the window, laid out as the drivers lay them out.
constexpr uint32_t ISCP = 0;
constexpr uint32_t SCB = 8;
constexpr uint32_t CB = 24;                 // sc->buf_area
constexpr uint32_t XBD = 0x100;
constexpr uint32_t XBUF = 0x200;
constexpr uint32_t RFD = 0x800;             // two frame descriptors
constexpr uint32_t RBD = 0x900;             // four buffer descriptors
constexpr uint32_t RBUF = 0x1000;           // four buffers
constexpr uint32_t RBUF_SIZE = 512;
constexpr int N_RFD = 2;
constexpr int N_RBD = 4;

constexpr uint16_t SCB_CUC_START = 0x0100;
constexpr uint16_t SCB_RUC_START = 0x0010;

uint8_t mem[WINDOW];
int failures;

void wr8(uint32_t o, uint8_t v) { mem[o] = v; }
void wr16(uint32_t o, uint16_t v) { mem[o] = v & 0xff; mem[o + 1] = v >> 8; }
void wr32(uint32_t o, uint32_t v) { wr16(o, v & 0xffff); wr16(o + 2, v >> 16); }
uint16_t rd16(uint32_t o) { return uint16_t(mem[o] | (mem[o + 1] << 8)); }

void check(bool ok, const char *what)
{
    printf("%-46s %s\n", what, ok ? "ok" : "FAILED");
    if (!ok) {
        failures++;
    }
}

void check_eq(uint16_t got, uint16_t want, const char *what)
{
    if (got != want) {
        printf("%-46s FAILED (got 0x%04x, wanted 0x%04x)\n", what, got, want);
        failures++;
    } else {
        printf("%-46s ok\n", what);
    }
}

// Everything the chip reports lands in the SCB status; clear it the way
// ie_ack() does before going on to the next thing.
void ack(WishRtl *r)
{
    for (int i = 0; i < 4; i++) {
        const uint16_t st = rd16(SCB) & 0xf000;
        if (!st) {
            return;
        }
        wr16(SCB + 2, st);
        wish_rtl_ca(r);
    }
}

// Put a command block at CB, point the SCB at it and let the command unit run.
uint16_t run_command(WishRtl *r, uint16_t opcode)
{
    wr16(CB + 0, 0);                       // status
    wr16(CB + 2, uint16_t(opcode | 0x8000));   // command, end of list
    wr16(CB + 4, 0xffff);                  // link
    wr16(SCB + 4, CB);                     // SCB command list
    wr16(SCB + 2, SCB_CUC_START);
    wish_rtl_ca(r);
    const uint16_t status = rd16(CB);
    ack(r);
    return status;
}

void build_receive_area()
{
    for (int n = 0; n < N_RFD; n++) {
        const uint32_t fd = RFD + n * 24;
        const int next = (n + 1) % N_RFD;
        wr16(fd + 0, 0);                                    // status
        wr16(fd + 2, n == N_RFD - 1 ? 0xc000 : 0);          // EOL | SUSPEND
        wr16(fd + 4, uint16_t(RFD + next * 24));            // next
        wr16(fd + 6, 0xffff);                               // buffer list
    }
    for (int n = 0; n < N_RBD; n++) {
        const uint32_t bd = RBD + n * 12;
        const int next = (n + 1) % N_RBD;
        wr16(bd + 0, 0);                                    // status
        wr16(bd + 2, uint16_t(RBD + next * 12));            // next
        wr32(bd + 4, BASE + RBUF + n * RBUF_SIZE);          // buffer
        wr16(bd + 8, uint16_t(RBUF_SIZE |
                              (n == N_RBD - 1 ? 0x8000 : 0)));  // size, EOL
    }
    // Only the first descriptor carries a buffer pointer.
    wr16(RFD + 6, RBD);
}

}  // namespace

int main(void)
{
    if (wish_rtl_abi() != WISH_RTL_ABI) {
        fprintf(stderr, "library ABI %u, expected %u\n", wish_rtl_abi(),
                WISH_RTL_ABI);
        return 1;
    }

    memset(mem, 0, sizeof(mem));

    // ---- what ef_attach() writes before it believes there is a chip -------
    const uint32_t scp = WINDOW - 12;
    wr8(scp + 2, 0);                    // bus use: 16-bit
    wr32(scp + 8, BASE + ISCP);         // 24-bit address of the ISCP
    wr16(ISCP + 0, 1);                  // busy
    wr16(ISCP + 2, SCB);                // SCB offset
    wr32(ISCP + 4, BASE + ISCP);        // base for every 16-bit offset

    WishRtl *r = wish_rtl_new(mem, WINDOW, BASE);

    // ---- i82586_proberam() -----------------------------------------------
    wish_rtl_reset(r);
    wish_rtl_ca(r);
    check(rd16(ISCP) == 0, "initialisation clears the ISCP busy flag");
    check_eq(uint16_t(rd16(SCB) & 0xf000), 0xa000, "initialisation reports CX and CNA");
    ack(r);

    // ---- ie_cfg_setup(), byte for byte as NetBSD writes it ----------------
    static const uint8_t cfg[12] = {
        0x0c, 0x08, 0x40, 0x2e, 0x00, 0x60, 0x00, 0xf2, 0x00, 0x00, 0x40, 0xff
    };
    memcpy(mem + CB + 6, cfg, sizeof(cfg));
    check_eq(uint16_t(run_command(r, 2) & 0xf000), 0xa000, "CONFIGURE completes with OK");

    // ---- ie_ia_setup() ----------------------------------------------------
    static const uint8_t mac[6] = { 0x52, 0x54, 0x00, 0x12, 0x34, 0x56 };
    memcpy(mem + CB + 6, mac, sizeof(mac));
    check_eq(uint16_t(run_command(r, 1) & 0xf000), 0xa000, "IA-SETUP completes with OK");

    // ---- i82586_start_transceiver() ---------------------------------------
    build_receive_area();
    wr16(SCB + 6, RFD);
    wr16(SCB + 2, SCB_RUC_START);
    wish_rtl_ca(r);
    ack(r);
    check(wish_rtl_ru_ready(r) != 0, "the receive unit is ready");

    // ---- a frame off the wire ---------------------------------------------
    std::vector<uint8_t> in;
    in.insert(in.end(), mac, mac + 6);                              // to us
    static const uint8_t peer[6] = { 0x02, 0x00, 0xde, 0xad, 0xbe, 0xef };
    in.insert(in.end(), peer, peer + 6);
    in.push_back(0x08); in.push_back(0x00);                         // IPv4
    for (int i = 0; i < 46; i++) {
        in.push_back(uint8_t(i));
    }
    wish_rtl_rx(r, in.data(), uint32_t(in.size()));

    check_eq(uint16_t(rd16(RFD) & 0xa000), 0xa000, "the frame is complete and OK");
    check_eq(uint16_t(rd16(RBD) & 0x4000), 0x4000, "the first buffer is marked used");
    check_eq(uint16_t(rd16(RBD) & 0x3fff), uint16_t(in.size()), "the byte count is the frame length");
    check(memcmp(mem + RBUF, in.data(), in.size()) == 0,
          "the buffer holds the frame that arrived");
    ack(r);

    // ---- a frame the other way, as iexmit() sets it up --------------------
    std::vector<uint8_t> out;
    out.insert(out.end(), peer, peer + 6);
    out.insert(out.end(), mac, mac + 6);
    out.push_back(0x08); out.push_back(0x06);                       // ARP
    for (int i = 0; i < 46; i++) {
        out.push_back(uint8_t(0x80 + i));
    }
    memcpy(mem + XBUF, out.data(), out.size());
    wr16(XBD + 0, uint16_t(out.size() | 0x8000));   // count, end of list
    wr16(XBD + 2, 0xffff);
    wr32(XBD + 4, BASE + XBUF);
    wr16(CB + 6, XBD);                              // buffer descriptor
    check_eq(uint16_t(run_command(r, 4) & 0xa000), 0xa000, "TRANSMIT completes with OK");

    uint8_t got[2048];
    const int n = wish_rtl_tx_pop(r, got, sizeof(got));
    check(n == int(out.size()), "the transmitted frame is the right length");
    check(n > 0 && memcmp(got, out.data(), out.size()) == 0,
          "the transmitted frame is the one asked for");
    check(wish_rtl_tx_pop(r, got, sizeof(got)) == 0, "nothing else was transmitted");

    printf("\n%.3f ms of simulated time\n", wish_rtl_time_ns(r) / 1e6);
    wish_rtl_free(r);

    printf("%s\n", failures ? "FAILED" : "PASS");
    return failures ? 1 : 0;
}
