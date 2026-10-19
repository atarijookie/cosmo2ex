#include <stdio.h>
#include <string.h>
#include <signal.h>
#include <stdlib.h>
#include <pthread.h>
#include <unistd.h>
#include <queue>
#include <sys/file.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/stat.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/ioctl.h>

#include "../misc/global.h"
#include "../misc/debug.h"
#include "../misc/utils.h"
#include "discovery.h"
#include "../chipinterface/chipinterface.h"

void onClientRequest(sockaddr_in *clientAddr, uint8_t *recvData, int len);
void udpSend(uint32_t ip, uint16_t port, uint8_t* data, uint16_t len);

#define IP_TO_MAC_COUNT  (3 * MAX_CLIENTS)
TClientIpToMac ipToMac[IP_TO_MAC_COUNT];
pthread_mutex_t ipToMacMutex = PTHREAD_MUTEX_INITIALIZER;     // ipToMac is written by discovery thread, read by HDD / FDD / IKBD threads

int discoveryServerOpenSocket(void)
{
    int sockfd = 0;
    struct sockaddr_in servaddr;

    // create socket fd
    if ((sockfd = Utils::socketCloexec(AF_INET, SOCK_DGRAM, 0)) < 0 ) {        // CLOEXEC - don't let processes started by system() inherit this socket
        logDiscovery(LOG_ERROR, "Failed to create main server socket: %s", strerror(errno));
        return -1;
    }

    memset(&servaddr, 0, sizeof(servaddr));

    // fill in server information
    servaddr.sin_family         = AF_INET;          // IPv4
    servaddr.sin_addr.s_addr    = INADDR_ANY;
    servaddr.sin_port           = htons(SERVER_UDP_PORT);

    // bind the socket with the server address
    if (bind(sockfd, (const struct sockaddr *) &servaddr, sizeof(servaddr)) < 0) {
        if(errno == EADDRINUSE) {       // some other running app (probably another instance of this app) is using this port
            logDiscovery(LOG_ERROR, "Failed to bind() main server socket, udp port %d is used by another running app", SERVER_UDP_PORT);
        } else {
            logDiscovery(LOG_ERROR, "Failed to bind() main server socket, udp port %d: %s", SERVER_UDP_PORT, strerror(errno));
        }
        close(sockfd);                  // don't leak the socket
        return -1;
    }

    return sockfd;
}

void *discoveryMain(void *ptr)
{
    int udpSocket = discoveryServerOpenSocket();        // create main UDP socket

    if(udpSocket < 0) {     // on error, terminate whole app - without discovery the devices can't find us
        logDiscovery(LOG_ERROR, "Could not open port %d, terminating", SERVER_UDP_PORT);
        printf("Could not open port %d, terminating\n", SERVER_UDP_PORT);
        fatalErrorOccurred = 1;
        sigintReceived = 1;
        return NULL;
    }

    logDiscovery(LOG_INFO, "Starting CosmosEx discovery server");
    printf("Starting CosmosEx discovery server\n");

    struct timeval timeout;
    fd_set readfds;
    uint8_t recvData[256];
    struct sockaddr_in clientAddr;

    // This is main discovery server loop, which does the following:
    // - waits for and responds to requests from CE lite (telling CE lite the TCP ports of HDD / FDD / IKBD cores)
    // - stores the ip-to-mac mapping from each request, so the cores know the mac of the client when it connects
    while(sigintReceived == 0) {
        memset(&timeout, 0, sizeof(timeout));
        timeout.tv_sec = 1;
        timeout.tv_usec = 0;

        FD_ZERO(&readfds);
        FD_SET(udpSocket, &readfds);

        if(select(udpSocket + 1, &readfds, NULL, NULL, &timeout) < 0) { // on select() error
            if(errno != EINTR) {        // interrupted by signal is fine, other errors are not
                logDiscovery(LOG_ERROR, "select() failed: %s", strerror(errno));
                Utils::sleepMs(100);    // don't spin at 100% cpu if the error persists
            }
            continue;
        }

        if(FD_ISSET(udpSocket, &readfds)) { // can read from UDP socket?
            socklen_t slen = sizeof(clientAddr);
            memset(&clientAddr, 0, sizeof(clientAddr));

            ssize_t cnt = recvfrom(udpSocket, recvData, sizeof(recvData), 0, (struct sockaddr *) &clientAddr, &slen);

            if(cnt < 0) {                                       // recvfrom() failed? nothing received, nothing to log as request
                if(errno != EINTR) {
                    logDiscovery(LOG_ERROR, "recvfrom() failed: %s", strerror(errno));
                }
            } else if(cnt >= 4 && memcmp(recvData, "CELC", 4) == 0) {   // CE Lite Client wants something?
                onClientRequest(&clientAddr, recvData, cnt);
            } else if(cnt >= 4) {                               // got enough bytes to show request start
                logDiscovery(LOG_INFO, "ignoring unknown request %02X %02X %02X %02X, packet length: %d", recvData[0], recvData[1], recvData[2], recvData[3], (int) cnt);
            } else {                                            // too short to be any request
                logDiscovery(LOG_INFO, "ignoring too short request, packet length: %d", (int) cnt);
            }
        }
    }

    close(udpSocket);

    logDiscovery(LOG_ERROR, "Terminating CosmosEx network server");

    return NULL;
}

