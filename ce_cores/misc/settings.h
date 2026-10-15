#ifndef _SETTINGS_H_
#define _SETTINGS_H_

#include <stdio.h>
#include <stdint.h>
#include <string>

#define ACSI_ID_NONE            0xff    // no ACSI ID, e.g. ceddId when there's no ID for CE_DD booting
#define CEDD_DEFAULT_ACSI_ID    1       // CE_DD boots from this ID when no TRANSLATED ID is set and this ID is not used

typedef struct {
    uint8_t acsiIDdevType[8];       // array of device types for each ACSI ID
    uint8_t ceddId;                 // id of CE_DD, which will be used to boot CE_DD.PRG; ACSI_ID_NONE if none

    uint8_t enabledIDbits;          // bit map of which ACSI IDs are enabled

    bool gotDevTypeRaw;
    bool gotDevTypeTranslated;
} AcsiIDinfo;

typedef struct {
    bool enabled;
    int  id;
    bool writeProtected;
    bool soundEnabled;
} FloppyConfig;

class Settings
{
public:
    Settings(void);
    Settings(uint8_t* mac);
    virtual ~Settings(void) { };

    void setPrefix(uint8_t* bytes, int len);

    bool getBool(const char *key, bool defValue);
    void setBool(const char *key, bool value);

    int  getInt(const char *key, int defValue);
    void setInt(const char *key, int value);

    float getFloat(const char *key, float defValue);
    void  setFloat(const char *key, float value);

    std::string getString(const char *key, const char *defValue);     // returns a copy - thread safe
    void  setString(const char *key, const char *value);

    char *getBinaryString(const char *key, int len);
    void  setBinaryString(const char *key, uint8_t *inBfr, int len);

    char getChar(const char *key, char defValue);
    void setChar(const char *key, char value);

    void loadAcsiIDs(AcsiIDinfo *aii, bool useDefaultsIfNoSettings=true);

    void loadFloppyConfig(FloppyConfig *fc);
    void saveFloppyConfig(FloppyConfig *fc);

    static void binToHex(uint8_t *inBfr, int len, char *outBfr);

private:
    void init(uint8_t* prefixBytes, int len);

    char prefix[32];
    std::string settingsDir;            // settings dir including prefix subdir - from settingsDirPath(), updated by setPrefix()

    FILE *sOpen(const char *key, bool readNotWrite);
    std::string settingsDirPath(void);      // settings dir, including prefix subdir if prefix is set
    void storeDefaultValues(void);
};

#endif
