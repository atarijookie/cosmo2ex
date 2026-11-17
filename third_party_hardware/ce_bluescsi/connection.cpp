// Connection to the CE host - the same protocol as ce_pico_hdd/connection.cpp, over the network from net.cpp.

#include "defs.h"
#include "connection.h"
#include "utils.h"
#include "ipc.h"
#include "net.h"
#include "led.h"
#include "fw_update.h"
#include "boot_status.h"
#include "sting_net.h"

#define SERVER_UDP_PORT 7200 // port number where CE listens for client requests
#define CLIENT_UDP_PORT 7201 // port where this client should listen for CE responses

static Client* clientHdd;   // from net.cpp
static UDP* udp;
static bool udpInitialized;

uint8_t hostIp[4];
static IPAddress hostAddr;
static uint16_t hostPortHdd;

volatile bool connected = false;

extern uint8_t atnSendFwVersion[];

struct {
    bool sendStatusAfterRead;
    uint8_t statusByte;
} c0;

static THeader hddHeader;       // keep the header global to preserve syncTag between calls
static bool hddHeaderPending;   // hddHeader received, waiting for its payload to arrive
static uint32_t hddHeaderTime;  // when hddHeader was received

#define PAYLOAD_WAIT_MS             1000    // drop the received header if its payload doesn't arrive within this time
#define PAYLOAD_MAX_WAIT_SIZE       64      // wait for at most this many payload bytes - all the commands except CMD_DATA_MARKER are smaller

#define CONNECT_FAILS_BEFORE_REDISCOVERY 5  // after this many failed connects forget the host and discover it again (it might have new IP / port)
#define CONNECT_RETRY_MS                1000    // connect attempt this often
#define DISCOVERY_LOG_EVERY             10      // log only every Nth discovery broadcast (1 per second), not to flood the log

static uint8_t connectFailCount;                    // count of failed connection attempts in a row
static uint32_t connLostAt;                         // millis() when the connection to host was lost (or boot)
static uint32_t connAttempts;                       // connect attempts since connLostAt
static uint32_t connLastAttempt = 0xffff0000;       // millis() of the last connect attempt, -65k: first one right away
static uint32_t discoverySendCount;                 // broadcasts sent in this discovery round, for throttled logging

void connectionInit(void)
{
    clientHdd = &netHddClient();
    udp = &netUdp();
}

// how long ago the connection was lost, for logs
static uint32_t msSinceLost(void)
{
    return millis() - connLostAt;
}

static bool hostKnown(void)
{
    return hostIp[0] != 0 && hostPortHdd != 0;
}

static void forgetHost(void)
{
    memset(hostIp, 0, sizeof(hostIp));
    hostPortHdd = 0;
}

// Send broadcast to find any CE server on the network.
static void ceDiscoverySend(void)
{
    static uint32_t lastAttempt = 0xffff0000; // -65k
    static bool whichBroadcastAddr = false;

    if (hostKnown() || !udpInitialized)
    {
        return;
    }

    // if last attempt was less than a moment ago, don't try
    if ((millis() - lastAttempt) < 1000)
    {
        return;
    }

    lastAttempt = millis();

    uint8_t updPacket[10];
    memcpy(updPacket, "CELC", 4);                               // 0..3 - CELC
    memcpy(updPacket + 4, atnSendFwVersion + TX_HEADER_SIZE + 6, 6);    // 4..9 - mac

    whichBroadcastAddr = !whichBroadcastAddr;   // toggle this flag
    IPAddress bcast(255, 255, 255, 255);        // generic broadcast addr...

    if(whichBroadcastAddr)                      // ...or subnet broadcast addr (e.g. 192.168.1.255)
    {
        IPAddress ip = netLocalIP();
        IPAddress mask = netSubnetMask();

        for(int i=0; i<4; i++) {
            bcast[i] = ip[i] | (uint8_t) ~mask[i];
        }
    }

    int okBegin = udp->beginPacket(bcast, SERVER_UDP_PORT);
    udp->write(updPacket, 10);
    int okEnd = udp->endPacket();

    // log the first one, then every Nth, and any failure
    bool failed = !okBegin || !okEnd;
    if((discoverySendCount % DISCOVERY_LOG_EVERY) == 0 || failed) {
        debug("ceDiscoverySend #%u to %s - begin: %d, end: %d, %u ms since connection lost\n",
              discoverySendCount + 1, bcast.toString().c_str(), okBegin, okEnd, msSinceLost());
    }
    discoverySendCount++;
}

