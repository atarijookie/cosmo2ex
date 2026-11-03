#ifndef _NETADAPTER_COMMANDS_H_
#define _NETADAPTER_COMMANDS_H_

// Commands of the CosmosEx network module (HOSTMOD_NETWORK_ADAPTER), sent by the fake STinG (../../../ce_fakesting,
// ce_commands.h there has the same numbers). From ce-atari/ce_main_app/network/netadapter_commands.h.
//
// Command format, pCmd as NetAdapter::processCommand() gets it (for ICD commands the 1st byte is already skipped):
//   short: [0] ID  [1] 'C' [2] 'E' [3] HOSTMOD [4] cmd [5] arg
//   long:  [0] xx  [1] 'C' [2] 'E' [3] HOSTMOD [4] cmd [5..] args
// The status byte is E_* (as uint8_t), or a connection handle (NET_STARTING_HANDLE ..) from TCP_OPEN / UDP_OPEN, or a
// resolver slot from RESOLVE.

// 0x0100 - ce-atari version
// 0x0101 - STinG 1.26 like states and errors: CN_UPDATE_INFO has per handle also the error (reported once) and the
//          'in use' flag, 96 handles (count in IDENTIFY), TCP_CLOSE gets the timeout, CNGETS reply starts with the length
#define REQUIRED_NETADAPTER_VERSION     0x0101      // in IDENTIFY reply, fake STinG refuses to install on mismatch

#define NET_CMD_IDENTIFY                0x00        // reply: "CosmosEx network module" (24 B), padding to 32, version (W), local IP (DW), handle count (W)

// TCP functions
#define NET_CMD_TCP_OPEN                0x10        // data: rem host (DW, 0 = listen), rem port (W), tos (W), buff_size (W), .., local port (W @ 14)
#define NET_CMD_TCP_CLOSE               0x11        // data: handle (W), timeout (W); status E_NORMAL / E_RRESET / E_BADCLOSE / E_BADHANDLE
#define NET_CMD_TCP_SEND                0x12        // [5] handle, [6..7] length, [8] isOdd, [9] oddByte + data
#define NET_CMD_TCP_WAIT_STATE          0x13        // not used on host
#define NET_CMD_TCP_ACK_WAIT            0x14        // not used on host
#define NET_CMD_TCP_INFO                0x15        // not used on host

// UDP functions - same parameters as the TCP ones
#define NET_CMD_UDP_OPEN                0x20
#define NET_CMD_UDP_CLOSE               0x21
#define NET_CMD_UDP_SEND                0x22

// ICMP functions
#define NET_CMD_ICMP_SEND_EVEN          0x30        // [5..8] dest IP, [9] type << 3 | code, [10..11] length + data
#define NET_CMD_ICMP_SEND_ODD           0x31        // same, data starts 1 byte later in the transfer
#define NET_CMD_ICMP_HANDLER            0x32        // not used on host
#define NET_CMD_ICMP_DISCARD            0x33        // not used on host
#define NET_CMD_ICMP_GET_DGRAMS         0x34        // [5] sectors; reply: (W length, IP_DGRAM + ICMP data) * n, W 0

// connection manager
#define NET_CMD_CNKICK                  0x40        // not used on host
#define NET_CMD_CNBYTE_COUNT            0x41        // not used on host - comes from CN_UPDATE_INFO
#define NET_CMD_CNGET_CHAR              0x42        // [5] handle, [9] charsUsed; reply: count (B) + up to 250 bytes
#define NET_CMD_CNGET_NDB               0x43        // [5] handle, [6] 0 = size (DW bytes, B sectors) / 1 = data, [9] charsUsed
#define NET_CMD_CNGET_BLOCK             0x44        // [5] handle, [6..7] length, [9] charsUsed
#define NET_CMD_CNGETINFO               0x45        // not used on host - comes from CN_UPDATE_INFO
#define NET_CMD_CNGETS                  0x46        // [5] handle, [6..7] max length, [8] delimiter, [9] charsUsed; reply: length (W) + line + 0
#define NET_CMD_CN_UPDATE_INFO          0x47        // reply: see NetAdapter::conUpdateInfo()

// misc
#define NET_CMD_RESOLVE                 0x50        // data: name (C string); status: resolver slot
#define NET_CMD_ON_PORT                 0x51        // not used on host
#define NET_CMD_OFF_PORT                0x52        // not used on host
#define NET_CMD_QUERY_PORT              0x53        // not used on host
#define NET_CMD_CNTRL_PORT              0x54        // not used on host
#define NET_CMD_RESOLVE_GET_RESPONSE    0x55        // [5] slot; reply: real name (256 B), count (B), 0, IPs (DW each)

#define RES_DIDNT_FINISH_YET            0xdf        // status of RESOLVE_GET_RESPONSE while the resolve runs

#endif
