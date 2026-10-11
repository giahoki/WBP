// SPDX-License-Identifier: GPL-2.0-or-later
// Launcher languages: every text is written in English with the Russian next to it; the other
// languages are in lang/<code>.lng (English text TAB translation), keyed by the English text.
#pragma once
#include <string>
#include <vector>

namespace i18n {

void SetLanguage(const std::string& code);  // "" = the Windows language
const std::string& Language();
std::string SystemLanguage();
bool RightToLeft();

// The text in the current language; `ru` is used for Russian when given.
std::string T(const char* en, const char* ru = nullptr);

struct Option {
    std::string code, name;
};
std::vector<Option> UiLanguages();  // ("" System), en, ru, ar, ...

}  // namespace i18n

using i18n::T;
