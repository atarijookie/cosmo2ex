#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <dirent.h>
#include <errno.h>
#include <stdlib.h>

#if defined(__CYGWIN__)
#include <sys/cygwin.h>     // cygwin_create_path()
#endif
#include "settings.h"
#include "global.h"
#include "debug.h"
#include "utils.h"

#define SETTINGS_DIR_MODE   (S_IRWXU | S_IRWXG | S_IROTH | S_IXOTH)     // mode: 0775

void Settings::init(uint8_t* prefixBytes, int len)
{
    setPrefix(prefixBytes, len);

    // create settings dir for this prefix (e.g. for this device mac) - if it didn't exist yet, store default values there
    std::string dir = settingsDir;

    if(Utils::dirExists(dir)) {     // dir already exists? nothing to do
        return;
    }

    if(Utils::mkpath(dir.c_str(), SETTINGS_DIR_MODE) == 0) {    // dir created
        logHdd(LOG_DEBUG, "Settings: directory %s was created.", dir.c_str());

        storeDefaultValues();
    } else if(errno != EEXIST) {    // dir not created, and it's not because it already exists...
        logHdd(LOG_ERROR, "Settings: failed to create settings directory %s - %s", dir.c_str(), strerror(errno));
    }
}

std::string Settings::settingsDirPath(void)
{
    std::string dir = Utils::dotEnvValue("SETTINGS_DIR", "./settings"); // path to settings dir

    if(prefix[0]) {     // prefix present? merge it into settings path
        return Utils::mergeHostPaths2(dir, std::string(prefix));
    }

    return dir;
}

Settings::Settings(void)
{
    init(NULL, 0);
}

Settings::Settings(uint8_t* mac)
{
    init(mac, 6);
}

void Settings::setPrefix(uint8_t* bytes, int len)
{
    if(bytes != NULL && len > 0) {      // got pointer and length, store prefix as string
        binToHex(bytes, len, prefix);
    } else {        // no pointer or length, clear prefix
        memset(prefix, 0, 32);
    }

    settingsDir = settingsDirPath();    // prefix doesn't change often, so don't build the path on every get / set
}

// default host path of translated drive C:
//  - Linux: /mnt/ - where disks get mounted (in docker the host filesystem is in /mnt/hostdrive)
//  - macOS: home dir, Windows (Cygwin): user profile dir (e.g. C:\Users\x as /cygdrive/c/Users/x) - no /mnt on desktops
std::string Settings::defaultTranslatedDrivePath(void)
{
#if defined(__APPLE__)
    const char* home = getenv("HOME");

    if(home && home[0]) {
        return std::string(home) + "/";
    }
#elif defined(__CYGWIN__)
    const char* userProfile = getenv("USERPROFILE");      // Windows path, e.g. C:\Users\x

    if(userProfile && userProfile[0]) {
        char* posixPath = (char*) cygwin_create_path(CCP_WIN_A_TO_POSIX, userProfile);

        if(posixPath) {
            std::string path = std::string(posixPath) + "/";
            free(posixPath);
            return path;
        }
    }
#endif

    return "/mnt/";     // Linux - and fallback if the dir can't be found on other OS
}

void Settings::storeDefaultValues(void)
{
    logHdd(LOG_DEBUG, "Settings::storeDefaultValues() - storing default settings, because it seems we miss those setting...");

    char key[32];
    for(int id=0; id<8; id++) {                         // read the list of device types from settings
        sprintf(key, "ACSI_DEVTYPE_%d", id);            // create settings KEY, e.g. ACSI_DEVTYPE_0

        if(id == 1) {                                   // ACSI id 1 enaled by default
            setInt(key, DEVTYPE_TRANSLATED);
        } else {                                        // other ACSI id's disabled
            setInt(key, DEVTYPE_OFF);
        }
    }

    setPath("PATH_GEM_C", defaultTranslatedDrivePath());       // shown in the web UI - e.g. C:\Users\x\ on Windows
    setChar("DRIVELETTER_CONFDRIVE", 'O');
}

std::string Settings::getPath(const char *key, const char *defValue)
{
    return Utils::pathFromUi(getString(key, defValue));
}

void Settings::setPath(const char *key, const std::string& path)
{
    setString(key, Utils::pathToUi(path).c_str());
}

bool Settings::getBool(const char *key, bool defValue)
{
    FILE *file = sOpen(key, true);
    if(!file) {                                         // failed to open settings?
//      logHdd(LOG_DEBUG, "Settings::getBool -- returning default value for %s", key);
        return defValue;
    }

    int val, res;
    res = fscanf(file, "%d", &val);                     // try to read the value
    fclose(file);

    if(res != 1) {                                      // failed to read value?
//      logHdd(LOG_DEBUG, "Settings::getBool -- returning default value for %s", key);
        return defValue;
    }

    bool ret = false;                                   // convert int to bool
    if(val == 1) {
        ret = true;
    }

    return ret;                                         // return
}

void Settings::setBool(const char *key, bool value)
{
    sWrite(key, value ? "1\n" : "0\n");      // true: 1, false: 0
}

