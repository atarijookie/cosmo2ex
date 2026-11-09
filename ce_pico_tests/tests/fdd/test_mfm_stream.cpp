// mfm_stream.cpp - v2 track stream to PWM intervals. The reference is the v1 encoder: the intervals of the same bytes
// must be the same, as v1 is verified to work on the hardware. Also a bad stream must never read out of the track buffer.

#include <cstdint>
#include <vector>
#include <algorithm>

#include "doctest.h"
#include "fakes_fdd.h"

#include "defs.h"
#include "mfm_stream.h"
#include "mfm_perf.h"

namespace {

const uint32_t CLOCK_HZ = 150000000;
const int TICKS_PER_US = 150;

// v1 encoder (appendByteToStream, appendA1MarkToStream in ce_cores/fdd/mfmcachedimage.cpp) - intervals in us
struct V1Encoder {
    uint8_t threeBits = 1;
    std::vector<int> times;

    void byte(uint8_t val) {
        for(int i=0; i<8; i++) {
            threeBits = ((threeBits << 1) | ((val & 0x80) >> 7)) & 7;
            val <<= 1;

            switch(threeBits) {
                case 0: case 3: case 7: times.push_back(4); break;
                case 1: case 4:         times.push_back(6); break;
                case 5:                 times.push_back(8); break;
            }
        }
    }

    void syncA1(void) {
        if(threeBits == 0 || threeBits == 4) {
            times.push_back(6);
        } else if(threeBits == 2 || threeBits == 6) {
            times.push_back(8);
        } else {
            times.push_back(4);
        }

        times.insert(times.end(), {8, 6, 8, 6});
        threeBits = 1;
    }
};

struct Track {
    std::vector<uint8_t> bytes;

    Track() : bytes(MFM_STREAM_HEADER_SIZE, 0) {}

    Track& put(std::initializer_list<uint8_t> vals) { bytes.insert(bytes.end(), vals); return *this; }
    Track& put(uint8_t val) { bytes.push_back(val); return *this; }
    Track& put(uint8_t val, int count) { bytes.insert(bytes.end(), count, val); return *this; }

    // ready to stream: buffer of exactly the stream size, so ASan sees any read beyond it
    std::vector<uint8_t> done(bool endMark = true) {
        std::vector<uint8_t> out = bytes;
        if(endMark) {
            out.push_back(MFM_ESC);
            out.push_back(MFM_OP_END);
        }
        out[0] = out.size() >> 8;
        out[1] = out.size() & 0xff;
        out[2] = MFM_STREAM_VERSION;
        return out;
    }
};

struct StreamFixture {
    MfmStream s;

    StreamFixture() {
        mfmStreamGlobalInit(CLOCK_HZ);
        s = MfmStream();
    }

    void start(const std::vector<uint8_t>& t) {
        mfmStreamStart(&s, t.data(), t.size());
    }

    std::vector<uint16_t> fill(int count) {
        std::vector<uint16_t> out(count);
        mfmStreamFill(&s, out.data(), count);
        return out;
    }

    std::vector<int> fillUs(int count) {
        std::vector<int> us;
        for(uint16_t top : fill(count)) {
            us.push_back((top + 1) / TICKS_PER_US);
        }
        return us;
    }

    // intervals back to cells: every interval is zeros and then a flux. cellTicks: length of 1 cell.
    std::vector<uint8_t> cellsOf(const std::vector<uint16_t>& tops, int cellTicks = 300) {
        std::vector<uint8_t> cells;
        for(uint16_t top : tops) {
            int count = (top + 1) / cellTicks;
            cells.insert(cells.end(), count - 1, 0);
            cells.push_back(1);
        }
        cells.erase(cells.begin());         // the stream starts as if the previous flux was 1 cell before it
        return cells;
    }

    uint16_t cellWord(const std::vector<uint8_t>& cells, int byteIndex) {
        uint16_t word = 0;
        for(int i=0; i<16; i++) {
            word = (word << 1) | cells[byteIndex * 16 + i];
        }
        return word;
    }

