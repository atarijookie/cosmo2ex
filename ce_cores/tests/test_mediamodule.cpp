// vim: shiftwidth=4 softtabstop=4 tabstop=4 expandtab
// Tests of the media module (HOSTMOD_MEDIA_STREAMING) - the host side of CE_VIDEO.TTP / CE_AUDIO.TTP. The commands are
// what the players send; ce_media is played by a socket connected to the port MediaLink listens on.

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <poll.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <functional>
#include <string>
#include <vector>

#include "doctest.h"

#include "misc/global.h"
#include "misc/utils.h"
#include "hdd/acsidatatrans.h"
#include "hdd/media/mediamodule.h"
#include "hdd/media/media_commands.h"
#include "hdd/media/medialink_protocol.h"

namespace {

class MedFakeDataTrans : public AcsiDataTrans
{
public:
    std::vector<uint8_t> fromSt;
    std::vector<uint8_t> reply;
    uint8_t lastStatus = 0xff;

    bool recvData(uint8_t *data, uint32_t cnt) {
        memset(data, 0, cnt);
        memcpy(data, fromSt.data(), fromSt.size() < cnt ? fromSt.size() : cnt);
        return true;
    }
    void setStatus(uint8_t stat) {
        lastStatus = stat;
        AcsiDataTrans::setStatus(stat);
    }
    void sendDataAndStatus(bool fromRetryModule = false) {
        (void) fromRetryModule;
        reply.assign(buffer, buffer + count);
    }
};

bool medWaitFor(std::function<bool()> cond, int ms = 2000)
{
    for(int i=0; i<ms / 10; i++) {
        if(cond()) {
            return true;
        }
        Utils::sleepMs(10);
    }
    return cond();
}

// plays ce_media
struct FakeMedia {
    int fd = -1;
    std::vector<uint8_t> rx;

    ~FakeMedia() { if(fd >= 0) close(fd); }

    bool connectTo(uint16_t port) {
        fd = socket(AF_INET, SOCK_STREAM, 0);
        struct sockaddr_in a;
        memset(&a, 0, sizeof(a));
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        a.sin_port = htons(port);
        return connect(fd, (struct sockaddr *) &a, sizeof(a)) == 0;
    }

    void send(uint8_t type, const std::vector<uint8_t>& payload) {
        std::vector<uint8_t> m(MLINK_HEADER_SIZE + payload.size());
        m[0] = type;
        Utils::storeDword(&m[1], payload.size());
        if(!payload.empty()) {
            memcpy(&m[MLINK_HEADER_SIZE], payload.data(), payload.size());
        }
        size_t sent = 0;
        while(sent < m.size()) {
            ssize_t r = ::send(fd, m.data() + sent, m.size() - sent, 0);
            REQUIRE(r > 0);
            sent += r;
        }
    }

    bool recv(uint8_t &type, std::vector<uint8_t> &payload, int timeoutMs = 2000) {
        for(int waited = 0; ; waited += 10) {
            if(rx.size() >= MLINK_HEADER_SIZE) {
                uint32_t len = Utils::getDword(&rx[1]);
                if(rx.size() >= MLINK_HEADER_SIZE + len) {
                    type = rx[0];
                    payload.assign(rx.begin() + MLINK_HEADER_SIZE, rx.begin() + MLINK_HEADER_SIZE + len);
                    rx.erase(rx.begin(), rx.begin() + MLINK_HEADER_SIZE + len);
                    return true;
                }
            }
            if(waited >= timeoutMs) {
                return false;
            }
            struct pollfd p = { fd, POLLIN, 0 };
            if(poll(&p, 1, 10) > 0) {
                uint8_t buf[65536];
                ssize_t r = ::recv(fd, buf, sizeof(buf), 0);
                if(r <= 0) {
                    return false;
                }
                rx.insert(rx.end(), buf, buf + r);
            }
        }
    }

