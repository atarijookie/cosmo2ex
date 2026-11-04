#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <signal.h>
#include <grp.h>
#include <pwd.h>
#include <spawn.h>
#include <unistd.h>
#include <sys/wait.h>

#include "debug.h"
#include "utils.h"
#include "childprocess.h"

extern char **environ;

pid_t ChildProcess::spawn(const std::string& bin, const std::vector<std::string>& args, const char *userEnvKey,
                          bool allowRoot, std::string& errorMsg)
{
    std::vector<char *> argv;
    argv.push_back((char *) bin.c_str());
    for(const std::string& a : args) {
        argv.push_back((char *) a.c_str());
    }
    argv.push_back(NULL);

    struct passwd *pw = NULL;
    if(geteuid() == 0) {
        std::string user = Utils::dotEnvValue(userEnvKey, "", false);
        if(user.empty() && getenv("SUDO_USER")) {
            user = getenv("SUDO_USER");
        }
        if(!user.empty() && user != "root") {
            pw = getpwnam(user.c_str());
            if(!pw) {
                errorMsg = std::string(userEnvKey) + " " + user + " doesn't exist";
                return 0;
            }
        } else if(!allowRoot) {
            errorMsg = std::string("Can't run as root - set ") + userEnvKey + " in .env";
            return 0;
        }
    }

    pid_t pid;
    if(!pw) {
        int res = posix_spawn(&pid, bin.c_str(), NULL, NULL, argv.data(), environ);
        if(res != 0) {
            errorMsg = std::string("Can't start ") + bin + ": " + strerror(res);
            return 0;
        }
        return pid;
    }

    // environment of that user: its HOME, USER, ...
    std::vector<std::string> env;
    for(char **e = environ; *e; e++) {
        if(strncmp(*e, "HOME=", 5) && strncmp(*e, "USER=", 5) && strncmp(*e, "LOGNAME=", 8) && strncmp(*e, "XDG_", 4)) {
            env.push_back(*e);
        }
    }
    env.push_back(std::string("HOME=") + pw->pw_dir);
    env.push_back(std::string("USER=") + pw->pw_name);
    env.push_back(std::string("LOGNAME=") + pw->pw_name);
    std::vector<char *> envp;
    for(std::string& s : env) {
        envp.push_back((char *) s.c_str());
    }
    envp.push_back(NULL);

    // supplementary groups of the user too - e.g. 'docker', for ffmpeg in a container
    gid_t groups[64];
    int ngroups = 64;
#if defined(__APPLE__)
    int gl = getgrouplist(pw->pw_name, (int) pw->pw_gid, (int *) groups, &ngroups);    // int instead of gid_t there
#else
    int gl = getgrouplist(pw->pw_name, pw->pw_gid, groups, &ngroups);
#endif
    if(gl < 0) {
        groups[0] = pw->pw_gid;
        ngroups = 1;
    }
    uid_t uid = pw->pw_uid;
    gid_t gid = pw->pw_gid;

    pid = fork();
    if(pid < 0) {
        errorMsg = std::string("Can't start ") + bin + ": " + strerror(errno);
        return 0;
    }
    if(pid == 0) {                                          // child - only async-signal-safe calls until exec
        if(setgroups(ngroups, groups) != 0 || setgid(gid) != 0 || setuid(uid) != 0) {
            _exit(126);
        }
        execve(bin.c_str(), argv.data(), envp.data());
        _exit(127);
    }
    return pid;
}

bool ChildProcess::exited(pid_t pid, int *exitCode)
{
    int wstatus;
    pid_t res = waitpid(pid, &wstatus, WNOHANG);
    if(res == pid || (res < 0 && errno == ECHILD)) {
        *exitCode = (res == pid && WIFEXITED(wstatus)) ? WEXITSTATUS(wstatus) : -1;
        return true;
    }
    return false;
}

void ChildProcess::stop(pid_t pid, int graceMs)
{
    if(pid <= 0) {
        return;
    }
    kill(pid, SIGTERM);
    for(int i=0; i<graceMs / 50; i++) {
        if(waitpid(pid, NULL, WNOHANG) != 0) {
            return;
        }
        Utils::sleepMs(50);
    }
    kill(pid, SIGKILL);
    waitpid(pid, NULL, 0);
}