    uint8_t dataByte(const std::vector<uint8_t>& cells, int byteIndex) {
        uint8_t val = 0;
        for(int i=0; i<8; i++) {
            val = (val << 1) | cells[byteIndex * 16 + i * 2 + 1];
        }
        return val;
    }
};

}   // namespace

TEST_SUITE("fdd mfm stream v2") {

TEST_CASE_FIXTURE(StreamFixture, "same intervals as the v1 encoder for a whole sector") {
    Track t;
    V1Encoder v1;

    auto plain = [&](uint8_t val, int count) {
        for(int i=0; i<count; i++) {
            t.put(val);
            v1.byte(val);
        }
    };

    plain(0x4e, 60);
    plain(0x00, 12);
    for(int i=0; i<3; i++) {
        t.put({MFM_ESC, MFM_OP_SYNC_A1});
        v1.syncA1();
    }
    for(uint8_t val : {0xFE, 0x00, 0x01, 0x05, 0x02, 0x12, 0x34}) {
        plain(val, 1);
    }
    plain(0x4e, 22);
    plain(0x00, 12);
    for(int i=0; i<3; i++) {
        t.put({MFM_ESC, MFM_OP_SYNC_A1});
        v1.syncA1();
    }

    t.put({MFM_ESC, MFM_OP_RAW, 515 >> 8, 515 & 0xff});
    uint32_t rnd = 0x1234567;
    for(int i=0; i<515; i++) {
        rnd = rnd * 1103515245 + 12345;
        uint8_t val = (i == 0) ? 0xFB : (rnd >> 16);
        t.put(val);
        v1.byte(val);
    }
    plain(0x4e, 40);

    std::vector<uint8_t> track = t.done();
    start(track);
    std::vector<int> v2 = fillUs(v1.times.size());

    // the very start of the stream may differ (v1 starts with made up previous bits), the rest must match exactly
    const int SKIP = 4;
    std::vector<int> expected(v1.times.begin() + SKIP, v1.times.end() - 1);
    auto found = std::search(v2.begin(), v2.begin() + 2 * SKIP + expected.size() / 2, expected.begin(), expected.begin() + expected.size() / 2);
    REQUIRE(found != v2.begin() + 2 * SKIP + expected.size() / 2);

    size_t at = found - v2.begin();
    size_t matching = 0;
    while(matching < expected.size() && at + matching < v2.size() && v2[at + matching] == expected[matching]) {
        matching++;
    }
    CHECK(matching >= expected.size() - 2 * SKIP);     // the end of the v2 list is cut by the possibly different start

    for(int us : v2) {
        CHECK((us == 4 || us == 6 || us == 8));
    }
}

TEST_CASE_FIXTURE(StreamFixture, "sync marks have the missing clock bit") {
    std::vector<uint8_t> track = Track().put({0x00, MFM_ESC, MFM_OP_SYNC_A1, 0x00, MFM_ESC, MFM_OP_SYNC_C2, 0x00, 0x00}).done();
    start(track);
    std::vector<uint8_t> cells = cellsOf(fill(40));

    CHECK(cellWord(cells, 1) == 0x4489);
    CHECK(cellWord(cells, 3) == 0x5224);
    CHECK(dataByte(cells, 1) == 0xA1);
    CHECK(dataByte(cells, 3) == 0xC2);
}

TEST_CASE_FIXTURE(StreamFixture, "clock bit depends on the last data bit of the previous byte") {
    std::vector<uint8_t> track = Track().put({0x01, 0x00, 0x00, 0x7f, 0x80, 0x00}).done();
    start(track);
    std::vector<uint8_t> cells = cellsOf(fill(40));

    CHECK(cellWord(cells, 1) == 0x2AAA);    // after 1: no clock before the first 0
    CHECK(cellWord(cells, 2) == 0xAAAA);    // after 0: clock
    CHECK(cellWord(cells, 4) == 0x4AAA);    // 0x80 after 1: data 1, no clock after it, then clocks
}

TEST_CASE_FIXTURE(StreamFixture, "the refill size doesn't change the intervals") {
    Track t;
    uint32_t rnd = 77;
    for(int i=0; i<500; i++) {
        rnd = rnd * 1103515245 + 12345;
        t.put((uint8_t) (rnd >> 16));
    }
    std::vector<uint8_t> track = t.done();

    start(track);
    std::vector<uint16_t> whole = fill(1000);

    start(track);
    std::vector<uint16_t> pieces;
    for(int size : {1, 2, 3, 5, 7, 11, 13, 960 - 1 - 2 - 3 - 5 - 7 - 11 - 13 + 40}) {
        std::vector<uint16_t> part = fill(size);
        pieces.insert(pieces.end(), part.begin(), part.end());
    }

    REQUIRE(pieces.size() == whole.size());
    CHECK(pieces == whole);
}

TEST_CASE_FIXTURE(StreamFixture, "F5 data byte: escaped outside of a raw run, as it is inside of it") {
    std::vector<uint8_t> track = Track().put({0x11, MFM_ESC, MFM_OP_LITERAL, 0x22, MFM_ESC, MFM_OP_RAW, 0, 3, MFM_ESC, MFM_OP_END, 0x33, 0x44, 0x00}).done();
    start(track);
    std::vector<uint8_t> cells = cellsOf(fill(60));

    const uint8_t expected[] = {0x11, 0xF5, 0x22, 0xF5, 0xFF, 0x33, 0x44};
    for(int i=0; i<7; i++) {
        CHECK(dataByte(cells, i) == expected[i]);
    }
}

TEST_CASE_FIXTURE(StreamFixture, "fuzzy: mask bit 1 keeps the data bit, mask bit 0 differs between the revolutions") {
    const int COUNT = 32;
    Track t;
    t.put({MFM_ESC, MFM_OP_FUZZY, COUNT});
    for(int i=0; i<COUNT; i++) {
        t.put({0xA5, 0xF0});                // high nibble fixed, low nibble random
    }
    t.put(0x4e, 8);
    std::vector<uint8_t> track = t.done();

    std::vector<std::vector<uint8_t>> revolutions;
    for(int rev=0; rev<2; rev++) {
        start(track);                       // the random state goes on over the track restart
        std::vector<uint8_t> cells = cellsOf(fill(250));
        std::vector<uint8_t> bytes;
        for(int i=0; i<COUNT; i++) {
            bytes.push_back(dataByte(cells, i));
            CHECK((bytes.back() & 0xF0) == 0xA0);
        }
        CHECK(dataByte(cells, COUNT) == 0x4e);
        revolutions.push_back(bytes);
    }

    CHECK(revolutions[0] != revolutions[1]);
}

TEST_CASE_FIXTURE(StreamFixture, "fuzzy count 0 means 256") {
    Track t;
    t.put({MFM_ESC, MFM_OP_FUZZY, 0});
    for(int i=0; i<256; i++) {
        t.put({0x12, 0xFF});
    }
    t.put(0x34, 2);
    std::vector<uint8_t> track = t.done();
    start(track);
    std::vector<uint8_t> cells = cellsOf(fill(2000));

    CHECK(dataByte(cells, 255) == 0x12);
    CHECK(dataByte(cells, 256) == 0x34);
}

TEST_CASE_FIXTURE(StreamFixture, "cell time: intervals follow it, and the fractions of a tick add up without drift") {
    const uint16_t CELL = 33507;            // +4.7 %: 314.13 ticks per cell
    Track t;
    t.put({MFM_ESC, MFM_OP_CELL_TIME, CELL >> 8, CELL & 0xff});
    t.put(0x00, 200);                       // 2 cells per flux
    std::vector<uint8_t> track = t.done();
    start(track);

    uint64_t ticks = 0;
    const int COUNT = 1000;
    for(uint16_t top : fill(COUNT)) {
        CHECK((top + 1 == 628 || top + 1 == 629));
        ticks += top + 1;
    }

    double expected = COUNT * 2 * (CELL / 16.0) * (CLOCK_HZ / 1e9);
    CHECK(ticks == doctest::Approx(expected).epsilon(0.0001));
}

TEST_CASE_FIXTURE(StreamFixture, "cell time starts as nominal again on track start") {
    std::vector<uint8_t> track = Track().put({MFM_ESC, MFM_OP_CELL_TIME, 0x90, 0x00}).put(0x00, 50).done();
    start(track);
    fill(20);
    start(track);
    std::vector<uint8_t> plain = Track().put(0x00, 50).done();
    MfmStream other = MfmStream();
    mfmStreamStart(&other, plain.data(), plain.size());
    CHECK(other.cellTicksQ16 == s.cellTicksQ16);
    CHECK((s.cellTicksQ16 >> 16) >= 299);
}

TEST_CASE_FIXTURE(StreamFixture, "PWM ticks follow the clock, the time stays the same") {
    std::vector<uint8_t> track = Track().put(0x00, 100).done();
    mfmStreamGlobalInit(300000000);
    start(track);
    fill(2);
    for(uint16_t top : fill(100)) {
        CHECK(top + 1 >= 1199);             // 4 us at 300 MHz
        CHECK(top + 1 <= 1200);
    }
}

TEST_CASE_FIXTURE(StreamFixture, "sector mark gives the sector being encoded") {
    std::vector<uint8_t> track = Track().put({MFM_ESC, MFM_OP_SECTOR, 1, 42, 7}).put(0x00, 20).done();
    start(track);
    fill(4);
    CHECK(s.side == 1);
    CHECK(s.trackNo == 42);
    CHECK(s.sector == 7);
}

TEST_CASE_FIXTURE(StreamFixture, "after the end of stream: 4 us filler with nominal cell time") {
    std::vector<uint8_t> track = Track().put({MFM_ESC, MFM_OP_CELL_TIME, 0x90, 0x00}).put(0x00, 4).done();
    start(track);
    fill(50);
    CHECK(s.ended);
    for(int us : fillUs(100)) {
        CHECK(us == 4);
    }
}

TEST_CASE_FIXTURE(StreamFixture, "wrong stream version: nothing of it is streamed") {
    std::vector<uint8_t> track = Track().put(0x12, 100).done();
    track[2] = MFM_STREAM_VERSION + 1;
    start(track);
    CHECK(s.ended);
    fill(1);                                // the first one joins to the previous flux, can be longer
    for(int us : fillUs(100)) {
        CHECK(us == 4);
    }
}

TEST_CASE_FIXTURE(StreamFixture, "unknown opcode ends the stream") {
    std::vector<uint8_t> track = Track().put({0x00, MFM_ESC, 0x77}).put(0x12, 100).done();
    start(track);
    fill(100);
    CHECK(s.ended);
    CHECK(s.pos == MFM_STREAM_HEADER_SIZE + 3);
}

TEST_CASE_FIXTURE(StreamFixture, "stream cut anywhere never reads beyond the stream end") {
    std::vector<uint8_t> full = Track()
        .put({0x11, MFM_ESC, MFM_OP_LITERAL, MFM_ESC, MFM_OP_SYNC_A1, MFM_ESC, MFM_OP_CELL_TIME, 0x7d, 0x00})
        .put({MFM_ESC, MFM_OP_SECTOR, 0, 1, 2, MFM_ESC, MFM_OP_RAW, 0, 2, 0x55, 0x66})
        .put({MFM_ESC, MFM_OP_FUZZY, 2, 0x55, 0xF0, 0x66, 0x0F, 0x22}).done(false);

    for(size_t size = MFM_STREAM_HEADER_SIZE; size <= full.size(); size++) {
        std::vector<uint8_t> cut(full.begin(), full.begin() + size);    // exact buffer, ASan catches reads beyond it
        cut[0] = cut.size() >> 8;
        cut[1] = cut.size() & 0xff;

        start(cut);
        fill(100);
        CHECK(s.ended);
        CHECK(s.pos <= cut.size());
    }
}

TEST_CASE_FIXTURE(StreamFixture, "end offset beyond the track buffer is limited to the buffer") {
    std::vector<uint8_t> track = Track().put(0x12, 10).done(false);
    track[0] = 0xff;
    track[1] = 0xff;
    start(track);
    CHECK(s.end == track.size());
    fill(200);
    CHECK(s.ended);
}

TEST_CASE_FIXTURE(StreamFixture, "seek: goes on as if it streamed up to there, for any layout and cell time") {
    Track t;
    uint32_t rnd = 5;
    t.put(0x4e, 100);
    t.put({MFM_ESC, MFM_OP_CELL_TIME, 33507 >> 8, 33507 & 0xff});
    t.put({MFM_ESC, MFM_OP_RAW, 2000 >> 8, 2000 & 0xff});
    for(int i=0; i<2000; i++) {
        rnd = rnd * 1103515245 + 12345;
        t.put((uint8_t) (rnd >> 16));
    }
    t.put({MFM_ESC, MFM_OP_CELL_TIME, MFM_CELL_TIME_NOMINAL >> 8, MFM_CELL_TIME_NOMINAL & 0xff});
    t.put(0x4e, 1000);
    std::vector<uint8_t> track = t.done();

    start(track);
    std::vector<uint16_t> whole = fill(16000);

    // 50 ms: inside the raw run. 100 B at 32 us, and 1450 B at 33.5 us.
    start(track);
    mfmStreamSeek(&s, 50000);
    CHECK(s.rawLeft > 0);
    CHECK(s.rawLeft < 2000);
    uint32_t bytesDone = s.pos - (MFM_STREAM_HEADER_SIZE + 100 + 4 + 4);
    CHECK(bytesDone == doctest::Approx((50000 - 100 * 32) / 33.5).epsilon(0.01));

    std::vector<uint16_t> after = fill(200);
    std::vector<uint16_t> tail(after.begin() + 4, after.begin() + 100);     // the first ones may join to a different previous flux; ticks differ by a fraction

    bool found = false;
    for(size_t i=0; i + tail.size() < whole.size() && !found; i++) {
        bool same = true;
        for(size_t j=0; j<tail.size() && same; j++) {
            same = abs((int) whole[i + j] - (int) tail[j]) <= 1;
        }
        found = same;
    }
    CHECK(found);
}

TEST_CASE_FIXTURE(StreamFixture, "seek beyond the end of the stream ends it, without reading beyond") {
    std::vector<uint8_t> track = Track().put(0x4e, 100).done();
    start(track);
    mfmStreamSeek(&s, 190000);
    CHECK(s.ended);
    CHECK(s.pos <= track.size());
}

TEST_CASE_FIXTURE(StreamFixture, "perf test tracks are valid: fit the track buffer, and play out for a whole track") {
    mfmPerfInit();

    for(int kind=0; kind<MFM_PERF_KINDS; kind++) {
        const uint8_t* track = mfmPerfTrack(kind);
        uint32_t size = (track[0] << 8) | track[1];

        CHECK(size > 5000);
        CHECK(size <= READTRACKDATA_SIZE_BYTES - 2);

        mfmStreamStart(&s, track, READTRACKDATA_SIZE_BYTES);

        uint64_t ticks = 0;
        uint16_t out[1024];
        while(!s.ended) {
            mfmStreamFill(&s, out, 1024);
            for(int i=0; i<1024; i++) {
                ticks += out[i] + 1;
            }
        }

        uint32_t ms = ticks / TICKS_PER_US / 1000;
        CHECK(ms >= 170);
        CHECK(ms <= 200);
    }
}

}   // TEST_SUITE
