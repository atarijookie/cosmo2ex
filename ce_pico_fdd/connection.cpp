#include <WiFi.h>
#include <WiFiUdp.h>
#include <cstring>

#include "defs.h"
#include "connection.h"
#include "utils.h"
#include "settings.h"
#include "display.h"
#include "panel.h"
#include "beeper.h"
#include "ipc.h"
#include "host_receive.h"

#define SERVER_UDP_PORT 7200 // port number where CE listens for client requests
#define CLIENT_UDP_PORT 7201 // port where this client should listen for CE responses

#define CONNECT_TIMEOUT_MS  3000    // connect() to host waits at most this long - it blocks the core 0 main loop
#define WRITE_TIMEOUT_MS    500     // write() to host gives up after this long without progress
#define WIFI_JOIN_TIMEOUT_MS 5000   // joining the WiFi network waits at most this long - it blocks the core 0 main loop

WiFiUDP udp;
bool udpInitialized;

TConnection connectionFdd;

extern uint8_t mac[6];

uint8_t hostIp[4];
std::string hostIpString;
IPAddress hostIpAddr;
uint16_t hostPortHdd;
uint16_t hostPortFdd;
uint16_t hostPortIkbd;

bool connectedToWifi;
bool connectedToHost;

extern char imageFileName[32];
extern int imageState;

void storeMacAddress(void);

// image name big on top, host and wifi below
void showRunningStateOnDisplay(void)
{
    panelSetLine(0, PANEL_CENTER, PANEL_DOUBLE | PANEL_BOLD, imageFileName[0] ? imageFileName : "no image");
    panelSetLine(1, PANEL_LEFT, 0, "");

    char msg[64];
    snprintf(msg, sizeof(msg), "host: %s%s", hostIpString.c_str(), Settings.ikbdEnabled ? " I" : "");
    panelSetLine(2, PANEL_LEFT, 0, msg);

    snprintf(msg, sizeof(msg), "ssid: %s", Settings.ssid);
    panelSetLine(3, PANEL_LEFT, 0, msg);
}

void udpInitialize(void)
{
    if(udpInitialized) {    // already initialized, quit
        return;
    }

    if(!udp.begin(CLIENT_UDP_PORT)) {
        debug("udp.begin failed\n");
        return;
    }

    debug("UDP receiver listening on port %u\n", CLIENT_UDP_PORT);
    udpInitialized = true;
}

// Receive response from server if there is any and store it if it's valid.
// Serves also for dropping any additional udp packets.
void ceDiscoveryReceive(void)
{
    if(!udpInitialized) {
        return;
    }

    while(true)
    {
        int packetSize = udp.parsePacket();

        if(packetSize == 0) {   // no packet received? can stop trying to read it
            break;
        }

        if(packetSize < 10) {   // too short? (the next parsePacket() drops it)
            continue;
        }

        /*
            received packet structure:
            0..3    'CELR' string
            4..5    port for hdd
            6..7    port for fdd
            8..9    port for ikbd
        */
        uint8_t payload[10];
        udp.read(payload, 10);

        if(strncmp((const char *) payload, "CELR", 4) != 0) {     // not CELR at start? skip it
            continue;
        }

        hostIpAddr = udp.remoteIP();
        for(int i=0; i<4; i++) {
            hostIp[i] = hostIpAddr[i];
        }

        hostIpString = hostIpAddr.toString().c_str();

        // store ports and stop receiving
        hostPortHdd = getWord(payload + 4);
        hostPortFdd = getWord(payload + 6);
        hostPortIkbd = getWord(payload + 8);

        debug("ceDiscoveryReceive - got host ip: %s, ports: %d, %d, %d\n", hostIpString.c_str(), hostPortHdd, hostPortFdd, hostPortIkbd);
    }
}

static uint32_t wifiLastAttempt = 0xffff0000; // -65k

