/*
    Tests of floppy action queue - fddActionPush() / fddActionPop() from misc/global.cpp
*/
#include <pthread.h>
#include <string>

#include "doctest.h"

#include "misc/global.h"

static void drainFddActions(void)
{
    int action;
    std::string mac;
    while(fddActionPop(action, mac)) {
    }
}

TEST_CASE("fdd actions are popped in order, each with its own mac")
{
    drainFddActions();

    int action = FDD_ACTION_NONE;
    std::string mac;
    CHECK_FALSE(fddActionPop(action, mac));     // empty queue

    fddActionPush(FDD_ACTION_INSERT, "001122334455");
    fddActionPush(FDD_ACTION_EJECT,  "AABBCCDDEEFF");   // second action before the first is handled - must not overwrite it

    REQUIRE(fddActionPop(action, mac));
    CHECK(action == FDD_ACTION_INSERT);
    CHECK(mac == "001122334455");

    REQUIRE(fddActionPop(action, mac));
    CHECK(action == FDD_ACTION_EJECT);
    CHECK(mac == "AABBCCDDEEFF");

    CHECK_FALSE(fddActionPop(action, mac));
}

TEST_CASE("fdd action queue drops the oldest actions when full")
{
    drainFddActions();

    for(int i=0; i<100; i++) {
        fddActionPush(FDD_ACTION_INSERT, std::to_string(i));
    }

    int action, count = 0;
    std::string mac, firstMac, lastMac;
    while(fddActionPop(action, mac)) {
        if(count == 0) {
            firstMac = mac;
        }
        lastMac = mac;
        count++;
    }

    CHECK(count == 32);             // FDD_ACTIONS_MAX
    CHECK(firstMac == "68");        // 100 - 32 = oldest kept
    CHECK(lastMac == "99");         // newest is always kept
}

static void* pushManyFddActions(void* arg)
{
    for(int i=0; i<1000; i++) {
        fddActionPush(FDD_ACTION_EJECT, "AABBCCDDEEFF");
    }
    return NULL;
}

TEST_CASE("fdd actions pushed from other thread arrive with matching mac")
{
    drainFddActions();

    pthread_t th;
    REQUIRE(pthread_create(&th, NULL, pushManyFddActions, NULL) == 0);

    // pop while the other thread pushes - every popped item must be complete
    int action, popped = 0;
    std::string mac;
    bool allGood = true;
    for(int i=0; i<100000 && popped < 1000; i++) {
        if(fddActionPop(action, mac)) {
            popped++;
            allGood = allGood && (action == FDD_ACTION_EJECT) && (mac == "AABBCCDDEEFF");
        }
    }

    pthread_join(th, NULL);
    while(fddActionPop(action, mac)) {          // whatever was not popped in the loop above
        allGood = allGood && (action == FDD_ACTION_EJECT) && (mac == "AABBCCDDEEFF");
    }

    CHECK(allGood);
}
