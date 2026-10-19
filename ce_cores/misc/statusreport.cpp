// vim: shiftwidth=4 softtabstop=4 tabstop=4 expandtab
#include <unistd.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <errno.h>

#include "utils.h"
#include "global.h"
#include "debug.h"
#include "statusreport.h"
#include "../hdd/translated/translateddisk.h"
#include "../hdd/native/scsi.h"

TClientStatus statuses[MAX_CLIENTS];
static pthread_mutex_t statusesMutex = PTHREAD_MUTEX_INITIALIZER;   // statuses are written by HDD / FDD / IKBD threads, read by command socket thread

void StatusReport::createSingleReportFile(int i)
{
    std::string report;
    report.clear();
    char tmp[256];

    snprintf(tmp, sizeof(tmp), "IP: %d.%d.%d.%d\n", (statuses[i].ipAddr >> 24) & 0xff, (statuses[i].ipAddr >> 16) & 0xff, (statuses[i].ipAddr >> 8) & 0xff, statuses[i].ipAddr & 0xff);
    report += tmp;

    snprintf(tmp, sizeof(tmp), "MAC: %02X:%02X:%02X:%02X:%02X:%02X\n", statuses[i].mac[0], statuses[i].mac[1], statuses[i].mac[2], statuses[i].mac[3], statuses[i].mac[4], statuses[i].mac[5]);
    report += tmp;

    struct tm tm_info;
    localtime_r(&statuses[i].timestamp, &tm_info);      // localtime_r() - thread safe version of localtime()
    strftime(tmp, sizeof(tmp), "%Y-%m-%d %H:%M", &tm_info);
    report += std::string("last seen: ");
    report += std::string(tmp);
    report += std::string("\n");

    if(statuses[i].features) {
        report += std::string("features: ");

        if(statuses[i].features & DEV_FEATURE_ACSI) report += std::string("ACSI ");
        if(statuses[i].features & DEV_FEATURE_SCSI) report += std::string("SCSI ");
        if(statuses[i].features & DEV_FEATURE_IKBD) report += std::string("IKBD ");
        if(statuses[i].features & DEV_FEATURE_FDD) report += std::string("FDD ");

        report += std::string("\n");
    }

    snprintf(tmp, sizeof(tmp), "FW version: %s\n", statuses[i].fwVer);
    report += tmp;

    if(statuses[i].tosVersion) {
        snprintf(tmp, sizeof(tmp), "TOS version: %04X\n", statuses[i].tosVersion);
        report += tmp;
    }

    if(statuses[i].scsiMachine) {
        const char* mch = "UNKNOWN";
        mch = (statuses[i].scsiMachine == SCSI_MACHINE_TT) ? "TT" : mch;
        mch = (statuses[i].scsiMachine == SCSI_MACHINE_FALCON) ? "Falcon" : mch;

        snprintf(tmp, sizeof(tmp), "SCSI machine: %s\n", mch);
        report += tmp;
    }

    // no change since last time? don't write to file
    if(!statuses[i].lastWrittenReport.compare(report)) {
        return;
    }

    // if changed, write to file
    std::string logDir = Utils::dotEnvValue("LOG_DIR", Utils::defaultDir("LOG_DIR").c_str());  // path to logs dir
    snprintf(tmp, sizeof(tmp), "%02X%02X%02X%02X%02X%02X.txt", statuses[i].mac[0], statuses[i].mac[1], statuses[i].mac[2], statuses[i].mac[3], statuses[i].mac[4], statuses[i].mac[5]);
    std::string fileAndPath = Utils::mergeHostPaths2(logDir, std::string(tmp));

    Utils::textToFile(report.c_str(), fileAndPath.c_str());     // write to file

    statuses[i].lastWrittenReport = report;     // keep copy of string, so we can tell if next write is needed or not
}

void StatusReport::createReportFiles(void)
{
    pthread_mutex_lock(&statusesMutex);

    for(int i=0; i<MAX_CLIENTS; i++)
    {
        if(statuses[i].ipAddr == 0) {       // no ip == no device here
            continue;
        }

        createSingleReportFile(i);
    }

    pthread_mutex_unlock(&statusesMutex);
}

int StatusReport::getIndexFromMac(uint8_t* mac, bool& isNew)
{
    isNew = false;

    uint8_t emptyMac[6];
    memset(emptyMac, 0, 6);

    if(memcmp(mac, emptyMac, 6) == 0) {                 // mac not known yet? can't store status for it
        return -1;
    }

    int idxEmpty = -1;

    for(int i=0; i<MAX_CLIENTS; i++)
    {
        if(memcmp(statuses[i].mac, mac, 6) == 0) {         // found position of this mac?
            return i;
        }

        if(idxEmpty < 0 && memcmp(statuses[i].mac, emptyMac, 6) == 0) {    // found position of empty mac? remember the first one
            idxEmpty = i;
        }
    }

    if(idxEmpty >= 0) {         // mac not found, but got empty position? it's a new device
        isNew = true;
    }

    return idxEmpty;
}

void StatusReport::storeIpAndFwVer(uint8_t* mac, uint32_t ipAddr, char* fwVer, uint8_t features)
{
    pthread_mutex_lock(&statusesMutex);

    bool isNew;
    int idx = getIndexFromMac(mac, isNew);

    if(idx != -1) {     // matching or empty position found? store at index
        memcpy(statuses[idx].mac, mac, 6);
        statuses[idx].ipAddr = ipAddr;
        snprintf(statuses[idx].fwVer, sizeof(statuses[idx].fwVer), "%s", fwVer);
        statuses[idx].timestamp = time(NULL);
        statuses[idx].features = features;

        if(isNew) {
            createSingleReportFile(idx);
        }
    }

    pthread_mutex_unlock(&statusesMutex);
}

void StatusReport::storeTosAndMachine(uint8_t* mac, uint16_t tosVersion, int scsiMachine)
{
    pthread_mutex_lock(&statusesMutex);

    bool isNew;
    int idx = getIndexFromMac(mac, isNew);

    if(idx != -1) {     // matching or empty position found? store at index
        memcpy(statuses[idx].mac, mac, 6);      // needed when this is a new device, so this slot is not empty anymore
        statuses[idx].tosVersion = tosVersion;
        statuses[idx].scsiMachine = scsiMachine;

        if(isNew) {
            createSingleReportFile(idx);
        }
    }

    pthread_mutex_unlock(&statusesMutex);
}
