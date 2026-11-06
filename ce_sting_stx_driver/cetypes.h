#ifndef _CETYPES_H_
#define _CETYPES_H_

// Basic types of the drivers talking to HOSTMOD_RAW_NET (this one and ../ce_mintnet_xif_driver)

#include <stdint.h>

typedef int16_t     int16;
typedef uint16_t    uint16;
typedef int32_t     int32;
typedef uint32_t    uint32;
typedef uint8_t     uint8;

#ifndef TRUE
#define TRUE    1
#define FALSE   0
#endif

#endif
