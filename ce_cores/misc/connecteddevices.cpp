// vim: shiftwidth=4 softtabstop=4 tabstop=4 expandtab
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <pthread.h>
#include <map>

#include "utils.h"
#include "debug.h"
#include "connecteddevices.h"

static std::map<const void*, std::set<std::string>> macsOfCores;     // key: ChipInterface of the core, value: its connected MACs
static std::set<std::string> lastAll;                               // union written to file last time
static pthread_mutex_t connectedDevicesMutex = PTHREAD_MUTEX_INITIALIZER;  // written by HDD / FDD / IKBD threads

void ConnectedDevices::update(const void* core, const std::set<std::string>& macs)
{
    pthread_mutex_lock(&connectedDevicesMutex);

    if(macs.empty()) {
        macsOfCores.erase(core);
    } else {
        macsOfCores[core] = macs;
    }

    std::set<std::string> all;
    for(auto& it : macsOfCores) {
        all.insert(it.second.begin(), it.second.end());
    }

    if(all != lastAll) {        // write file only on change - this is called from each core's main loop
        writeFile(all);
        lastAll = all;
    }

    pthread_mutex_unlock(&connectedDevicesMutex);
}

std::set<std::string> ConnectedDevices::getAll(void)
{
    pthread_mutex_lock(&connectedDevicesMutex);
    std::set<std::string> all = lastAll;
    pthread_mutex_unlock(&connectedDevicesMutex);
    return all;
}

void ConnectedDevices::clear(void)
{
    pthread_mutex_lock(&connectedDevicesMutex);
    macsOfCores.clear();
    lastAll.clear();
    writeFile(lastAll);
    pthread_mutex_unlock(&connectedDevicesMutex);
}

std::string ConnectedDevices::macToString(const uint8_t* mac)
{
    static const uint8_t zeroMac[6] = {0};

    if(memcmp(mac, zeroMac, 6) == 0) {      // mac not known yet
        return "";
    }

    char tmp[16];
    snprintf(tmp, sizeof(tmp), "%02X%02X%02X%02X%02X%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return std::string(tmp);
}

void ConnectedDevices::writeFile(const std::set<std::string>& all)
{
    std::string path = Utils::dotEnvValue("CONNECTED_DEVICES_FILE", "", false);

    if(path.empty()) {          // not in .env (e.g. in tests)? nothing to write
        return;
    }

    std::string content;
    for(auto& mac : all) {
        content += mac + "\n";
    }

    // atomically - the webserver must never see a half written file
    if(!Utils::writeFileAtomically(path.c_str(), (const uint8_t*) content.data(), content.size())) {
        logHdd(LOG_ERROR, "ConnectedDevices::writeFile - failed to write %s: %s", path.c_str(), strerror(errno));
        return;
    }

    logHdd(LOG_DEBUG, "ConnectedDevices::writeFile - %d connected device(s)", (int) all.size());
}
