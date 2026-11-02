#include <mint/osbind.h>

#include "helpers.h"

// Log lines to the CosmosEx host: CE_DD catches Fopen("CELG<text>", TAG_CE) and sends <text> as ST_LOG_TEXT to the host.
// Without CE_DD (or with older CE_DD) the Fopen just fails on a nonsense filename, so the probe is harmless.

#define TAG_CE      0x4345
#define RET_CELG    0x43454C47

static int8_t hostLogState = -1;        // -1: not probed yet, 0: not available, 1: available
static char   hostLogBuf[4 + 500];

static int32_t hostLogSend(const char *str)
{
    strcpy(hostLogBuf, "CELG");
    strncpy(hostLogBuf + 4, str, sizeof(hostLogBuf) - 5);
    hostLogBuf[sizeof(hostLogBuf) - 1] = 0;

    return Fopen(hostLogBuf, TAG_CE);
}

uint8_t hostLogAvailable(void)
{
    if(hostLogState < 0) {
        hostLogState = (hostLogSend("ce_tests: host log connected") == RET_CELG) ? 1 : 0;
    }

    return hostLogState;
}

void hostLog(const char *str)
{
    if(!hostLogAvailable()) {
        return;
    }

    hostLogSend(str);
}
