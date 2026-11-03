#ifndef _BROWSERMODULE_H_
#define _BROWSERMODULE_H_

#include <stdint.h>
#include <memory>

#include "browserlink.h"

// CosmosEx browser module (HOSTMOD_BROWSER) - the host side of CE_BROWS.PRG. Commands are in browser_commands.h; the
// pages come from ce_browser through BrowserLink. Phase 2: every update is a full frame (split into parts when it
// doesn't fit in one DMA transfer); updates of only the changed areas come later.

class AcsiDataTrans;

class BrowserModule
{
public:
    BrowserModule(AcsiDataTrans *dt);
    ~BrowserModule();

    void processCommand(uint8_t *command);      // pCmd: for ICD commands without the 1st byte, as TranslatedDisk gets it
    void reset(void);                           // new device connected - stop ce_browser, forget the mode

    BrowserLink& getLink(void) { return link; } // for tests

    // screen size and bitplanes of a SCREENCAST_RES_* resolution, false for those the browser doesn't support
    static bool resolutionInfo(uint8_t res, uint16_t *width, uint16_t *height, uint8_t *planes);

private:
    uint8_t       *cmd;
    AcsiDataTrans *dataTrans;
    BrowserLink    link;
    BrowserMode    mode;

    std::shared_ptr<const BrowserFrame> sending;   // frame whose parts are being sent (BRW_UPD_MORE)

    void identify(void);
    void setMode(void);
    void navigate(void);
    void navAction(void);
    void mouse(void);
    void key(void);
    void scroll(void);
    void getUpdate(void);
    void getStatus(void);
    void getHistory(void);

    void linkResult(bool sent);                 // status BRW_OK or BRW_ERR_NOT_RUNNING
    static const char *cmdName(uint8_t cmd);
};

#endif