void getMacForIp(uint32_t clientIp, uint8_t* mac)
{
    memset(mac, 0, 6);      // start with zeros

    pthread_mutex_lock(&ipToMacMutex);

    for(int i=0; i<IP_TO_MAC_COUNT; i++) {
        if(ipToMac[i].clientIp == clientIp) {   // found ip? copy mac
            memcpy(mac, ipToMac[i].mac, 6);
            break;
        }
    }

    pthread_mutex_unlock(&ipToMacMutex);
}

void onClientRequest(sockaddr_in *clientAddr, uint8_t *recvData, int len)
{
    uint32_t clientIp = clientAddr->sin_addr.s_addr;    // get client address

    if(len >= 10) {         // request contains mac? (0..3: "CELC", 4..9: mac) store ip-to-mac mapping
        pthread_mutex_lock(&ipToMacMutex);

        // find slot for this ip-to-mac mapping: slot with the same ip, otherwise first empty slot, otherwise the oldest slot
        int idxSame = -1, idxEmpty = -1, idxOldest = 0;
        for(int i=0; i<IP_TO_MAC_COUNT; i++) {
            if(ipToMac[i].clientIp == clientIp) {                           // existing slot for this ip? use it
                idxSame = i;
                break;
            }

            if(ipToMac[i].clientIp == 0) {                                  // empty slot? remember the first one
                if(idxEmpty < 0) {
                    idxEmpty = i;
                }
                continue;
            }

            if(ipToMac[i].lastTime < ipToMac[idxOldest].lastTime) {         // this slot is older than the oldest so far?
                idxOldest = i;
            }
        }

        int idx = (idxSame >= 0) ? idxSame : ((idxEmpty >= 0) ? idxEmpty : idxOldest);

        // store ip and mac into the slot
        ipToMac[idx].clientIp = clientIp;
        memcpy(ipToMac[idx].mac, recvData + 4, 6);
        ipToMac[idx].lastTime = time(NULL);

        pthread_mutex_unlock(&ipToMacMutex);
    } else {                // too short to contain mac? don't store garbage as mac, but still tell client the ports
        logDiscovery(LOG_ERROR, "onClientRequest - request too short to contain mac, length: %d", len);
    }

    // send response back
    uint8_t response[10];
    memset(response, 0, sizeof(response));  // clear all bytes
    memcpy(response, "CELR", 4);            // 0..3: CE Lite Response

    Utils::storeWord(response + 4, SERVER_TCP_PORT_HDD);
    Utils::storeWord(response + 6, SERVER_TCP_PORT_FDD);
    Utils::storeWord(response + 8, SERVER_TCP_PORT_IKBD);

    udpSend(clientIp, CLIENT_UDP_PORT, response, sizeof(response));     // send response to client with info about ports

    logDiscovery(LOG_INFO, "onClientRequest response - ports %d, %d, %d", SERVER_TCP_PORT_HDD, SERVER_TCP_PORT_FDD, SERVER_TCP_PORT_IKBD);
}

void udpSend(uint32_t ip, uint16_t port, uint8_t* data, uint16_t len)
{
    sockaddr_in servaddr;

    int sockFd = socket(AF_INET, SOCK_DGRAM, 0);

    if(sockFd < 0) {        // on error
        logDiscovery(LOG_ERROR, "updSend - failed to open socket for response");
        return;
    }

    bzero(&servaddr,sizeof(servaddr));

    servaddr.sin_family = AF_INET;
    servaddr.sin_addr.s_addr = ip;
    servaddr.sin_port = htons(port);

    int rv = sendto(sockFd, data, len, 0, (sockaddr*) &servaddr, sizeof(servaddr));
    close(sockFd);

    if(rv < 0) {
        logDiscovery(LOG_ERROR, "updSend - failed to sendto() response");
    }
}