    std::vector<uint8_t> expect(uint8_t wantType) {
        uint8_t type;
        std::vector<uint8_t> payload;
        while(recv(type, payload)) {
            if(type == wantType) {
                return payload;
            }
        }
        FAIL("message " << (int) wantType << " didn't come");
        return {};
    }

    void sendInfo(uint8_t state, uint32_t durationMs = 60000, const char *msg = "") {
        std::vector<uint8_t> b(MED_INFO_SIZE, 0);
        b[MED_INFO_STATE] = state;
        b[MED_INFO_VMODE] = MED_VMODE_ST_LOW;
        Utils::storeDword(&b[MED_INFO_DURATION_MS], durationMs);
        strcpy((char *) &b[MED_INFO_MSG], msg);
        send(MLINK_INFO, b);
    }

    // chunk: header + 'size' bytes in total, bytes after the header count up from 'first'
    void sendChunk(uint32_t gen, uint32_t seq, uint32_t size = 4096, uint8_t flags = 0, uint8_t first = 0) {
        std::vector<uint8_t> c(size, 0);
        Utils::storeDword(&c[MED_CHUNK_GENERATION], gen);
        Utils::storeDword(&c[MED_CHUNK_SEQ], seq);
        c[MED_CHUNK_FLAGS] = flags;
        for(uint32_t i=MED_CHUNK_HEADER_SIZE; i<size; i++) {
            c[i] = (uint8_t) (first + i);
        }
        send(MLINK_CHUNK, c);
    }
};

struct MedHarness {
    MedFakeDataTrans dt;
    MediaModule     *mm;
    std::string      resolvedAtari;

    MedHarness() {
        unsetenv("SUDO_USER");                      // the module must not try to start a real ce_media
        mm = new MediaModule(&dt, NULL);
        mm->setPathResolver([this](const std::string& atari, std::string& host) {
            resolvedAtari = atari;
            if(atari.find("MISSING") != std::string::npos) {
                return false;
            }
            host = "/media/" + atari.substr(3);
            return true;
        });
    }
    ~MedHarness() { delete mm; }

    uint8_t command(uint8_t c, std::vector<uint8_t> args = {}, std::vector<uint8_t> data = {}) {
        uint8_t cmd[16];
        memset(cmd, 0, sizeof(cmd));
        cmd[1] = 'C';
        cmd[2] = 'E';
        cmd[3] = HOSTMOD_MEDIA_STREAMING;
        cmd[4] = c;
        for(size_t i=0; i<args.size() && i<7; i++) {
            cmd[5 + i] = args[i];
        }
        dt.fromSt = data;
        dt.reply.clear();
        dt.lastStatus = 0xff;
        mm->processCommand(cmd);
        return dt.lastStatus;
    }

    uint8_t open(const std::string& path, uint8_t flags = 0, uint32_t startMs = 0) {
        std::vector<uint8_t> d(MED_OPEN_SIZE, 0);
        d[MED_OPEN_MACHINE]     = MED_MACH_STE;
        d[MED_OPEN_VMODE]       = MED_VMODE_ST_LOW;
        d[MED_OPEN_AOUT]        = MED_AOUT_DMA;
        d[MED_OPEN_FLAGS]       = flags;
        Utils::storeWord(&d[MED_OPEN_LATENCY], 1000);
        d[MED_OPEN_MAX_SECTORS] = 254;
        Utils::storeDword(&d[MED_OPEN_START_MS], startMs);
        memcpy(&d[MED_OPEN_PATH], path.data(), path.size());
        return command(MED_CMD_OPEN, {}, d);
    }

    uint8_t getChunk(uint32_t gen, uint8_t maxSectors = 254) {
        return command(MED_CMD_GET_CHUNK, { maxSectors, (uint8_t) (gen >> 24), (uint8_t) (gen >> 16), (uint8_t) (gen >> 8), (uint8_t) gen });
    }

    uint8_t seek(uint32_t ms) {
        return command(MED_CMD_SEEK, { (uint8_t) (ms >> 24), (uint8_t) (ms >> 16), (uint8_t) (ms >> 8), (uint8_t) ms });
    }

