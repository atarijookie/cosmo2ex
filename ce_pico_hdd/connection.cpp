#include <Ethernet.h>
#include <Dhcp.h>        // DHCP_CHECK_* results of Ethernet.maintain()

#include "defs.h"
#include "connection.h"
#include "utils.h"
#include "ipc.h"
#include "sting_net.h"

#define SERVER_UDP_PORT 7200 // port number where CE listens for client requests
#define CLIENT_UDP_PORT 7201 // port where this client should listen for CE responses

EthernetUDP udp;
bool udpInitialized;

extern EthernetClient clientIkbd;

EthernetClient clientHdd;
EthernetClient clientFdd;
EthernetClient clientIkbd;

uint8_t hostIp[4];
String hostIpString;
uint16_t hostPortHdd;
uint16_t hostPortFdd;
uint16_t hostPortIkbd;

volatile bool connected = false;
volatile bool ikbdConnected = false;

struct {
    bool sendStatusAfterRead;
    uint8_t statusByte;
} c0;

THeader hddHeader;      // keep the header global to preserve syncTag between calls
bool hddHeaderPending;  // hddHeader received, waiting for its payload to arrive
uint32_t hddHeaderTime; // when hddHeader was received

#define PAYLOAD_WAIT_MS             1000    // drop the received header if its payload doesn't arrive within this time
#define PAYLOAD_MAX_WAIT_SIZE       64      // wait for at most this many payload bytes - all the commands except CMD_DATA_MARKER are smaller

uint8_t connectFailCount;                   // count of failed connection attempts in a row
static uint32_t connLostAt;                 // millis() when the connection to host was lost (or boot)
static uint32_t connAttempts;               // connect attempts since connLostAt
static uint32_t connLastAttempt = 0xffff0000;   // millis() of the last connect attempt, -65k: first one right away
static uint32_t discoverySendCount;         // broadcasts sent in this discovery round, for throttled logging

// states of all the hw sockets (SnSR: 00 closed, 17 established, 1C close wait, 22 udp, ...) and who has which one
void logSockets(const char* where)
{
    char states[MAX_SOCK_NUM * 3 + 1];
    for(int i=0; i<MAX_SOCK_NUM; i++) {
        snprintf(states + i * 3, 4, "%02X ", Ethernet.socketStatusOf(i));
    }

    debug("%s - sockets: %s(hdd #%d, ikbd #%d, udp #%d)\n", where, states,
          clientHdd.getSocketNumber(), clientIkbd.getSocketNumber(), udp.getSocketNumber());
}

// how long ago the connection was lost, for logs
static uint32_t msSinceLost(void)
{
    return millis() - connLostAt;
}
#define CONNECT_FAILS_BEFORE_REDISCOVERY 5  // after this many failed connects forget the host and discover it again (it might have new IP / port)
#define CONNECT_RETRY_MS                1000    // connect attempt this often - refused connect takes ~1 ms, only unreachable host blocks (up to 1 s)
#define DISCOVERY_LOG_EVERY             10      // log only every Nth discovery broadcast (1 per second), not to flood the log

