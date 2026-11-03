#include <stdio.h>
#include <string.h>
#include <netdb.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <thread>

#include "../../misc/debug.h"
#include "../../misc/utils.h"
#include "resolver.h"

int ResolverRequest::addRequest(const char *hostName)
{
    int index = -1;
    uint32_t oldestTime = 0xffffffff;
    int oldestIndex = 0;

    for(int i=0; i<RESOLV_COUNT; i++) {
        if(slots[i].startTime == 0) {                   // free slot
            index = i;
            break;
        }

        if(slots[i].startTime < oldestTime) {
            oldestTime  = slots[i].startTime;
            oldestIndex = i;
        }
    }

    if(index < 0) {                                     // all used - reuse the oldest, its thread finishes for nobody
        index = oldestIndex;
    }

    std::shared_ptr<ResolveJob> job = std::make_shared<ResolveJob>();
    job->hostName = hostName;

    slots[index].job        = job;
    slots[index].startTime  = Utils::getCurrentMs() | 1;    // never 0

    // dotted IP - no need to ask DNS. As in STinG 1.26, each number is taken modulo 256 ("1.2.3.400" is 1.2.3.144),
    // and the real name is the IP written out again.
    unsigned int a, b, c, d;
    char rest;
    if(sscanf(hostName, "%u.%u.%u.%u%c", &a, &b, &c, &d, &rest) == 4) {
        a &= 0xff; b &= 0xff; c &= 0xff; d &= 0xff;

        char ip[48];
        snprintf(ip, sizeof(ip), "%u.%u.%u.%u", a, b, c, d);

        job->ips[0]     = (a << 24) | (b << 16) | (c << 8) | d;
        job->count      = 1;
        job->canonName  = ip;
        job->done       = true;
        return index;
    }

    try {
        std::thread(resolveThread, job).detach();
    } catch(...) {
        logHdd(LOG_ERROR, "ResolverRequest::addRequest() - failed to start resolve thread");
        clearSlot(index);
        return -1;
    }

    logHdd(LOG_DEBUG, "ResolverRequest::addRequest() - resolving '%s' in slot %d", hostName, index);
    return index;
}

void ResolverRequest::resolveThread(std::shared_ptr<ResolveJob> job)
{
    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family     = AF_INET;
    hints.ai_flags      = AI_CANONNAME;

    struct addrinfo *res = NULL;
    job->error = getaddrinfo(job->hostName.c_str(), NULL, &hints, &res);

    if(job->error == 0) {
        if(res && res->ai_canonname) {
            job->canonName = res->ai_canonname;
        }

        for(struct addrinfo *ai = res; ai && job->count < RESOLV_MAX_IPS; ai = ai->ai_next) {
            if(ai->ai_addr == NULL || ai->ai_addr->sa_family != AF_INET) {
                continue;
            }

            uint32_t ip = ntohl(((struct sockaddr_in *) ai->ai_addr)->sin_addr.s_addr);

            bool have = false;                          // getaddrinfo() returns each IP for every socket type
            for(int i=0; i<job->count; i++) {
                if(job->ips[i] == ip) {
                    have = true;
                    break;
                }
            }

            if(!have) {
                job->ips[job->count++] = ip;
            }
        }
    }

    if(res) {
        freeaddrinfo(res);
    }

    job->done = true;                                   // atomic - publishes the results above
}

bool ResolverRequest::checkAndhandleSlot(int index)
{
    if(!slotIndexValid(index) || !slots[index].job) {
        return false;
    }

    return slots[index].job->done;
}

std::shared_ptr<ResolveJob> ResolverRequest::getJob(int index)
{
    if(!slotIndexValid(index)) {
        return nullptr;
    }

    return slots[index].job;
}

void ResolverRequest::clearSlot(int index)
{
    if(!slotIndexValid(index)) {
        return;
    }

    slots[index].job.reset();
    slots[index].startTime = 0;
}

bool ResolverRequest::slotIndexValid(int index)
{
    return index >= 0 && index < RESOLV_COUNT;
}
