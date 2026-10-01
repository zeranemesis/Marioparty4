#pragma once

// French/English strings for the launcher. The engine has its own
// localisation; this table only covers what the shelf shows before it boots.

namespace partyboard::launcher {

enum class Language : unsigned char {
    French,
    English,
};

enum class Str : unsigned short {
    AppTitle,
    AppSubtitle,

    Play,
    Options,
    Controllers,
    Quit,
    Back,
    Change,
    Refresh,
    Ok,

    NoGamesTitle,
    NoGamesBody,
    NoGamesFormats,

    StatusSupported,
    StatusUnsupportedRegion,
    StatusOtherGame,
    StatusUnreadable,

    RegionUsa,
    RegionEurope,
    RegionJapan,
    RegionOther,
    Revision,
    Players,
    BannerMissing,

    OptionsTitle,
    OptBootAnimation,
    OptBootAnimationHelp,
    OptAspect,
    OptAspectHelp,
    OptFilter,
    OptFilterHelp,
    OptLanguage,
    OptLanguageHelp,
    OptRumble,
    OptRumbleHelp,
    ValueOn,
    ValueOff,
    ValueOnSingular,
    ValueOffSingular,
    Aspect43,
    AspectStretch,
    AspectWide,
    FilterNone,
    FilterSmooth,
    FilterScanlines,
    LanguageAuto,
    LanguageFrench,
    LanguageEnglish,

    ControllersTitle,
    ControllersSubtitle,
    ControllersPlayer,
    ControllersConnected,
    ControllersEmpty,
    ControllersHandheld,
    ControllersChangeGrip,
    ControllersAppletFailed,
    MapStick,
    MapCStick,
    MapDpad,
    MapStart,
    MapBottomButton,
    MapRightButton,
    MapLeftButton,
    MapTopButton,
    MapOrZL,
    MapLeftStick,
    MapRightStick,
    MapDirectional,

    LaunchEngineMissingTitle,
    LaunchEngineMissingBody,
    LaunchUnsupportedTitle,
    LaunchUnsupportedBody,
    LaunchFailedTitle,
    LaunchFailedBody,

    GamesFound,
    EngineLabel,
    EngineInstalled,
    EngineMissing,
    BootPresents,

    Count,
};

const char* tr(Language language, Str id);

} // namespace partyboard::launcher