// Send broadcast to find any CE server on the network.
void ceDiscoverySend(void)
{
    static uint32_t lastAttempt = 0xffff0000; // -65k
    static bool whichBroadcastAddr = false;
    // already got hostIp and port? don't do discovery
    if (hostIp[0] != 0 && hostPortHdd != 0)
    {
        return;
    }

    if (!udpInitialized)        // didn't get udp socket? can't send (sockindex would point to a socket we don't have)
    {
        return;
    }

    // if last attempt was less than a moment ago, don't try
    if ((millis() - lastAttempt) < 1000)
    {
        return;
    }

    lastAttempt = millis();

    // send upd broadcast
    uint8_t updPacket[10];
    strcpy((char *)updPacket, "CELC");          // 0..3 - CELC
    memcpy(updPacket + 4, settings.mac, 6);     // 4..9 - mac

    whichBroadcastAddr = !whichBroadcastAddr;   // toggle this flag
    String target;

    if(whichBroadcastAddr)      // send to subnet broadcast addr?
    {
        // get local ip and mask, create broadcast ip
        IPAddress ip = Ethernet.localIP();
        uint32_t ip32 = (((uint32_t)ip[0]) << 24) | (((uint32_t)ip[1]) << 16) | (((uint32_t)ip[2]) << 8) | (((uint32_t)ip[3]));

        IPAddress mask = Ethernet.subnetMask();
        uint32_t mask32 = (((uint32_t)mask[0]) << 24) | (((uint32_t)mask[1]) << 16) | (((uint32_t)mask[2]) << 8) | (((uint32_t)mask[3]));
        uint32_t mask32inv = ~mask32;

        uint32_t ip32broadcast = ip32 | mask32inv; // create broadcast addr by setting all subnet bits to 1

        IPAddress addrBroadcast((uint8_t) (ip32broadcast >> 24), (uint8_t) (ip32broadcast >> 16), (uint8_t) (ip32broadcast >> 8), (uint8_t) ip32broadcast);    // from uint32_t to object

        target = addrBroadcast.toString();    // broadcast to subnet devices (e.g. 192.168.1.255)
    }
    else        // send to generic broadcast addr
    {
        target = "255.255.255.255";             // broadcast to all possible devices
    }

    int okBegin = udp.beginPacket(target.c_str(), SERVER_UDP_PORT);
    udp.write(updPacket, 10);
    int okEnd = udp.endPacket();

    // log the first one, then every Nth, and any failure
    bool failed = !okBegin || !okEnd;
    if((discoverySendCount % DISCOVERY_LOG_EVERY) == 0 || failed) {
        debug("ceDiscoverySend #%u to %s - udp #%d, begin: %d, end: %d, %u ms since connection lost\n",
              discoverySendCount + 1, target.c_str(), udp.getSocketNumber(), okBegin, okEnd, msSinceLost());
    }
    discoverySendCount++;
}

// Receive response from server if there is any and store it if it's valid.
// Serves also for dropping any additional udp packets.
void ceDiscoveryReceive(void)
{
    static uint32_t lastAttempt = 0xffff0000; // -65k

    // if last attempt was less than a moment ago, don't try
    if ((millis() - lastAttempt) < 100)
    {
        return;
    }

    lastAttempt = millis();

    if (!udpInitialized)        // didn't get udp socket? nothing to receive
    {
        return;
    }

    // check if any udp packet was received, handle it
    while (true)
    {
        int packetSize = udp.parsePacket();

        // no packet received? can stop trying to read it
        if (packetSize == 0)
        {
            break;
        }

        /*
            received packet structure:
            0..3    'CELR' string
            4..5    port for hdd
            6..7    port for fdd
            8..9    port for ikbd
        */
        uint8_t buffer[10];
        memset(buffer, 0, 10);
        udp.read(buffer, 10);

        // start of the data isn't CELR? skip the rest
        if (strncmp((const char *)buffer, "CELR", 4) != 0)
        {
            debug("ceDiscoveryReceive - ignoring %d bytes packet from %s\n", packetSize, udp.remoteIP().toString().c_str());
            continue;
        }

        // store server's ip address
        IPAddress remoteIp = udp.remoteIP();
        for (int i = 0; i < 4; i++)
        {
            hostIp[i] = remoteIp[i];
        }

        IPAddress addr(hostIp[0], hostIp[1], hostIp[2], hostIp[3]); // octets to IPAddress
        hostIpString = addr.toString();                             // copy the ip address as string

        // store ports and stop receiving
        hostPortHdd = getWord(buffer + 4);
        hostPortFdd = getWord(buffer + 6);
        hostPortIkbd = getWord(buffer + 8);

        debug("ceDiscoveryReceive - got host ip: %s, ports: %d, %d, %d, %u ms since connection lost\n", hostIpString.c_str(), hostPortHdd, hostPortFdd, hostPortIkbd, msSinceLost());

        connLastAttempt = millis() - CONNECT_RETRY_MS;  // host just answered - connect right away, don't wait for the retry time
        discoverySendCount = 0;                         // next discovery round logs its first broadcast again
    }
}

