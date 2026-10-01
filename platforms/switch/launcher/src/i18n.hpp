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
    StatusNoSwitchRuntime,
    StatusUnsupportedRevision,

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
    OptLanguage,
    OptLanguageHelp,
    ValueOnSingular,
    ValueOffSingular,
    Aspect43,
    AspectStretch,
    AspectWide,
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

    Mods,
    ModsTitle,
    ModsActiveCount,
    ModsNone,
    ModsEmpty,
    ModsCopyHint,
    ModsToggle,
    ModsOrder,
    ModsActive,
    ModsInactive,
    ModsMissing,
    ModsPlayerDisabled,
    ModsHelp,
    LaunchNoRuntimeTitle,
    LaunchNoRuntimeBody,
    RuntimeLabel,

    Count,
};

const char* tr(Language language, Str id);

} // namespace partyboard::launcher