// Receive response from server if there is any and store it if it's valid.
// Serves also for dropping any additional udp packets.
static void ceDiscoveryReceive(void)
{
    static uint32_t lastAttempt = 0xffff0000; // -65k

    // if last attempt was less than a moment ago, don't try
    if ((millis() - lastAttempt) < 100 || !udpInitialized)
    {
        return;
    }

    lastAttempt = millis();

    // check if any udp packet was received, handle it
    while (true)
    {
        int packetSize = udp->parsePacket();

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
        udp->read(buffer, 10);

        // start of the data isn't CELR? skip the rest
        if (strncmp((const char *)buffer, "CELR", 4) != 0)
        {
            debug("ceDiscoveryReceive - ignoring %d bytes packet from %s\n", packetSize, udp->remoteIP().toString().c_str());
            continue;
        }

        // store server's ip address and port
        hostAddr = udp->remoteIP();
        for (int i = 0; i < 4; i++)
        {
            hostIp[i] = hostAddr[i];
        }
        hostPortHdd = getWord(buffer + 4);

        debug("ceDiscoveryReceive - got host ip: %s, hdd port: %d, %u ms since connection lost\n", hostAddr.toString().c_str(),
              hostPortHdd, msSinceLost());

        connLastAttempt = millis() - CONNECT_RETRY_MS;  // host just answered - connect right away, don't wait for the retry time
        discoverySendCount = 0;                         // next discovery round logs its first broadcast again
    }
}

static void connectToCEhost(void)
{
    // if last attempt was less than a moment ago, don't try
    if ((millis() - connLastAttempt) < CONNECT_RETRY_MS)
    {
        return;
    }

    // we're connecting now
    connLastAttempt = millis();

    // no host IP? not connecting
    if (!hostKnown())
    {
        return;
    }

    connAttempts++;
    debug("connectToCEhost - IP: %s, port: %d, attempt #%u, %u ms since connection lost\n",
          hostAddr.toString().c_str(), hostPortHdd, connAttempts, msSinceLost());

    // start connection attempt
    clientHdd->stop();                                      // close socket if still open
    netBeforeConnect();
    uint32_t start = millis();
    connected = clientHdd->connect(hostAddr, hostPortHdd);

    debug("connectToCEhost - connected: %d (connect took %u ms)\n", connected, millis() - start);

    if(connected)
    {
        netAfterConnect();

        debug("connectToCEhost - RECONNECTED %u ms after connection lost, %u attempts\n", msSinceLost(), connAttempts);
        connectFailCount = 0;

        hddHeader.syncTag = 0;          // new stream - don't use anything left from the previous connection
        hddHeaderPending = false;
        fwInfoResetSent();              // the host gets the firmware info again on this connection
    }
    else if(++connectFailCount >= CONNECT_FAILS_BEFORE_REDISCOVERY)
    {
        debug("connectToCEhost - %d failed attempts, discovering host again (%u ms since connection lost)\n", connectFailCount, msSinceLost());
        connectFailCount = 0;
        forgetHost();
    }
}

// if UDP not initialized, do that now
static void udpSocketOpen(void)
{
    static uint32_t lastFailLog = 0;

    if (!udpInitialized)
    {
        udpInitialized = udp->begin(CLIENT_UDP_PORT);

        if(udpInitialized) {
            debug("udpSocketOpen - ok, %u ms since connection lost\n", msSinceLost());
        } else if(millis() - lastFailLog >= 1000) {
            lastFailLog = millis();
            debug("udpSocketOpen - FAILED\n");
        }
    }
}

