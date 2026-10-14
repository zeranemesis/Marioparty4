#include "catalog.hpp"

#include <cstdio>

namespace partyboard::launcher {

namespace {

Localized localized(const json::Value& value) {
    Localized out;
    if (value.isString()) {
        out.fr = out.en = value.asString();
    } else if (value.isObject()) {
        out.fr = value["fr"].asString();
        out.en = value["en"].asString();
    }
    return out;
}

std::vector<std::string> strings(const json::Value& value) {
    std::vector<std::string> out;
    for (const json::Value& item : value.items()) {
        if (item.isString() && !item.asString().empty())
            out.push_back(item.asString());
    }
    return out;
}

bool parseEntry(const json::Value& object, CatalogEntry& out) {
    out.id = object["id"].asString();
    out.title = object["title"].asString();
    if (out.id.empty() || out.title.empty())
        return false;
    for (const auto& [region, title] : object["regionTitles"].members()) {
        if (region.size() == 1 && title.isString())
            out.regionTitles.emplace_back(region[0], title.asString());
    }
    out.year = static_cast<int>(object["year"].asInteger(0));
    out.genre = localized(object["genre"]);
    out.players = localized(object["players"]);
    out.description = localized(object["description"]);
    out.runtime = object["runtime"].asString();
    out.discs = strings(object["discs"]);
    out.recognised = strings(object["recognised"]);
    out.engines = strings(object["engines"]);
    out.mods = object["mods"].asBool(false);
    for (const auto& [region, cover] : object["covers"].members()) {
        CoverArt art;
        art.front = cover["front"].asString();
        art.spine = cover["spine"].asString();
        if (!art.front.empty())
            out.covers.emplace_back(region, art);
    }
    return !out.discs.empty() || !out.recognised.empty();
}

// How one catalogue disc id matches a disc: exact revision, any revision
// (six characters), or only the game id.
DiscMatch matchId(const std::string& id, const DiscInfo& disc, DiscMatch onMatch) {
    if (id.size() == 6)
        return id == disc.gameId ? onMatch : DiscMatch::None;
    if (id.size() > 7 && id.compare(0, 6, disc.gameId) == 0 && id[6] == '_')
        return id == revisionId(disc) ? onMatch : DiscMatch::OtherRevision;
    return DiscMatch::None;
}

} // namespace

std::string revisionId(const DiscInfo& disc) {
    char buffer[16];
    std::snprintf(buffer, sizeof(buffer), "%s_%02u", disc.gameId.c_str(), static_cast<unsigned>(disc.revision));
    return buffer;
}

std::string CatalogEntry::titleFor(char region) const {
    for (const auto& [code, title] : regionTitles) {
        if (code == region)
            return title;
    }
    return title;
}

const CoverArt* CatalogEntry::coverFor(char region) const {
    // PAL discs carry country letters (D, F, S, I, U...): they use the P cover.
    const char pal = region == 'E' || region == 'J' || region == 'K' ? region : 'P';
    for (const char wanted : {region, pal}) {
        for (const auto& [code, art] : covers) {
            if (code.size() == 1 && code[0] == wanted)
                return &art;
        }
    }
    for (const auto& [code, art] : covers) {
        if (code == "*")
            return &art;
    }
    return covers.empty() ? nullptr : &covers.front().second;
}

bool Catalog::load(const json::Value& document) {
    const json::Value& games = document.isArray() ? document : document["games"];
    if (!games.isArray())
        return false;
    for (const json::Value& object : games.items()) {
        CatalogEntry entry;
        if (!object.isObject() || !parseEntry(object, entry))
            continue;
        bool replaced = false;
        for (CatalogEntry& existing : m_games) {
            if (existing.id == entry.id) {
                existing = entry;
                replaced = true;
            }
        }
        if (!replaced)
            m_games.push_back(std::move(entry));
    }
    return true;
}

bool Catalog::loadFile(const std::string& path, std::string* error) {
    json::Value document;
    if (!json::parseFile(path, document, error))
        return false;
    return load(document);
}

int Catalog::find(const DiscInfo& disc, DiscMatch& match) const {
    match = DiscMatch::None;
    int found = -1;
    auto better = [](DiscMatch a, DiscMatch b) {
        auto rank = [](DiscMatch m) {
            switch (m) {
            case DiscMatch::Accepted: return 3;
            case DiscMatch::OtherRevision: return 2;
            case DiscMatch::Recognised: return 1;
            case DiscMatch::None: break;
            }
            return 0;
        };
        return rank(a) > rank(b);
    };
    for (size_t i = 0; i < m_games.size(); ++i) {
        for (const std::string& id : m_games[i].discs) {
            const DiscMatch m = matchId(id, disc, DiscMatch::Accepted);
            if (better(m, match)) {
                match = m;
                found = static_cast<int>(i);
            }
        }
        for (const std::string& id : m_games[i].recognised) {
            DiscMatch m = matchId(id, disc, DiscMatch::Recognised);
            if (m == DiscMatch::OtherRevision)
                m = DiscMatch::Recognised;
            if (better(m, match)) {
                match = m;
                found = static_cast<int>(i);
            }
        }
    }
    return found;
}

Catalog Catalog::builtin() {
    Catalog catalog;
    CatalogEntry mp4;
    mp4.id = "GMPE01_00";
    mp4.title = "Mario Party 4";
    mp4.year = 2002;
    mp4.genre = {"Party", "Party"};
    mp4.players = {"1 à 4 joueurs", "1-4 players"};
    mp4.runtime = "PartyBoard";
    // src/port/iso_validate.cpp: NTSC-U and PAL boot, NTSC-J is recognised.
    mp4.discs = {"GMPE01", "GMPP01"};
    mp4.recognised = {"GMPJ01"};
    mp4.engines = {"switch/partyboard/partyboard.nro", "switch/partyboard.nro"};
    mp4.mods = true;
    catalog.m_games.push_back(std::move(mp4));
    return catalog;
}

} // namespace partyboard::launcher
