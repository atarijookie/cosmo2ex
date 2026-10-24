// vim: tabstop=4 softtabstop=4 shiftwidth=4 expandtab
#ifndef _IMAGELIST_H_
#define _IMAGELIST_H_

#include <stdint.h>
#include <time.h>
#include <string>
#include <vector>

// Sorted list of the floppy images in one folder - for browsing the images on the device (prev / next / insert).
// Must be the same list as in the web UI (webserver/fdd.go - floppyImageNames()), so that both step through the images
// in the same order: only .st and .msa files (any case), regular files (symlinks followed), names up to 255 bytes,
// sorted case insensitive (ASCII only), names which differ only in case by their bytes.

#define IMAGE_LIST_NONE         0xffffffff  // no such index / current image not in the list / page around the current image
#define IMAGE_LIST_PAGE_MAX     16          // max names in one page sent to device
#define IMAGE_LIST_NAME_MAX     255         // name length is sent in one byte

#define IMAGE_LIST_PAGE_HEADER  18          // gen, total, current, start (4 B each), count (2 B)
#define IMAGE_LIST_PAGE_MAX_SIZE    (IMAGE_LIST_PAGE_HEADER + IMAGE_LIST_PAGE_MAX * (1 + IMAGE_LIST_NAME_MAX))

#define IMAGE_LIST_RACY_SEC     3           // folder changed this close to the scan? scan again on the next refresh()

class ImageList
{
public:
    ImageList();

    // Folder to list, "" = nothing to browse. Scans it when it's not the folder listed now. Returns true if the list
    // changed (gen incremented) - a different folder always counts as a change.
    bool setFolder(const std::string& newFolder);

    // Scans the folder again if it changed on disk (its mtime - files added, deleted, renamed). Returns true if the
    // list changed (gen incremented).
    bool refresh(void);

    const std::string& getFolder(void) const    { return folder; }
    uint32_t getGen(void) const                 { return gen; }
    uint32_t count(void) const                  { return (uint32_t) names.size(); }
    const std::string& getName(uint32_t index) const;
    uint32_t indexOf(const std::string& name) const;        // IMAGE_LIST_NONE if not in the list

    // Stores a page of names for the device (see ATN_GET_IMAGE_LIST), returns its size (at most IMAGE_LIST_PAGE_MAX_SIZE).
    // start = IMAGE_LIST_NONE: page around currentIndex. The page is moved to fit into the list, so it's full if the
    // list has enough names.
    uint32_t storePage(uint8_t* bfr, uint32_t start, uint32_t pageCount, uint32_t currentIndex) const;

    static bool isImageName(const char* name);              // .st / .msa, any case
    static bool nameLess(const std::string& a, const std::string& b);   // the sort order

private:
    std::string folder;
    std::vector<std::string> names;
    uint32_t gen;

    bool    haveMtime;      // folder was found on the last scan
    int64_t mtimeNs;        // folder mtime on the last scan
    bool    racy;           // the folder changed too short before the last scan - a change in the same mtime tick could be missed

    bool scan(void);        // read and sort the names, true if they differ from the previous ones
    bool getFolderMtime(int64_t& ns, time_t& sec);
};

#endif
