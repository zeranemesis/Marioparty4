#include "i18n.hpp"

#include <array>
#include <cstddef>

namespace partyboard::launcher {

namespace {

struct Entry {
    Str id;
    const char* fr;
    const char* en;
};

// Kept in enum order; the static_assert below and the index check in tr()
// catch a missing or misplaced line.
constexpr Entry kStrings[] = {
    {Str::AppTitle, "GameCube", "GameCube"},
    {Str::AppSubtitle, "PartyBoard Classics", "PartyBoard Classics"},

    {Str::Play, "Jouer", "Play"},
    {Str::Options, "Options", "Options"},
    {Str::Controllers, "Manettes", "Controllers"},
    {Str::Quit, "Quitter", "Quit"},
    {Str::Back, "Retour", "Back"},
    {Str::Change, "Modifier", "Change"},
    {Str::Refresh, "Actualiser", "Refresh"},
    {Str::Ok, "OK", "OK"},

    {Str::NoGamesTitle, "Aucun jeu trouvé", "No games found"},
    {Str::NoGamesBody, "Copiez votre image disque de Mario Party 4 dans :",
     "Copy your Mario Party 4 disc image to:"},
    {Str::NoGamesFormats, "Formats acceptés : .iso .gcm .ciso .gcz .rvz .wia",
     "Accepted formats: .iso .gcm .ciso .gcz .rvz .wia"},

    {Str::StatusSupported, "Compatible PartyBoard", "Runs on PartyBoard"},
    {Str::StatusUnsupportedRegion, "Version japonaise : pas encore prise en charge",
     "Japanese release: not supported yet"},
    {Str::StatusOtherGame, "Ce disque n'est pas Mario Party 4", "This disc is not Mario Party 4"},
    {Str::StatusUnreadable, "Image disque illisible", "Unreadable disc image"},
    {Str::StatusNoSwitchRuntime, "Pas encore disponible sur Switch", "Not available on Switch yet"},
    {Str::StatusUnsupportedRevision, "Révision du disque non prise en charge", "Disc revision not supported"},

    {Str::RegionUsa, "NTSC-U", "NTSC-U"},
    {Str::RegionEurope, "PAL", "PAL"},
    {Str::RegionJapan, "NTSC-J", "NTSC-J"},
    {Str::RegionOther, "Région inconnue", "Unknown region"},
    {Str::Revision, "Rév. %u", "Rev. %u"},
    {Str::Players, "1 à 4 joueurs", "1-4 players"},
    {Str::BannerMissing, "Bannière indisponible pour ce format", "Banner unavailable for this format"},

    {Str::OptionsTitle, "Options", "Options"},
    {Str::OptBootAnimation, "Animation de démarrage", "Startup animation"},
    {Str::OptBootAnimationHelp,
     "Joue l'animation du cube avant de lancer le jeu. Maintenez ZR pendant l'animation pour une surprise.",
     "Plays the cube animation before the game starts. Hold ZR during it for a surprise."},
    {Str::OptAspect, "Format d'image", "Aspect ratio"},
    {Str::OptAspectHelp, "4:3 respecte l'image d'origine. 16:9 écran large élargit le champ de vision.",
     "4:3 keeps the original picture. 16:9 widescreen extends the field of view."},
    {Str::OptLanguage, "Langue", "Language"},
    {Str::OptLanguageHelp, "Automatique suit la langue de la console.",
     "Automatic follows the console language."},
    {Str::ValueOnSingular, "Activée", "On"},
    {Str::ValueOffSingular, "Désactivée", "Off"},
    {Str::Aspect43, "4:3 (original)", "4:3 (original)"},
    {Str::AspectStretch, "16:9 (étiré)", "16:9 (stretched)"},
    {Str::AspectWide, "16:9 (écran large)", "16:9 (widescreen)"},
    {Str::LanguageAuto, "Automatique", "Automatic"},
    {Str::LanguageFrench, "Français", "Français"},
    {Str::LanguageEnglish, "English", "English"},

    {Str::ControllersTitle, "Manettes", "Controllers"},
    {Str::ControllersSubtitle, "Disposition GameCube sur les manettes Switch",
     "GameCube layout on Switch controllers"},
    {Str::ControllersPlayer, "J%u", "P%u"},
    {Str::ControllersConnected, "Connectée", "Connected"},
    {Str::ControllersEmpty, "Non connectée", "Not connected"},
    {Str::ControllersHandheld, "Mode portable", "Handheld mode"},
    {Str::ControllersChangeGrip, "Changer la prise/l'ordre", "Change grip/order"},
    {Str::ControllersAppletFailed, "L'applet des manettes n'est pas disponible ici.",
     "The controller applet is not available here."},
    {Str::MapStick, "Stick", "Stick"},
    {Str::MapCStick, "Stick C", "C stick"},
    {Str::MapDpad, "Croix", "D-pad"},
    {Str::MapStart, "START", "START"},
    {Str::MapBottomButton, "bouton du bas", "bottom button"},
    {Str::MapRightButton, "bouton de droite", "right button"},
    {Str::MapLeftButton, "bouton de gauche", "left button"},
    {Str::MapTopButton, "bouton du haut", "top button"},
    {Str::MapOrZL, "ou ZL", "or ZL"},
    {Str::MapLeftStick, "stick gauche", "left stick"},
    {Str::MapRightStick, "stick droit", "right stick"},
    {Str::MapDirectional, "boutons directionnels", "directional buttons"},

    {Str::LaunchEngineMissingTitle, "Moteur PartyBoard introuvable", "PartyBoard engine not found"},
    {Str::LaunchEngineMissingBody, "Installez partyboard.nro ici, puis réessayez :",
     "Install partyboard.nro here, then try again:"},
    {Str::LaunchUnsupportedTitle, "Lancement impossible", "Cannot start this game"},
    {Str::LaunchUnsupportedBody, "PartyBoard ne fait tourner que Mario Party 4 (NTSC-U ou PAL).",
     "PartyBoard only runs Mario Party 4 (NTSC-U or PAL)."},
    {Str::LaunchFailedTitle, "Le lancement a échoué", "Launch failed"},
    {Str::LaunchFailedBody, "Le chargeur homebrew a refusé de lancer le moteur.",
     "The homebrew loader refused to start the engine."},

    {Str::GamesFound, "Jeux trouvés : %u", "Games found: %u"},
    {Str::EngineLabel, "Moteur PartyBoard", "PartyBoard engine"},
    {Str::EngineInstalled, "installé", "installed"},
    {Str::EngineMissing, "introuvable", "not found"},
    {Str::BootPresents, "Mario Party R&D présente", "Mario Party R&D presents"},

    {Str::Mods, "Mods", "Mods"},
    {Str::ModsTitle, "Mods", "Mods"},
    {Str::ModsActiveCount, "Mods actifs : %u", "Active mods: %u"},
    {Str::ModsNone, "Aucun mod", "No mods"},
    {Str::ModsEmpty, "Aucun mod installé pour ce jeu.", "No mods installed for this game."},
    {Str::ModsCopyHint, "Copie le dossier Mods de CubeShelf (%LOCALAPPDATA%\\CubeShelf\\Mods) dans :",
     "Copy CubeShelf's Mods folder (%LOCALAPPDATA%\\CubeShelf\\Mods) to:"},
    {Str::ModsToggle, "Activer", "Toggle"},
    {Str::ModsOrder, "Ordre", "Order"},
    {Str::ModsActive, "Actif", "On"},
    {Str::ModsInactive, "Inactif", "Off"},
    {Str::ModsMissing, "Dossier introuvable sur la carte SD", "Folder missing from the SD card"},
    {Str::ModsPlayerDisabled, "Coupé depuis le jeu", "Switched off in game"},
    {Str::ModsHelp, "En cas de conflit, le mod le plus haut dans la liste l'emporte. Les choix sont enregistrés dans installed.json, comme dans CubeShelf.",
     "When two mods change the same file, the one higher in the list wins. Choices are saved to installed.json, as in CubeShelf."},
    {Str::LaunchNoRuntimeTitle, "Pas encore sur Switch", "Not on Switch yet"},
    {Str::LaunchNoRuntimeBody, "Ce jeu tourne sur PC avec CubeShelf, mais son runtime n'a pas encore de version Switch.",
     "This game runs on PC through CubeShelf, but its runtime has no Switch build yet."},
    {Str::RuntimeLabel, "Runtime", "Runtime"},
};

static_assert(std::size(kStrings) == static_cast<size_t>(Str::Count), "every Str needs a translation");

} // namespace

const char* tr(Language language, Str id) {
    const size_t index = static_cast<size_t>(id);
    if (index >= std::size(kStrings) || kStrings[index].id != id)
        return "?";
    return language == Language::French ? kStrings[index].fr : kStrings[index].en;
}

} // namespace partyboard::launcher
