#include "dateacsicommand.h"
#include "../../misc/global.h"
#include "../../misc/debug.h"

DateAcsiCommand::DateAcsiCommand(AcsiDataTrans *dt):dataTrans(dt)
{
}

DateAcsiCommand::~DateAcsiCommand()
{
}

void DateAcsiCommand::processCommand(uint8_t *command)
{
    cmd = command;

    if(dataTrans == 0) {
        logHdd(LOG_ERROR, "DateAcsiCommand::processCommand was called without valid dataTrans, can't tell ST that his went wrong...");
        return;
    }

    dataTrans->clear();                 // clean data transporter before handling

    switch(cmd[4]) {
        case TRAN_CMD_GETDATETIME:                    // return the current date of DateService
          // we don't have a reliable date to report, so we won't (ST gets date / time from GD_CUSTOM_getConfig instead)
          dataTrans->setStatus(DATE_DATETIME_UNKNOWN);
          break;
    }

    // data and status are sent by the caller (TranslatedDisk::processCommand), like for the other translated disk commands
    logHdd(LOG_DEBUG, "DateAcsiCommand done.");
}