    void connect(FakeMedia& fm) {
        REQUIRE(command(MED_CMD_IDENTIFY) == MED_OK);
        REQUIRE(fm.connectTo(mm->getLink().port()));
        REQUIRE(medWaitFor([&]() { return mm->getLink().isConnected(); }));
        fm.expect(MLINK_HELLO);
    }
};

} // namespace

TEST_CASE("media module - identify") {
    MedHarness h;
    REQUIRE(h.command(MED_CMD_IDENTIFY) == MED_OK);
    REQUIRE(h.dt.reply.size() == 512);
    CHECK(strcmp((const char *) h.dt.reply.data(), "CosmosEx media module") == 0);
    CHECK(Utils::getWord(&h.dt.reply[32]) == MEDIA_MODULE_VERSION);
    CHECK((h.dt.reply[34] & MED_IDF_CONNECTED) == 0);
    CHECK(h.mm->getLink().port() != 0);
}

TEST_CASE("media module - commands need a stream, bad params") {
    MedHarness h;
    CHECK(h.getChunk(1) == MED_ERR_NO_STREAM);
    CHECK(h.seek(1000) == MED_ERR_NO_STREAM);
    CHECK(h.command(MED_CMD_GET_YM_TABLE) == MED_ERR_NO_STREAM);
    CHECK(h.command(0x7f) == MED_ERR_BAD_PARAM);

    REQUIRE(h.command(MED_CMD_GET_INFO) == MED_OK);
    CHECK(h.dt.reply[MED_INFO_STATE] == MED_STATE_NONE);

    CHECK(h.open("") == MED_ERR_BAD_PARAM);
    CHECK(h.open("E:\\MISSING.MP4") == MED_ERR_NOT_FOUND);
    CHECK(h.resolvedAtari == "E:\\MISSING.MP4");
}

TEST_CASE("media module - open before ce_media connects, info, ym table, chunks") {
    MedHarness h;
    REQUIRE(h.open("E:\\VIDEO\\CLIP.MP4") == MED_OK);

    REQUIRE(h.command(MED_CMD_GET_INFO) == MED_OK);                 // ce_media isn't there yet
    CHECK(h.dt.reply[MED_INFO_STATE] != MED_STATE_READY);
    CHECK(h.dt.reply[MED_INFO_STATE] != MED_STATE_NONE);

    FakeMedia fm;
    h.connect(fm);
    std::vector<uint8_t> op = fm.expect(MLINK_OPEN);                // the kept OPEN comes on connect
    REQUIRE(op.size() > 4 + MED_OPEN_PATH);
    uint32_t gen = Utils::getDword(op.data());
    CHECK(op[4 + MED_OPEN_MACHINE] == MED_MACH_STE);
    CHECK(op[4 + MED_OPEN_VMODE] == MED_VMODE_ST_LOW);
    CHECK(Utils::getWord(&op[4 + MED_OPEN_LATENCY]) == 1000);
    CHECK(std::string(op.begin() + 4 + MED_OPEN_PATH, op.end()) == Utils::pathToUi("/media/VIDEO\\CLIP.MP4"));    // ce_media gets the path in the UI form (C:\... on Cygwin)

    REQUIRE(h.command(MED_CMD_GET_INFO) == MED_OK);
    CHECK(h.dt.reply[MED_INFO_STATE] == MED_STATE_STARTING);

    std::vector<uint8_t> ym(MED_YM_TABLE_SIZE, 0x5a);
    fm.send(MLINK_YM_TABLE, ym);
    fm.sendInfo(MED_STATE_READY);
    REQUIRE(medWaitFor([&]() { h.command(MED_CMD_GET_INFO); return h.dt.reply[MED_INFO_STATE] == MED_STATE_READY; }));
    CHECK(h.dt.reply.size() == MED_INFO_SIZE);
    REQUIRE(h.command(MED_CMD_GET_YM_TABLE) == MED_OK);
    CHECK(h.dt.reply == ym);

    // nothing yet: an empty header
    REQUIRE(h.getChunk(gen) == MED_OK);
    CHECK(h.dt.reply.size() == MED_CHUNK_HEADER_SIZE);
    CHECK(h.dt.reply[MED_CHUNK_FLAGS] == MED_CHF_EMPTY);
    CHECK(Utils::getDword(&h.dt.reply[MED_CHUNK_GENERATION]) == gen);

    fm.sendChunk(gen, 0, 100000, 0, 1);
    fm.sendChunk(gen, 1, 2048, MED_CHF_END, 2);
    REQUIRE(medWaitFor([&]() { return h.mm->getLink().queuedBytes() == 102048; }));

    REQUIRE(h.getChunk(gen + 1) == MED_OK);                         // wrong generation - not given
    CHECK(h.dt.reply[MED_CHUNK_FLAGS] == MED_CHF_EMPTY);

    REQUIRE(h.getChunk(gen, 100) == MED_ERR_BAD_PARAM);             // doesn't fit in what the ST takes now - dropped

    REQUIRE(h.getChunk(gen) == MED_OK);
    REQUIRE(h.dt.reply.size() == 2048);
    CHECK(Utils::getDword(&h.dt.reply[MED_CHUNK_SEQ]) == 1);
    CHECK(h.dt.reply[600] == (uint8_t) (2 + 600));

    REQUIRE(h.getChunk(gen) == MED_OK);                             // after the END: empty + END
    CHECK(h.dt.reply[MED_CHUNK_FLAGS] == (MED_CHF_EMPTY | MED_CHF_END));

    REQUIRE(h.command(MED_CMD_CLOSE) == MED_OK);
    fm.expect(MLINK_CLOSE);
    CHECK(h.getChunk(gen) == MED_ERR_NO_STREAM);
}

