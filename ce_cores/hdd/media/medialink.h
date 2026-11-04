#ifndef _MEDIALINK_H_
#define _MEDIALINK_H_

#include <stdint.h>
#include <sys/types.h>
#include <atomic>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// Link of the media module to ce_media - the separate program which runs ffmpeg and converts files to what the ST
// plays (../../../ce_media). Listens on a free port on 127.0.0.1, starts ce_media (MEDIA_BIN in .env) pointed at that
// port, and keeps what it sends: the INFO block, the YM table and a queue of chunks - so the ACSI commands are answered
// right away. While the queue is full, the link isn't read: ce_media blocks on the socket and stops decoding.
// Messages: medialink_protocol.h.

typedef std::shared_ptr<const std::vector<uint8_t>> MediaChunk;

class MediaLink
{
public:
    MediaLink();
    ~MediaLink();

    bool start(void);                   // listen + thread if not yet, start ce_media if it's not there; false if can't listen
    void stop(void);                    // stop ce_media (if we started it), the thread, close sockets
    bool isConnected(void) const { return connected; }
    uint16_t port(void) const { return listenPort; }

    // a new stream (replaces the current one): generation, the first 16 B of the OPEN data, host path.
    // Kept and sent when ce_media connects, so it works also while ce_media is starting.
    void open(uint32_t gen, const uint8_t *openHeader, const std::string& hostPath, bool live);
    void seek(uint32_t gen, uint32_t posMs);
    bool input(const uint8_t *codes, int count);
    void close(void);

    // ACSI side
    bool hasStream(void);
    uint32_t generation(void);
    std::vector<uint8_t> info(void);    // MED_INFO_SIZE bytes - from ce_media, or made here while starting / on link errors
    std::vector<uint8_t> ymTable(void); // empty when there's none
    MediaChunk popChunk(uint32_t gen);  // next chunk of this generation, NULL when none is ready
    bool ended(void);                   // the END chunk of the current generation was given out

    void noteActivity(void);            // the ST used it - ce_media is stopped after MEDIA_IDLE_MIN without it

    size_t queuedBytes(void);           // for tests
    void setQueueLimit(size_t bytes) { queueLimit = bytes; }

    // ce_media binary (MEDIA_BIN, else BIN_DIR/ce_media), "" when there's none; its options from .env (-ffmpeg ...) -
    // also used by HostTools for 'ce_media check'
    static std::string mediaBin(void);
    static void appendOptionArgs(std::vector<std::string>& args);

private:
    int  fdListen;
    int  fdClient;
    std::atomic<uint16_t> listenPort;
    std::atomic<bool> connected;
    std::atomic<bool> threadRun;
    std::thread thread;

    pid_t    childPid;
    uint32_t lastSpawnMs;
    bool     spawnedOnce;
    bool     binMissingLogged;
    std::string childBin;               // what was started - for the error messages
    std::atomic<uint32_t> lastActivityMs;
    std::atomic<bool> wantMedia;        // the ST uses the module - keep ce_media running

    std::mutex sendMutex;
    std::mutex dataMutex;               // everything below

    bool        streamOpen;
    bool        live;
    bool        liveAudio;              // live stream with sound - its chunks are not dropped (queueFull() waits instead)
    uint32_t    gen;
    std::vector<uint8_t> openMsg;       // MLINK_OPEN payload of the current stream, sent again when ce_media (re)connects
    std::vector<uint8_t> infoBlk;       // from ce_media, empty until it came
    std::vector<uint8_t> ym;
    std::deque<MediaChunk> queue;
    size_t      queueBytes;
    size_t      queueLimit;
    bool        endGiven;
    std::string linkMsg;
    std::vector<uint8_t> rx;

    void threadCode(void);
    bool openListener(void);
    void acceptClient(void);
    void closeClient(const char *why);
    bool readClient(void);
    void handleMessage(uint8_t type, const uint8_t *payload, uint32_t len);
    bool queueFull(void);

    bool sendMessage(uint8_t type, const std::vector<uint8_t>& payload);
    void clearStream(void);             // dataMutex held

    void spawnMedia(void);
    void checkChild(void);
    void stopChild(void);
    void setLinkMsg(const std::string& msg);
};

#endif
