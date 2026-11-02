#include <Arduino.h>
#include "Ethernet.h"
#include "w5100.h"

void debug(const char *fmt, ...);

#define MACRAW_SOCK         0
#define MACRAW_MR_W6100     0x07        // Sn_MR protocol value of MACRAW on W6100 (WIZnet ioLibrary w6100.h)
#define MACRAW_MR_MF        0x80        // Sn_MR in MACRAW mode: MAC filter - only frames for own MAC + broadcast / multicast

#define MACRAW_HDR_SIZE     2           // each received frame starts with 2 bytes length - including these 2 bytes
#define FRAME_MIN_SIZE      60          // without FCS
#define FRAME_MAX_SIZE      1514        // without FCS
#define SEND_TIMEOUT_US     5000

bool EthernetMacraw::openWithMode(uint8_t mode)
{
    Ethernet.socketBeginOn(MACRAW_SOCK, mode, 0);
    return Ethernet.socketStatus(MACRAW_SOCK) == SnSR::MACRAW;
}

bool EthernetMacraw::begin(bool macFilter)
{
    stop();

    uint8_t chip = W5100.getChip();
    if (!chip) return false;

    _macFilter = macFilter;
    uint8_t mf = macFilter ? MACRAW_MR_MF : 0;
    uint8_t mode = (chip == 61) ? MACRAW_MR_W6100 : SnMR::MACRAW;

    _open = openWithMode(mode | mf);

    if (!_open && chip == 61) {         // sources disagree on the W6100 value, so try the W5x00 one too
        debug("EthernetMacraw: Sn_MR 0x%02X didn't open MACRAW (SR: 0x%02X), trying 0x%02X\n", mode | mf, Ethernet.socketStatus(MACRAW_SOCK), SnMR::MACRAW | mf);
        _open = openWithMode(SnMR::MACRAW | mf);
    }

    if (!_open) {
        debug("EthernetMacraw: MACRAW open failed, SR: 0x%02X\n", Ethernet.socketStatus(MACRAW_SOCK));
        Ethernet.socketClose(MACRAW_SOCK);
    }

    _remaining = 0;
    return _open;
}

void EthernetMacraw::stop()
{
    if (_open) {
        Ethernet.socketClose(MACRAW_SOCK);
    }
    _open = false;
    _remaining = 0;
}

int EthernetMacraw::parseFrame()
{
    if (!_open) return 0;

    skip();                                                     // rest of previous frame, if not read

    if (Ethernet.socketRecvAvailable(MACRAW_SOCK) < MACRAW_HDR_SIZE) {
        return 0;
    }

    uint8_t hdr[MACRAW_HDR_SIZE];
    if (Ethernet.socketRecv(MACRAW_SOCK, hdr, MACRAW_HDR_SIZE) != MACRAW_HDR_SIZE) {
        return 0;
    }

    uint16_t len = ((uint16_t)(hdr[0] & 0x07) << 8) | hdr[1];  // the upper bits are not part of the length (ioLibrary masks them too)

    if (len < MACRAW_HDR_SIZE + 14 || len > MACRAW_HDR_SIZE + FRAME_MAX_SIZE) {    // lost track of frame boundaries - WIZnet also reopens the socket then
        debug("EthernetMacraw: bad frame length %d (header %02X %02X), reopening\n", len, hdr[0], hdr[1]);
        begin(_macFilter);
        return -1;
    }

    _remaining = len - MACRAW_HDR_SIZE;
    return _remaining;
}

int EthernetMacraw::read(uint8_t *buf, uint16_t len)
{
    if (len > _remaining) len = _remaining;
    if (len == 0) return 0;

    int got = Ethernet.socketRecv(MACRAW_SOCK, buf, len);       // the whole frame is in the chip already, so this gets all of it
    if (got <= 0) return 0;

    _remaining -= got;
    return got;
}

void EthernetMacraw::skip()
{
    if (_remaining > 0) {
        Ethernet.socketRecv(MACRAW_SOCK, NULL, _remaining);     // NULL - just move the read pointer
        _remaining = 0;
    }
}

bool EthernetMacraw::sendFrame(const uint8_t *hdr, uint16_t hdrLen, const uint8_t *data, uint16_t dataLen)
{
    static const uint8_t zeros[FRAME_MIN_SIZE] = { 0 };

    if (!_open) return false;

    uint16_t len = hdrLen + dataLen;
    uint16_t pad = (len < FRAME_MIN_SIZE) ? (FRAME_MIN_SIZE - len) : 0;

    if (len > FRAME_MAX_SIZE) return false;

    // free space in TX buffer - not socketSendAvailable(), that one says 0 for anything else than a connected TCP socket
    SPI.beginTransaction(SPI_ETHERNET_SETTINGS);
    uint16_t freeSize, prev = W5100.readSnTX_FSR(MACRAW_SOCK);
    while ((freeSize = W5100.readSnTX_FSR(MACRAW_SOCK)) != prev) {     // the chip may be updating it, read until stable
        prev = freeSize;
    }
    SPI.endTransaction();

    if (freeSize < len + pad) {
        debug("EthernetMacraw: no room in TX buffer (%d < %d)\n", freeSize, len + pad);
        return false;
    }

    // Each part at its offset from the start of the frame. Reading Sn_TX_WR back gives the value from before the last SEND
    // (W6100 at least), so write_data() can't continue where the previous part ended - that's what the offset is for (UDP uses it the same way).
    Ethernet.socketBufferData(MACRAW_SOCK, 0, hdr, hdrLen);
    if (dataLen) Ethernet.socketBufferData(MACRAW_SOCK, hdrLen, data, dataLen);
    if (pad)     Ethernet.socketBufferData(MACRAW_SOCK, hdrLen + dataLen, zeros, pad);

    // SEND and wait for SEND_OK. Not socketSendUDP() - it clears Sn_IR by writing it, which doesn't work on W6100 (Sn_IRCLR does)
    SPI.beginTransaction(SPI_ETHERNET_SETTINGS);
    W5100.writeSnIRCLR(MACRAW_SOCK, SnIR::SEND_OK | SnIR::TIMEOUT);
    W5100.execCmdSn(MACRAW_SOCK, Sock_SEND);

    bool ok = false;
    uint32_t start = micros();
    while ((micros() - start) < SEND_TIMEOUT_US) {              // a full frame takes 0.12 ms on 100 Mbit, 1.2 ms on 10 Mbit
        if (W5100.readSnIR(MACRAW_SOCK) & SnIR::SEND_OK) {
            ok = true;
            break;
        }
    }
    W5100.writeSnIRCLR(MACRAW_SOCK, SnIR::SEND_OK);
    SPI.endTransaction();

    if (!ok) {
        debug("EthernetMacraw: SEND_OK didn't come\n");
    }
    return ok;
}
