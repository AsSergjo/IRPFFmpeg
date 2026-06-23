#pragma once

#include <string>
#include <vector>

struct LanguageOption {
    std::wstring id;
    std::wstring displayName;
};

extern std::wstring g_languageId;

void InitializeLanguageSystem();
bool LoadLanguageById(const std::wstring& languageId);
void SaveLanguageSelection();
void LoadAvailableLanguages();
const std::vector<LanguageOption>& GetAvailableLanguages();
const wchar_t* Tr(const char* key, const wchar_t* fallback);
