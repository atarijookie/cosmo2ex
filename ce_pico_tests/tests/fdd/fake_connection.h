// Fake of the host connection (ce_pico_fdd/connection.cpp) - what was sent, and whether sending works.

#ifndef FAKE_CONNECTION_H
#define FAKE_CONNECTION_H

#include <vector>
#include <deque>
#include <cstdint>

extern bool fakeHostConnected;                          // sendHeaderAndDataToHost() succeeds only when true
extern std::vector<std::vector<uint8_t>> fakeHostSent;  // every successfully sent message: header + data

// > 0: the next send times out after this many bytes - like the real sendDataToHost() it then closes the connection
// and returns false. The bytes that went out are in fakeHostSentPartial. One shot, back to 0 after it happened.
extern int fakeHostSendStopsAfter;
extern std::vector<std::vector<uint8_t>> fakeHostSentPartial;

extern std::deque<uint8_t> fakeHostRx;                  // received from host, not read yet - hostRead() takes from here
extern int fakeHostDisconnects;                         // hostDisconnect() calls - it also sets fakeHostConnected = false

void fakeHostReceive(const std::vector<uint8_t>& data); // data arrives from host

void fakeHostReset(void);

#endif
