#ifndef _STING_H_
#define _STING_H_

// The part of the STinG API (STinG 1.26 include/transprt.h, stx.h, port.h) a port driver needs, rewritten for gcc.
//
// STinG and its modules are built with Pure C. Its cdecl functions take the arguments on stack with their real size
// (int16 = 2 bytes), return in d0 (pointers too) and may destroy d0-d2/a0-a2. gcc puts every argument on stack as 4 bytes
// and expects d2/a2 to survive, so all calls into STinG go through pc_call_*() in purec.s, and the two driver functions
// STinG calls with int16 arguments (set_state, cntrl) are entered through pc_set_state / pc_cntrl there.
// The structures have the same layout in both compilers (m68k: 16 bit ints, 2 byte alignment of longs and pointers),
// except for bit fields - IP_HDR uses plain bytes here, checked by the asserts below.

#include <stdint.h>
#include <stddef.h>
#include <mint/basepage.h>

typedef int16_t     int16;
typedef uint16_t    uint16;
typedef int32_t     int32;
typedef uint32_t    uint32;
typedef uint8_t     uint8;

#ifndef TRUE
#define TRUE    1
#define FALSE   0
#endif

//--------------------------------------------------------------------------------
// IP datagram

typedef struct {
    uint8   ver_hd_len;         // version (upper 4 bits), header length in longs (lower 4 bits)
    uint8   tos;
    uint16  length;             // header + options + data
    uint16  ident;
    uint16  frag;               // flags (upper 3 bits) + fragment offset
    uint8   ttl;
    uint8   protocol;
    uint16  hdr_chksum;
    uint32  ip_src;
    uint32  ip_dest;
} IP_HDR;

typedef struct ip_packet {
    IP_HDR  hdr;
    void   *options;
    int16   opt_length;
    void   *pkt_data;
    int16   pkt_length;
    uint32  timeout;
    uint32  ip_gateway;         // next hop for packets not on local network
    void   *recvd;              // port that received it
    struct ip_packet *next;
} IP_DGRAM;

_Static_assert(sizeof(IP_HDR) == 20, "IP_HDR must match STinG");
_Static_assert(offsetof(IP_DGRAM, pkt_data) == 26, "IP_DGRAM must match STinG");
_Static_assert(offsetof(IP_DGRAM, next) == 44, "IP_DGRAM must match STinG");

//--------------------------------------------------------------------------------
// port and driver

struct drv_desc;

typedef struct port_desc {
    const char *name;
    int16   type;
    int16   active;
    uint32  flags;
    uint32  ip_addr;
    uint32  sub_mask;
    uint16  mtu;
    uint16  max_mtu;
    int32   stat_sd_data;
    IP_DGRAM *send;             // datagrams to send, filled by kernel
    int32   stat_rcv_data;
    IP_DGRAM *receive;          // received datagrams, filled by driver
    int16   stat_dropped;
    struct drv_desc *driver;
    struct port_desc *next;
} PORT;

typedef struct drv_desc {
    int16   (*set_state)(PORT *port, int16 enable);         // Pure C cdecl - see pc_set_state
    int16   (*cntrl)(PORT *port, uint32 arg, int16 code);   // Pure C cdecl - see pc_cntrl
    void    (*send)(PORT *port);
    void    (*receive)(PORT *port);
    const char *name;
    const char *version;        // "xx.yy"
    uint16  date;               // GEMDOS format
    const char *author;
    struct drv_desc *next;
    BASEPAGE *basepage;
} DRIVER;

_Static_assert(offsetof(PORT, stat_dropped) == 40, "PORT must match STinG");
_Static_assert(offsetof(DRIVER, date) == 24, "DRIVER must match STinG");

#define L_SER_BUS       3       // port type: serial bus type link (ethernet)

//--------------------------------------------------------------------------------
// kernel tables - only the part used here, the rest is not touched

typedef struct {
    const char *module;
    const char *author;
    const char *version;
    void   *KRmalloc;           // void *  KRmalloc (int32 length)
    void   *KRfree;             // void    KRfree (void *block)
    void   *KRgetfree;          // int32   KRgetfree (int16 largest_block)
} TPL;

