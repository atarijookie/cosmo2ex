#ifndef _MINT_H_
#define _MINT_H_

// The part of the MiNT kernel + inet4.xdd interface a network driver needs, rewritten from FreeMiNT's sys/mint/kerinfo.h,
// sys/sockets/netinfo.h, buf.h and inet4/if.h. Only what's used here is declared, the layout must stay the same as there.
//
// The kernel is built with -mshort, so int16 arguments are 2 bytes on stack. This driver is built without it, like
// libacsiscsi - the calls into the kernel go through pc_call_*() (purecall.s), see the macros below, and the two driver
// functions the kernel calls with int16 arguments (output, ioctl) are entered through pc_xif_output / pc_xif_ioctl (xifglue.s).

#include <stddef.h>

#include "cetypes.h"
#include "purecall.h"

//--------------------------------------------------------------------------------
// packet buffer

typedef struct buf {
    uint32  buflen;
    char   *dstart;             // start of data
    char   *dend;               // end of data
    struct buf *next;
    struct buf *prev;
    struct buf *link3;
    int16   links;
    int32   info;               // priority of the packet
    // more follows, not touched by drivers
} BUF;

#define BUF_NORMAL      0       // calls from kernel context
#define BUF_ATOMIC      1       // calls from interrupts

//--------------------------------------------------------------------------------
// network interface

#define IF_NAMSIZ       16
#define IF_MAXQ         60
#define IF_PRIORITIES   2

#define IFF_BROADCAST   0x0002
#define HWTYPE_ETH      1
#define ETH_ALEN        6
#define ETH_HLEN        14
#define ETH_MAX_DLEN    1500

struct hwaddr {
    int16   len;
    uint8   bytes[10];
};

struct ifq {
    int16   maxqlen;
    int16   qlen;
    int16   curr;
    BUF    *qfirst[IF_PRIORITIES];
    BUF    *qlast[IF_PRIORITIES];
};

struct netif {
    char    name[IF_NAMSIZ];
    int16   unit;
    uint16  flags;
    uint32  metric;
    uint32  mtu;
    uint32  timer;              // not useful - nif->timeout gets called once per second whatever is here
    int16   hwtype;
    struct hwaddr hwlocal;
    struct hwaddr hwbrcst;
    void   *addrlist;
    struct ifq snd;
    struct ifq rcv;
    long  (*open)(struct netif *);
    long  (*close)(struct netif *);
    long  (*output)(struct netif *, BUF *, const char *, int16, int16);    // -mshort, see pc_xif_output
    long  (*ioctl)(struct netif *, int16, long);                           // -mshort, see pc_xif_ioctl
    void  (*timeout)(struct netif *);
    void   *data;
    uint32  in_packets;
    uint32  in_errors;
    uint32  out_packets;
    uint32  out_errors;
    uint32  collisions;
    struct netif *next;
    int16   maxpackets;         // limits TCP receive window to this count of full segments, 0 = no limit
    void   *bpf;                // packet filter, if somebody listens
    uint8  *base_addr;
    void   *igmp_mac_filter;
    long    reserved[2];
};

_Static_assert(offsetof(struct netif, hwlocal)    == 34,  "netif must match the kernel");
_Static_assert(offsetof(struct netif, snd)        == 62,  "netif must match the kernel");
_Static_assert(offsetof(struct netif, open)       == 106, "netif must match the kernel");
_Static_assert(offsetof(struct netif, maxpackets) == 154, "netif must match the kernel");

//--------------------------------------------------------------------------------
// what inet4.xdd gives to the driver

struct netinfo {
    void   *buf_alloc;          // BUF *  buf_alloc (ulong size, ulong reserve, short mode)
    void   *buf_free;
    void   *buf_reserve;
    void   *buf_deref;          // void   buf_deref (BUF *, short mode)
    void   *if_enqueue;         // short  if_enqueue (struct ifq *, BUF *, short priority)
    void   *if_dequeue;         // BUF *  if_dequeue (struct ifq *)
    void   *if_register;        // long   if_register (struct netif *)
    void   *if_input;           // short  if_input (struct netif *, BUF *, long delay, short type)
    void   *if_flushq;          // void   if_flushq (struct ifq *)
    void   *in_chksum;
    void   *if_getfreeunit;     // short  if_getfreeunit (char *name)
    void   *eth_build_hdr;      // BUF *  eth_build_hdr (BUF *, struct netif *, const char *hwaddr, short type)
    void   *eth_remove_hdr;     // short  eth_remove_hdr (BUF *)
    const char *fname;
    void   *bpf_input;          // long   bpf_input (struct netif *, BUF *)
};

//--------------------------------------------------------------------------------
// what the kernel gives - fields of struct kerinfo up to the root timeouts, the order never changes

