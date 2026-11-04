#ifndef _CHILDPROCESS_H_
#define _CHILDPROCESS_H_

#include <sys/types.h>
#include <string>
#include <vector>

// Helper programs which ce_cores starts (ce_media, ...). When ce_cores runs as root (sudo), the helper runs as a normal
// user: the one in the .env key userEnvKey, else the one who ran sudo; root only when allowRoot.
class ChildProcess
{
public:
    // returns the pid, 0 on failure (errorMsg says why - for the ST). exit codes of the child: 126 = can't switch
    // user, 127 = can't run the binary.
    static pid_t spawn(const std::string& bin, const std::vector<std::string>& args, const char *userEnvKey,
                       bool allowRoot, std::string& errorMsg);

    static bool exited(pid_t pid, int *exitCode);   // reaps it when it exited; exitCode -1 when killed by a signal
    static void stop(pid_t pid, int graceMs);       // SIGTERM, after graceMs SIGKILL; waits for it
};

#endif
