#include <stdio.h>
#include <string.h>
#include <algorithm>
#include <mutex>

#include "../../misc/debug.h"
#include "../../misc/utils.h"
#include "browser_commands.h"
#include "browserhistory.h"

static std::mutex               historyMutex;
static std::vector<std::string> urls;               // newest first
static bool                     loaded = false;
static std::string              filePath;           // empty = DATA_DIR/browser_history.txt

static std::string path(void)
{
    if(!filePath.empty()) {
        return filePath;
    }
    std::string dir = Utils::dotEnvValue("DATA_DIR", "", false);
    if(dir.empty()) {
        return "";
    }
    return dir + "/browser_history.txt";
}

void BrowserHistory::setFilePath(const std::string& p)
{
    std::lock_guard<std::mutex> lock(historyMutex);
    filePath = p;
    urls.clear();
    loaded = false;
}

void BrowserHistory::loadIfNeeded(void)
{
    if(loaded) {
        return;
    }
    loaded = true;

    std::string p = path();
    FILE *f = p.empty() ? NULL : fopen(p.c_str(), "r");
    if(!f) {
        return;                                     // no history yet
    }

    char line[BRW_URL_SIZE + 2];
    while(fgets(line, sizeof(line), f) && urls.size() < BRW_HISTORY_MAX) {
        size_t len = strcspn(line, "\r\n");
        line[len] = 0;
        if(len > 0 && std::find(urls.begin(), urls.end(), line) == urls.end()) {
            urls.push_back(line);
        }
    }
    fclose(f);
}

void BrowserHistory::save(void)
{
    std::string p = path();
    if(p.empty()) {
        return;
    }

    std::string tmp = p + ".tmp";                   // write whole, then rename - a crash doesn't leave half a file
    FILE *f = fopen(tmp.c_str(), "w");
    if(!f) {
        logHdd(LOG_WARNING, "BrowserHistory - can't write %s", tmp.c_str());
        return;
    }
    for(const std::string& u : urls) {
        fprintf(f, "%s\n", u.c_str());
    }
    fclose(f);
    rename(tmp.c_str(), p.c_str());
}

void BrowserHistory::add(const std::string& url)
{
    if(url.empty() || url.size() >= BRW_URL_SIZE || url.find_first_of("\r\n") != std::string::npos) {
        return;
    }

    std::lock_guard<std::mutex> lock(historyMutex);
    loadIfNeeded();

    auto it = std::find(urls.begin(), urls.end(), url);
    if(it == urls.begin() && !urls.empty()) {      // already the newest - nothing to do
        return;
    }
    if(it != urls.end()) {
        urls.erase(it);
    }
    urls.insert(urls.begin(), url);
    if(urls.size() > BRW_HISTORY_MAX) {
        urls.resize(BRW_HISTORY_MAX);
    }
    save();
}

std::vector<std::string> BrowserHistory::list(size_t maxCount)
{
    std::lock_guard<std::mutex> lock(historyMutex);
    loadIfNeeded();

    if(maxCount == 0 || maxCount > urls.size()) {
        return urls;
    }
    return std::vector<std::string>(urls.begin(), urls.begin() + maxCount);
}