// Read wifi settings, connect to wifi if not connected, don't try too often.
void connectToWifi(void)
{
    // if last attempt was less than a moment ago, don't try
    if ((millis() - wifiLastAttempt) < 15000)
    {
        return;
    }

    // we're connecting now
    wifiLastAttempt = millis();

    char msg[128];

    // no wifi SSID stored? cannot connect
    if (strlen(Settings.ssid) == 0) {
        debug("connectToWifi - not connecting to WIFI yet, because no SSID stored\n");
        return;
    }

    // not connected to wifi yet, try to connect
    int status = WiFi.status();

    if (status != WL_CONNECTED) {           // not connected? connect now
        debug("connectToWifi - connecting, ssid: %s\n", Settings.ssid);

        sprintf(msg, "ssid: %s", Settings.ssid);
        displayMessage("wifi connecting", msg);

        // waits for joining the network (at most WIFI_JOIN_TIMEOUT_MS), not for the IP from DHCP - that comes later
        WiFi.setTimeout(WIFI_JOIN_TIMEOUT_MS);
        WiFi.beginNoBlock(Settings.ssid, Settings.password);
    }
    else
    {
        debug("connectToWifi - not connecting, status: %d\n", status);
    }

    storeMacAddress();      // copy wifi mac address to fw report buffer
}

// Send broadcast to find any CE server on the network.
void ceDiscoverySend(void)
{
    static uint32_t lastAttempt = 0xffff0000; // -65k
    static bool whichBroadcastAddr = false;

    // already got hostIp and port? don't do discovery
    if (hostIp[0] != 0 && hostPortFdd != 0)
    {
        return;
    }

    // if last attempt was less than a moment ago, don't try
    if ((millis() - lastAttempt) < 1000)
    {
        return;
    }
    lastAttempt = millis();

    // get ip, mask, create broadcast address
    IPAddress ip = WiFi.localIP();
    IPAddress mask = WiFi.subnetMask();

    if(ip[0] == 0 && ip[1] == 0 && ip[2] == 0 && ip[3] == 0) {     // no ip? don't send broadcast
        debug("ceDiscoverySend - no IP yet\n");
        return;
    }

    udpInitialize();

    displayMessage("wifi connected", "CE host discovery");

    uint8_t packet[10];
    memcpy(packet, "CELC", 4);      // 0..3 - CELC
    memcpy(packet + 4, mac, 6);     // 4..9 - mac

    whichBroadcastAddr = !whichBroadcastAddr;   // toggle this flag

    IPAddress bcast(255, 255, 255, 255);        // generic broadcast addr...

    if(whichBroadcastAddr)                      // ...or subnet broadcast addr (e.g. 192.168.1.255)
    {
        for(int i=0; i<4; i++) {
            bcast[i] = ip[i] | ~mask[i];
        }
    }

    debug("ceDiscoverySend to %s\n", bcast.toString().c_str());

    udp.beginPacket(bcast, SERVER_UDP_PORT);
    udp.write(packet, sizeof(packet));
    udp.endPacket();
}

size_t conWrite(TConnection* con, const uint8_t *buf, size_t size)
{
    if (!size) {
        return 0;
    }

    return con->write(buf, size);       // waits at most WRITE_TIMEOUT_MS for progress, set on connect
}

size_t conRead(TConnection* con, uint8_t *buf, size_t size)
{
    if (!size || connectionCanReadBytes(con) == 0) {    // WiFiClient::read() can't be called without a connection
        return 0;
    }

    int got = con->read(buf, size);
    return (got > 0) ? got : 0;
}

bool isConnected(TConnection* con)
{
    return con->connected();
}

size_t connectionCanReadBytes(TConnection* con)
{
    int available = con->available();
    return (available > 0) ? available : 0;
}

void stop(TConnection* con)
{
    con->stop();        // waits for the sent data to be acked (limited time), then closes
}

void connectToCEhost(void)
{
    static uint32_t lastAttempt = 0xffff0000; // -65k
    static bool loggedOnce = false;

    if (isConnected(&connectionFdd))   // already connected? quit
    {
        if(!loggedOnce) {
            debug("connectToCEhost - connected!\n");
            loggedOnce = true;
        }

        return;
    }
    loggedOnce = false;

    // if last attempt was less than a moment ago, don't try
    if ((millis() - lastAttempt) < 3000)
    {
        return;
    }

    // we're connecting now
    lastAttempt = millis();

    // no host IP? not connecting
    if (hostIpString.length() == 0)
    {
        return;
    }

    displayMessage("wifi connected", "connecting to host:", hostIpString.c_str());
    debug("connectToCEhost - IP: %s, port: %d\n", hostIpString.c_str(), hostPortFdd);

    connect(&connectionFdd, hostIpAddr, hostPortFdd);
}

