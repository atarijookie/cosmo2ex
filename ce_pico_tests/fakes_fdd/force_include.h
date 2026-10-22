// Force-included (-include) before every ce_pico_fdd source in the test build.
//
// connection.h pulls in client_context.h, cyw43 and lwIP, none of which the tested sources use - they need only SYNC_TAG_FDD
// (utils.cpp, storeHeader) and sendHeaderAndDataToHost() (written_sectors.cpp). So define its include guard here, which makes
// connection.h empty, and provide just those.

#ifndef FDD_FORCE_INCLUDE_H
#define FDD_FORCE_INCLUDE_H

#define __CONNECTION_H__
#define SYNC_TAG_FDD    0xc0500fdd      // must match ce_pico_fdd/connection.h

#include <cstdint>
bool sendHeaderAndDataToHost(uint8_t *bfr, uint32_t dataSizeBytes);     // from connection.h, faked in tests/fdd/fake_connection.cpp

#endif