void connectToCEhost(void)
{
    // if last attempt was less than a moment ago, don't try
    if ((millis() - connLastAttempt) < CONNECT_RETRY_MS)
    {
        return;
    }

    // we're connecting now
    connLastAttempt = millis();

    // no host IP? not connecting
    if (hostIpString.length() == 0)
    {
        return;
    }

    connAttempts++;
    debug("connectToCEhost - IP: %s, port: %d, attempt #%u, %u ms since connection lost\n",
          hostIpString.c_str(), hostPortHdd, connAttempts, msSinceLost());

    // start connection attempt
    clientHdd.stop();                                       // close socket if still open
    uint32_t start = millis();
    connected = clientHdd.connect(hostIpString.c_str(), hostPortHdd);

    debug("connectToCEhost - connected: %d (connect took %u ms)\n", connected, millis() - start);

    if(!connected) {
        logSockets("connectToCEhost failed");
    }

    if(connected)
    {
        debug("connectToCEhost - RECONNECTED %u ms after connection lost, %u attempts\n", msSinceLost(), connAttempts);
        connectFailCount = 0;

        hddHeader.syncTag = 0;          // new stream - don't use anything left from the previous connection
        hddHeaderPending = false;
    }
    else if(++connectFailCount >= CONNECT_FAILS_BEFORE_REDISCOVERY)
    {
        debug("connectToCEhost - %d failed attempts, discovering host again (%u ms since connection lost)\n", connectFailCount, msSinceLost());
        connectFailCount = 0;

        memset(hostIp, 0, sizeof(hostIp));
        hostIpString = "";
        hostPortHdd = hostPortFdd = hostPortIkbd = 0;
    }

    // clientHdd.setNoDelay(true);

    // clientIkbd.connect(hostIpString.c_str(), hostPortIkbd);
}

// if UDP not initialized, do that now
void udpSocketOpen(void)
{
    static uint32_t lastFailLog = 0;

    if (!udpInitialized)
    {
        udpInitialized = udp.begin(CLIENT_UDP_PORT);     // no free socket? try again next time, don't send through a socket we don't have

        if(udpInitialized) {
            debug("udpSocketOpen - ok, %u ms since connection lost\n", msSinceLost());
            logSockets("udpSocketOpen");
        } else if(millis() - lastFailLog >= 1000) {
            lastFailLog = millis();
            debug("udpSocketOpen - FAILED, no free socket\n");
            logSockets("udpSocketOpen");
        }
    }
}

// if UDP is initialized, deinitialize
void udpSocketClose(void)
{
    if(udpInitialized)
    {
        udp.stop();
        udpInitialized = false;
    }
}

// Renew DHCP lease when needed. If renewal brings a new IP address, the connection to host is dead, so close it.
void dhcpMaintain(void)
{
    static uint32_t lastCheck = 0;

    uint32_t now = millis();
    if((now - lastCheck) < 1000) {      // check once per second, more is not needed
        return;
    }
    lastCheck = now;

    IPAddress ipBefore = Ethernet.localIP();
    int res = Ethernet.maintain();      // might block for a while when the DHCP server doesn't respond

    switch(res)
    {
        case DHCP_CHECK_RENEW_OK:
        case DHCP_CHECK_REBIND_OK:
        {
            IPAddress ipNow = Ethernet.localIP();
            if(ipNow == ipBefore) {
                break;
            }

            debug("dhcpMaintain - ip changed from %s to %s, reconnecting\n", ipBefore.toString().c_str(), ipNow.toString().c_str());

            clientHdd.stop();
            connected = false;
            break;
        }

        case DHCP_CHECK_RENEW_FAIL:
        case DHCP_CHECK_REBIND_FAIL:
            debug("dhcpMaintain - DHCP lease renewal failed (%d), will retry\n", res);
            break;
    }
}

