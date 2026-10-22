// Fake of the host connection (ce_pico_fdd/connection.cpp), see fake_connection.h

#include "fake_connection.h"
#include "defs.h"
#include "utils.h"

bool fakeHostConnected = true;
std::vector<std::vector<uint8_t>> fakeHostSent;
int fakeHostSendStopsAfter = 0;
std::vector<std::vector<uint8_t>> fakeHostSentPartial;
std::deque<uint8_t> fakeHostRx;
int fakeHostDisconnects = 0;

void fakeHostReset(void)
{
    fakeHostConnected = true;
    fakeHostSent.clear();
    fakeHostSendStopsAfter = 0;
    fakeHostSentPartial.clear();
    fakeHostRx.clear();
    fakeHostDisconnects = 0;
}

void fakeHostReceive(const std::vector<uint8_t>& data)
{
    fakeHostRx.insert(fakeHostRx.end(), data.begin(), data.end());
}

bool hostConnected(void)
{
    return fakeHostConnected;
}

size_t hostCanReadBytes(void)
{
    return fakeHostConnected ? fakeHostRx.size() : 0;
}

size_t hostRead(uint8_t* bfr, size_t size)
{
    size_t n = 0;
    while (n < size && !fakeHostRx.empty()) {
        bfr[n++] = fakeHostRx.front();
        fakeHostRx.pop_front();
    }
    return n;
}

void hostDisconnect(void)
{
    fakeHostConnected = false;
    fakeHostDisconnects++;
}

void showRunningStateOnDisplay(void)
{
}

// display.cpp - not compiled into the tests
void displayMessage(const char* msg1, const char* msg2, const char* msg3)
{
}

// same as the real one: store the data size into the header, send header + data - here just record it
bool sendHeaderAndDataToHost(uint8_t *bfr, uint32_t dataSizeBytes)
{
    if(!fakeHostConnected) {
        return false;
    }

    storeDword(bfr + 6, dataSizeBytes);

    if(fakeHostSendStopsAfter > 0 && fakeHostSendStopsAfter < (int) (TX_HEADER_SIZE + dataSizeBytes)) {
        fakeHostSentPartial.push_back(std::vector<uint8_t>(bfr, bfr + fakeHostSendStopsAfter));
        fakeHostSendStopsAfter = 0;
        hostDisconnect();
        return false;
    }

    fakeHostSent.push_back(std::vector<uint8_t>(bfr, bfr + TX_HEADER_SIZE + dataSizeBytes));
    return true;
}
