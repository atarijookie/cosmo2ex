#include <stdio.h>
#include <string.h>

#include "pico/cyw43_arch.h"
#include "lwip/netif.h"
#include "lwip/ip4_addr.h"
#include "lwip/tcp.h "

#include "pico/stdlib.h"
#include "lwip/sockets.h"
#include "lwip/inet.h"

#include "defs.h"
#include "connection.h"
#include "utils.h"
#include "display.h"
#include "psram.h"
#include "client_context.h"
#include "utils.h"
#include "ipc.h"
#include "host_receive.h"

#define SERVER_UDP_PORT 7200 // port number where CE listens for client requests
#define CLIENT_UDP_PORT 7201 // port where this client should listen for CE responses

extern Settings_t Settings;

bool udpInitialized;

struct udp_pcb* pcbUpd;

TConnection connectionFdd;

extern uint8_t mac[6];

uint8_t hostIp[4];
std::string hostIpString;
ip_addr_t hostIpAddr;
uint16_t hostPortHdd;
uint16_t hostPortFdd;
uint16_t hostPortIkbd;

bool connectedToWifi;
bool connectedToHost;

extern char imageFileName[32];
extern int imageState;

extern TDrivePosition posStreamed, hwPosition;

void storeMacAddress(void);

void showRunningStateOnDisplay(void)
{
    char msg1[128];
    sprintf(msg1, "ssid: %s", Settings.ssid);

    char msg2[64];
    sprintf(msg2, "host: %s %c", hostIpString.c_str(), Settings.ikbdEnabled ? 'I' : ' ');

    char msg3[64];
    sprintf(msg3, "image: %s", imageFileName);

    displayMessage(msg1, msg2, msg3);
}

// Callback for incoming UDP packets
static void udp_recv_callback(void *arg, struct udp_pcb *pcb, struct pbuf *p, const ip_addr_t *addr, u16_t port)
{
    if (p == NULL) {        // no buffer?
        return;
    }

    if(p->len < 10) {       // too short?
        pbuf_free(p);       // we own the received buffer, free it on every return
        return;
    }

    uint8_t* payload = (uint8_t*) p->payload;

    /*
        received packet structure:
        0..3    'CELR' string
        4..5    port for hdd
        6..7    port for fdd
        8..9    port for ikbd
    */

    // CELR found at start
    if (strncmp((const char *) payload, "CELR", 4) == 0)
    {
        hostIpAddr = *addr;
        uint32_t sender_ip = lwip_ntohl(ip4_addr_get_u32(ip_2_ip4(addr)));

        int shift = 24;
        for(int i=0; i<4; i++) {
            hostIp[i] = sender_ip >> shift;
            shift -= 8;
        }

        hostIpString = ip4addr_ntoa(ip_2_ip4(addr));

        // store ports and stop receiving
        hostPortHdd = getWord(payload + 4);
        hostPortFdd = getWord(payload + 6);
        hostPortIkbd = getWord(payload + 8);

        debug("ceDiscoveryReceive - got host ip: %s, ports: %d, %d, %d\n", hostIpString.c_str(), hostPortHdd, hostPortFdd, hostPortIkbd);
    }

    // Free the buffer after processing
    pbuf_free(p);
}

void udpInitialize(void)
{
    if(udpInitialized) {    // already initialized, quit
        return;
    }

    pcbUpd = udp_new();

    // Bind to any IP, given port
    err_t err = udp_bind(pcbUpd, IP_ADDR_ANY, CLIENT_UDP_PORT);
    if (err != ERR_OK) {
        debug("udp_bind failed: %d\n", err);
        udp_remove(pcbUpd);
        return;
    }

    // Register receive callback
    udp_recv(pcbUpd, udp_recv_callback, NULL);
    debug("UDP receiver listening on port %u\n", CLIENT_UDP_PORT);

    udpInitialized = true;
}