void connectToHost(void)
{
    static bool prevConnected = false;

    dhcpMaintain();
    connected = clientHdd.connected();

    // on connected state changed
    if(prevConnected != connected)
    {
        prevConnected = connected;

        if(!connected) {    // just lost the connection - remember when, to see how long the reconnect takes
            connLostAt = millis();
            connAttempts = 0;
            debug("connectToHost - CONNECTION LOST at %u ms\n", connLostAt);
            logSockets("connection lost");
        }
    }

    // socket connected? just quit
    if(connected)
    {
        udpSocketClose();
        return;
    }

    // do discovery if needed
    if(hostIp[0] == 0 || hostPortHdd == 0)
    {
        udpSocketOpen();

        ceDiscoverySend();
        ceDiscoveryReceive();
    }

    // if got ip and port, then before connecting, close udp socket
    if(hostIp[0] != 0 && hostPortHdd != 0)
    {
        udpSocketClose();
    }

    // connect to CE server
    connectToCEhost();
}

bool getIncommingHeader(EthernetClient* client, uint32_t expectedSyncTag, THeader* header)
{
    if(!connected)    // client not connected, no header received
    {
        return false;
    }

    while(true)
    {
        // Determine how many bytes are needed to be received, if we want to get 10 bytes of header.
        // If we already got some bytes in the syncTag, we need less than 10 bytes.
        int needed = 10;
        if((header->syncTag & 0xffffff) == (expectedSyncTag >> 8))      // got c050d1 (3 bytes) already? need only 7 more
        {
            needed = 7;
        }
        else if((header->syncTag & 0xffff) == (expectedSyncTag >> 16))  // got c050 (2 bytes) already? need only 8 more
        {
            needed = 8;
        }
        else if((header->syncTag & 0xff) == (expectedSyncTag >> 24))    // got c0 (1 bytes) already? need only 9 more
        {
            needed = 9;
        }
        else                // in other cases, expect to have all 10 bytes available before trying to read header
        {
            needed = 10;
        }

        // not enough data for full header, no header received
        int available = client->available();
        if(available < needed)
        {
            return false;
        }

        uint32_t data = ((uint8_t) client->read()); // read byte
        header->syncTag = header->syncTag << 8;     // shift previous sync tag one byte up
        header->syncTag |= data;                    // add lowest byte to syncTag

        if(header->syncTag == expectedSyncTag)      // found expected syncTag
        {
            uint8_t rest[6];
            client->read(rest, 6);               // read rest of header
            header->cmdCode = getWord(rest);
            header->len = getDword(rest + 2);

            return true;                        // got complete header
        }
    }

    return false;       // no valid header
}

void handleAcsiConfig(uint32_t len)
{
    uint8_t data[32];
    memset(data, 0, 32);
    uint32_t readLen = MIN(32, len);        // limit read length to buffer length

    clientHdd.read(data, readLen);          // read data

    for(uint32_t i=readLen; i<len; i++)     // drop the rest of data which doesn't fit in the buffer, so the next header can be found
    {
        clientHdd.read();
    }

    for(int i=0; (i + 1) < readLen; i++)    // go through all the received data, the config byte must be followed by the value byte
    {
        if(data[i] == CMD_ACSI_CONFIG) {
            uint8_t newAcsiIds = data[i + 1];
            i++;    // move 1 more byte forward, as we've read it

            // check if new config different from previous, then write settings
            if(settings.enabledIDs != newAcsiIds) {
                settings.enabledIDs = newAcsiIds;

                debug("handleAcsiConfig - storing new ids: %02X\n", newAcsiIds);

                saveSettings();
            }
        }
    }
}

void handleSendStatus(void)
{
    uint8_t status = clientHdd.read();

    IPCbuffer* bfr = ipcGetFreeBuffer(1, CMD_TIMEOUT_SHORT);
    if(bfr) {
        ipcSetBufferAndPutToFifo(bfr, 1, STATE_READ_STATUS, 1, &status, 1);
    } else {
        debug("handleSendStatus - ipcGetFreeBuffer failed\n");
    }
}

void handleReadStart(bool withStatus)
{
    uint8_t data[4];
    memset(data, 0, 4);
    clientHdd.read(data, 4);

    c0.sendStatusAfterRead = withStatus;
    c0.statusByte = data[3];
}

void handleWriteStart(void)
{
    uint8_t data[4];
    memset(data, 0, 4);
    clientHdd.read(data, 4);

    IPCbuffer* bfr = ipcGetFreeBuffer(1, CMD_TIMEOUT_SHORT);
    if(bfr) {
        ipcSetBufferAndPutToFifo(bfr, 1, STATE_DATA_WRITE, get24bits(data), &data[3], 1);
    } else {
        debug("handleWriteStart - ipcGetFreeBuffer failed\n");
    }
}