//-------------------------
int Settings::getInt(const char *key, int defValue)
{
    FILE *file = sOpen(key, true);
    if(!file) {                                         // failed to open settings?
//      logHdd(LOG_DEBUG, "Settings::getInt -- returning default value for %s", key);
        return defValue;
    }

    int val, res;
    res = fscanf(file, "%d", &val);                     // try to read the value
    fclose(file);

    if(res != 1) {                                      // failed to read value?
//      logHdd(LOG_DEBUG, "Settings::getInt -- returning default value for %s", key);
        return defValue;
    }

    return val;                                         // return
}

void Settings::setInt(const char *key, int value)
{
    char bfr[32];
    snprintf(bfr, sizeof(bfr), "%d\n", value);
    sWrite(key, bfr);
}
//-------------------------
float Settings::getFloat(const char *key, float defValue)
{
    FILE *file = sOpen(key, true);
    if(!file) {                                         // failed to open settings?
        return defValue;
    }

    int res;
    float val;
    res = fscanf(file, "%f", &val);                     // try to read the value
    fclose(file);

    if(res != 1) {                                      // failed to read value?
        return defValue;
    }

    return val;                                         // return
}

void Settings::setFloat(const char *key, float value)
{
    char bfr[64];
    snprintf(bfr, sizeof(bfr), "%f\n", value);
    sWrite(key, bfr);
}
//-------------------------
// returns a copy (std::string) - no shared static buffer, so it's safe to call from multiple threads (FDD + HDD cores)
std::string Settings::getString(const char *key, const char *defValue)
{
    char buffer[256];
    memset(buffer, 0, 256);

    FILE *file = sOpen(key, true);
    if(!file) {                                         // failed to open settings?
//      logHdd(LOG_DEBUG, "Settings::getString -- returning default value for %s", key);
        snprintf(buffer, sizeof(buffer), "%s", defValue);   // copy default value, cut if too long
        return buffer;
    }

    char *res;
    res = fgets(buffer, 256, file);                     // try to read the value
    fclose(file);

    if(res == NULL) {                                   // failed to read value?
//      logHdd(LOG_DEBUG, "Settings::getString -- returning default value for %s", key);
        snprintf(buffer, sizeof(buffer), "%s", defValue);   // copy default value, cut if too long
        return buffer;
    }

    // remove trailing new line chars - setString() doesn't write them, but file could be edited by hand
    size_t len = strlen(buffer);
    while(len > 0 && (buffer[len - 1] == '\n' || buffer[len - 1] == '\r')) {
        buffer[--len] = 0;
    }

    return buffer;                                          // return
}

void Settings::setString(const char *key, const char *value)
{
    sWrite(key, value);
}

//-------------------------
char *Settings::getBinaryString(const char *key, int len)
{
    static thread_local char buffer[256];           // one buffer per thread - valid until next call from the same thread
    memset(buffer, 0, 256);

    if(len < 0) {                                   // invalid length? return empty buffer
        return buffer;
    }

    if(len > (int) sizeof(buffer)) {                // would not fit in our buffer? convert only what fits
        len = sizeof(buffer);
    }

    std::string hexValue = getString(key, "INVALID");
    const char *hexString = hexValue.c_str();

    bool good = (strcmp(hexString, "INVALID") != 0);    // if retrieved is something other than the default value, probably valid

    if(good && strlen(hexString) < (size_t) (2 * len)) {    // hex string too short for requested length? don't read past its end
        logHdd(LOG_WARNING, "Settings::getBinaryString -- value of %s is too short for %d bytes", key, len);
        good = false;
    }

    if(good) {                                      // if value seems good, convert it from hex to bin
        char tmp[3];

        for(int i=0; i<len; i++) {
            memcpy(tmp, hexString + 2*i, 2);        // copy 2 bytes from value to tmp
            tmp[2] = 0;                             // terminate with zero

            int val = 0;                            // stays 0 if not a valid hex value
            sscanf(tmp, "%X", &val);                // try to read HEX value from tmp
            buffer[i] = val;                        // store at position i
        }
    }

    return buffer;
}
//-------------------------
void Settings::binToHex(uint8_t *inBfr, int len, char *outBfr)
{
    outBfr[0] = 0;                          // put string terminator at the supplied pointer to output buffer, to start output here
    char tmp[16];

    for(int i=0; i<len; i++) {
        sprintf(tmp, "%02X", inBfr[i]);     // get value from inBfr, convert to HEX into tmp
        strcat(outBfr, tmp);                // append to existing content
    }
}
//-------------------------
void Settings::setBinaryString(const char *key, uint8_t *inBfr, int len)
{
    char tmp[512];
    memset(tmp, 0, 512);

    binToHex(inBfr, len, tmp);  // binary to hex text string
    setString(key, tmp);        // store hex text string as normal string
}
//-------------------------
char Settings::getChar(const char *key, char defValue)
{
    FILE *file = sOpen(key, true);
    if(!file) {                                         // failed to open settings?
//        logHdd(LOG_DEBUG, "Settings::getChar -- returning default value for %s", key);
        return defValue;
    }

    int res;
    char val;
    res = fscanf(file, "%c", &val);                     // try to read the value
    fclose(file);

    if(res != 1) {                                      // failed to read value?
//        logHdd(LOG_DEBUG, "Settings::getChar -- returning default value for %s", key);
        return defValue;
    }

    return val;                                         // return
}