TEST_CASE("media module - seek drops old chunks, clamps to the duration") {
    MedHarness h;
    FakeMedia fm;
    h.connect(fm);
    REQUIRE(h.open("E:\\A.MP4", 0, 5000) == MED_OK);
    std::vector<uint8_t> op = fm.expect(MLINK_OPEN);
    uint32_t gen = Utils::getDword(op.data());
    CHECK(Utils::getDword(&op[4 + MED_OPEN_START_MS]) == 5000);
    fm.sendInfo(MED_STATE_READY, 60000);
    fm.sendChunk(gen, 0);
    REQUIRE(medWaitFor([&]() { return h.mm->getLink().queuedBytes() > 0; }));

    REQUIRE(h.seek(30000) == MED_OK);
    uint32_t gen2 = Utils::getDword(&h.dt.reply[0]);
    CHECK(gen2 != gen);
    CHECK(Utils::getDword(&h.dt.reply[4]) == 30000);
    std::vector<uint8_t> sk = fm.expect(MLINK_SEEK);
    CHECK(Utils::getDword(&sk[0]) == gen2);
    CHECK(Utils::getDword(&sk[4]) == 30000);
    CHECK(h.mm->getLink().queuedBytes() == 0);

    fm.sendChunk(gen, 1);                                           // still in flight from before the seek - dropped
    fm.sendChunk(gen2, 0, 4096, 0, 9);
    REQUIRE(medWaitFor([&]() { return h.mm->getLink().queuedBytes() == 4096; }));
    REQUIRE(h.getChunk(gen2) == MED_OK);
    CHECK(Utils::getDword(&h.dt.reply[MED_CHUNK_GENERATION]) == gen2);

    REQUIRE(h.seek(999999) == MED_OK);                              // past the end - the last second
    CHECK(Utils::getDword(&h.dt.reply[4]) == 59000);
}

