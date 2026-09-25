#include "port/port_version.h"
#include "aurora/lib/logging.hpp"
#include "ui/localization.hpp"

#include <SDL3/SDL_messagebox.h>
#include <dvd.h>
#include <fmt/format.h>

#include <string>

namespace partyboard::version {

aurora::Module PartyBoardVersionLog("partyboard::version");

static bool versionInitialized;
static GameVersion gameVersion;
static DVDDiskID diskId;
static DiscIdentity discIdentity;

// Tells the player why the disc cannot be played, then stops: a message box is the only thing
// they see when the game was started without the launcher UI.
[[noreturn]] static void refuseDisc(const std::string& logMessage, std::string_view playerMessage) {
    const auto text = ui::ui_translate(playerMessage);
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Party Board", text.c_str(), nullptr);
    PartyBoardVersionLog.fatal("{}", logMessage);
}

void init() {
    versionInitialized = true;

    if (!DVDLowReadDiskID(&diskId, nullptr)) {
        PartyBoardVersionLog.fatal("DVDLowReadDiskID failed to return instantly.");
    }

    std::string_view company(diskId.company, sizeof(diskId.company));
    std::string_view game(diskId.gameName, sizeof(diskId.gameName));
    const std::string gameId = fmt::format("{}{}", game, company);

    const auto identity = identify_disc(gameId, diskId.gameVersion);
    if (!identity) {
        refuseDisc(fmt::format("Unknown game in disc: {} (revision {})", gameId, diskId.gameVersion),
            "The selected game is not supported by Party Board. Choose a USA or European Mario Party 4 GameCube "
            "disc image.");
    }
    discIdentity = *identity;

    // The disc validator refuses Japanese discs; this only happens when validation was skipped
    // (launcher or online session path), so explain it instead of crashing later in the game.
    if (!discIdentity.supported) {
        refuseDisc(fmt::format("Unsupported game version in disc: {} ({})", gameId, describe()),
            discIdentity.region == DiscRegion::Japan
                ? "The Japanese version of Mario Party 4 is not supported yet. Please use the USA or European "
                  "version."
                : "This version of Mario Party 4 is not supported yet. Please use the USA or European version.");
    }

    if (!discIdentity.exactRevision) {
        PartyBoardVersionLog.warn("Unknown revision {} of {}, running it as the newest known revision",
            discIdentity.revision, gameId);
    }
    gameVersion = discIdentity.version;

    PartyBoardVersionLog.info("Loaded game disc is {} ({})", gameId, describe());
}

std::string describe() {
    return partyboard::version::describe(discIdentity.region, discIdentity.revision);
}

bool isRegionJpn() {
    return getGameVersion() == GameVersion::Jpn;
}

bool isRegionPal() {
    return getGameVersion() == GameVersion::PalRev0 || getGameVersion() == GameVersion::PalRev1 || getGameVersion() == GameVersion::PalRev2;
}

bool isRegionUsa() {
    return getGameVersion() == GameVersion::UsaRev0 || getGameVersion() == GameVersion::UsaRev1;
}

GameVersion getGameVersion() {
    if (!versionInitialized) {
        abort();
    }

    return gameVersion;
}

const DVDDiskID& getDiskID() {
    return diskId;
}

}  // namespace partyboard::version

extern "C" u8 partyboard_version_get_version(void)
{
    return static_cast<u8>(partyboard::version::getGameVersion());
}

extern "C" BOOL partyboard_version_is_pal(void)
{
    return partyboard::version::isRegionPal();
}

extern "C" BOOL partyboard_version_is_usa(void)
{
    return partyboard::version::isRegionUsa();
}

extern "C" BOOL partyboard_version_is_ntsc(void)
{
    return partyboard::version::isRegionUsa() || partyboard::version::isRegionJpn();
}