// Connect to host - waits at most CONNECT_TIMEOUT_MS. Check the result with isConnected().
void connect(TConnection* con, const IPAddress& addr, uint16_t port)
{
    con->stop();                        // close the previous connection, if still open

    con->setTimeout(CONNECT_TIMEOUT_MS);
    if(!con->connect(addr, port)) {
        return;
    }

    con->setTimeout(WRITE_TIMEOUT_MS);  // for write() from now on
    con->setSync(true);                 // write() returns once the data is acked
    con->setNoDelay(true);
}

void connectToHost(void)
{
    static bool prevConnectedToHost = false;
    static bool prevConnectedToWifi = false;

    connectedToWifi = WiFi.status() == WL_CONNECTED;
    connectedToHost = connectedToWifi && isConnected(&connectionFdd);

    if(prevConnectedToWifi != connectedToWifi) {    // connectedToWifi changed since last time? log it
        prevConnectedToWifi = connectedToWifi;
        debug("connectedToWifi: %d\n", connectedToWifi);
    }

    // on connected state changed
    if(prevConnectedToHost != connectedToHost)
    {
        prevConnectedToHost = connectedToHost;

        if(connectedToHost) {     // now in connected state, display state on display
            imageState = IMAGE_NOT_LOADED;
            showRunningStateOnDisplay();
            beep(BEEP_HOST_CONNECTED_MS);
        } else {
            beep(BEEP_HOST_LOST_MS);
        }
    }

    if(!connectedToWifi) {
        connectToWifi();
    }

    // responses to discovery, other udp packets dropped
    ceDiscoveryReceive();

    if(connectedToHost)     // we're connected to host? don't send discovery packets
    {
        return;
    }

    // device connected to wifi, do discovery if needed
    ceDiscoverySend();

    // connect to CE server
    connectToCEhost();
}

// Close the host connection and forget the host - before the WiFi goes to AP mode (captive portal).
void connectionStop(void)
{
    stop(&connectionFdd);
    connectedToHost = false;

    memset(hostIp, 0, sizeof(hostIp));      // maybe another network after this - discover the host again
    hostPortFdd = 0;
    hostIpString.clear();
}

// Start over with the stored settings: connect to WiFi on the next connectToHost(), don't wait for the retry time.
void connectionRestart(void)
{
    connectionStop();
    wifiLastAttempt = millis() - 15000;
}

// fdd connection to host, for host_receive.cpp

bool hostConnected(void)
{
    return isConnected(&connectionFdd);
}

size_t hostCanReadBytes(void)
{
    return connectionCanReadBytes(&connectionFdd);
}

size_t hostRead(uint8_t* bfr, size_t size)
{
    return conRead(&connectionFdd, bfr, size);
}

void hostDisconnect(void)
{
    stop(&connectionFdd);
}

/*
    Send data as is to host using the desired socket.
    This just selects the right socket and sends the count of data specified in dataSizeBytes.
    If only a part of the data was sent (write timeout), the connection is closed - the host would take the start of
    whatever is sent next as the rest of this message. So on false nothing of the data reached the host on a working
    connection, and the caller can send all of it again later (after reconnect).
    @param bfr Pointer to start of the data buffer
    @param dataSizeBytes Size of the data you want to send.
*/
bool sendDataToHost(uint8_t *bfr, uint32_t dataSizeBytes)
{
    if(!isConnected(&connectionFdd))
    {
        return false;
    }

    uint32_t writtenCount = conWrite(&connectionFdd, bfr, dataSizeBytes);

    if(writtenCount == dataSizeBytes) {
        return true;
    }

    if(writtenCount > 0) {      // part of the message sent? the host can't find the start of the next one, start over
        debug("sendDataToHost - sent only %d of %d bytes, disconnecting\n", writtenCount, dataSizeBytes);
        stop(&connectionFdd);
    }

    return false;
}

/*
    Send header and data to host using the desired socket.
    This is just extension to sendDataToHost, because it also stores the data size in the header and sends the header, too.
    @param bfr Pointer to start of the data buffer
    @param dataSizeBytes Size of the data portion after the header (header is TX_HEADER_SIZE bytes big) in bytes
*/
bool sendHeaderAndDataToHost(uint8_t *bfr, uint32_t dataSizeBytes)
{
    storeDword(bfr + 6, dataSizeBytes);     // store the tx length on index 6..9
    return sendDataToHost(bfr, TX_HEADER_SIZE + dataSizeBytes);
}