// ACSI only (SLM printer) - send bytes to ST with CS / INT handshake; no bytes = just end the command
void handlePioRead(uint32_t len)
{
    uint8_t data[PAYLOAD_MAX_WAIT_SIZE];
    uint32_t readLen = MIN(len, PAYLOAD_MAX_WAIT_SIZE);

    if(readLen > 0) {
        clientHdd.read(data, readLen);
    }

    for(uint32_t i=readLen; i<len; i++) {   // drop what doesn't fit (host doesn't send more), so the next header can be found
        clientHdd.read();
    }

    IPCbuffer* bfr = ipcGetFreeBuffer(1, CMD_TIMEOUT_SHORT);
    if(bfr) {
        ipcSetBufferAndPutToFifo(bfr, 1, STATE_PIO_READ, readLen, data, readLen);
    } else {
        debug("handlePioRead - ipcGetFreeBuffer failed\n");
    }
}

// ACSI only (SLM printer) - get bytes from ST with CS / INT handshake
void handlePioWriteStart(void)
{
    uint8_t data[4];
    memset(data, 0, 4);
    clientHdd.read(data, 4);

    IPCbuffer* bfr = ipcGetFreeBuffer(1, CMD_TIMEOUT_SHORT);
    if(bfr) {
        ipcSetBufferAndPutToFifo(bfr, 1, STATE_PIO_WRITE, get24bits(data), NULL, 0);
    } else {
        debug("handlePioWriteStart - ipcGetFreeBuffer failed\n");
    }
}

// ACSI only (SLM printer) - DMA write in bursts of printer scan lines, see onDataWritePaced()
void handleWritePacedStart(uint32_t len)
{
    uint8_t data[DATA_WRITE_PACED_PAYLOAD_SIZE];
    memset(data, 0, sizeof(data));
    uint32_t readLen = MIN(len, sizeof(data));
    clientHdd.read(data, readLen);

    for(uint32_t i=readLen; i<len; i++) {
        clientHdd.read();
    }

    IPCbuffer* bfr = ipcGetFreeBuffer(1, CMD_TIMEOUT_SHORT);
    if(bfr) {
        ipcSetBufferAndPutToFifo(bfr, 1, STATE_DATA_WRITE_PACED, sizeof(data), data, sizeof(data));
    } else {
        debug("handleWritePacedStart - ipcGetFreeBuffer failed\n");
    }
}

void handleReadDataReceived(void)
{
    uint32_t dataCnt = hddHeader.len;
    uint32_t start = millis();

    while(dataCnt > 0)
    {
        uint32_t now = millis();
        if(now - start > 5000) {
            debug("handleReadDataReceived - timeout!");
            break;
        }

        IPCbuffer* bfr = ipcGetFreeBuffer(1, CMD_TIMEOUT_SHORT);
        if(!bfr) {
            debug("handleReadDataReceived - ipcGetFreeBuffer failed\n");
            continue;
        }

        uint32_t cntNow = MIN(512, dataCnt);
        int actualCnt = 0;

        while(true)
        {
            now = millis();
            if(now - start > 5000) {
                debug("handleReadDataReceived - timeout!");
                break;
            }

            actualCnt = clientHdd.read(bfr->data, cntNow);   // try to read desired cntNow to buffer

            if(actualCnt > 0) {     // got something?
                break;
            }
        }

        if(actualCnt <= 0) {        // timeout, nothing read - return the buffer and quit
            ipcReleaseBuffer(bfr);
            break;
        }

        ipcSetBufferAndPutToFifo(bfr, 1, STATE_DATA_READ, actualCnt, NULL, 0);
        dataCnt -= actualCnt;                   // update total data needed to be read

        stingNetProcess();                      // long reads would let W6100 socket 0 buffer overflow meanwhile
    }

    // if we should end reading by sending status byte
    if(c0.sendStatusAfterRead)
    {
        IPCbuffer* bfr = ipcGetFreeBuffer(1, CMD_TIMEOUT_SHORT);
        if(bfr) {
            ipcSetBufferAndPutToFifo(bfr, 1, STATE_READ_STATUS, 1, &c0.statusByte, 1);
        } else {
            debug("handleReadDataReceived status - ipcGetFreeBuffer failed\n");
        }
    }
}

