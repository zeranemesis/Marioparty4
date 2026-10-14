#include "port/launch_args.hpp"

#include <utility>

namespace partyboard::launch {

namespace {

LaunchArgs sCurrent;

// "--key=value" or "--key value"; returns true and the value when `arg` is `key`.
bool takeValue(std::string_view arg, std::string_view key, const std::vector<std::string_view>& args, size_t& i,
               std::string_view& value) {
    if (arg.substr(0, key.size()) != key)
        return false;
    if (arg.size() > key.size() && arg[key.size()] == '=') {
        value = arg.substr(key.size() + 1);
        return true;
    }
    if (arg.size() == key.size() && i + 1 < args.size()) {
        value = args[++i];
        return true;
    }
    return false;
}

} // namespace

LaunchArgs parse(const std::vector<std::string_view>& args) {
    LaunchArgs out;
    // args[0] is the program (or, on Switch, the NRO path).
    for (size_t i = 1; i < args.size(); ++i) {
        const std::string_view arg = args[i];
        std::string_view value;
        if (takeValue(arg, "--disc-image", args, i, value)) {
            out.discImage = value;
        } else if (takeValue(arg, "--mod-list", args, i, value)) {
            out.modList = value;
        } else if (takeValue(arg, "--launcher", args, i, value)) {
            out.launcherPath = value;
        } else if (takeValue(arg, "--lang", args, i, value)) {
            if (value == "fr")
                out.language = LanguageArg::French;
            else if (value == "en")
                out.language = LanguageArg::English;
            else
                out.ignored.emplace_back(arg);
        } else if (takeValue(arg, "--aspect", args, i, value)) {
            if (value == "4:3")
                out.aspect = AspectArg::Locked43;
            else if (value == "stretch")
                out.aspect = AspectArg::Stretch;
            else if (value == "wide")
                out.aspect = AspectArg::Wide;
            else
                out.ignored.emplace_back(arg);
        }
    }
    return out;
}

const LaunchArgs& current() { return sCurrent; }

void setCurrent(LaunchArgs args) { sCurrent = std::move(args); }

LaunchArgs parse(int argc, const char* const* argv) {
    std::vector<std::string_view> args;
    for (int i = 0; i < argc && argv != nullptr; ++i) {
        if (argv[i] != nullptr)
            args.emplace_back(argv[i]);
    }
    return parse(args);
}

} // namespace partyboard::launch