// if UDP is initialized, deinitialize
static void udpSocketClose(void)
{
    if(udpInitialized)
    {
        udp->stop();
        udpInitialized = false;
    }
}

// boot status + LED while not connected to the host
static void showNetState(void)
{
    switch(netState())
    {
        case NETST_JOINING:
            ledSetInitStep(LED_STEP_NETWORK);
            bootStatusSet(BOOTST_NET_JOINING);
            break;

        case NETST_JOIN_FAILED:
            ledSetError(LED_ERR_NO_LINK);
            bootStatusSet(BOOTST_NET_JOIN_FAILED);
            break;

        case NETST_WAIT_IP:
            ledSetInitStep(LED_STEP_NETWORK);
            bootStatusSet(BOOTST_WAIT_IP);
            break;

        case NETST_NO_LINK:
            ledSetError(LED_ERR_NO_LINK);
            bootStatusSet(BOOTST_NET_NO_LINK);
            break;

        case NETST_NO_DHCP:
            ledSetError(LED_ERR_NO_DHCP);
            bootStatusSet(BOOTST_NET_NO_DHCP);
            break;

        default:
            break;
    }
}

void connectToHost(void)
{
    static bool prevConnected = false;
    static IPAddress lastIp;

    netProcess();

    if(!netReady()) {                       // no network - nothing to connect through
        if(connected || prevConnected) {
            debug("connectToHost - network lost\n");
            netLogLinkInfo();
            clientHdd->stop();
        }
        connected = false;
        prevConnected = false;
        udpSocketClose();
        showNetState();
        return;
    }

    IPAddress ip = netLocalIP();
    bool ipValid = (ip[0] | ip[1] | ip[2] | ip[3]) != 0;    // 0.0.0.0 during a short WiFi drop - not a new IP
    if(ipValid && ip != lastIp) {                      // new IP (DHCP) - the old connection is dead
        debug("connectToHost - own IP: %s\n", ip.toString().c_str());

        if(lastIp != IPAddress(0, 0, 0, 0) && connected) {
            clientHdd->stop();
        }

        lastIp = ip;
        uint8_t ipBytes[4] = {ip[0], ip[1], ip[2], ip[3]};
        bootStatusSetOwnIp(ipBytes);
    }

    connected = clientHdd->connected();

    // on connected state changed
    if(prevConnected != connected)
    {
        prevConnected = connected;

        if(!connected) {    // just lost the connection - remember when, to see how long the reconnect takes
            connLostAt = millis();
            connAttempts = 0;
            debug("connectToHost - CONNECTION LOST at %u ms\n", connLostAt);
            netLogLinkInfo();
            fwUpdateAbort("connection lost");
        }
    }

    // socket connected? just quit
    if(connected)
    {
        udpSocketClose();
        return;
    }

    ledSetInitStep(LED_STEP_CONNECTING);

    // do discovery if needed
    if(!hostKnown())
    {
        udpSocketOpen();

        ceDiscoverySend();
        ceDiscoveryReceive();
    }
    else
    {
        udpSocketClose();       // got ip and port, close udp socket before connecting
    }

    // connect to CE server
    connectToCEhost();

    if(!connected) {    // for the boot status sector - shown while not connected
        bootStatusSet(hostKnown() ? BOOTST_CONNECTING : BOOTST_SEARCHING, hostKnown() ? hostIp : NULL);
    }
}

static bool getIncommingHeader(Client* client, uint32_t expectedSyncTag, THeader* header)
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
            client->read(rest, 6);                  // read rest of header
            header->cmdCode = getWord(rest);
            header->len = getDword(rest + 2);

            return true;                            // got complete header
        }
    }

    return false;       // no valid header
}

static void dropBytes(uint32_t count)
{
    for(uint32_t i=0; i<count; i++) {
        clientHdd->read();
    }
}

