#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <netinet/in.h>

#include "../../misc/debug.h"
#include "sting.h"
#include "readwrapper.h"

#ifndef MIN
#define MIN(a, b)   ((a) < (b) ? (a) : (b))
#endif

ReadWrapper::ReadWrapper(void)
{
    init(-1, 0, 0);
}

void ReadWrapper::init(int inFd, int inType, int inBuffSize)
{
    fd          = inFd;
    type        = inType;
    buff_size   = inBuffSize;
    clearAll();
}

void ReadWrapper::clearAll(void)
{
    items.clear();
    itemsTotalSize = 0;
}

int ReadWrapper::getCount(void)
{
    if(fd < 0) {                                        // no socket - nothing to read
        return 0;
    }

    if(type == TCP) {
        return tcpBytesWaiting();
    }

    if(type == UDP) {
        udpTryReceive();
        return itemsTotalSize;
    }

    return 0;
}

int ReadWrapper::getNdb(uint8_t *tmpBuffer)
{
    int size = getNextNdbSize();                        // how much we should get

    if(size < 1) {
        return 0;
    }

    if(type == TCP) {
        int res = recv(fd, tmpBuffer, size, MSG_DONTWAIT);
        return (res < 0) ? 0 : res;
    }

    if(type == UDP) {
        if(items.empty()) {
            return 0;
        }

        std::vector<uint8_t>& item = items.front();
        memcpy(tmpBuffer, item.data(), size);           // size is capped to 32 kB by getNextNdbSize()

        itemsTotalSize -= item.size();
        items.pop_front();
        return size;
    }

    return 0;
}

int ReadWrapper::getNextNdbSize(void)
{
    if(type == TCP) {
        return MIN(tcpBytesWaiting(), buff_size);       // a NDB is at most the buffer size from TCP_open()
    }

    if(type == UDP) {
        udpTryReceive();

        if(items.empty()) {
            return 0;
        }

        return MIN((int) items.front().size(), 32 * 1024);
    }

    return 0;
}

int ReadWrapper::peekBlock(uint8_t *tmpBuffer, int size)
{
    if(size <= 0) {
        return 0;
    }

    if(type == TCP) {
        int totalCount = tcpBytesWaiting();
        if(totalCount <= 0) {
            return 0;
        }

        size = MIN(totalCount, size);
        int res = recv(fd, tmpBuffer, size, MSG_DONTWAIT | MSG_PEEK);   // leave the data in the socket

        if(res < 0) {
            if(errno != EAGAIN && errno != EWOULDBLOCK) {
                logHdd(LOG_ERROR, "ReadWrapper::peekBlock() - TCP recv() %d bytes error: %s", size, strerror(errno));
            }
            return 0;
        }
        return res;
    }

    if(type == UDP) {
        udpTryReceive();

        uint8_t *p    = tmpBuffer;
        int remaining = size;

        for(size_t i=0; i<items.size() && remaining > 0; i++) {     // merge the datagrams
            const std::vector<uint8_t>& item = items[i];
            int use = MIN((int) item.size(), remaining);

            memcpy(p, item.data(), use);
            p           += use;
            remaining   -= use;
        }

        return size - remaining;
    }

    return 0;
}

void ReadWrapper::removeBlock(int size)
{
    if(type == TCP) {
        int toRemove = MIN(tcpBytesWaiting(), size);

        while(toRemove > 0) {                           // in TMP_BFR_SIZE blocks
            int res = recv(fd, tmpBfr, MIN(toRemove, TMP_BFR_SIZE), MSG_DONTWAIT);

            if(res <= 0) {
                if(res < 0) {
                    logHdd(LOG_ERROR, "ReadWrapper::removeBlock() TCP recv error: %s", strerror(errno));
                }
                break;
            }

            toRemove -= res;
        }
        return;
    }

    if(type == UDP) {
        udpTryReceive();

        int remaining = size;

        while(remaining > 0 && !items.empty()) {
            std::vector<uint8_t>& item = items.front();

            if((int) item.size() <= remaining) {        // whole datagram goes
                remaining       -= item.size();
                itemsTotalSize  -= item.size();
                items.pop_front();
            } else {                                    // part of it goes, the rest stays as the first datagram
                item.erase(item.begin(), item.begin() + remaining);
                itemsTotalSize  -= remaining;
                remaining        = 0;
            }
        }
    }
}

int ReadWrapper::tcpBytesWaiting(void)
{
    int value = 0;

    if(ioctl(fd, FIONREAD, &value) < 0) {
        value = 0;
    }

    return value;
}

void ReadWrapper::udpTryReceive(void)
{
    // Read datagrams until the socket is empty. Not through FIONREAD - Linux gives the size of the next datagram there,
    // macOS the size of all of them, and both 0 for an empty datagram.
    for(int loops = 0; loops < 1000; loops++) {
        struct sockaddr_in from;
        socklen_t fromLen = sizeof(from);

        ssize_t res = recvfrom(fd, tmpBfr, TMP_BFR_SIZE, MSG_DONTWAIT, (struct sockaddr *) &from, &fromLen);

        if(res < 0) {
            if(errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
                logHdd(LOG_DEBUG, "ReadWrapper::udpTryReceive() - recvfrom() error: %s", strerror(errno));   // e.g. ICMP port unreachable
            }
            break;
        }

        if(res == 0) {                                  // empty datagram - nothing for the ST to read
            continue;
        }

        if(itemsTotalSize + res > MAX_TOTAL_ITEMS_SIZE) {   // queue full - drop it, as a full receive buffer would
            logHdd(LOG_DEBUG, "ReadWrapper::udpTryReceive() - queue full, dropping %d B datagram", (int) res);
            continue;
        }

        items.emplace_back(tmpBfr, tmpBfr + res);
        itemsTotalSize += res;
    }
}
