//--------------------------------------------------
// CosmosEx test collection - one app containing all the tests,
// each sub-test lives in its own sub-folder and keeps its own menu.
//--------------------------------------------------
#include <mint/osbind.h>

#include "../libacsiscsi/hdd_if.h"
#include "common/helpers.h"
#include "common/VT52.h"
#include "common/version.h"

// entry points of the sub-tests, these were the main() functions of the original apps
int test_acsi_main(void);
int test_filesystem_main(void);
int test_floppy_main(void);
int test_floppy_read_main(void);
int test_ikbd_main(void);
int test_manual_main(void);
int test_manual2_main(void);
int test_rw_main(void);
int test_rw_user_main(void);

typedef struct {
    char        key;
    const char *name;
    const char *description;
    int         (*run)(void);
} TSubTest;

static const TSubTest subTests[] = {
    {'1', "test_acsi",        "ACSI/SCSI read/write, DMA check",  test_acsi_main},
    {'2', "test_filesystem",  "GEMDOS filesystem functions",      test_filesystem_main},
    {'3', "test_floppy",      "floppy read / write / verify",     test_floppy_main},
    {'4', "test_floppy_read", "manual floppy sector requests",    test_floppy_read_main},
    {'5', "test_ikbd",        "IKBD injection chain",             test_ikbd_main},
    {'6', "test_manual",      "manual HDD test tool",             test_manual_main},
    {'7', "test_manual2",     "manual HDD tool, large reads",     test_manual2_main},
    {'8', "test_rw",          "ACSI sector / file write+read",    test_rw_main},
    {'9', "test_rw_user",     "destructive ACSI write+read",      test_rw_user_main},
    {0,   0,                  0,                                  0}
};

static void showMainMenu(void)
{
    int i;

    VT52_Clear_home();
    (void) Cconws("\33p[ CosmosEx tests      ver ");
    showAppVersion();
    (void) Cconws(" ]\33q\r\n\r\n");

    for(i=0; subTests[i].key != 0; i++) {
        (void) Cconws(" \33p[ ");
        Cconout(subTests[i].key);
        (void) Cconws(" ]\33q ");
        (void) Cconws(subTests[i].name);
        (void) Cconws("\r\n         ");
        (void) Cconws(subTests[i].description);
        (void) Cconws("\r\n");
    }

    (void) Cconws("\r\n \33p[ Q ]\33q quit\r\n");
}

static void runSubTest(const TSubTest *test)
{
    memset(&hdIf, 0, sizeof(hdIf));     // each test starts with HDD IF state as if it was a standalone app

    removeAllWaitingKeys();
    VT52_Clear_home();

    test->run();

    // the test might have left the screen in some weird state, restore it
    VT52_Rev_off();
    (void) VT52_Cur_on();

    removeAllWaitingKeys();
}

int main(void)
{
    removeAllWaitingKeys();             // read all the possibly waiting keys, so we can ignore them...

    while(1) {
        showMainMenu();

        char key = Cnecin();

        if(key == 'q' || key == 'Q') {  // quit?
            break;
        }

        int i;
        for(i=0; subTests[i].key != 0; i++) {
            if(subTests[i].key == key) {
                runSubTest(&subTests[i]);
                break;
            }
        }
    }

    VT52_Clear_home();
    return 0;
}