static void handleAcsiConfig(uint32_t len)
{
    uint8_t data[32];
    memset(data, 0, 32);
    uint32_t readLen = MIN(32, len);        // limit read length to buffer length

    clientHdd->read(data, readLen);         // read data
    dropBytes(len - readLen);               // drop the rest of data which doesn't fit in the buffer, so the next header can be found

    for(uint32_t i=0; (i + 1) < readLen; i++)   // go through all the received data, the config byte must be followed by the value byte
    {
        if(data[i] == CMD_ACSI_CONFIG) {
            uint8_t newIds = data[i + 1];
            i++;    // move 1 more byte forward, as we've read it

            if(newIds == 0) {               // no ID - the drive would never answer
                debug("handleAcsiConfig - no ID enabled, ignored\n");
                continue;
            }

            // check if new config different from previous, then write settings
            if(settings.enabledIDs != newIds) {
                settings.enabledIDs = newIds;

                debug("handleAcsiConfig - storing new ids: %02X\n", newIds);

                saveSettings();
            }
        }
    }
}

static void handleSendStatus(void)
{
    uint8_t status = clientHdd->read();

    IPCbuffer* bfr = ipcGetFreeBuffer(1, CMD_TIMEOUT_SHORT);
    if(bfr) {
        ipcSetBufferAndPutToFifo(bfr, 1, STATE_READ_STATUS, 1, &status, 1);
    } else {
        debug("handleSendStatus - ipcGetFreeBuffer failed\n");
    }
}

static void handleReadStart(bool withStatus)
{
    uint8_t data[4];
    memset(data, 0, 4);
    clientHdd->read(data, 4);

    c0.sendStatusAfterRead = withStatus;
    c0.statusByte = data[3];
}

static void handleWriteStart(void)
{
    uint8_t data[4];
    memset(data, 0, 4);
    clientHdd->read(data, 4);

    IPCbuffer* bfr = ipcGetFreeBuffer(1, CMD_TIMEOUT_SHORT);
    if(bfr) {
        ipcSetBufferAndPutToFifo(bfr, 1, STATE_DATA_WRITE, get24bits(data), &data[3], 1);
    } else {
        debug("handleWriteStart - ipcGetFreeBuffer failed\n");
    }
}

#define READ_DATA_STALL_MS      5000    // no byte of the read data from the host / no free buffer for this long - give up