TEST_CASE("media module - full queue stops reading, live keeps the newest") {
    MedHarness h;
    FakeMedia fm;
    h.connect(fm);
    h.mm->getLink().setQueueLimit(150000);
    REQUIRE(h.open("E:\\A.MP4") == MED_OK);
    uint32_t gen = Utils::getDword(fm.expect(MLINK_OPEN).data());

    // chunks bigger than one recv() of the link - it stops reading soon after the limit
    for(int i=0; i<6; i++) {
        fm.sendChunk(gen, i, 100000);
    }
    Utils::sleepMs(300);
    size_t q = h.mm->getLink().queuedBytes();
    CHECK(q >= 150000);
    CHECK(q <= 300000);
    for(int i=0; i<6; i++) {                                        // room again - reads on; in order, none lost
        REQUIRE(medWaitFor([&]() { return h.mm->getLink().queuedBytes() > 0; }));
        REQUIRE(h.getChunk(gen) == MED_OK);
        CHECK(Utils::getDword(&h.dt.reply[MED_CHUNK_SEQ]) == (uint32_t) i);
    }

    REQUIRE(h.open("E:\\GAME", MED_OPF_LIVE) == MED_OK);
    gen = Utils::getDword(fm.expect(MLINK_OPEN).data());
    for(int i=0; i<5; i++) {
        fm.sendChunk(gen, i, 4096);
    }
    REQUIRE(medWaitFor([&]() { return h.mm->getLink().queuedBytes() == 8192; }));
    Utils::sleepMs(100);
    REQUIRE(h.getChunk(gen) == MED_OK);
    CHECK(Utils::getDword(&h.dt.reply[MED_CHUNK_SEQ]) == 3);         // only the 2 newest are kept

    uint8_t keys[2] = { 0x39, 0xb9 };
    REQUIRE(h.command(MED_CMD_INPUT, { 2, keys[0], keys[1] }) == MED_OK);
    std::vector<uint8_t> in = fm.expect(MLINK_INPUT);
    CHECK(in == std::vector<uint8_t>(keys, keys + 2));
    CHECK(h.command(MED_CMD_INPUT, { 7 }) == MED_ERR_BAD_PARAM);
}

TEST_CASE("media module - ce_media errors and disconnects show in INFO") {
    MedHarness h;
    FakeMedia fm;
    h.connect(fm);
    REQUIRE(h.open("E:\\BAD.MP4") == MED_OK);
    fm.expect(MLINK_OPEN);
    fm.sendInfo(MED_STATE_ERROR, 0, "ffprobe: Invalid data");
    REQUIRE(medWaitFor([&]() { h.command(MED_CMD_GET_INFO); return h.dt.reply[MED_INFO_STATE] == MED_STATE_ERROR; }));
    CHECK(std::string((const char *) &h.dt.reply[MED_INFO_MSG]) == "ffprobe: Invalid data");

    REQUIRE(h.open("E:\\GOOD.MP4") == MED_OK);
    fm.expect(MLINK_OPEN);
    fm.sendInfo(MED_STATE_READY);
    REQUIRE(medWaitFor([&]() { h.command(MED_CMD_GET_INFO); return h.dt.reply[MED_INFO_STATE] == MED_STATE_READY; }));

    close(fm.fd);                                                   // ce_media crashed
    fm.fd = -1;
    REQUIRE(medWaitFor([&]() { return !h.mm->getLink().isConnected(); }));
    REQUIRE(h.command(MED_CMD_GET_INFO) == MED_OK);
    CHECK(h.dt.reply[MED_INFO_STATE] == MED_STATE_ERROR);
}

//------------------------------------------------------------------------------------------------------------------
// With the real ce_media - only when CE_MEDIA_TEST_BIN and CE_MEDIA_TEST_FILE are set (needs ffmpeg, or docker with
// MEDIA_DOCKER_IMAGE set), e.g.:
//   CE_MEDIA_TEST_BIN=../ce_media/ce_media CE_MEDIA_TEST_FILE=/tmp/test.mp4 MEDIA_DOCKER_IMAGE=linuxserver/ffmpeg
//     ./tests/run_tests.elf -tc="*real ce_media*"

