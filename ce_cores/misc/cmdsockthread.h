// vim: tabstop=4 softtabstop=4 shiftwidth=4 expandtab
#ifndef _CMDSOCKTHREAD_H_
#define _CMDSOCKTHREAD_H_

#include <string>

class AcsiDataTrans;
class Scsi;
class ImageStorage;

bool cmdSockOpen(int& err);     // false + errno-like err on failure, EADDRINUSE = other instance is running
void *cmdSockThreadCode(void *ptr);
const char* cmdSockStripToken(const char* msg, const std::string& token);

#endif
