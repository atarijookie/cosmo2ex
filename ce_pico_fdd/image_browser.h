#ifndef __IMAGE_BROWSER_H__
#define __IMAGE_BROWSER_H__

#include <cstdint>

// Browsing the images in the folder of the current image on the front panel: left / right shows the previous / next
// image (wrapping around at the ends, like the web UI), enter inserts it, 15 s without a key goes back to the current
// image. The host has the sorted list (ce_cores/fdd/imagelist.cpp), the device gets it in pages of up to 16 names:
//
// ATN_GET_IMAGE_LIST
//   device -> host: start (4 B, IMAGE_LIST_NONE = page around the current image), count (2 B)
//   host -> device: gen, total, current index (IMAGE_LIST_NONE = not in the list), start (4 B each), count (2 B),
//                   then count * [name len (1 B), name] - the reply, and also on its own when the list or the current
//                   image changed (then it's the page asked for last, gen incremented if the list changed)
// ATN_INSERT_IMAGE
//   device -> host: gen, index (4 B each), name len (1 B), name - the host finds the image by the name
//   host -> device: status (1 B) - INSERT_IMAGE_*
//
// All the numbers big endian. The list is asked for only with the panel attached.

#define IMAGE_LIST_NONE         0xffffffff
#define IMAGE_LIST_PAGE_MAX     16
#define IMAGE_LIST_NAME_MAX     255
#define IMAGE_LIST_PAGE_HEADER  18
#define IMAGE_LIST_PAGE_MAX_SIZE    (IMAGE_LIST_PAGE_HEADER + IMAGE_LIST_PAGE_MAX * (1 + IMAGE_LIST_NAME_MAX))

#define BROWSE_TIMEOUT_MS       15000   // no key for this long - back to showing the current image
#define BROWSE_REQUEST_RETRY_MS 1000    // no reply to a page request for this long - ask again
#define BROWSE_PAGE_MARGIN      2       // ask for the next page when the cursor gets this close to the page edge
#define BROWSE_STATUS_MS        2000    // how long "not found" stays shown
#define BROWSE_INSERT_TIMEOUT_MS    3000    // no reply to the insert for this long - give up on it

void browserReset(void);                                    // host connection lost - forget the list
void browserLoop(void);                                     // page requests, timeout, display - call from the main loop
bool browserKey(char button, char event);                   // panel key, true if used
bool browserActive(void);                                   // browsing - the browser has the display

void browserImageListReceived(const uint8_t* data, uint32_t len);   // ATN_GET_IMAGE_LIST from host
void browserInsertStatusReceived(uint8_t status);                  // ATN_INSERT_IMAGE from host

#endif
