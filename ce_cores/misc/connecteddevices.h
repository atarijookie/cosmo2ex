#ifndef _CONNECTEDDEVICES_H_
#define _CONNECTEDDEVICES_H_

#include <stdint.h>
#include <set>
#include <string>

// Which devices have at least one connection to any core (HDD / FDD / IKBD) right now. Each core reports the MACs of its
// connected clients, the union of them is written to CONNECTED_DEVICES_FILE (one MAC per line, 12 hex chars, like the
// device dirs in SETTINGS_DIR) whenever it changes - the webserver reads it to show which devices are connected.
class ConnectedDevices {
public:
    // these lock the connected devices mutex, can be called from any thread
    static void update(const void* core, const std::set<std::string>& macs);   // all connected MACs of this core
    static std::set<std::string> getAll(void);                                  // union of MACs from all cores
    static void clear(void);            // forget all, write empty file - on start (leftover from crash) and on exit

    static std::string macToString(const uint8_t* mac);     // "2CCF67C4B1E5", "" for unknown (all zeros) mac

private:
    // must be called with the mutex locked
    static void writeFile(const std::set<std::string>& all);
};

#endif