struct kerinfo {
    uint16  maj_version;
    uint16  min_version;
    uint16  default_perm;
    uint16  version;
    void   *bios_tab;
    void  **dos_tab;            // GEMDOS functions, index is the function number
    void   *drvchng;
    void   *trace;
    void   *debug;
    void   *alert;
    void   *fatal;
    void   *kmalloc;
    void   *kfree;
    void   *umalloc;
    void   *ufree;
    void   *kstrnicmp;
    void   *kstricmp;
    void   *kstrlwr;
    void   *kstrupr;
    void   *ksprintf;
    void   *millis_time;
    void   *unixtime;
    void   *dostime;
    void   *nap;
    void   *sleep;
    void   *wake;
    void   *wakeselect;
    void   *denyshare;
    void   *denylock;
    void   *addtimeout;
    void   *canceltimeout;
    void   *addroottimeout;     // TIMEOUT * addroottimeout (long ms, void (*)(PROC *, long), ushort flags)
    void   *cancelroottimeout;  // void cancelroottimeout (TIMEOUT *)
    // more follows, not needed here. Don't use dmabuf_alloc from it - it's NULL in the kernel
};

_Static_assert(offsetof(struct kerinfo, addroottimeout) == 116, "kerinfo must match the kernel");

#define GEMDOS_CCONWS   0x09
#define GEMDOS_MXALLOC  0x44
#define GEMDOS_MFREE    0x49

#define MX_STONLY       0x0000
#define MX_KEEP         0x4000  // F_KEEP - memory stays when the process which asked for it ends

#define ROOT_TIMEOUT_FROM_INT   1   // addroottimeout() flag: called from interrupt, or where one is possible

extern struct kerinfo *KERNEL;
extern struct netinfo *NETINFO;

#define buf_alloc(size, res, mode)      ((BUF *) pc_call_llw(NETINFO->buf_alloc, (int32) (size), (int32) (res), (mode)))
#define buf_deref(b, mode)              ((void) pc_call_lw(NETINFO->buf_deref, (int32) (b), (mode)))
#define if_enqueue(q, b, pri)           ((int16) pc_call_llw(NETINFO->if_enqueue, (int32) (q), (int32) (b), (pri)))
#define if_dequeue(q)                   ((BUF *) pc_call_l(NETINFO->if_dequeue, (int32) (q)))
#define if_register(nif)                (pc_call_l(NETINFO->if_register, (int32) (nif)))
#define if_input(nif, b, delay, type)   ((int16) pc_call_lllw(NETINFO->if_input, (int32) (nif), (int32) (b), (delay), (type)))
#define if_flushq(q)                    ((void) pc_call_l(NETINFO->if_flushq, (int32) (q)))
#define if_getfreeunit(name)            ((int16) pc_call_l(NETINFO->if_getfreeunit, (int32) (name)))
#define eth_build_hdr(b, nif, hw, type) ((BUF *) pc_call_lllw(NETINFO->eth_build_hdr, (int32) (b), (int32) (nif), (int32) (hw), (type)))
#define eth_remove_hdr(b)               ((int16) pc_call_l(NETINFO->eth_remove_hdr, (int32) (b)))
#define bpf_input(nif, b)               ((void) pc_call_ll(NETINFO->bpf_input, (int32) (nif), (int32) (b)))

#define addroottimeout(ms, fn, flags)   ((void *) pc_call_llw(KERNEL->addroottimeout, (ms), (int32) (fn), (flags)))
#define cancelroottimeout(t)            ((void) pc_call_l(KERNEL->cancelroottimeout, (int32) (t)))
#define c_conws(str)                    ((void) pc_call_l(KERNEL->dos_tab[GEMDOS_CCONWS], (int32) (str)))
#define m_xalloc(size, mode)            (pc_call_lw(KERNEL->dos_tab[GEMDOS_MXALLOC], (size), (mode)))
#define m_free(block)                   ((void) pc_call_l(KERNEL->dos_tab[GEMDOS_MFREE], (int32) (block)))

//--------------------------------------------------------------------------------
// ioctls and errors

#define SIOCSIFFLAGS    (('S' << 8) | 14)
#define SIOCSIFADDR     (('S' << 8) | 16)
#define SIOCSIFNETMASK  (('S' << 8) | 22)
#define SIOCSIFMTU      (('S' << 8) | 28)

#define ENOSYS_         (-32L)  // named with _ not to clash with the headers of libacsiscsi users
#define ENOMEM_         (-39L)
#define ENODEV_         (-15L)

// entries for the kernel with -mshort arguments, they call xifOutput() / xifIoctl()
long pc_xif_output(struct netif *nif, BUF *buf, const char *hwaddr, int16 hwlen, int16 pktype);
long pc_xif_ioctl(struct netif *nif, int16 cmd, long arg);

#endif
