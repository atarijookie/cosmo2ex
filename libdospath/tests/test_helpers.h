#ifndef __LDP_TEST_HELPERS_H__
#define __LDP_TEST_HELPERS_H__

#include <string>
#include <stdlib.h>

#include "doctest.h"

#include "libdospath.h"

// all test files are created under this dir, which is wiped before and after each test case - in /tmp, or in
// $CE_TEST_TMPDIR, to run the tests on another filesystem (e.g. a case-insensitive one, like APFS on macOS)
inline std::string ldpTestRoot(void)
{
    const char* dir = getenv("CE_TEST_TMPDIR");
    return std::string((dir && dir[0]) ? dir : "/tmp") + "/ldptest";
}

// run shell command, fail the test case if it doesn't succeed
inline void sh(const std::string& cmd)
{
    INFO("shell: ", cmd);
    REQUIRE(system(cmd.c_str()) == 0);
}

/*
    Fixture used by every test case - gives each test case a clean scratch dir
    and a fresh library state (the global DirTranslator is dropped).
*/
struct ScratchDir
{
    std::string root;

    ScratchDir() : root(ldpTestRoot()) {
        ldp_cleanup();
        sh("rm -rf " + root + " && mkdir -p " + root);
    }

    ~ScratchDir() {
        ldp_cleanup();
        system(("rm -rf " + root).c_str());
    }

    // full host path of item inside the scratch dir
    std::string p(const std::string& relPath) const {
        return root + "/" + relPath;
    }
};

#endif
