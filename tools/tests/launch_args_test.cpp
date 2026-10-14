// Launcher arguments (the Switch launcher's argv contract).
// Standalone:
//   c++ -std=c++20 -Iinclude tools/tests/launch_args_test.cpp src/port/launch_args.cpp
#include "port/launch_args.hpp"

#include <cstdio>
#include <cstdlib>

using namespace partyboard::launch;

namespace {

int failures = 0;

void check(bool condition, const char *what)
{
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}

} // namespace

int main()
{
    {
        // What platforms/switch/launcher sends, path with spaces included.
        const char *argv[] = {
            "sdmc:/switch/partyboard/partyboard.nro",
            "--disc-image=sdmc:/partyboard/games/Mario Party 4 (USA).iso",
            "--aspect=wide",
            "--lang=fr",
            "--mod-list=sdmc:/cubeshelf/Mods/GMPE01_00/active-mods.txt",
            "--launcher=sdmc:/switch/partyboard-launcher.nro",
        };
        const LaunchArgs args = parse(6, argv);
        check(args.discImage == "sdmc:/partyboard/games/Mario Party 4 (USA).iso", "disc image");
        check(args.aspect == AspectArg::Wide, "aspect");
        check(args.language == LanguageArg::French, "language");
        check(args.modList == "sdmc:/cubeshelf/Mods/GMPE01_00/active-mods.txt", "mod list");
        check(args.launcherPath == "sdmc:/switch/partyboard-launcher.nro", "launcher");
        check(args.ignored.empty(), "nothing ignored");
    }
    {
        // Space-separated values, and arguments that belong to someone else.
        const char *argv[] = {"partyboard", "--netplay-rollback", "--aspect", "4:3", "--lang", "en",
                              "--disc-image", "/games/mp4.rvz", "-v"};
        const LaunchArgs args = parse(9, argv);
        check(args.aspect == AspectArg::Locked43, "aspect as separate value");
        check(args.language == LanguageArg::English, "language as separate value");
        check(args.discImage == "/games/mp4.rvz", "disc as separate value");
        check(args.ignored.empty(), "foreign arguments are left alone");
    }
    {
        // Unknown values are reported, prefixes do not match longer options,
        // and a key without a value is not an option.
        const char *argv[] = {"partyboard", "--lang=de", "--aspect=16:10", "--language=fr", "--mod-list"};
        const LaunchArgs args = parse(5, argv);
        check(!args.language, "unknown language rejected");
        check(!args.aspect, "unknown aspect rejected");
        check(args.ignored.size() == 2, "both bad values reported");
        check(args.modList.empty(), "dangling key ignored");
    }
    {
        const LaunchArgs none = parse(0, nullptr);
        check(none.discImage.empty() && !none.aspect && !none.language, "no arguments");
        setCurrent(parse(std::vector<std::string_view>{"x", "--mod-list=/m.txt"}));
        check(current().modList == "/m.txt", "current() keeps the parsed arguments");
    }

    if (failures == 0) {
        std::printf("launch_args_test: all checks passed\n");
        return EXIT_SUCCESS;
    }
    return EXIT_FAILURE;
}
