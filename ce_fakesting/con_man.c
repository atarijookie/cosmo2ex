// vim: shiftwidth=4 softtabstop=4 expandtab
#include <mint/sysbind.h>
#include <mint/osbind.h>
#include <mint/basepage.h>
#include <mint/ostruct.h>
#include <support.h>
#include <stdint.h>
#include <stdio.h>

#include "globdefs.h"
#include "global.h"
#include "con_man.h"
#include "icmp.h"
#include "setup.h"
#include "vbl.h"

//---------------------
// ACSI / CosmosEx stuff
#include "../libacsiscsi/acsi.h"
#include "../libacsiscsi/acsi.h"
#include "../libacsiscsi/stdlib.h"
#include "../libacsiscsi/hdd_if.h"
#include "stdlib2.h"
#include "ce_commands.h"
#include "cecmd.h"

//--------------------------------------
// The following variable should be used to force update_con_info() when there was some reading / writing to socket.
// Functions used for determining connection state / byte count should use it to force update_con_info() when possibly needed.
// Functions used for sending / receiving stuff should SET it to true to force update before next socket info retrieval.
uint8_t forceNextUpdateConInfo;

//--------------------------------------

extern  uint32  localIP;

TConInfo conInfo[NET_HANDLES_COUNT];        // this holds info about each connection

static void connection_send_block(uint8_t netCmd, uint8_t handle, uint16_t length, uint8_t *buffer);
//--------------------------------------
#define NO_CHANGE_W     0xfffe
#define NO_CHANGE_DW    0xfffffffe

void setCIB(uint8_t *cib, uint16_t protocol, uint16_t lPort, uint16_t rPort, uint32_t rHost, uint32_t lHost, uint16_t status);

#define CIB_PROTO   0
#define CIB_LPORT   1
#define CIB_RPORT   2
#define CIB_RHOST   3
#define CIB_LHOST   4
#define CIB_STATUS  5

uint32_t getCIBitem(uint8_t *cib, uint8_t item);
//--------------------------------------
static void initConInfoStruct(int i);

int tcpUdpGotSomeConnection(void);      // return TRUE if there is a valid connection (in or out), return FALSE otherwise
int icmpGotSomeHandler     (void);      // return TRUE if there is a valid ICMP handler, return FALSE otherwise

//--------------------------------------
// STinG 1.26 order of checks, used by the CN* functions and TCP_send()

static uint8_t stateIsClosing(uint8_t state)        // remote closed / reset / we closed - no more data will come
{
    return state == TCLOSED || state >= TFIN_WAIT1;
}

static int32_t availBytes(TConInfo *ci)             // the host count still has the chars CNget_char() used locally
{
    return (ci->bytesToRead > ci->charsUsed) ? (int32_t) (ci->bytesToRead - ci->charsUsed) : 0;
}

static int16 takeError(TConInfo *ci)                // an error from the host (reset, refused) is returned once
{
    int16 err = ci->netError;
    ci->netError = 0;
    return err;
}

//--------------------------------------
// Local stream data. Each connection has a small buffer (chars[], filled by CNget_char()), CNgets() uses the shared
// line cache instead: one fetch gets many lines, and they're found here without a command per line. Only one connection
// owns the cache at a time - another one taking it just drops the cached data (they're still on the host).

static uint8_t lineCache[CN_PEEK_MAX];
static int     lineCacheOwner = -1;                 // slot

static uint8_t *localBuf(int slot)
{
    return (lineCacheOwner == slot) ? lineCache : conInfo[slot].chars;
}

static void dropCache(int slot)                     // the slot loses the cache - its bytes still unused stay on the host
{
    if(lineCacheOwner == slot) {
        conInfo[slot].charsGot = conInfo[slot].charsUsed;  // nothing more local, the used count is still to be reported
        lineCacheOwner = -1;
    }
}

static void takeCache(int slot)
{
    if(lineCacheOwner == slot) {
        return;
    }

    if(lineCacheOwner >= 0) {
        dropCache(lineCacheOwner);
    }

    conInfo[slot].charsGot = conInfo[slot].charsUsed;      // what's in chars[] gets fetched again into the cache
    lineCacheOwner = slot;
}

// tell the host how many bytes were used and get up to 'max' next ones into the local buffer (cache or chars[])
static uint8_t fillLocal(int slot, uint8_t handle, uint16_t max)
{
    TConInfo *ci = &conInfo[slot];

    commandLong[ 5] = NET_CMD_CN_PEEK;
    commandLong[ 6] = handle;
    commandLong[ 7] = (uint8_t) (ci->charsUsed >> 8);
    commandLong[ 8] = (uint8_t) (ci->charsUsed     );
    commandLong[ 9] = (uint8_t) (max >> 8);
    commandLong[10] = (uint8_t) (max     );

    ci->charsUsed = 0;
    ci->charsGot  = 0;

    ceCmd(ACSI_READ, commandLong, CMD_LENGTH_LONG, pDmaBuffer, (2 + max + 511) >> 9);
    forceNextUpdateConInfo = TRUE;                  // the host removed the used bytes - CNbyte_count() must see it

    if(!hdIf.success || hdIf.statusByte != E_NORMAL) {
        return FALSE;
    }

    uint16_t got = getWord(pDmaBuffer);
    if(got > max) {
        got = max;
    }

    memcpy(localBuf(slot), pDmaBuffer + 2, got);
    ci->charsGot = got;
    return TRUE;
}