void Settings::setChar(const char *key, char value)
{
    sWrite(key, std::string(1, value));
}
//-------------------------
void Settings::loadAcsiIDs(AcsiIDinfo *aii, bool useDefaultsIfNoSettings)
{
    aii->enabledIDbits = 0;                                 // no bits / IDs enabled yet
    aii->ceddId        = ACSI_ID_NONE;                      // no CE_DD boot ID until TRANSLATED device is found

    aii->gotDevTypeRaw          = false;                    // no raw and translated types found yet
    aii->gotDevTypeTranslated   = false;

    char key[32];
    for(int id=0; id<8; id++) {                         // read the list of device types from settings
        sprintf(key, "ACSI_DEVTYPE_%d", id);            // create settings KEY, e.g. ACSI_DEVTYPE_0

        int devType = getInt(key, DEVTYPE_OFF);

        if(devType != DEVTYPE_RAW && devType != DEVTYPE_TRANSLATED) {   // only RAW and TRANSLATED are supported, anything else (e.g. old DEVTYPE_SD_UNUSED) is OFF
            devType = DEVTYPE_OFF;
        }

        //-------------------------

        aii->acsiIDdevType[id] = devType;

        if(devType != DEVTYPE_OFF) {                    // if ON
            aii->enabledIDbits |= (1 << id);            // set the bit to 1
        }

        if(devType == DEVTYPE_RAW) {                    // found at least one RAW device?
            aii->gotDevTypeRaw = true;
        }

        if(devType == DEVTYPE_TRANSLATED && !aii->gotDevTypeTranslated) {   // first TRANSLATED device? CE_DD boots from it
            aii->ceddId = id;                           // ID of CE_DD device
            aii->gotDevTypeTranslated = true;
        }
    }

    // no RAW or TRANSLATED device enabled? store defaults (ACSI ID 1 as TRANSLATED) and load them
    if(!aii->gotDevTypeRaw && !aii->gotDevTypeTranslated) {
        if(useDefaultsIfNoSettings) {                   // if should use defaults if no settings found, store those defaults and call this function again
            storeDefaultValues();
            loadAcsiIDs(aii, false);                    // ...but call this function without storing defaults next time - to avoid endless loop in some weird case
            return;
        }
    }

    // no TRANSLATED device (e.g. only RAW devices)? CE_DD still boots from default ID, if that ID is not used by anything else
    if(aii->ceddId == ACSI_ID_NONE && aii->acsiIDdevType[CEDD_DEFAULT_ACSI_ID] == DEVTYPE_OFF) {
        aii->ceddId = CEDD_DEFAULT_ACSI_ID;
        aii->enabledIDbits |= (1 << CEDD_DEFAULT_ACSI_ID);     // device must answer on this ID, otherwise nothing could boot from it
    }
}
//-------------------------
void Settings::loadFloppyConfig(FloppyConfig *fc)
{
    fc->enabled         = getBool("FLOPPYCONF_ENABLED",           true);
    fc->id              = getInt ("FLOPPYCONF_DRIVEID",           0);
    fc->writeProtected  = getBool("FLOPPYCONF_WRITEPROTECTED",    false);
    fc->soundEnabled    = getBool("FLOPPYCONF_SOUND_ENABLED",     true);
}

void Settings::saveFloppyConfig(FloppyConfig *fc)
{
    setBool("FLOPPYCONF_ENABLED",           fc->enabled);
    setInt ("FLOPPYCONF_DRIVEID",           fc->id);
    setBool("FLOPPYCONF_WRITEPROTECTED",    fc->writeProtected);
    setBool("FLOPPYCONF_SOUND_ENABLED",     fc->soundEnabled);
}
//-------------------------
FILE *Settings::sOpen(const char *key, bool readNotWrite)
{
    std::string path = Utils::mergeHostPaths2(settingsDir, std::string(key));  // add key at the end of settings + prefix path

    // only for reading - writes go through sWrite(); dir doesn't exist? just fail (getter returns default value)
    return fopen(path.c_str(), readNotWrite ? "r" : "w");
}

// Store setting value atomically (temp file + rename): a reader in another thread or process (webserver) never sees
// an empty / half written value, and a crash during write can't lose the old value.
bool Settings::sWrite(const char *key, const std::string &content)
{
    std::string path = Utils::mergeHostPaths2(settingsDir, std::string(key));  // add key at the end of settings + prefix path

    bool good = Utils::writeFileAtomically(path.c_str(), (const uint8_t *) content.data(), content.size());

    if(!good && errno == ENOENT) {      // dir doesn't exist? create it and try again
        Utils::mkpath(settingsDir.c_str(), SETTINGS_DIR_MODE);
        good = Utils::writeFileAtomically(path.c_str(), (const uint8_t *) content.data(), content.size());
    }

    if(!good) {
        logHdd(LOG_ERROR, "Settings - could not write key %s : %s", key, strerror(errno));
    }

    return good;
}
