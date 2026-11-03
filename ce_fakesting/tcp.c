// vim: shiftwidth=4 softtabstop=4 tabstop=4 expandtab
//----------------------------------------
// CosmosEx fake STiNG - by Jookie, 2014
// Based on sources of original STiNG
//----------------------------------------

#include <mint/sysbind.h>
#include <mint/osbind.h>
#include <mint/basepage.h>
#include <mint/ostruct.h>
#include <support.h>
#include <stdint.h>
#include <stdio.h>

#include "globdefs.h"
#include "tcp.h"
#include "con_man.h"

//---------------------
// ACSI / CosmosEx stuff
#include "../libacsiscsi/acsi.h"
#include "../libacsiscsi/stdlib.h"
#include "ce_commands.h"
#include "stdlib2.h"
#include "cecmd.h"

//---------------------

extern TConInfo conInfo[MAX_HANDLE];                   // this holds info about each connection

int tcpUdpGotSomeConnection(void);                      // return TRUE if there is a valid connection (in or out), return FALSE otherwise

//---------------------
int16 TCP_send(int16 handle, void *buffer, int16 length)
{
    int16 res = connection_send(1, handle, buffer, length);
    return res;
}

// TCP handle? (TCP functions on a UDP handle give E_BADHANDLE, as in STinG)
static int tcpHandle(int16 handle)
{
    if(!handle_valid(handle)) {
        return FALSE;
    }

    return getWordByByteOffset(&conInfo[network_handleToSlot(handle)].cib, 0) == TCP;     // CIB.protocol
}

int16 TCP_wait_state(int16 handle, int16 wantedState, int16 timeout)
{
    if(!tcpHandle(handle)) {                            // we don't have this handle? fail
        return E_BADHANDLE;
    }
    int slot = network_handleToSlot(handle);

    update_con_info(TRUE);
    if(conInfo[slot].netError) {                        // refused / reset - STinG returns it (once) from here too
        int16 err = conInfo[slot].netError;
        conInfo[slot].netError = 0;
        return err;
    }

    uint32_t timeStart = ceTicks();
    uint32_t timeout2 = timeout * 200;
    uint32_t nextConUpdate = ceTicks() + 100;

    while(1) {
        // the connection info should be updated in VBL using update_con_info()
        if(ceTicks() >= nextConUpdate) {                           // if half a second passed since last connection state check
            update_con_info(FALSE);
            nextConUpdate = ceTicks() + 100;
        }

        uint16_t currentState = conInfo[slot].tcpConnectionState;       // get the current state

        // if the wanted state is CLOSED or CLOSING, and the current state is similar - success
        if(wantedState == TCLOSED || wantedState == TFIN_WAIT1 ||  wantedState == TFIN_WAIT2 ||  wantedState == TCLOSE_WAIT ||  wantedState == TCLOSING ||  wantedState == TLAST_ACK ||  wantedState == TTIME_WAIT) {
            if(currentState == TCLOSED || currentState == TFIN_WAIT1 ||  currentState == TFIN_WAIT2 ||  currentState == TCLOSE_WAIT ||  currentState == TCLOSING ||  currentState == TLAST_ACK ||  currentState == TTIME_WAIT) {
                break;
            }
        }

        // if the wanted state is CONNECTING, and the current state is similar - success
        if(wantedState == TSYN_SENT || wantedState == TSYN_RECV) {
            if(currentState == TSYN_SENT || currentState == TSYN_RECV) {
                break;
            }
        }

        // for other matching cases, success
        if(wantedState == currentState) {
            break;
        }

        if(conInfo[slot].netError) {                        // refused / reset while waiting
            int16 err = conInfo[slot].netError;
            conInfo[slot].netError = 0;
            return err;
        }

        if((ceTicks() - timeStart) >= timeout2) {              // if timed out, fail
            return E_CNTIMEOUT;
        }

        //appl_yield();                                           // keep GEM apps responding -- commented out, because causes 'Bad Function #' error
    }

    return E_NORMAL;
}

int16 TCP_ack_wait(int16 handle, int16 timeout)
{
    if(!tcpHandle(handle)) {                                    // we don't have this handle? fail
        return E_BADHANDLE;
    }

    // GlueSTiK inspired -- incompatibility:  Does nothing. Linux handles this internally.
    return E_NORMAL;
}

// TCPIB (Pure C layout): request (DW) @0, state (W) @4, unacked (DW) @6, srtt (DW) @10
#define TCPI_state      0x00000001L
#define TCPI_unacked    0x00000002L
#define TCPI_srtt       0x00000004L
#define TCPI_defer      0x00000008L
#define TCPI_bits       4
#define TCPI_mask       0x0000000FL

int16 TCP_info(int16 handle, TCPIB *tcp_info)
{
    if(!tcpHandle(handle)) {                                    // we don't have this handle? fail
        return E_BADHANDLE;
    }
    int slot = network_handleToSlot(handle);

    if(tcp_info == NULL) {
        return E_PARAMETER;
    }

    uint32_t request = getDwordByByteOffset(tcp_info, 0);
    if(request & ~TCPI_mask) {                                  // undefined bits - as STinG
        return E_PARAMETER;
    }

    update_con_info(FALSE);

    if(request & TCPI_state) {
        setWordByByteOffset(tcp_info, 4, conInfo[slot].tcpConnectionState);
    }
    if(request & TCPI_unacked) {                                // the host doesn't tell - nothing unacked
        setDwordByByteOffset(tcp_info, 6, 0);
    }
    if(request & TCPI_srtt) {
        setDwordByByteOffset(tcp_info, 10, 0);
    }

    return TCPI_bits;                                           // as STinG: the count of defined request bits
}

int tcpUdpGotSomeConnection(void)                // return TRUE if there is a valid connection (in or out), return FALSE otherwise
{
    int i;
    for(i=0; i<MAX_HANDLE; i++) {
        if(conInfo[i].tcpConnectionState != TCLOSED) {  // if found a connection, which is not closed, return TRUE
            return TRUE;
        }
    }

    return FALSE;                               // no open connection was found
}