// the used count for the commands which take it as a byte - above 255 it goes first by CN_PEEK. The local data are
// gone after such command (the host removes the used bytes and continues from there).
static uint8_t usedForCommand(int slot, uint8_t handle)
{
    TConInfo *ci = &conInfo[slot];
    uint8_t used;

    if(ci->charsUsed > 255) {
        fillLocal(slot, handle, 0);                 // just report them
        used = 0;
    } else {
        used = (uint8_t) ci->charsUsed;
    }

    ci->charsUsed = 0;
    ci->charsGot  = 0;
    dropCache(slot);
    return used;
}

//--------------------------------------
// connection info function

int16 CNkick(int16 handle)
{
    update_con_info(FALSE);             // update connections info structs (max once per 100 ms)

    if(!handle_valid(handle)) {         // we don't have this handle? fail
        return E_BADHANDLE;
    }

    return E_NORMAL;
}

CIB *CNgetinfo(int16 handle)
{
    // as STinG: NULL for handles not in use. A connection which is closing (lingering after TCP_close()) still has it.
    if(!handle_valid(handle)) {
        return (CIB *) NULL;
    }
    int slot = network_handleToSlot(handle);

    update_con_info(forceNextUpdateConInfo);        // update connections info structs (max once per 100 ms)
    forceNextUpdateConInfo = FALSE;

    return &conInfo[slot].cib;                      // return pointer to correct CIB
}

int16 CNbyte_count (int16 handle)
{
    if(!handle_valid(handle)) {                     // we don't have this handle? fail
        return E_BADHANDLE;
    }
    int slot = network_handleToSlot(handle);

    update_con_info(forceNextUpdateConInfo);        // update connections info structs (max once per 100 ms)
    forceNextUpdateConInfo = FALSE;

    TConInfo *ci = &conInfo[slot];                  // we're working with this connection

    int16 err = takeError(ci);
    if(err) {
        return err;
    }

    if(ci->tcpConnectionState == TLISTEN) {         // still listening
        return E_LISTEN;
    }

    int32_t avail = availBytes(ci);

    if(avail == 0 && stateIsClosing(ci->tcpConnectionState) && getCIBitem((uint8_t *) &ci->cib, CIB_PROTO) == TCP) {
        return E_EOF;                               // nothing more will come
    }

    return (avail > 0x7fff) ? 0x7fff : avail;       // more would look like an error
}

//-------------------------------------
// data retrieval functions
int16 CNget_char(int16 handle)
{
    if(!handle_valid(handle)) {                 // we don't have this handle? fail
        return E_BADHANDLE;
    }
    int slot = network_handleToSlot(handle);

    update_con_info(FALSE);                     // update connections info structs (max once per 100 ms)
    TConInfo *ci = &conInfo[slot];              // we're working with this connection

    int16 err = takeError(ci);
    if(err) {
        return err;
    }

    if(ci->tcpConnectionState == TLISTEN) {
        return E_LISTEN;
    }

    if(availBytes(ci) == 0 && stateIsClosing(ci->tcpConnectionState) && ci->charsUsed >= ci->charsGot &&
       getCIBitem((uint8_t *) &ci->cib, CIB_PROTO) == TCP) {
        return E_EOF;
    }

    // nothing local? get the next bytes - into the line cache if this connection has it, else into chars[]
    if(ci->charsUsed >= ci->charsGot) {
        uint16_t max = (lineCacheOwner == slot) ? CN_PEEK_MAX : 250;

        if(!fillLocal(slot, handle, max)) {
            return E_NODATA;
        }
    }

    if(ci->charsUsed >= ci->charsGot) {         // still no data? fail
        return E_NODATA;
    }

    forceNextUpdateConInfo = TRUE;              // force update_con_info() if asked to do it next time.

    int value = localBuf(slot)[ci->charsUsed];  // get the char
    ci->charsUsed++;                            // update used count
    return value;                               // return that char
}