namespace {

void playSome(MedHarness& h, uint8_t machine, uint8_t vmode, uint8_t aout, int chunks)
{
    std::vector<uint8_t> d(MED_OPEN_SIZE, 0);
    d[MED_OPEN_MACHINE] = machine;
    d[MED_OPEN_VMODE] = vmode;
    d[MED_OPEN_AOUT] = aout;
    Utils::storeWord(&d[MED_OPEN_LATENCY], 1000);
    d[MED_OPEN_MAX_SECTORS] = 254;
    memcpy(&d[MED_OPEN_PATH], "E:\\TEST.MP4", 11);
    REQUIRE(h.command(MED_CMD_OPEN, {}, d) == MED_OK);

    REQUIRE(medWaitFor([&]() { h.command(MED_CMD_GET_INFO); return h.dt.reply[MED_INFO_STATE] != MED_STATE_STARTING; }, 30000));
    std::vector<uint8_t> info = h.dt.reply;
    INFO("message: " << std::string((const char *) &info[MED_INFO_MSG]));
    REQUIRE(info[MED_INFO_STATE] == MED_STATE_READY);
    MESSAGE("title '" << std::string((const char *) &info[MED_INFO_TITLE]) << "', " << Utils::getDword(&info[MED_INFO_DURATION_MS]) << " ms, vmode "
            << (int) info[MED_INFO_VMODE] << ", afmt " << (int) info[MED_INFO_AFMT] << ", " << (int) info[MED_INFO_FRAMES_PER_CHUNK]
            << " frames/chunk, " << Utils::getWord(&info[MED_INFO_CHUNK_SECTORS]) << " sectors, clock " << Utils::getDword(&info[MED_INFO_CLOCK_RATE]));
    uint16_t chunkSectors = Utils::getWord(&info[MED_INFO_CHUNK_SECTORS]);
    uint16_t slot = Utils::getWord(&info[MED_INFO_SLOT_SECTORS]) * 512;
    if(aout == MED_AOUT_YM) {
        REQUIRE(info[MED_INFO_AFMT] == MED_AFMT_YM);
        REQUIRE(h.command(MED_CMD_GET_YM_TABLE) == MED_OK);
        CHECK(h.dt.reply.size() == MED_YM_TABLE_SIZE);
    }

    uint32_t gen = h.mm->getLink().generation();
    uint32_t nextAudio = 0;
    int frames = 0;
    for(int got = 0; got < chunks; ) {
        REQUIRE(h.getChunk(gen) == MED_OK);
        std::vector<uint8_t>& c = h.dt.reply;
        if(c[MED_CHUNK_FLAGS] & MED_CHF_EMPTY) {
            REQUIRE((c[MED_CHUNK_FLAGS] & MED_CHF_END) == 0);
            Utils::sleepMs(5);
            continue;
        }
        CHECK(Utils::getDword(&c[MED_CHUNK_SEQ]) == (uint32_t) got);
        CHECK(c.size() <= (size_t) chunkSectors * 512);
        uint32_t ab = Utils::getDword(&c[MED_CHUNK_AUDIO_BYTES]);
        if(ab) {
            CHECK(Utils::getDword(&c[MED_CHUNK_AUDIO_START]) == nextAudio);
            nextAudio += ab;
        }
        CHECK(c.size() >= MED_CHUNK_HEADER_SIZE + c[MED_CHUNK_FRAME_COUNT] * slot + ab);
        frames += c[MED_CHUNK_FRAME_COUNT];
        got++;
        if(c[MED_CHUNK_FLAGS] & MED_CHF_END) {
            break;
        }
    }
    CHECK((frames > 0) == (vmode != MED_VMODE_NONE));

    uint32_t seekStart = Utils::getCurrentMs();
    REQUIRE(h.seek(3000) == MED_OK);
    uint32_t gen2 = Utils::getDword(&h.dt.reply[0]);
    REQUIRE(medWaitFor([&]() { h.getChunk(gen2); return (h.dt.reply[MED_CHUNK_FLAGS] & MED_CHF_EMPTY) == 0; }, 30000));
    MESSAGE("seek: first chunk after " << Utils::elapsedMs(seekStart) << " ms");
    CHECK(Utils::getDword(&h.dt.reply[MED_CHUNK_GENERATION]) == gen2);
    CHECK(Utils::getDword(&h.dt.reply[MED_CHUNK_SEQ]) == 0);
    CHECK(Utils::getDword(&h.dt.reply[MED_CHUNK_GEN_START_MS]) == 3000);

    REQUIRE(h.command(MED_CMD_CLOSE) == MED_OK);
}

} // namespace

