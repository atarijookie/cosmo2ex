#ifndef _MEDIAMODULE_H_
#define _MEDIAMODULE_H_

#include <stdint.h>
#include <functional>
#include <string>

#include "medialink.h"

// CosmosEx media module (HOSTMOD_MEDIA_STREAMING) - the host side of CE_VIDEO.TTP and CE_AUDIO.TTP. Commands are in
// media_commands.h; the streams come from ce_media through MediaLink. The ST names the file by its Atari path - it
// must be on a translated drive, so the host can read it.

class AcsiDataTrans;
class TranslatedDisk;

class MediaModule
{
public:
    // Atari path (full, with drive letter) -> host path; false when it's not on a translated drive / doesn't exist
    typedef std::function<bool(const std::string& atariPath, std::string& hostPath)> PathResolver;

    MediaModule(AcsiDataTrans *dt, TranslatedDisk *translated);
    ~MediaModule();

    void processCommand(uint8_t *command);
    void reset(void);                           // new device connected - stop ce_media and the stream

    MediaLink& getLink(void) { return link; }   // for tests
    void setPathResolver(PathResolver r) { resolvePath = r; }

private:
    uint8_t       *cmd;
    AcsiDataTrans *dataTrans;
    MediaLink      link;
    PathResolver   resolvePath;
    uint32_t       nextGen;

    void identify(void);
    void open(void);
    void getInfo(void);
    void getYmTable(void);
    void close(void);
    void getChunk(void);
    void seek(void);
    void input(void);

    static const char *cmdName(uint8_t cmd);
};

#endif
