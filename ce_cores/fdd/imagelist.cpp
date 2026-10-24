// vim: tabstop=4 softtabstop=4 shiftwidth=4 expandtab
#include <string.h>
#include <strings.h>
#include <dirent.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <algorithm>

#include "../misc/utils.h"
#include "../misc/debug.h"
#include "imagelist.h"

ImageList::ImageList()
{
    gen = 0;
    haveMtime = false;
    mtimeNs = 0;
    racy = false;
}

bool ImageList::setFolder(const std::string& newFolder)
{
    if(newFolder == folder) {       // same folder? nothing to do, refresh() catches the changes in it
        return false;
    }

    folder = newFolder;
    scan();
    gen++;                          // other folder = other list, even if the names are the same
    return true;
}

bool ImageList::refresh(void)
{
    if(folder.empty()) {
        return false;
    }

    int64_t ns;
    time_t sec;
    bool found = getFolderMtime(ns, sec);

    if(found == haveMtime && (!found || ns == mtimeNs) && !racy) {    // not changed since the last scan? nothing to do
        return false;
    }

    if(!scan()) {                   // scanned, but the same names?
        return false;
    }

    gen++;
    return true;
}

bool ImageList::getFolderMtime(int64_t& ns, time_t& sec)
{
    struct stat st;
    if(stat(folder.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) {
        ns = 0;
        sec = 0;
        return false;
    }

#if defined(__APPLE__)
    ns = ((int64_t) st.st_mtimespec.tv_sec) * 1000000000LL + st.st_mtimespec.tv_nsec;
#else
    ns = ((int64_t) st.st_mtim.tv_sec) * 1000000000LL + st.st_mtim.tv_nsec;
#endif
    sec = st.st_mtime;
    return true;
}

bool ImageList::scan(void)
{
    std::vector<std::string> newNames;

    time_t mtimeSec = 0;
    haveMtime = !folder.empty() && getFolderMtime(mtimeNs, mtimeSec);  // mtime before reading - a change while reading shows up next time

    // FAT / exFAT keep the mtime in 2 s steps - a change right after the scan could have the same mtime, so scan again
    // until the mtime is safely in the past
    racy = haveMtime && (time(NULL) - mtimeSec) < IMAGE_LIST_RACY_SEC;

    DIR* dir = haveMtime ? opendir(folder.c_str()) : NULL;

    if(dir) {
        struct dirent* de;

        while((de = readdir(dir)) != NULL) {
            if(!isImageName(de->d_name) || strlen(de->d_name) > IMAGE_LIST_NAME_MAX) {
                continue;
            }

            bool isFile = false;

            if(de->d_type == DT_REG) {                  // plain file? no need to stat it
                isFile = true;
            } else if(de->d_type == DT_LNK || de->d_type == DT_UNKNOWN) {  // symlink, or the fs doesn't say? stat it
                struct stat st;
                std::string path = Utils::mergeHostPaths2(folder, de->d_name);
                isFile = (stat(path.c_str(), &st) == 0) && S_ISREG(st.st_mode);
            }

            if(isFile) {
                newNames.push_back(de->d_name);
            }
        }

        closedir(dir);
    } else if(!folder.empty()) {
        logFdd(LOG_DEBUG, "ImageList::scan -- can't read folder %s", folder.c_str());
    }

    std::sort(newNames.begin(), newNames.end(), nameLess);

    if(newNames == names) {
        return false;
    }

    logFdd(LOG_DEBUG, "ImageList::scan -- %s: %d images", folder.c_str(), (int) newNames.size());
    names.swap(newNames);
    return true;
}

const std::string& ImageList::getName(uint32_t index) const
{
    static const std::string empty;
    return (index < names.size()) ? names[index] : empty;
}

uint32_t ImageList::indexOf(const std::string& name) const
{
    std::vector<std::string>::const_iterator it = std::lower_bound(names.begin(), names.end(), name, nameLess);

    if(it == names.end() || *it != name) {
        return IMAGE_LIST_NONE;
    }

    return (uint32_t) (it - names.begin());
}

uint32_t ImageList::storePage(uint8_t* bfr, uint32_t start, uint32_t pageCount, uint32_t currentIndex) const
{
    uint32_t total = count();
    pageCount = MIN(pageCount, (uint32_t) IMAGE_LIST_PAGE_MAX);

    if(currentIndex >= total) {
        currentIndex = IMAGE_LIST_NONE;
    }

    if(start == IMAGE_LIST_NONE) {      // page around the current image
        start = (currentIndex == IMAGE_LIST_NONE || currentIndex < pageCount / 2) ? 0 : (currentIndex - pageCount / 2);
    }

    if(start > total || (total - start) < pageCount) {     // beyond the end? move it back, so the page is full
        start = (total > pageCount) ? (total - pageCount) : 0;
    }

    pageCount = MIN(pageCount, total - start);

    Utils::storeDword(bfr +  0, gen);
    Utils::storeDword(bfr +  4, total);
    Utils::storeDword(bfr +  8, currentIndex);
    Utils::storeDword(bfr + 12, start);
    Utils::storeWord (bfr + 16, (uint16_t) pageCount);

    uint32_t len = IMAGE_LIST_PAGE_HEADER;

    for(uint32_t i=0; i<pageCount; i++) {
        const std::string& name = names[start + i];     // names are max IMAGE_LIST_NAME_MAX long, see scan()
        bfr[len++] = (uint8_t) name.size();
        memcpy(bfr + len, name.c_str(), name.size());
        len += name.size();
    }

    return len;
}

bool ImageList::isImageName(const char* name)
{
    const char* ext = strrchr(name, '.');

    if(!ext || ext == name) {           // no extension, or just ".st" - nothing before the dot
        return false;
    }

    return strcasecmp(ext, ".st") == 0 || strcasecmp(ext, ".msa") == 0;
}

static inline unsigned char lowerAscii(char c)
{
    unsigned char u = (unsigned char) c;
    return (u >= 'A' && u <= 'Z') ? (u + ('a' - 'A')) : u;
}

bool ImageList::nameLess(const std::string& a, const std::string& b)
{
    size_t n = MIN(a.size(), b.size());

    for(size_t i=0; i<n; i++) {
        unsigned char ca = lowerAscii(a[i]);
        unsigned char cb = lowerAscii(b[i]);

        if(ca != cb) {
            return ca < cb;
        }
    }

    if(a.size() != b.size()) {          // one is the start of the other? the shorter first
        return a.size() < b.size();
    }

    return a < b;                       // differ only in case - by bytes, so the order is always the same (as in fdd.go)
}
