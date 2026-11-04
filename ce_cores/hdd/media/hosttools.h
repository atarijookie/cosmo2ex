#ifndef _HOSTTOOLS_H_
#define _HOSTTOOLS_H_

// Which of the host-side helpers can work here - so the config drive shows only the Atari programs which will:
//   CE_VIDEO.TTP, CE_AUDIO.TTP  need ce_media + ffmpeg (or docker with MEDIA_DOCKER_IMAGE)   'ce_media check ffmpeg'
//   CE_DOOM.PRG                 need ce_media + ce_doomgeneric + a WAD                        'ce_media check doom'
//   CE_BROWS.PRG                needs ce_browser + Chrome                                     'ce_browser check'
// The helpers answer themselves (exit code 0 = there), run as the user they'd run as - they know their defaults
// (Chrome in that user's cache dir, the WAD next to ce_media, ...). The checks run in the background, at start and
// again when the config drive's root is listed and the last check is older than CHECK_INTERVAL_MS; until the first
// one is done, nothing is hidden.

#include <stdint.h>
#include <string>
#include <vector>

class HostTools
{
public:
    enum Tool { FFMPEG = 0, DOOM, CHROME, TOOL_COUNT };

    static void refresh(void);                          // start the checks in the background, unless done recently
    static int  state(Tool t);                          // 1 there, 0 missing, -1 not known (yet)
    static bool hideOnConfigDrive(const std::string& name);    // a file in the config drive's root to leave out

    static void setStateForTests(Tool t, int s);        // also stops the background checks from changing it
    static void resetForTests(void);                    // back to 'not known', checks run again on refresh()
    static bool checkRunning(void);

private:
    static void runChecks(void);
    static int  runCheck(const std::string& bin, const std::vector<std::string>& args, const char *userEnvKey, bool allowRoot);
};

#endif
