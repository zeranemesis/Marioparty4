#pragma once

#include "port/settings.h"

#include <string>
#include <string_view>

namespace partyboard::ui {

inline std::string ui_translate(std::string_view text)
{
    if (partyboard::getSettings().game.language.getValue() != partyboard::GameLanguage::French) {
        return std::string(text);
    }
    struct Entry { std::string_view source; std::string_view translated; };
    static constexpr Entry entries[] = {
        {"Settings", "Paramètres"}, {"Achievements", "Succès"}, {"Quit", "Quitter"},
        {"Play", "Jouer"}, {"Play Online", "Jouer en ligne"}, {"Select Disc Image", "Sélectionner l’image disque"}, {"Prelaunch", "Avant le lancement"},
        {"Online mode", "Mode en ligne"}, {"PartyBoardOnline.exe is missing or could not be started.", "PartyBoardOnline.exe est absent ou ne peut pas être démarré."},
        {"Welcome to Party Board", "Bienvenue dans Party Board"},
        {"Choose a preset to get started. You can change any setting later from the Settings menu.", "Choisissez un préréglage pour commencer. Vous pourrez le modifier plus tard dans les paramètres."},
        {"Classic", "Classique"}, {"Enhancements disabled to match the GameCube version.", "Améliorations désactivées pour correspondre à la version GameCube."},
        {"Quality of life tweaks, our recommended way to play!", "Améliorations pratiques, notre façon recommandée de jouer !"},
        {"Verifying disc image", "Vérification de l’image disque"}, {"Opening disc image...", "Ouverture de l’image disque…"}, {"Cancelling...", "Annulation…"},
        {"No disc image found.", "Aucune image disque trouvée."}, {"Pending restart.", "Redémarrage en attente."}, {"Disc ready.", "Image disque prête."},
        {"Disc hash mismatch.", "Le hash de l’image disque ne correspond pas."}, {"Disc not verified.", "Image disque non vérifiée."}, {"Disc unavailable.", "Image disque indisponible."},
        {"The selected disc image could not be validated.", "L’image disque sélectionnée n’a pas pu être validée."},
        {"Unable to read the selected file.", "Impossible de lire le fichier sélectionné."},
        {"The selected file is not a valid disc image.", "Le fichier sélectionné n’est pas une image disque valide."},
        {"The selected game is not supported by Party Board.", "Le jeu sélectionné n’est pas pris en charge par Party Board."},
        {"Party Board currently supports GameCube USA Rev 0 disc images only.", "Party Board prend actuellement en charge uniquement les images disque GameCube USA Rév. 0."},
        {"Disc verification was canceled. Party Board cannot guarantee the selected disc image is compatible.", "La vérification de l’image disque a été annulée. Party Board ne peut pas garantir sa compatibilité."},
        {"The selected disc image did not pass hash verification. It may be corrupt or modified.", "L’image disque sélectionnée n’a pas passé la vérification du hash. Elle est peut-être corrompue ou modifiée."},
        {"The selected disc image is valid.", "L’image disque sélectionnée est valide."},
        {"Disc verification warning", "Avertissement de vérification de l’image disque"}, {"Continue anyway", "Continuer quand même"},
        {"Disc verification error", "Erreur de vérification de l’image disque"}, {"Apply Options", "Appliquer les options"},
        {"Restart later", "Redémarrer plus tard"}, {"Restart now", "Redémarrer maintenant"},
        {"A restart is required to apply selected options.<br/><br/>Restart now to apply them immediately?", "Un redémarrage est nécessaire pour appliquer les options sélectionnées.<br/><br/>Redémarrer maintenant pour les appliquer ?"},
        {"A restart is required to apply selected options.<br/><br/>Close and reopen Party Board to apply them.", "Un redémarrage est nécessaire pour appliquer les options sélectionnées.<br/><br/>Fermez puis rouvrez Party Board pour les appliquer."},
        {"Display", "Affichage"}, {"Resolution", "Résolution"}, {"Input", "Entrées"}, {"Controller", "Manette"},
        {"Tools", "Outils"}, {"Gameplay", "Jeu"}, {"Speedrunning", "Course rapide"}, {"Cheats", "Triches"},
        {"Minigames", "Mini-jeux"}, {"Boards", "Plateaux"}, {"Interface", "Interface"}, {"Party Board", "Party Board"},
        {"Language", "Langue"}, {"English", "Anglais"}, {"German", "Allemand"}, {"French", "Français"},
        {"Spanish", "Espagnol"}, {"Italian", "Italien"}, {"Disc Image", "Image disque"},
        {"Open Data Folder", "Ouvrir le dossier de données"}, {"Notifications", "Notifications"}, {"Types", "Types"},
        {"Actions", "Actions"}, {"Select All", "Tout sélectionner"}, {"Select None", "Tout désélectionner"},
        {"Challenge", "Défi"}, {"Collection", "Collection"}, {"Minigame", "Mini-jeu"}, {"Misc", "Divers"}, {"Glitched", "Glitchés"},
        {"Clear?", "Effacer ?"}, {"Unlocked", "Déverrouillé"}, {"Locked", "Verrouillé"}, {"unlocked", "déverrouillés"},
        {"Configure Controller", "Configurer la manette"}, {"Allow Background Input", "Autoriser les entrées en arrière-plan"},
        {"Turbo Key", "Touche turbo"}, {"Speedrun Mode", "Mode course rapide"}, {"Unlock All Minigames", "Déverrouiller tous les mini-jeux"},
        {"Unlock Bowser's Gnarly Party", "Déverrouiller la fête infernale de Bowser"}, {"Frame Rate", "Fréquence d’images"},
        {"Lock 4:3 Aspect Ratio", "Verrouiller le format 4:3"}, {"Adaptive Widescreen HUD", "HUD écran large adaptatif"},
        {"Internal Resolution", "Résolution interne"}, {"Shadow Resolution", "Résolution des ombres"},
        {"Pause On Focus Lost", "Pause si la fenêtre perd le focus"}, {"Skip Party Board Main Menu", "Ignorer le menu principal Party Board"},
        {"Skip Game Boot Sequence", "Ignorer le démarrage du jeu"}, {"Off", "Désactivées"}, {"All", "Toutes"}, {"Some", "Certaines"},
        {"Clear All Achievements", "Effacer tous les succès"}, {"Are you sure?", "Confirmer ?"}, {"No controller assigned", "Aucune manette assignée"},
        {"to open menu", "pour ouvrir le menu"}, {"Configure controller port 1 in Settings.", "Configurez la manette du port 1 dans les paramètres."}, {"Quit Party Board", "Quitter Party Board"},
        {"Unsaved progress will be lost.", "La progression non sauvegardée sera perdue."}, {"Cancel", "Annuler"}, {"Confirm", "Confirmer"},
        {"No controllers detected", "Aucune manette détectée"}, {"No controller selected", "Aucune manette sélectionnée"},
        {"Buttons", "Boutons"}, {"D-Pad", "Croix directionnelle"}, {"Analog", "Analogique"}, {"Digital", "Numérique"},
        {"Control Stick", "Stick principal"}, {"C Stick", "Stick C"}, {"Sensor", "Capteur"}, {"Mouse", "Souris"},
        {"Not bound", "Non attribué"}, {"Unknown", "Inconnu"}, {"Return", "Retour"}, {"Reset to default", "Réinitialiser"},
    };
    for (const auto &entry : entries) if (entry.source == text) return std::string(entry.translated);
    return std::string(text);
}

} // namespace partyboard::ui
