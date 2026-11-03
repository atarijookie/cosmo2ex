#ifndef _READWRAPPER_H_
#define _READWRAPPER_H_

#include <stdint.h>
#include <deque>
#include <vector>

// Received data of one connection of the network module, as the STinG CN* functions see it.
// TCP: the data stay in the socket, read with MSG_PEEK / recv(). UDP: datagrams queued here, CNget_NDB() takes
// one datagram, the other functions see them as one stream. From ce-atari/ce_main_app/network/readwrapper.cpp.

#define MAX_TOTAL_ITEMS_SIZE    (1024 * 1024)       // UDP queue limit - the newest datagrams get dropped above it
#define TMP_BFR_SIZE            (  64 * 1024)

class ReadWrapper
{
public:
    ReadWrapper(void);

    void init(int inFd, int inType, int inBuffSize);
    void clearAll(void);

    int  getCount      (void);                       // bytes we can read - for UDP all queued datagrams together

    int  getNdb        (uint8_t *tmpBuffer);         // for UDP one datagram, for TCP a block from the stream
    int  getNextNdbSize(void);

    int  peekBlock     (uint8_t *tmpBuffer, int size);   // data without removing them, UDP datagrams merged
    void removeBlock   (int size);

private:
    int fd;             // socket
    int type;           // TCP / UDP
    int buff_size;      // TCP: size of a NDB, from TCP_open()

    uint8_t tmpBfr[TMP_BFR_SIZE];

    // UDP
    std::deque<std::vector<uint8_t>> items;          // received datagrams
    int  itemsTotalSize;

    void udpTryReceive(void);

    // TCP
    int  tcpBytesWaiting(void);
};

#endif
