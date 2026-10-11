// SPDX-License-Identifier: GPL-2.0-or-later
#include "i18n.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstring>
#include <map>
#include <mutex>

#include "util.h"

namespace i18n {
namespace {

std::string language = "en";
std::map<std::string, std::string> table;
std::mutex lock;

std::string Unescape(std::string_view text) {
    std::string out;
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '\\' && i + 1 < text.size()) {
            char c = text[++i];
            out += c == 'n' ? '\n' : c == 't' ? '\t' : c;
        } else {
            out += text[i];
        }
    }
    return out;
}

void Load(const std::string& code) {
    table.clear();
    if (code == "en") return;
    for (const fs::path& dir : {PortDir() / L"launcher" / L"lang", PortDir() / L"lang"}) {
        auto text = ReadText(dir / Wide(code + ".lng"));
        if (!text) continue;
        for (std::string line : Split(*text, '\n')) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty() || line[0] == '#') continue;
            size_t tab = line.find('\t');
            if (tab == std::string::npos) continue;
            table[Unescape(line.substr(0, tab))] = Unescape(line.substr(tab + 1));
        }
        return;
    }
}

}  // namespace

std::string SystemLanguage() {
    static const std::map<int, std::string> windows = {{0x19, "ru"}, {0x01, "ar"}, {0x0a, "es"}, {0x16, "pt"},
                                                       {0x0c, "fr"}, {0x07, "de"}, {0x10, "it"}, {0x15, "pl"},
                                                       {0x1f, "tr"}, {0x04, "zh"}, {0x11, "ja"}, {0x12, "ko"}};
    auto it = windows.find(GetUserDefaultUILanguage() & 0x3ff);
    return it == windows.end() ? "en" : it->second;
}

void SetLanguage(const std::string& code) {
    std::lock_guard guard(lock);
    language = code.empty() ? SystemLanguage() : code;
    Load(language);
}

const std::string& Language() { return language; }
bool RightToLeft() { return language == "ar"; }

std::string T(const char* en, const char* ru) {
    std::string key = en;
    // "  ⚠" marks a risky choice; the text before it is translated as usual.
    static const std::string warn = "  ⚠";
    if (EndsWith(key, warn)) {
        std::string ru_base;
        if (ru) ru_base = std::string(ru).substr(0, EndsWith(ru, warn) ? std::strlen(ru) - warn.size() : std::strlen(ru));
        return T(key.substr(0, key.size() - warn.size()).c_str(), ru ? ru_base.c_str() : nullptr) + warn;
    }
    std::lock_guard guard(lock);
    if (language == "ru" && ru) return ru;
    auto it = table.find(key);
    return it == table.end() ? key : it->second;
}

std::vector<Option> UiLanguages() {
    return {{"", T("System", "Как в системе")}, {"en", "English"}, {"ru", "Русский"}, {"ar", "العربية"},
            {"es", "Español"}, {"pt", "Português (Brasil)"}, {"fr", "Français"}, {"de", "Deutsch"},
            {"it", "Italiano"}, {"pl", "Polski"}, {"tr", "Türkçe"}, {"zh", "简体中文"}, {"ja", "日本語"},
            {"ko", "한국어"}};
}

}  // namespace i18n
