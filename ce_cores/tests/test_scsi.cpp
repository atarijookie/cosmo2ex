// vim: shiftwidth=4 softtabstop=4 tabstop=4 expandtab
// Tests of Scsi::findAttachedDisks() - which media gets served on which ACSI / SCSI ID

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <map>
#include <string>

#include "doctest.h"

#include "misc/settings.h"
#include "misc/global.h"
#include "hdd/native/scsi.h"
#include "hdd/native/scsi_defs.h"

extern std::map<std::string, std::string> dotEnv;   // the loaded .env values, defined in utils.cpp

// temp SETTINGS_DIR + one small image file in it
struct ScsiSettingsDir {
    std::string dir, image;

    ScsiSettingsDir() {
        char tmpl[] = "/tmp/ce_test_scsi_XXXXXX";
        REQUIRE(mkdtemp(tmpl) != NULL);
        dir = tmpl;
        dotEnv["SETTINGS_DIR"] = dir;

        image = dir + "/disk.img";
        FILE* f = fopen(image.c_str(), "wb");
        REQUIRE(f != NULL);
        static uint8_t zeros[64 * 512];
        fwrite(zeros, 1, sizeof(zeros), f);
        fclose(f);
    }

    ~ScsiSettingsDir() {
        dotEnv.erase("SETTINGS_DIR");
        std::string cmd = "rm -rf '" + dir + "'";
        system(cmd.c_str());
    }

    // set dev type and raw path for all IDs of the device with this MAC
    void setIds(uint8_t* mac, const int types[8], const std::string paths[8]) {
        Settings s(mac);
        char key[32];
        for(int id=0; id<8; id++) {
            snprintf(key, sizeof(key), "ACSI_DEVTYPE_%d", id);
            s.setInt(key, types[id]);
            snprintf(key, sizeof(key), "PATH_RAW_%d", id);
            s.setString(key, paths[id].c_str());
        }
    }
};

TEST_CASE_FIXTURE(ScsiSettingsDir, "findAttachedDisks - PATH_RAW served only on RAW IDs")
{
    uint8_t mac[6] = {0xCA, 0x4E, 0x3E, 0xA4, 0x00, 0x30};

    SUBCASE("CE_DD ID with stale raw path serves CE_DD boot media, not the image") {
        const int types[8] = {DEVTYPE_OFF, DEVTYPE_TRANSLATED, DEVTYPE_OFF, DEVTYPE_OFF, DEVTYPE_OFF, DEVTYPE_OFF, DEVTYPE_OFF, DEVTYPE_OFF};
        const std::string paths[8] = {"", image, "", image, "", "", "", ""};
        setIds(mac, types, paths);

        Scsi scsi;
        scsi.setMac(mac);

        CHECK(scsi.getAcsiIDinfo()->ceddId == 1);
        CHECK(scsi.getDevInfo(1)->enabled);
        CHECK(scsi.getDevInfo(1)->hostSourceType == SOURCETYPE_IMAGE_TRANSLATEDBOOT);
        CHECK(scsi.getDevInfo(3)->enabled == false);            // OFF ID with stale path - nothing served

        Settings s(mac);
        CHECK(s.getString("PATH_RAW_1", "") == image);          // path kept, so switching back to RAW restores it
    }

    SUBCASE("RAW ID serves its image") {
        const int types[8] = {DEVTYPE_OFF, DEVTYPE_TRANSLATED, DEVTYPE_RAW, DEVTYPE_OFF, DEVTYPE_OFF, DEVTYPE_OFF, DEVTYPE_OFF, DEVTYPE_OFF};
        const std::string paths[8] = {"", "", image, "", "", "", "", ""};
        setIds(mac, types, paths);

        Scsi scsi;
        scsi.setMac(mac);

        CHECK(scsi.getDevInfo(1)->hostSourceType == SOURCETYPE_IMAGE_TRANSLATEDBOOT);
        CHECK(scsi.getDevInfo(2)->enabled);
        CHECK(scsi.getDevInfo(2)->hostSourceType == SOURCETYPE_IMAGE);
    }

    SUBCASE("SLM ID with stale raw path serves nothing") {
        const int types[8] = {DEVTYPE_OFF, DEVTYPE_TRANSLATED, DEVTYPE_OFF, DEVTYPE_OFF, DEVTYPE_OFF, DEVTYPE_SLM, DEVTYPE_OFF, DEVTYPE_OFF};
        const std::string paths[8] = {"", "", "", "", "", image, "", ""};
        setIds(mac, types, paths);

        Scsi scsi;
        scsi.setMac(mac);

        CHECK(scsi.getDevInfo(5)->enabled == false);
    }
}
