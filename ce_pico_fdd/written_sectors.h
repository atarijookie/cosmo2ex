#ifndef __WRITTEN_SECTORS_H__
#define __WRITTEN_SECTORS_H__

// Sending sectors written by the ST (captured on core1) to the host - on core0.

void writtenSectorsHandle(void);    // take new written sectors from core1, send all not sent ones in order while it works
bool writtenSectorsPending(void);   // true if some written sector wasn't sent to host yet
void writtenSectorsReset(void);     // forget all not sent ones - for tests

#endif