// how many payload bytes must be available before the command can be handled
uint32_t payloadSizeToWaitFor(const THeader* header)
{
    if(header->cmdCode == CMD_DATA_MARKER) {        // data of any size, handleReadDataReceived() reads it in parts as it comes
        return 0;
    }

    return MIN(header->len, PAYLOAD_MAX_WAIT_SIZE);
}

void handleIncommingData(void)
{
    if(!hddHeaderPending)
    {
        if(!getIncommingHeader(&clientHdd, SYNC_TAG_HDD, &hddHeader))   // no valid hdd header? quit
        {
            return;
        }

        hddHeader.syncTag = 0;      // clear sync tag
        hddHeaderPending = true;
        hddHeaderTime = millis();
    }

    // The handlers read the payload right away, so wait until all of it is here - TCP might deliver the header and the payload separately.
    // Keep returning meanwhile, so the rest of the main loop keeps running.
    if(clientHdd.available() < (int) payloadSizeToWaitFor(&hddHeader))
    {
        if((millis() - hddHeaderTime) > PAYLOAD_WAIT_MS)    // waiting too long? drop this header, look for the next one
        {
            debug("handleIncommingData - payload of cmd %d didn't arrive, dropping it\n", hddHeader.cmdCode);
            hddHeaderPending = false;
        }
        return;
    }

    hddHeaderPending = false;

    switch(hddHeader.cmdCode) {
        case CMD_ACSI_CONFIG: handleAcsiConfig(hddHeader.len); break;
        case CMD_DATA_READ_WITH_STATUS: handleReadStart(true); break;
        case CMD_DATA_READ_WITHOUT_STATUS: handleReadStart(false); break;
        case CMD_DATA_MARKER: handleReadDataReceived(); break;
        case CMD_DATA_WRITE: handleWriteStart(); break;
        case CMD_SEND_STATUS: handleSendStatus(); break;
        case CMD_PIO_READ: handlePioRead(hddHeader.len); break;
        case CMD_PIO_WRITE: handlePioWriteStart(); break;
        case CMD_DATA_WRITE_PACED: handleWritePacedStart(hddHeader.len); break;
        default: debug("unknown cmdCode %d\n", hddHeader.cmdCode); break;
    }
}

/*
    Send data as is to host using the desired socket.
    This just selects the right socket and sends the count of data specified in dataSizeBytes.
    @param whichSock SOCK_HDD or SOCK_FDD
    @param bfr Pointer to start of the data buffer
    @param dataSizeBytes Size of the data you want to send.
*/
bool sendDataToHost(uint8_t whichSock, uint8_t *bfr, uint32_t dataSizeBytes)
{
    EthernetClient* client = NULL;

    switch(whichSock)
    {
        case SOCK_HDD: client = &clientHdd; break;
        case SOCK_FDD: client = &clientFdd; break;
        case SOCK_IKBD: client = &clientIkbd; break;
        default: return false;
    }

    if(!connected)
    {
        return false;
    }

    uint32_t writtenCount = client->write(bfr, dataSizeBytes);
    return (writtenCount == dataSizeBytes);
}

/*
    Send header and data to host using the desired socket.
    This is just extension to sendDataToHost, because it also stores the data size in the header and sends the header, too.
    @param whichSock SOCK_HDD or SOCK_FDD
    @param bfr Pointer to start of the data buffer
    @param dataSizeBytes Size of the data portion after the header (header is TX_HEADER_SIZE bytes big) in bytes
*/
bool sendHeaderAndDataToHost(uint8_t whichSock, uint8_t *bfr, uint32_t dataSizeBytes)
{
    storeDword(bfr + 6, dataSizeBytes);     // store the tx length on index 6..9
    return sendDataToHost(whichSock, bfr, TX_HEADER_SIZE + dataSizeBytes);
}
