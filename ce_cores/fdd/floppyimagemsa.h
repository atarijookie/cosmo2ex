// vim: shiftwidth=4 softtabstop=4 tabstop=4 expandtab
#ifndef FLOPPYIMAGEMSA_H
#define FLOPPYIMAGEMSA_H

#include "floppyimage.h"

class FloppyImageMsa: public FloppyImage
{
public:
    virtual ~FloppyImageMsa();                  // saves unsaved changes - must be done here, not in base destructor

    virtual bool open(const char *fileName);
    virtual bool save(void);

protected:
    virtual bool loadImageIntoMemory(void);

private:
    int firstStoredTrack = 0;                   // MSA 'starting track' - tracks before it aren't stored in the file (they're empty in memory)
};

#endif // FLOPPYIMAGEST_H
