// RiftTests entry point (doctest). Options: RiftTests.exe --help
#define DOCTEST_CONFIG_IMPLEMENT
#include "doctest.h"
#include "core/Log.h"
#include "core/Paths.h"

int main(int argc, char** argv) {
    Paths::setUserDataFolderName("RIFT-tests");
    Log::init("tests");
    doctest::Context context(argc, argv);
    const int result = context.run();
    Log::shutdown();
    return result;
}
