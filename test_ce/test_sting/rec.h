#ifndef _STING_REC_H_
#define _STING_REC_H_

// Recorder for the STinG compatibility tests.
//
// The tests don't decide what is right - they record what the STinG under test returned, one value per line:
//
//   0001 # TCP_close() on invalid handle        - test header
//   0001 = ret -9                               - exact value: must be the same on real STinG and on a clone
//   0001 ~ ms 120                               - fuzzy value: depends on timing / config, shown in the diff, never fails
//   ## ...                                      - comments (stack identity, server address, ...)
//
// Run once on real STinG (golden log), once on the clone, and compare the 2 logs with sting_server/sting_diff.py.

#include <stdint.h>

// The log file is created by rec_init() and appended after every test header, so after a crash it holds everything
// up to the header of the test that crashed.
uint8_t rec_init(const char *fileName);         // create <current drive>:\fileName (asks for drive on fail), 0 = no log
void    rec_done(void);                         // write the rest, free the buffer

void rec_comment(const char *str);              // "## ..." line
void rec_test(uint16_t testNo, const char *name);

void rec_x (const char *key, int32_t value);    // exact int
void rec_f (const char *key, int32_t value);    // fuzzy int
void rec_xs(const char *key, const char *str);  // exact string (NULL is recorded as <null>)
void rec_fs(const char *key, const char *str);  // fuzzy string
void rec_xh(const char *key, uint32_t value);   // exact, in hex (IP addresses, ...)
#define rec_xp(key, ptr)    rec_x((key), (ptr) != 0)  // exact: pointer NULL (0) or not (1) - addresses differ per run

#endif
