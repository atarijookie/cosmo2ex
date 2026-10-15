#ifndef _BUFFERS_H_
#define _BUFFERS_H_

#include <stdint.h>

// Big transfer buffers shared by all the sub-tests. Only one sub-test runs at a time,
// so they can all use the same memory instead of each having its own static buffers.
#define SHARED_BUFFER_SIZE  (256 * 512)

extern uint8_t readBuffer [SHARED_BUFFER_SIZE];
extern uint8_t writeBuffer[SHARED_BUFFER_SIZE];

#endif