TEST_CASE("media module: real ce_media" * doctest::skip(getenv("CE_MEDIA_TEST_BIN") == NULL || getenv("CE_MEDIA_TEST_FILE") == NULL))
{
    setenv("MEDIA_BIN", getenv("CE_MEDIA_TEST_BIN"), 1);
    MedHarness h;
    std::string file = getenv("CE_MEDIA_TEST_FILE");
    h.mm->setPathResolver([file](const std::string&, std::string& host) { host = file; return true; });

    REQUIRE(h.command(MED_CMD_IDENTIFY) == MED_OK);
    REQUIRE(medWaitFor([&]() { return h.mm->getLink().isConnected(); }, 15000));

    SUBCASE("STE: ST low + DMA sound") { playSome(h, MED_MACH_STE, MED_VMODE_ST_LOW, MED_AOUT_DMA, 20); }
    SUBCASE("ST: ST low + YM") { playSome(h, MED_MACH_ST, MED_VMODE_ST_LOW, MED_AOUT_YM, 20); }
    SUBCASE("TT: TT mid window") { playSome(h, MED_MACH_TT, MED_VMODE_TT_MID_WIN, MED_AOUT_DMA, 20); }
    SUBCASE("audio only") { playSome(h, MED_MACH_STE, MED_VMODE_NONE, MED_AOUT_DMA, 5); }
}

TEST_CASE("media module - progress from ce_media while it gets ready, OPEN again after a reconnect") {
    MedHarness h;
    {
        FakeMedia fm;
        h.connect(fm);
        REQUIRE(h.open("E:\\A.MP4") == MED_OK);
        fm.expect(MLINK_OPEN);
        fm.sendInfo(MED_STATE_STARTING, 0, "Downloading ffmpeg image 2/7");
        REQUIRE(medWaitFor([&]() {
            h.command(MED_CMD_GET_INFO);
            return std::string((const char *) &h.dt.reply[MED_INFO_MSG]) == "Downloading ffmpeg image 2/7";
        }));
        CHECK(h.dt.reply[MED_INFO_STATE] == MED_STATE_STARTING);
    }                                                               // ce_media went away during the download

    REQUIRE(medWaitFor([&]() { return !h.mm->getLink().isConnected(); }));
    REQUIRE(h.command(MED_CMD_GET_INFO) == MED_OK);
    CHECK(h.dt.reply[MED_INFO_STATE] == MED_STATE_STARTING);        // still not an error - it wasn't playing yet

    FakeMedia fm2;                                                  // the new one gets the OPEN
    REQUIRE(fm2.connectTo(h.mm->getLink().port()));
    fm2.expect(MLINK_HELLO);
    std::vector<uint8_t> op = fm2.expect(MLINK_OPEN);
    CHECK(std::string(op.begin() + 4 + MED_OPEN_PATH, op.end()) == Utils::pathToUi("/media/A.MP4"));
}

TEST_CASE("media module - MEDIA_BIN that doesn't exist is reported to the ST") {
    setenv("MEDIA_BIN", "/nonexistent/ce_media", 1);
    MedHarness h;
    REQUIRE(h.open("E:\\A.MP4") == MED_OK);
    REQUIRE(medWaitFor([&]() { h.command(MED_CMD_GET_INFO); return h.dt.reply[MED_INFO_STATE] == MED_STATE_ERROR; }));
    CHECK(std::string((const char *) &h.dt.reply[MED_INFO_MSG]) == "ce_media not found: /nonexistent/ce_media");
    unsetenv("MEDIA_BIN");
}
