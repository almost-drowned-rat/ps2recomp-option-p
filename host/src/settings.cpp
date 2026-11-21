/*
 * The port's settings (settings.h): a map of keys to values, read from and
 * written to one text file. Lines are `key=value`; anything else (comments,
 * blank lines) is left out when the file is written again.
 */

#include "ps2host/settings.h"

#include <cstdio>
#include <fstream>
#include <map>
#include <mutex>

namespace ps2host {

namespace {

std::mutex g_lock;
std::map<std::string, std::string> g_values;
std::map<std::string, std::string> g_session;     // SessionSetting: over g_values, never written
std::string g_path;

std::string Trim(const std::string& text) {
    const auto first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return "";
    return text.substr(first, text.find_last_not_of(" \t\r\n") - first + 1);
}

void Write() {
    if (g_path.empty()) return;
    std::ofstream out(g_path, std::ios::trunc);
    if (!out) {
        std::fprintf(stderr, "[settings] cannot write %s\n", g_path.c_str());
        return;
    }
    out << "# The PC port's settings (PC SETTINGS in the main menu).\n";
    for (const auto& [key, value] : g_values) out << key << '=' << value << '\n';
}

}  // namespace

void LoadSettings(const std::string& path) {
    std::lock_guard<std::mutex> hold(g_lock);
    g_path = path;
    g_values.clear();
    std::ifstream in(path);
    std::string line;
    bool first = true;
    while (std::getline(in, line)) {
        // A UTF-8 byte order mark before the first line (a file saved so by an editor, or PowerShell's
        // Set-Content -Encoding utf8): not its key's - the first setting was lost to it (ps2-port-engine 90281dd).
        if (first && line.compare(0, 3, "\xEF\xBB\xBF") == 0) line.erase(0, 3);
        first = false;
        const auto equals = line.find('=');
        if (line.empty() || line[0] == '#' || equals == std::string::npos) continue;
        const std::string key = Trim(line.substr(0, equals));
        if (!key.empty()) g_values[key] = Trim(line.substr(equals + 1));
    }
}

const std::string& SettingsPath() { return g_path; }

std::string Setting(const std::string& key, const std::string& fallback) {
    std::lock_guard<std::mutex> hold(g_lock);
    const auto session = g_session.find(key);
    if (session != g_session.end()) return session->second;
    const auto found = g_values.find(key);
    return found == g_values.end() ? fallback : found->second;
}

bool SettingOn(const std::string& key, bool fallback) {
    const std::string value = Setting(key, fallback ? "on" : "off");
    return value == "on" || value == "1" || value == "true" || value == "yes";
}

void SetSetting(const std::string& key, const std::string& value) {
    std::lock_guard<std::mutex> hold(g_lock);
    if (g_values.count(key) && g_values[key] == value) return;
    g_values[key] = value;
    Write();
    // Each change in the log too (the maintainer 2026-11-20, issue 10: a report shows what was set when).
    if (key != "window_rect") std::fprintf(stderr, "[settings] %s=%s\n", key.c_str(), value.c_str());
}

void LogSettings() {
    std::lock_guard<std::mutex> hold(g_lock);
    std::fprintf(stderr, "[settings] %s:", g_path.empty() ? "(none)" : g_path.c_str());
    for (const auto& [key, value] : g_values)
        if (key != "window_rect") std::fprintf(stderr, " %s=%s", key.c_str(), value.c_str());
    std::fprintf(stderr, "\n");
}

void SetSettingOn(const std::string& key, bool on) { SetSetting(key, on ? "on" : "off"); }

void RunSetting(const std::string& key, const std::string& value) {
    std::lock_guard<std::mutex> hold(g_lock);
    g_values[key] = value;
}

// (ps2-port-engine PR #7, 568781e: a LAN game's host's settings taken for the game, the player's file as it is)
void SessionSetting(const std::string& key, const std::string& value) {
    std::lock_guard<std::mutex> hold(g_lock);
    g_session[key] = value;
}

void ClearSessionSettings() {
    std::lock_guard<std::mutex> hold(g_lock);
    g_session.clear();
}

}  // namespace ps2host
