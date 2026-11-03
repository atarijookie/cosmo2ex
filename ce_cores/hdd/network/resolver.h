#ifndef _RESOLVER_H_
#define _RESOLVER_H_

#include <stdint.h>
#include <atomic>
#include <memory>
#include <mutex>
#include <string>

// DNS resolving for the network module - the fake STinG starts a resolve and polls for the result.
// Each request runs a blocking getaddrinfo() in its own detached thread (getaddrinfo_a() of the old ce_main_app is
// glibc only). The thread holds its job through a shared_ptr, so a slot can be cleared / reused while it still runs.

#define RESOLV_COUNT        10
#define RESOLV_MAX_IPS      32

struct ResolveJob {
    std::string         hostName;
    std::atomic<bool>   done { false };

    // valid when done
    int                 error = 0;              // getaddrinfo() result, 0 = OK
    std::string         canonName;
    int                 count = 0;
    uint32_t            ips[RESOLV_MAX_IPS];    // host byte order
};

struct ResolveSlot {
    std::shared_ptr<ResolveJob> job;
    uint32_t                    startTime = 0;  // 0 = slot free
};

class ResolverRequest {
public:
    int  addRequest         (const char *hostName);     // returns slot index, -1 on failure
    bool checkAndhandleSlot (int index);                // true when done (then see getJob())
    std::shared_ptr<ResolveJob> getJob(int index);
    void clearSlot          (int index);
    bool slotIndexValid     (int index);

private:
    ResolveSlot slots[RESOLV_COUNT];

    static void resolveThread(std::shared_ptr<ResolveJob> job);
};

#endif
