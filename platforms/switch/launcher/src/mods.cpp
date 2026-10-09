#include "mods.hpp"

#include <algorithm>
#include <cstdio>
#include <set>

#include <sys/stat.h>

#include "json.hpp"

namespace partyboard::launcher {

namespace {

bool isDirectory(const std::string& path) {
    struct stat st{};
    return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

bool writeText(const std::string& path, const std::string& text) {
    // Through a temporary file, like CubeShelf: the engine may be reading.
    const std::string temporary = path + ".tmp";
    FILE* file = std::fopen(temporary.c_str(), "wb");
    if (!file)
        return false;
    const bool written = std::fwrite(text.data(), 1, text.size(), file) == text.size();
    if (std::fclose(file) != 0 || !written) {
        std::remove(temporary.c_str());
        return false;
    }
    std::remove(path.c_str());
    return std::rename(temporary.c_str(), path.c_str()) == 0;
}

// CubeShelf stores absolute PC paths ("C:\...\Mods\GMPE01_00\546878\files").
// Keep what follows the mod's own folder and hang it under this directory.
std::string resolveRoot(const std::string& directory, const InstalledMod& mod) {
    const std::string idText = std::to_string(mod.id);
    std::vector<std::string> segments;
    std::string current;
    for (const char c : mod.contentRoot) {
        if (c == '/' || c == '\\') {
            if (!current.empty())
                segments.push_back(current);
            current.clear();
        } else {
            current.push_back(c);
        }
    }
    if (!current.empty())
        segments.push_back(current);

    std::string resolved = directory + "/" + idText;
    for (size_t i = segments.size(); i-- > 0;) {
        if (segments[i] == idText) {
            for (size_t k = i + 1; k < segments.size(); ++k)
                resolved += "/" + segments[k];
            return resolved;
        }
    }
    // No recognisable path: the usual CubeShelf layout.
    return isDirectory(resolved + "/files") ? resolved + "/files" : resolved;
}

} // namespace

std::string ModSet::findDirectory(const std::string& modsRoot, const std::vector<std::string>& candidates) {
    for (const std::string& name : candidates) {
        if (name.empty() || name.find_first_of("/\\.") != std::string::npos)
            continue;
        const std::string path = modsRoot + "/" + name;
        if (isDirectory(path))
            return path;
    }
    return {};
}

bool ModSet::load(const std::string& directory) {
    m_directory = directory;
    m_mods.clear();
    json::Value installed;
    if (!json::parseFile(directory + "/installed.json", installed) || !installed.isArray())
        return false;

    std::set<long long> playerDisabled;
    json::Value disabled;
    if (json::parseFile(directory + "/player-disabled.json", disabled)) {
        for (const json::Value& id : disabled.items())
            playerDisabled.insert(id.asInteger(-1));
    }

    for (const json::Value& item : installed.items()) {
        InstalledMod mod;
        mod.id = static_cast<int>(item["Id"].asInteger(0));
        if (mod.id <= 0)
            continue;
        mod.name = item["Name"].asString();
        mod.updated = item["Updated"].asInteger(0);
        mod.enabled = item["Enabled"].asBool(true);
        mod.priority = static_cast<int>(item["Priority"].asInteger(0));
        mod.contentRoot = item["ContentRoot"].asString();
        mod.sha256 = item["Sha256"].asString();
        mod.resolvedRoot = resolveRoot(directory, mod);
        mod.present = isDirectory(mod.resolvedRoot);
        mod.playerDisabled = playerDisabled.count(mod.id) != 0;
        if (mod.name.empty())
            mod.name = "Mod " + std::to_string(mod.id);
        m_mods.push_back(std::move(mod));
    }
    sort();
    return true;
}

void ModSet::sort() {
    std::stable_sort(m_mods.begin(), m_mods.end(), [](const InstalledMod& a, const InstalledMod& b) {
        return a.priority != b.priority ? a.priority > b.priority : a.id < b.id;
    });
}

bool ModSet::save() const {
    if (m_directory.empty())
        return false;
    // installed.json in CubeShelf's field order and casing, paths untouched.
    json::Value installed = json::Value::array();
    std::vector<const InstalledMod*> byId;
    for (const InstalledMod& mod : m_mods)
        byId.push_back(&mod);
    std::sort(byId.begin(), byId.end(), [](const InstalledMod* a, const InstalledMod* b) { return a->id < b->id; });
    for (const InstalledMod* mod : byId) {
        json::Value item = json::Value::object();
        item.set("Id", json::Value::number(mod->id));
        item.set("Name", json::Value::string(mod->name));
        item.set("Updated", json::Value::number(static_cast<double>(mod->updated)));
        item.set("Enabled", json::Value::boolean(mod->enabled));
        item.set("Priority", json::Value::number(mod->priority));
        item.set("ContentRoot", json::Value::string(mod->contentRoot));
        item.set("Sha256", json::Value::string(mod->sha256));
        installed.push(std::move(item));
    }
    json::Value disabled = json::Value::array();
    for (const InstalledMod* mod : byId) {
        if (mod->playerDisabled)
            disabled.push(json::Value::number(mod->id));
    }
    return writeText(m_directory + "/installed.json", json::serialize(installed)) &&
           writeText(m_directory + "/player-disabled.json", json::serialize(disabled));
}

std::string ModSet::writeActiveList() const {
    if (m_directory.empty())
        return {};
    std::string lines;
    json::Value details = json::Value::array();
    for (const InstalledMod& mod : m_mods) {
        if (!mod.active())
            continue;
        lines += mod.resolvedRoot + "\n";
        json::Value item = json::Value::object();
        item.set("Id", json::Value::number(mod.id));
        item.set("Name", json::Value::string(mod.name));
        item.set("Priority", json::Value::number(mod.priority));
        item.set("ContentRoot", json::Value::string(mod.resolvedRoot));
        details.push(std::move(item));
    }
    const std::string path = m_directory + "/active-mods.txt";
    if (!writeText(path, lines) || !writeText(m_directory + "/active-mods.json", json::serialize(details)))
        return {};
    return lines.empty() ? std::string{} : path;
}

void ModSet::toggle(size_t index) {
    if (index >= m_mods.size())
        return;
    InstalledMod& mod = m_mods[index];
    if (mod.enabled && !mod.playerDisabled) {
        mod.enabled = false;
    } else {
        // Switching a mod on here also clears the in-game switch, as CubeShelf does.
        mod.enabled = true;
        mod.playerDisabled = false;
    }
}

bool ModSet::move(size_t index, int direction) {
    const size_t other = direction < 0 ? index - 1 : index + 1;
    if (index >= m_mods.size() || (direction < 0 && index == 0) || other >= m_mods.size())
        return false;
    std::swap(m_mods[index].priority, m_mods[other].priority);
    if (m_mods[index].priority == m_mods[other].priority) {
        // Equal priorities would not change the order: make room.
        m_mods[direction < 0 ? index : other].priority += 1;
    }
    sort();
    return true;
}

size_t ModSet::activeCount() const {
    return static_cast<size_t>(std::count_if(m_mods.begin(), m_mods.end(), [](const InstalledMod& m) { return m.active(); }));
}

} // namespace partyboard::launcher
