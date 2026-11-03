#ifndef _BROWSERHISTORY_H_
#define _BROWSERHISTORY_H_

#include <string>
#include <vector>

// URLs of the pages which loaded successfully (ce_browser reports them), newest first, each once, at most
// BRW_HISTORY_MAX. One history for the whole host - all devices share it. Kept in DATA_DIR/browser_history.txt,
// one URL per line, so it survives restarts. CE_BROWS.PRG gets it with GET_HISTORY and filters it as the user types.

class BrowserHistory
{
public:
    static void add(const std::string& url);
    static std::vector<std::string> list(size_t maxCount);  // newest first, maxCount 0 = all

    static void setFilePath(const std::string& path);       // tests: another file (forgets what was loaded)

private:
    static void loadIfNeeded(void);                         // historyMutex must be held
    static void save(void);                                 // historyMutex must be held
};

#endif