// The data for a READ from the host. All of it must be taken out of the TCP stream, otherwise the rest would be parsed as
// headers. If core 1 gave up on this command (bus timeout), it drops the buffers - so they keep coming back free.
// Gives up only when nothing moves for READ_DATA_STALL_MS, and then closes the connection: the stream is out of sync, the
// reconnect starts it clean.
static void handleReadDataReceived(void)
{
    uint32_t dataCnt = hddHeader.len;
    uint32_t start = millis();
    uint32_t lastProgress = start;
    uint32_t maxGap = 0;
    uint32_t noBufferCount = 0;
    bool stalled = false;

    while(dataCnt > 0)
    {
        IPCbuffer* bfr = ipcGetFreeBuffer(1, CMD_TIMEOUT_SHORT);
        if(!bfr) {
            noBufferCount++;

            if(millis() - lastProgress > READ_DATA_STALL_MS) {
                stalled = true;
                break;
            }
            continue;
        }

        uint32_t cntNow = MIN(IPC_BUFFER_SIZE, dataCnt);
        int actualCnt = 0;

        while(true)
        {
            if(clientHdd->available() > 0) {                // WiFiClient::read() without data is not a good idea
                actualCnt = clientHdd->read(bfr->data, cntNow);   // try to read desired cntNow to buffer
            }

            if(actualCnt > 0) {     // got something?
                break;
            }

            if(!clientHdd->connected() || (millis() - lastProgress > READ_DATA_STALL_MS)) {
                stalled = true;
                break;
            }
        }

        if(actualCnt <= 0) {        // nothing read - return the buffer and quit
            ipcReleaseBuffer(bfr);
            break;
        }

        uint32_t now = millis();
        maxGap = MAX(maxGap, now - lastProgress);
        lastProgress = now;

        ipcSetBufferAndPutToFifo(bfr, 1, STATE_DATA_READ, actualCnt, NULL, 0);
        dataCnt -= actualCnt;                   // update total data needed to be read

        stingNetProcess();                      // long reads would let W6100 socket 0 (MACRAW) buffer overflow meanwhile
    }

    uint32_t took = millis() - start;

    // slow over WiFi? The TT gives the whole data phase 3 s (SCSI_TIMEOUT_LONG in libacsiscsi), the bridge 1 s per chunk
    if(dataCnt > 0 || maxGap >= 200 || took >= 1000 || noBufferCount > 0) {
        debug("readData - %u of %u B in %u ms, max gap %u ms, no free buffer %u x%s\n", hddHeader.len - dataCnt, hddHeader.len,
              took, maxGap, noBufferCount, stalled ? " - STALLED" : "");
    }

    if(dataCnt > 0) {               // rest of the data still in the stream - can't find the next header in it reliably
        debug("readData - dropping the connection to get the stream in sync\n");
        clientHdd->stop();
        return;                     // no status - the bridge gave up / gives up on this command anyway
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
static uint32_t payloadSizeToWaitFor(const THeader* header)
{
    if(header->cmdCode == CMD_DATA_MARKER) {        // data of any size, handleReadDataReceived() reads it in parts as it comes
        return 0;
    }

    if(fwUpdateIsCommand(header->cmdCode)) {        // firmware data chunks are bigger, fwUpdateCommand() reads them as they come
        return fwUpdatePayloadToWaitFor(header->cmdCode, header->len);
    }

    return MIN(header->len, PAYLOAD_MAX_WAIT_SIZE);
}

void handleIncommingData(void)
{
    if(!connected) {
        return;
    }

    if(!hddHeaderPending)
    {
        if(!getIncommingHeader(clientHdd, SYNC_TAG_HDD, &hddHeader))   // no valid hdd header? quit
        {
            return;
        }

        hddHeader.syncTag = 0;      // clear sync tag
        hddHeaderPending = true;
        hddHeaderTime = millis();
    }

    // The handlers read the payload right away, so wait until all of it is here - TCP might deliver the header and the payload separately.
    // Keep returning meanwhile, so the rest of the main loop keeps running.
    if(clientHdd->available() < (int) payloadSizeToWaitFor(&hddHeader))
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
        case CMD_LED_COLORS: dropBytes(hddHeader.len); break;       // no neopixel on this board
        case CMD_DATA_READ_WITH_STATUS: handleReadStart(true); break;
        case CMD_DATA_READ_WITHOUT_STATUS: handleReadStart(false); break;
        case CMD_DATA_MARKER: handleReadDataReceived(); break;
        case CMD_DATA_WRITE: handleWriteStart(); break;
        case CMD_SEND_STATUS: handleSendStatus(); break;
        case CMD_FW_UPDATE_START:
        case CMD_FW_UPDATE_DATA:
        case CMD_FW_UPDATE_END:
        case CMD_FW_UPDATE_ABORT: fwUpdateCommand(hddHeader.cmdCode, hddHeader.len); break;
        default:
            debug("unknown cmdCode %d, dropping %u bytes\n", hddHeader.cmdCode, MIN(hddHeader.len, PAYLOAD_MAX_WAIT_SIZE));
            dropBytes(MIN(hddHeader.len, PAYLOAD_MAX_WAIT_SIZE));
            break;
    }
}

// Send data as is to host.
bool sendDataToHost(uint8_t *bfr, uint32_t dataSizeBytes)
{
    if(!connected)
    {
        return false;
    }

    uint32_t writtenCount = clientHdd->write(bfr, dataSizeBytes);
    return (writtenCount == dataSizeBytes);
}

// Send header and data to host - also stores the data size in the header (header is TX_HEADER_SIZE bytes big).
bool sendHeaderAndDataToHost(uint8_t *bfr, uint32_t dataSizeBytes)
{
    storeDword(bfr + 6, dataSizeBytes);     // store the tx length on index 6..9
    return sendDataToHost(bfr, TX_HEADER_SIZE + dataSizeBytes);
}