// Read wifi settings, connect to wifi if not connected, don't try too often.
void connectToWifi(void)
{
    static uint32_t lastAttempt = 0xffff0000; // -65k

    // if last attempt was less than a moment ago, don't try
    if ((millis() - lastAttempt) < 15000)
    {
        return;
    }

    // we're connecting now
    lastAttempt = millis();

    char msg[128];

    // no wifi SSID stored? cannot connect
    if (strlen(Settings.ssid) == 0) {
        debug("connectToWifi - not connecting to WIFI yet, because no SSID stored\n");
        return;
    }

    // not connected to wifi yet, try to connect
    int new_status = cyw43_tcpip_link_status(&cyw43_state, CYW43_ITF_STA);

    if (new_status != CYW43_LINK_UP) {      // not connected? connect now
        debug("connectToWifi - connecting, ssid: %s, password: %s\n", Settings.ssid, Settings.password);

        sprintf(msg, "ssid: %s", Settings.ssid);
        displayMessage("wifi connecting", msg);

        cyw43_arch_wifi_connect_async(Settings.ssid, Settings.password, CYW43_AUTH_WPA2_AES_PSK);
    }
    else
    {
        debug("connectToWifi - not connecting, status: %d\n", new_status);
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
    u32_t ip_u32 = ip4_addr_get_u32(netif_ip_addr4(netif_default));
    u32_t mask_u32 = ip4_addr_get_u32(netif_ip4_netmask(netif_default));
    u32_t bcast_u32 = (ip_u32 & mask_u32) | (~mask_u32);

    if(ip_u32 == 0) {   // no ip? don't send broadcast
        debug("ceDiscoverySend - no IP yet\n");
        return;
    }

    udpInitialize();

    displayMessage("wifi connected", "CE host discovery");

    // alloc buffer, copy data to payload part
    struct pbuf *p = pbuf_alloc(PBUF_TRANSPORT, 10 + 1, PBUF_RAM);
    if(p == NULL) {         // out of lwIP memory? try again on next attempt
        debug("ceDiscoverySend - pbuf_alloc failed\n");
        return;
    }

    memcpy((char *)p->payload, "CELC", 4);      // 0..3 - CELC
    memcpy(((char *)p->payload) + 4, mac, 6);    // 4..9 - mac

    whichBroadcastAddr = !whichBroadcastAddr;   // toggle this flag

    if(whichBroadcastAddr)      // send to subnet broadcast addr?
    {
        ip4_addr_t bcast;
        ip4_addr_set_u32(&bcast, bcast_u32);
        debug("ceDiscoverySend to %d.%d.%d.%d\n", bcast_u32 & 0xff, (bcast_u32 >> 8) & 0xff, (bcast_u32 >> 16) & 0xff, (bcast_u32 >> 24) & 0xff);
        udp_sendto(pcbUpd, p, &bcast, SERVER_UDP_PORT);                 // broadcast to subnet devices (e.g. 192.168.1.255)
    }
    else        // send to generic broadcast addr
    {
        debug("ceDiscoverySend to 255.255.255.255\n");
        udp_sendto(pcbUpd, p, IP_ADDR_BROADCAST, SERVER_UDP_PORT);     // broadcast to all possible devices (255.255.255.255)
    }

    pbuf_free(p);
}

size_t conWrite(TConnection* con, const uint8_t *buf, size_t size)
{
    if (!con->cc || !size) {
        return 0;
    }

    con->cc->setTimeout(500);
    return con->cc->write((const char*)buf, size);
}

size_t conRead(TConnection* con, uint8_t *buf, size_t size)
{
    if (!con->cc || !size) {
        return 0;
    }

    return con->cc->read(buf, size);
}

bool connectionAvailable(TConnection* con)
{
    if(!con->cc || !con->cc->availableForWrite()) {
        return false;
    }

    return true;
}

bool isConnected(TConnection* con)
{
    if (!con->cc || con->cc->state() == CLOSED) {
        return false;
    }

    return con->cc->state() == ESTABLISHED || connectionAvailable(con);
}

size_t connectionCanReadBytes(TConnection* con)
{
    if(!con->cc || con->cc->state() == CLOSED) {
        return 0;
    }

    return con->cc->getSize();
}

bool flush(TConnection* con, unsigned int maxWaitMs) {
    if (!con->cc) {
        return true;
    }

    if (maxWaitMs == 0) {
        maxWaitMs = WIFICLIENT_MAX_FLUSH_WAIT_MS;
    }
    return con->cc->wait_until_acked(maxWaitMs);
}

void stop(TConnection* con)
{
    if (!con->cc) {
        return;
    }

    flush(con, 0);
    con->cc->close();
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

    // start connection attempt
    connect(&connectionFdd, &hostIpAddr, hostPortFdd);
}

void connect(TConnection* con, ip_addr_t* addr, uint16_t port)
{
    if (con->cc) {
        con->cc->close();
        con->cc->unref();
        con->cc = nullptr;
    }

    con->pcb = tcp_new();
    if (!con->pcb) {
        return;
    }

    con->cc = new ClientContext(con->pcb, nullptr, nullptr);
    con->cc->ref();
    con->cc->setTimeout(5000);

    con->cc->connectAsync(addr, port);

    con->cc->setSync(true);
    con->cc->setNoDelay(true);
}

void connectToHost(void)
{
    static bool prevConnectedToHost = false;
    static bool prevConnectedToWifi = false;

    connectedToWifi = cyw43_tcpip_link_status(&cyw43_state, CYW43_ITF_STA) == CYW43_LINK_UP;
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
        }
    }

    if(!connectedToWifi) {
        connectToWifi();
    }

    if(connectedToHost)     // we're connected to host? don't send discovery packets
    {
        return;
    }

    // device connected to wifi, do discovery if needed
    ceDiscoverySend();

    // connect to CE server
    connectToCEhost();
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