NDB *CNget_NDB(int16 handle)
{
    if(!handle_valid(handle)) {                 // we don't have this handle? fail
        return (NDB *) NULL;
    }
    int slot = network_handleToSlot(handle);

    update_con_info(FALSE);                     // update connections info structs (max once per 100 ms)
    TConInfo *ci = &conInfo[slot];              // we're working with this connection

    if(ci->netError || ci->tcpConnectionState == TLISTEN || availBytes(ci) == 0) {     // error, listening or no data
        return (NDB *) NULL;
    }

    //-----------------
    // first find out how big is the next NDB
    uint8_t used = usedForCommand(slot, handle);    // first - above 255 it sends its own command

    commandLong[ 5] = NET_CMD_CNGET_NDB;    // store function number
    commandLong[ 6] = handle;                // store file handle
    commandLong[ 7] = 0;                    // get next NDB size
    commandLong[10] = used;                 // how many chars were used

    ceCmd(ACSI_READ, commandLong, CMD_LENGTH_LONG, pDmaBuffer, 1);

    if(!hdIf.success || hdIf.statusByte != E_NORMAL) {  // failed? quit
        return NULL;
    }

    // get size of NDB
    int  nexdNdbSizeBytes   = getDword(pDmaBuffer);
    uint8_t nextNdbSizeSectors = pDmaBuffer[4];

    //-----------------
    // then try to allocate the RAM for NDB - the DMA writes whole sectors, so the buffer must hold all of them,
    // plus 3 bytes for longword aligning the data (TT SCSI DMA). Not enough memory? No NDB this time.
    uint32_t readCount  = nexdNdbSizeBytes;
    uint32_t allocSize  = (((uint32_t) nextNdbSizeSectors) << 9) + 3;

    if(nextNdbSizeSectors == 0 || readCount > (((uint32_t) nextNdbSizeSectors) << 9) ||
       allocSize + sizeof(NDB) > (uint32_t) KRgetfree_internal(TRUE)) {
        return (NDB *) NULL;
    }

    uint8_t *bfrRaw = KRmalloc_internal(allocSize);                         // try to malloc() RAM for data

    if(bfrRaw == NULL) {                                                    // malloc() failed, return NULL
        return (NDB *) NULL;
    }

    uint8_t *bfr = (uint8_t *) ((((uint32_t) bfrRaw) + 3) & 0xfffffffc);   // DMA goes here, ndb->ptr keeps bfrRaw for KRfree()

    NDB *pNdb = KRmalloc_internal(sizeof(NDB));                             // try to malloc() RAM for the containing NDB struct

    if(pNdb == NULL) {                                                      // malloc() failed, return NULL
        KRfree_internal(bfrRaw);                                            // free the data buffer
        return (NDB *) NULL;
    }

    //-----------------
    // setup the NDB structure - but using direct access, as calling code uses different packing than gcc
    storeDword(((uint8_t *)pNdb) +  0, (uint32_t) bfrRaw);        // pointer to allocated data - for free()
    storeDword(((uint8_t *)pNdb) +  4, (uint32_t) bfr);           // pointer to data
    storeWord (((uint8_t *)pNdb) +  8, (uint16_t)  readCount);     // length of data in buffer
    storeDword(((uint8_t *)pNdb) + 10, (uint32_t) 0);             // pointer to next NDB

    //-----------------
    // get the NDB data from host
       commandLong[ 5] = NET_CMD_CNGET_NDB;    // store function number
    commandLong[ 6] = handle;                // store file handle
    commandLong[ 7] = 1;                    // get next NDB data
    commandLong[10] = 0;                    // how many chars were used

    ceCmd(ACSI_READ, commandLong, CMD_LENGTH_LONG, bfr, nextNdbSizeSectors);

    if(!hdIf.success || hdIf.statusByte != E_NORMAL) {  // failed? quit
        KRfree_internal(bfrRaw);
        KRfree_internal(pNdb);
        return (NDB *) NULL;
    }

    forceNextUpdateConInfo = TRUE;          // force update_con_info() if asked to do it next time.
    return pNdb;                            // return the pointer to NDB structure
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////

static uint8_t getBlockSingle(TConInfo *ci, uint8_t handle, uint8_t *buffer, uint16_t byteCount, uint16_t sectorCount)
{
    int charsUsed = usedForCommand(ci - conInfo, handle);

    //----------------------------------------
    commandLong[ 5] = NET_CMD_CNGET_BLOCK;  // store function number
    commandLong[ 6] = handle;                // store file handle
    commandLong[ 7] = (uint8_t) (byteCount >> 8);
    commandLong[ 8] = (uint8_t) (byteCount     );
    commandLong[10] = charsUsed;            // how many chars were used

    ceCmd(ACSI_READ, commandLong, CMD_LENGTH_LONG, buffer, sectorCount);

    if(!hdIf.success || hdIf.statusByte != E_NORMAL) {  // failed? quit
        return FALSE;
    }

    return TRUE;
}

static uint8_t getBlockLong(TConInfo *ci, uint8_t handle, uint8_t *buffer, uint16_t byteCount, uint16_t sectorCount)
{
    uint8_t res;
    uint8_t toFastRam = ceNeedsBounce(buffer);      // FAST RAM, odd, or not longword aligned on TT SCSI? through middle buffer

    if(toFastRam == FALSE) {                                            // not to FAST RAM? Do a single transfer and quit
        res = getBlockSingle(ci, handle, buffer, byteCount, sectorCount);
        return res;
    }

    // it's to FAST RAM (or odd address), let's go through middle buffer

    while(byteCount > 0) {
        // If the needed count is bigger that what we can fit in maximum transfer size, limit it to that maximum; otherwise just use it.
        uint16_t thisGetSizeBytes   = (byteCount < FASTRAM_BUFFER_SIZE) ? byteCount : FASTRAM_BUFFER_SIZE;
        uint16_t thisGetSizeSectors = (thisGetSizeBytes >> 9) + (((thisGetSizeBytes & 0x1ff) == 0) ? 0 : 1);

        res = getBlockSingle(ci, handle, FastRAMBuffer, thisGetSizeBytes, thisGetSizeSectors);  // make transfer to middle buffer
        memcpy(buffer, FastRAMBuffer, thisGetSizeBytes);                                    // copy to the final buffer

        if(res == FALSE) {
            return FALSE;
        }

        buffer            += thisGetSizeBytes;        // update the buffer pointer
        byteCount       -= thisGetSizeBytes;        // update the count that we still should read
    }

    return TRUE;
}

int16 CNget_block(int16 handle, void *buffer, int16 length)
{
    if(!handle_valid(handle)) {             // we don't have this handle? fail
        return E_BADHANDLE;
    }
    int slot = network_handleToSlot(handle);

    update_con_info(FALSE);                 // update connections info structs (max once per 100 ms)
    TConInfo *ci = &conInfo[slot];          // we're working with this connection

    int16 err = takeError(ci);
    if(err) {
        return err;
    }

    if(ci->tcpConnectionState == TLISTEN) {
        return E_LISTEN;
    }

    if(availBytes(ci) == 0 && stateIsClosing(ci->tcpConnectionState) && getCIBitem((uint8_t *) &ci->cib, CIB_PROTO) == TCP) {
        return E_EOF;
    }

    if(length < 1) {                        // nothing to do? good
        return 0;
    }

    if(ci->charsGot - ci->charsUsed >= length) {    // all of it is here already (after CNgets() / CNget_char())
        memcpy(buffer, localBuf(slot) + ci->charsUsed, length);
        ci->charsUsed += length;
        forceNextUpdateConInfo = TRUE;
        return length;
    }

    //----------------------------------------
    // how many sectors and bytes we need for long transfer
    int lenLongSectors  = length         >> 9;
    int lenLongBytes    = lenLongSectors << 9;

    // how many bytes we need for short transfer
    int lenShortBytes   = length      & 0x1ff;

    //----------------------------------------
    // first do the long transfer
    uint8_t res;

    if(lenLongSectors > 0) {                    // if we should do the long transfer (something to transfer?)
        res = getBlockLong(ci, handle, buffer, lenLongBytes, lenLongSectors);

        if(res == FALSE) {                      // if failed, quit
            return E_NODATA;
        }
    }
    //----------------------------------------
    // then do the short transfer
    if(lenShortBytes > 0) {                     // if we should do the short transfer (something to transfer?)
        res = getBlockSingle(ci, handle, pDmaBuffer, lenShortBytes, 1);

        if(res == FALSE) {                      // if failed, quit
            return E_NODATA;
        }

        memcpy(buffer + lenLongBytes, pDmaBuffer, lenShortBytes);   // copy it from intermediate buffer to final buffer
    }
    //----------------------------------------

    forceNextUpdateConInfo = TRUE;              // force update_con_info() if asked to do it next time.
    return length;
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////

int16 CNgets(int16 handle, char *buffer, int16 length, char delimiter)
{
    if(!handle_valid(handle)) {             // we don't have this handle? fail
        return E_BADHANDLE;
    }
    int slot = network_handleToSlot(handle);

    update_con_info(FALSE);                 // update connections info structs (max once per 100 ms)
    TConInfo *ci = &conInfo[slot];          // we're working with this connection

    int16 err = takeError(ci);
    if(err) {
        return err;
    }

    if(ci->tcpConnectionState == TLISTEN) {
        return E_LISTEN;
    }

    if(availBytes(ci) == 0 && stateIsClosing(ci->tcpConnectionState) && getCIBitem((uint8_t *) &ci->cib, CIB_PROTO) == TCP) {
        return E_EOF;
    }

    if(length < 1) {                        // nothing to do? good
        return E_BIGBUF;
    }

    takeCache(slot);

    int fetched = 0;
    while(1) {
        uint8_t *b = lineCache;
        int i;

        for(i = ci->charsUsed; i < ci->charsGot; i++) {     // a complete line here?
            if(b[i] == (uint8_t) delimiter) {
                break;
            }
        }

        if(i < ci->charsGot) {
            int len = i - ci->charsUsed;

            if(len >= length) {             // the line + terminator doesn't fit - it stays, as in STinG
                return E_BIGBUF;
            }

            memcpy(buffer, b + ci->charsUsed, len);
            buffer[len]     = 0;
            ci->charsUsed   = i + 1;        // the line and the delimiter
            forceNextUpdateConInfo = TRUE;
            return len;
        }

        if(fetched) {                       // fresh data, still no complete line
            if(ci->charsGot < CN_PEEK_MAX) {
                return E_NODATA;            // the rest of the line didn't come yet
            }
            break;                          // longer than the cache - let the host look for it
        }

        if(!fillLocal(slot, handle, CN_PEEK_MAX)) {    // the used part goes away, the rest comes again with more
            return E_NODATA;
        }
        fetched = 1;
    }

    if(length <= CN_PEEK_MAX) {             // the line is longer than the cache, so longer than the buffer too
        return E_BIGBUF;
    }

    //-----------------------
    // limit the length - the reply is length (W) + line + terminator
    length              = (length < DMA_BUFFER_SIZE - 2) ? length : DMA_BUFFER_SIZE - 2;
    int sectorLength    = (length + 2 + 511) >> 9;
    //-----------------------
    // issue the command
    uint8_t used = usedForCommand(slot, handle);    // first - above 255 it sends its own command

    commandLong[ 5] = NET_CMD_CNGETS;       // store function number
    commandLong[ 6] = handle;                // store file handle
    commandLong[ 7] = (uint8_t) (length >> 8); // store max length
    commandLong[ 8] = (uint8_t) (length     );
    commandLong[ 9] = delimiter;            // store delimiter
    commandLong[10] = used;                 // how many chars were used

    ceCmd(ACSI_READ, commandLong, CMD_LENGTH_LONG, pDmaBuffer, sectorLength);

    if(!hdIf.success) {                     // communication failed? quit
        return E_NODATA;
    }

    if(hdIf.statusByte == E_NORMAL) {       // got the string? good
        int len = getWord(pDmaBuffer);      // the line can contain 0 bytes - not strlen()
        if(len >= length) {
            len = length - 1;
        }
        memcpy(buffer, pDmaBuffer + 2, len);
        buffer[len] = 0;
        forceNextUpdateConInfo = TRUE;      // force update_con_info() if asked to do it next time.
        return len;
    }

    // in other cases - just return the status byte
    return extendByteToWord(hdIf.statusByte);
}

//--------------------------------------
// helper functions

int handle_valid(int16 handle)
{
    if(!network_handleIsValid(handle)) {        // handle out of range?
        return FALSE;
    }

    int slot    = network_handleToSlot(handle);
    uint16_t proto  = getCIBitem((uint8_t *) &conInfo[slot].cib, CIB_PROTO);

    if(proto == 0) {                // if connection not used
        return FALSE;
    }

    return TRUE;
}

void update_con_info(uint8_t forceUpdate)
{
    static uint32_t lastUpdate = 0;
    uint32_t now = ceTicks();

    if(!forceUpdate) {                                                      // if shouldn't force update, check last update time, and possibly ignore update
        if((now - lastUpdate) < 20) {                                        // if the last update was less than 100 ms ago, don't update
            return;
        }
    }

    lastUpdate = now;                                                        // mark that we've just updated the ceDrives

    //---------------
    if(fromVbl) {
        int gotConnection   = tcpUdpGotSomeConnection();                    // return TRUE if there is a valid connection (in or out), return FALSE otherwise
        int gotIcmpHandler  = icmpGotSomeHandler();                         // return TRUE if there is a valid ICMP handler, return FALSE otherwise

        if(!gotConnection && !gotIcmpHandler) {                             // no connection, no ICMP handler? Don't update info.
            return;
        }
    }
    //---------------

    // now do the real update
    commandShort[4] = NET_CMD_CN_UPDATE_INFO;                                // store function number
    #define UPDATE_INFO_SECTORS ((15 * NET_HANDLES_COUNT + 4 + 511) / 512)
    commandShort[5] = 0;

    ceCmd(ACSI_READ, commandShort, CMD_LENGTH_SHORT, pDmaBuffer, UPDATE_INFO_SECTORS);   // send command to host over ACSI

    if(!hdIf.success || hdIf.statusByte != E_NORMAL) {                                                            // error?
        return;
    }

    // N = NET_HANDLES_COUNT: N x DW bytes to read, N x B state, N x W local port, N x DW remote host, N x W remote port,
    // N x B error (reported once), N x B in use on host, DW ICMP bytes waiting
    #define N   NET_HANDLES_COUNT
    uint8_t   *pBytesToRead       = pDmaBuffer;
    uint8_t   *pConnStatus        = pDmaBuffer +  4 * N;
    uint8_t   *pLPort             = pDmaBuffer +  5 * N;
    uint8_t   *pRHost             = pDmaBuffer +  7 * N;
    uint8_t   *pRPort             = pDmaBuffer + 11 * N;
    uint8_t   *pError             = pDmaBuffer + 13 * N;
    uint8_t   *pInUse             = pDmaBuffer + 14 * N;
    uint32_t  *pBytesToReadIcmp   = (uint32_t *) (pDmaBuffer + 15 * N);
    #undef N

    int i;
    for(i=0; i<MAX_HANDLE; i++) {
        TConInfo *ci = &conInfo[i];

        if(getCIBitem((uint8_t *) &ci->cib, CIB_PROTO) == 0) {     // not used on the ST
            continue;
        }

        if(!pInUse[i]) {                                            // the host doesn't have it any more (closed, linger ended)
            initConInfoStruct(i);
            continue;
        }

        ci->bytesToRead         = getDword(pBytesToRead + i * 4);
        ci->tcpConnectionState  = pConnStatus[i];

        if(pError[i]) {                                             // the host reports it once - keep it until used
            ci->netError = (int8_t) pError[i];
        }

        uint16_t lPort = getWord (pLPort + i * 2);
        uint32_t rHost = getDword(pRHost + i * 4);
        uint16_t rPort = getWord (pRPort + i * 2);

        uint32_t lHost = localIP;                                   // STinG 1.26 has its IP there also while listening

        // CIB update through helper functions because of Pure C vs gcc packing of structs. The protocol stays.
        setCIB((uint8_t *) &ci->cib, NO_CHANGE_W, lPort, rPort, rHost, lHost, 0);
    }

    uint32_t bytesToReadIcmp = (uint32_t) *pBytesToReadIcmp;                          // get how many bytes we can read from ICMP socket(s)

    if(bytesToReadIcmp > 0) {                                                   // if we have something for ICMP to process?
        icmp_processData(bytesToReadIcmp);
    }
}

//-------------------------------------------------------------------------------

int16 connection_open(int tcpNotUdp, uint32 rem_host, uint16 rem_port, uint16 tos, uint16 buff_size)
{
    // first store command code
    if(tcpNotUdp) {                         // open TCP
        commandShort[4] = NET_CMD_TCP_OPEN;
    } else {                                // open UDP
        commandShort[4] = NET_CMD_UDP_OPEN;
    }

    commandShort[5] = 0;

    uint16_t  lPort = 0;
    uint32_t lHost = 0;

    //--------------------------
    // find out if it's active or passive connection
    uint8_t activeNotPassive = TRUE;       // active by default

    if(tcpNotUdp) {                     // for TCP
        if(rem_host == 0 || rem_port == TCP_PASSIVE) {      // if no remote host specified, or specified TCP_PASSIVE, it's passive connection
            activeNotPassive = FALSE;
        }
    }
    //--------------------------

    if(rem_port == TCP_ACTIVE || rem_port == TCP_PASSIVE) {     // if the remote port is special flag (passive or active), then remote host contains pointer to CAB structure
        if(rem_host != 0) {                                     // it's not a NULL pointer
            uint8_t *pCAB = (uint8_t *) rem_host;                     // convert int to pointer and retrieve struct values

            lPort       = getWord (pCAB + 0);
            rem_port    = getWord (pCAB + 2);
            rem_host    = getDword(pCAB + 4);
            lHost       = getDword(pCAB + 8);
        }
    }

    // then store the params in buffer
    uint8_t *pBfr = pDmaBuffer;
    pBfr = storeDword   (pBfr, rem_host);       //  0 ..  3
    pBfr = storeWord    (pBfr, rem_port);       //  4 ..  5
    pBfr = storeWord    (pBfr, tos);            //  6 ..  7
    pBfr = storeWord    (pBfr, buff_size);      //  8 ..  9
    pBfr = storeDword   (pBfr, lHost);          // 10 .. 13
    pBfr = storeWord    (pBfr, lPort);          // 14 .. 15

    // send it to host
    ceCmd(ACSI_WRITE, commandShort, CMD_LENGTH_SHORT, pDmaBuffer, 1);

    if(network_handleIsValid(hdIf.statusByte)) {            // if it's CE handle
        int netHandle   = hdIf.statusByte;
        int slot        = network_handleToSlot(netHandle);
        TConInfo *ci = &conInfo[slot];

        initConInfoStruct(slot);                            // init struct

        int proto, status;

        // store info to CIB and CAB structures
        if(tcpNotUdp) {
            proto = TCP;
        } else {
            proto = UDP;
        }

        uint32_t local_host;
        if(activeNotPassive) {      // active connection - trying to connect now, local host is local IP
            status      = TSYN_SENT;
            local_host  = localIP;
        } else {                    // passive connection - listening, local host is our IP too (as STinG 1.26)
            status      = TLISTEN;
            local_host  = localIP;
        }

        // local port to 0 - we currently don't know that (yet)
        setCIB((uint8_t *) &ci->cib, proto, lPort, rem_port, rem_host, local_host, status);
        ci->tcpConnectionState  = status;
        ci->hostInUse           = 1;
        ci->closing             = 0;
        ci->netError            = 0;
        update_con_info(TRUE);                      // may already bring E_REFUSE - must stay for TCP_wait_state()                      // let's FORCE update connection info - CIB should be updated with real local port after this (e.g. aFTP relies on the info when using active ftp connection)

        ci->buff_size           = buff_size;        // store the maximu buffer size fot TCP
        ci->activeNotPassive    = activeNotPassive; // store active/passive flag

        if(!vblInstalled) {                         // from now on the VBL keeps the CIBs up to date, as STinG does in its
            Supexec(install_vbl);                   // interrupt - apps watch CNgetinfo()->rhost while waiting for a connection
        }

        return netHandle;                           // return the new handle
    }

    // it's not a CE handle
    return extendByteToWord(hdIf.statusByte);       // extend the uint8_t error code to uint16_t
}

//-------------------------------------------------------------------------------

int16 connection_close(int tcpNotUdp, int16 handle, int16 timeout)
{
    if(!handle_valid(handle)) {                     // we don't have this handle? fail
        return E_BADHANDLE;
    }
    int slot = network_handleToSlot(handle);
    TConInfo *ci = &conInfo[slot];

    if(getCIBitem((uint8_t *) &ci->cib, CIB_PROTO) != (tcpNotUdp ? TCP : UDP)) {     // TCP_close() of UDP and v.v.
        return E_BADHANDLE;
    }

    commandShort[4] = tcpNotUdp ? NET_CMD_TCP_CLOSE : NET_CMD_UDP_CLOSE;
    commandShort[5] = 0;

    uint8_t *pBfr = pDmaBuffer;
    pBfr = storeWord    (pBfr, handle);
    pBfr = storeWord    (pBfr, timeout);

    ceCmd(ACSI_WRITE, commandShort, CMD_LENGTH_SHORT, pDmaBuffer, 1);

    if(!hdIf.success) {                             // can't talk to the host - forget the handle
        initConInfoStruct(slot);
        return E_NORMAL;
    }

    int16 res = extendByteToWord(hdIf.statusByte);

    if(res == E_BADCLOSE || res == E_BADHANDLE) {   // already closed and lingering / not there
        return res;
    }

    if(!tcpNotUdp) {                                // UDP: gone right away
        initConInfoStruct(slot);
        return res;
    }

    ci->closing  = 1;                               // TCP: the host may keep it for a while (TTIME_WAIT, TLAST_ACK)
    ci->netError = 0;
    update_con_info(TRUE);                          // if the host freed it already, the CIB gets cleared here
    return res;                                     // E_NORMAL, or E_RRESET after a reset
}

//-------------------------------------------------------------------------------

int16 connection_send(int tcpNotUdp, int16 handle, void *buffer, int16 length)
{
    if(!handle_valid(handle)) {                     // we don't have this handle? fail
        return E_BADHANDLE;
    }
    int slot = network_handleToSlot(handle);
    TConInfo *ci = &conInfo[slot];

    if(getCIBitem((uint8_t *) &ci->cib, CIB_PROTO) != (tcpNotUdp ? TCP : UDP)) {     // TCP_send() on UDP and v.v.
        return E_BADHANDLE;
    }

    if(tcpNotUdp) {                                 // the STinG 1.26 order: error, closed / listening, buffer size
        update_con_info(FALSE);

        int16 err = takeError(ci);
        if(err) {
            return err;
        }

        if(ci->tcpConnectionState == TLISTEN) {
            return E_LISTEN;
        }

        if(ci->tcpConnectionState == TCLOSED || ci->tcpConnectionState >= TFIN_WAIT1) {
            if(ci->tcpConnectionState != TCLOSE_WAIT) {     // the remote closed only its side - sending is fine
                return E_EOF;
            }
        }

        if(length > ci->buff_size) {                // more than the buffer size from TCP_open()
            return E_OBUFFULL;
        }
    }

    // first store command code
    uint8_t netCmd;
    if(tcpNotUdp) {                         // for TCP
        netCmd = NET_CMD_TCP_SEND;
    } else {                                // for UDP
        netCmd = NET_CMD_UDP_SEND;
    }

    // transfer the remaining data in a loop
    // FAST RAM, or not aligned as the DMA needs (odd addresses are handled by connection_send_block() on ACSI / Falcon,
    // but TT SCSI needs longword alignment) - through the middle buffer
    uint8_t  toFastRam = ceNeedsBounce(buffer) && !(hdIf.dmaAlignMask == 1 && ((uint32_t) buffer) < 0x01000000);
    uint32_t blockSize = toFastRam ? FASTRAM_BUFFER_SIZE : (MAXSECTORS * 512); // size of block, which we will read

    while(length > 0) {                                            // while there's something to send
        // calculate how much data we should transfer in this loop - with respect to MAX SECTORS we can transfer at once
        uint32_t thisWriteSizeBytes = (length < blockSize) ? length : blockSize; // will the needed write size within the blockSize, or not?

        if(toFastRam) {     // if writing from FAST RAM, first cop to fastRamBuffer, and then write using DMA
            memcpy(FastRAMBuffer, buffer, thisWriteSizeBytes);
            connection_send_block(netCmd, handle, thisWriteSizeBytes, FastRAMBuffer);
        } else {            // if writing from ST RAM, just writing directly there
            connection_send_block(netCmd, handle, thisWriteSizeBytes, buffer);
        }

        buffer            += thisWriteSizeBytes;                    // and move pointer in buffer further
        length            -= thisWriteSizeBytes;                    // decrease the count that is remaining

        if(!hdIf.success) {
            return E_LOSTCARRIER;
        }
        if(hdIf.statusByte != E_NORMAL) {
            break;
        }
    }

    return extendByteToWord(hdIf.statusByte);           // return the status, possibly extended to int16
}

void connection_send_block(uint8_t netCmd, uint8_t handle, uint16_t length, uint8_t *buffer)
{
    commandLong[5] = netCmd;                        // store command code
    commandLong[6] = handle;                        // store handle

    commandLong[7] = (uint8_t) (length >> 8);          // cmd[7 .. 8]  = length
    commandLong[8] = (uint8_t) (length     );

    // prepare the command for buffer sending - add flag 'buffer address is odd' and possible byte #0, in case the buffer address was odd
    uint8_t *pBfr  = (uint8_t *) buffer;
    uint32_t dwBfr = (uint32_t)  buffer;

    if(dwBfr & 1) {                                 // buffer pointer is ODD
        commandLong[9]  = TRUE;                     // buffer is odd
        commandLong[10] = pBfr[0];                  // byte #0 is cmd[10]
        pBfr++;                                     // and pBfr is now EVEN
    } else {                                        // buffer pointer is EVEN
        commandLong[9]  = FALSE;                    // buffer is even
        commandLong[10] = 0;                        // this is zero, not byte #0
    }

    // calculate sector count
    uint16_t sectorCount = length >> 9;                 // get number of sectors we need to send

    if((length & 0x1ff) != 0) {                     // if the number of bytes is not multiple of 512, then we need to send one sector more
        sectorCount++;
    }

    // send it to host
    ceCmd(ACSI_WRITE, commandLong, CMD_LENGTH_LONG, pBfr, sectorCount);
}

//-------------------------------------------------------------------------------

int16 resolve (char *domain, char **real_domain, uint32 *ip_list, int16 ip_num)
{
    commandShort[4] = NET_CMD_RESOLVE;
    commandShort[5] = 0;

    if(domain == NULL) {                                                // if nothing to resolve, quit
        return E_CANTRESOLVE;
    }

    strcpy((char *) pDmaBuffer, domain);                                // copy in the domain or dotted quad IP address

    // send it to host
    ceCmd(ACSI_WRITE, commandShort, CMD_LENGTH_SHORT, pDmaBuffer, 1);

    if(!hdIf.success) {
        return E_CANTRESOLVE;
    }

    if(hdIf.statusByte >= 10) {                                          // 0 .. 9 is handle, 10 and more means error
        return E_CANTRESOLVE;
    }

    uint8_t resolverHandle = hdIf.statusByte;                              // this is the new handle for resolver

    // now receive the response
    memset(pDmaBuffer, 0, 512);
    commandShort[4] = NET_CMD_RESOLVE_GET_RESPONSE;
    commandShort[5] = resolverHandle;

    uint32_t end = ceTicks() + 10 * 200;                                  // 10 second timeout

    while(1) {                                                          // repeat this command few times, as it might reply with 'I didn't finish yet'
        msleep(250);                                                    // wait 250 ms before trying again
        if(ceTicks() >= end) {                                         // if timeout
            return E_CANTRESOLVE;
        }

        ceCmd(ACSI_READ, commandShort, CMD_LENGTH_SHORT, pDmaBuffer, 1);

        if(hdIf.statusByte == E_NORMAL) {                               // if finished, continue after loop
            break;
        }

        if(hdIf.statusByte == RES_DIDNT_FINISH_YET) {                   // if not finished, try again
            continue;
        }
    }

    // now copy the list of IPs to ip_list
    int ipListCount = (int) pDmaBuffer[256];                            // get how many IPs we got from host
    int ipCount     = (ip_num < ipListCount) ? ip_num : ipListCount;    // get the lower count - either what we may store (ip_num) or what we found (ipListCount)

    int i;
    uint32 *pIPs = (uint32 *) &pDmaBuffer[258];
    for(i=0; i<ipCount; i++) {                                          // copy all the IPs
        ip_list[i] = pIPs[i];
    }

    if(ipCount == 0) {                                                  // no IPs? can't resolve
        if(real_domain != NULL) {                                       // as STinG: 'real' is set even then
            char *realName = KRmalloc_internal(4);
            if(realName) {
                realName[0] = 0;
                *real_domain = realName;
            }
        }
        return E_CANTRESOLVE;
    }

    // possibly copy the real domain name
    if(real_domain != NULL) {                                           // if got pointer to real domain
        char *realName = KRmalloc_internal(512);
        if(!realName) {                                                 // failed to allocate RAM?
            return E_NOMEM;
        }

        strcpy(realName, (char *) pDmaBuffer);                          // copy in the real name
        *real_domain = realName;                                        // store the pointer to where the real domain should be
    }

    return ipCount;                                                     // Returns the number of dotted quad IP addresses filled in, or an error.
}

//-------------------------------------------------------------------------------
static void initConInfoStruct(int i)
{
    if(i >= MAX_HANDLE) {
        return;
    }

    TConInfo *ci = &conInfo[i];

    ci->bytesToRead          = 0;
    ci->tcpConnectionState   = TCLOSED;
    ci->charsUsed            = 0;
    ci->charsGot             = 0;
    ci->netError             = 0;
    if(lineCacheOwner == i) {
        lineCacheOwner = -1;
    }
    ci->hostInUse            = 0;
    ci->closing              = 0;
    setCIB((uint8_t *) &ci->cib, 0, 0, 0, 0, 0, 0);    // clear the CIB structure
    memset(ci->chars, 0, READ_BUFFER_SIZE);
}
//-------------------------------------------------------------------------------
void init_con_info(void)
{
    int i;

    for(i=0; i<MAX_HANDLE; i++) {
        initConInfoStruct(i);
    }
}
//-------------------------------------------------------------------------------
void setCIB(uint8_t *cib, uint16_t protocol, uint16_t lPort, uint16_t rPort, uint32_t rHost, uint32_t lHost, uint16_t status)
{
    // first create pointers to the right addresses
    uint16_t  *pProto   = (uint16_t  *) (cib +  0);
    uint16_t  *plPort   = (uint16_t  *) (cib +  2);
    uint16_t  *prPort   = (uint16_t  *) (cib +  4);
    uint32_t *prHost   = (uint32_t *) (cib +  6);
    uint32_t *plHost   = (uint32_t *) (cib + 10);
    uint16_t  *pStatus  = (uint16_t  *) (cib + 14);

    // now if we should set the new value, set it
    if(protocol != NO_CHANGE_W)     *pProto     = protocol;
    if(lPort    != NO_CHANGE_W)     *plPort     = lPort;
    if(rPort    != NO_CHANGE_W)     *prPort     = rPort;
    if(rHost    != NO_CHANGE_DW)    *prHost     = rHost;
    if(lHost    != NO_CHANGE_DW)    *plHost     = lHost;
    if(status   != NO_CHANGE_W)     *pStatus    = status;
}
//-------------------------------------------------------------------------------
uint32_t getCIBitem(uint8_t *cib, uint8_t item)
{
    // first create pointers to the right addresses
    uint16_t  *pProto   = (uint16_t  *) (cib +  0);
    uint16_t  *plPort   = (uint16_t  *) (cib +  2);
    uint16_t  *prPort   = (uint16_t  *) (cib +  4);
    uint32_t *prHost   = (uint32_t *) (cib +  6);
    uint32_t *plHost   = (uint32_t *) (cib + 10);
    uint16_t  *pStatus  = (uint16_t  *) (cib + 14);

    switch(item) {
        case CIB_PROTO:     return *pProto;
        case CIB_LPORT:     return *plPort;
        case CIB_RPORT:     return *prPort;
        case CIB_RHOST:     return *prHost;
        case CIB_LHOST:     return *plHost;
        case CIB_STATUS:    return *pStatus;
    }

    return 0;
}
//-------------------------------------------------------------------------------