typedef struct {
    const char *module;
    const char *author;
    const char *version;
    void   *set_dgram_ttl;      // void    set_dgram_ttl (IP_DGRAM *)
    void   *check_dgram_ttl;    // int16   check_dgram_ttl (IP_DGRAM *)
    void   *load_routing_table;
    void   *set_sysvars;
    void   *query_chains;       // void    query_chains (PORT **, DRIVER **, LAYER **)
    void   *IP_send;
    void   *IP_fetch;
    void   *IP_handler;
    void   *IP_discard;         // void    IP_discard (IP_DGRAM *, int16 all)
    void   *PRTCL_announce;
    void   *PRTCL_get_parameters;
    void   *PRTCL_request;
    void   *PRTCL_release;
    void   *PRTCL_lookup;
    void   *TIMER_call;
    void   *TIMER_now;          // int32   TIMER_now (void)
    void   *TIMER_elapsed;      // int32   TIMER_elapsed (int32 then)
} STX;

typedef struct {
    char    magic[10];          // STIK_DRVR_MAGIC
    void   *get_dftab;          // void *  get_dftab (const char *name)
    void   *ETM_exec;
    void   *cfg;
    BASEPAGE *sting_basepage;
} DRV_LIST;

#define STIK_COOKIE_MAGIC   0x5354694bUL        // 'STiK'
#define STIK_DRVR_MAGIC     "STiKmagic"
#define TRANSPORT_DRIVER    "TRANSPORT_TCPIP"
#define MODULE_DRIVER       "MODULE_LAYER"

extern TPL *tpl;
extern STX *stx;

//--------------------------------------------------------------------------------
// calls into Pure C code, see purec.s

int32 pc_call_0  (void *fn);
int32 pc_call_l  (void *fn, int32 a);                   // fn (long / pointer)
int32 pc_call_w  (void *fn, int32 w);                   // fn (int16)
int32 pc_call_lw (void *fn, int32 a, int32 w);          // fn (long / pointer, int16)
int32 pc_call_lll(void *fn, int32 a, int32 b, int32 c); // fn (long / pointer, long / pointer, long / pointer)

#define KRmalloc(len)           ((void *) pc_call_l(tpl->KRmalloc, (int32) (len)))
#define KRfree(blk)             ((void) pc_call_l(tpl->KRfree, (int32) (blk)))
#define KRgetfree(largest)      (pc_call_w(tpl->KRgetfree, (largest)))
#define set_dgram_ttl(dg)       ((void) pc_call_l(stx->set_dgram_ttl, (int32) (dg)))
#define check_dgram_ttl(dg)     ((int16) pc_call_l(stx->check_dgram_ttl, (int32) (dg)))
#define IP_discard(dg, all)     ((void) pc_call_lw(stx->IP_discard, (int32) (dg), (all)))
#define query_chains(p, d, l)   ((void) pc_call_lll(stx->query_chains, (int32) (p), (int32) (d), (int32) (l)))
#define TIMER_now()             (pc_call_0(stx->TIMER_now))
#define TIMER_elapsed(then)     (pc_call_l(stx->TIMER_elapsed, (then)))

// entries for STinG with Pure C cdecl arguments, they call my_set_state() / my_cntrl()
int16 pc_set_state(PORT *port, int16 enable);
int16 pc_cntrl(PORT *port, uint32 arg, int16 code);

// copy, fast when both addresses are even
void fastCopy(void *dst, const void *src, uint32 len);

//--------------------------------------------------------------------------------
// error codes

#define E_NORMAL        0
#define E_NODATA        -2
#define E_UNREACHABLE   -24
#define E_PARAMETER     -30
#define E_FNAVAIL       -32

// cntrl codes of ethernet ports
#define CTL_ETHER_SET_MAC       (('E' << 8) | 'M')
#define CTL_ETHER_GET_MAC       (('E' << 8) | 'N')
#define CTL_ETHER_INQ_SUPPTYPE  (('E' << 8) | 'Q')
#define CTL_ETHER_SET_TYPE      (('E' << 8) | 'T')
#define CTL_ETHER_GET_TYPE      (('E' << 8) | 'U')

#endif
