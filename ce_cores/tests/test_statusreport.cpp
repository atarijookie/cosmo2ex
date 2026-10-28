/*
    Tests of StatusReport - status file of each device in LOG_DIR, shown by the webserver on status.html
*/
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <map>
#include <string>

#include "doctest.h"

#include "misc/global.h"
#include "misc/statusreport.h"

extern std::map<std::string, std::string> dotEnv;   // the loaded .env values, defined in utils.cpp

static std::string fileContent(const std::string& path)
{
    std::string s;
    FILE *f = fopen(path.c_str(), "r");
    if(f) {
        char bfr[256];
        size_t n;
        while((n = fread(bfr, 1, sizeof(bfr), f)) > 0) {
            s.append(bfr, n);
        }
        fclose(f);
    }
    return s;
}

TEST_CASE("statusreport: status file follows changes, not just the first report of the device")
{
    char tmpl[] = "/tmp/ce_test_status_XXXXXX";
    REQUIRE(mkdtemp(tmpl) != NULL);
    std::string logDir = tmpl;
    dotEnv["LOG_DIR"] = logDir;

    uint8_t mac[6] = {0x02, 0x57, 0xa7, 0x00, 0x00, 0x01};
    std::string path = logDir + "/0257A7000001.txt";
    char fw1[] = "2026-01-01";
    char fw2[] = "2026-10-28";

    StatusReport::storeIpAndFwVer(mac, 0xc0a87b0a, fw1, DEV_FEATURE_ACSI);     // new device - first report
    std::string first = fileContent(path);
    CHECK(first.find("IP: 192.168.123.10\n") != std::string::npos);
    CHECK(first.find("FW version: 2026-01-01\n") != std::string::npos);

    StatusReport::storeIpAndFwVer(mac, 0xc0a87b0b, fw2, DEV_FEATURE_ACSI);     // reconnected: new IP and FW
    std::string second = fileContent(path);
    CHECK(second.find("IP: 192.168.123.11\n") != std::string::npos);
    CHECK(second.find("FW version: 2026-10-28\n") != std::string::npos);

    StatusReport::storeTosAndMachine(mac, 0x0206, 0);                          // TOS version from the HDD core
    CHECK(fileContent(path).find("TOS version: 0206\n") != std::string::npos);

    dotEnv.erase("LOG_DIR");
    unlink(path.c_str());
    rmdir(logDir.c_str());
}
