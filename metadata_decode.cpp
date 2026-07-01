#include "metadata_decode.h"

#include <algorithm>
#include <cctype>
#include <cwchar>
#include <string>
#include <windows.h>

namespace {

std::wstring DecodeTextWithCodepage(const std::string& str, UINT codePage, DWORD flags)
{
    if (str.empty())
        return std::wstring();

    int sizeNeeded = MultiByteToWideChar(codePage, flags, str.data(), (int)str.size(), NULL, 0);
    if (sizeNeeded <= 0)
        return std::wstring();

    std::wstring result(sizeNeeded, 0);
    if (MultiByteToWideChar(codePage, flags, str.data(), (int)str.size(), &result[0], sizeNeeded) <= 0)
        return std::wstring();

    return result;
}

std::string EncodeTextWithCodepage(const std::wstring& str, UINT codePage)
{
    if (str.empty())
        return std::string();

    int sizeNeeded = WideCharToMultiByte(codePage, 0, str.data(), (int)str.size(), NULL, 0, NULL, NULL);
    if (sizeNeeded <= 0)
        return std::string();

    std::string result(sizeNeeded, 0);
    if (WideCharToMultiByte(codePage, 0, str.data(), (int)str.size(), &result[0], sizeNeeded, NULL, NULL) <= 0)
        return std::string();

    return result;
}

std::string EncodeTextWithCodepageStrict(const std::wstring& str, UINT codePage)
{
    if (str.empty())
        return std::string();

    BOOL usedDefaultChar = FALSE;
    int sizeNeeded = WideCharToMultiByte(codePage, 0, str.data(), (int)str.size(),
        NULL, 0, NULL, &usedDefaultChar);
    if (sizeNeeded <= 0 || usedDefaultChar)
        return std::string();

    std::string result(sizeNeeded, 0);
    usedDefaultChar = FALSE;
    if (WideCharToMultiByte(codePage, 0, str.data(), (int)str.size(),
        &result[0], sizeNeeded, NULL, &usedDefaultChar) <= 0 || usedDefaultChar)
        return std::string();

    return result;
}

bool IsBasicLatinLetter(wchar_t ch)
{
    return (ch >= L'A' && ch <= L'Z') || (ch >= L'a' && ch <= L'z');
}

bool IsCyrillicLetter(wchar_t ch)
{
    return ch >= 0x0400 && ch <= 0x04FF;
}

bool IsApostropheLike(wchar_t ch)
{
    return ch == L'\'' || ch == L'\u2019' || ch == L'\u0060' || ch == L'\u00B4';
}

bool IsWesternAccentedLetter(wchar_t ch)
{
    static const wchar_t* chars =
        L"\u00C0\u00C1\u00C2\u00C3\u00C4\u00C5\u00C6\u00C7"
        L"\u00C8\u00C9\u00CA\u00CB\u00CC\u00CD\u00CE\u00CF"
        L"\u00D1\u00D2\u00D3\u00D4\u00D5\u00D6\u00D8"
        L"\u00D9\u00DA\u00DB\u00DC\u00DD\u00DE\u00DF"
        L"\u00E0\u00E1\u00E2\u00E3\u00E4\u00E5\u00E6\u00E7"
        L"\u00E8\u00E9\u00EA\u00EB\u00EC\u00ED\u00EE\u00EF"
        L"\u00F1\u00F2\u00F3\u00F4\u00F5\u00F6\u00F8"
        L"\u00F9\u00FA\u00FB\u00FC\u00FD\u00FE\u00FF";

    return std::wcschr(chars, ch) != nullptr;
}

bool IsCaronLetter(wchar_t ch)
{
    static const wchar_t* chars =
        L"\u010C\u010D\u010E\u010F\u011A\u011B\u0147\u0148"
        L"\u0158\u0159\u0160\u0161\u0164\u0165\u017D\u017E";

    return std::wcschr(chars, ch) != nullptr;
}

bool IsWesternVowel(wchar_t ch)
{
    static const wchar_t* chars =
        L"AaEeIiOoUu"
        L"\u00C0\u00C1\u00C2\u00C3\u00C4\u00C5"
        L"\u00C8\u00C9\u00CA\u00CB\u00CC\u00CD\u00CE\u00CF"
        L"\u00D2\u00D3\u00D4\u00D5\u00D6"
        L"\u00D9\u00DA\u00DB\u00DC"
        L"\u00E0\u00E1\u00E2\u00E3\u00E4\u00E5"
        L"\u00E8\u00E9\u00EA\u00EB\u00EC\u00ED\u00EE\u00EF"
        L"\u00F2\u00F3\u00F4\u00F5\u00F6"
        L"\u00F9\u00FA\u00FB\u00FC";

    return std::wcschr(chars, ch) != nullptr;
}

int CountCyrillicLetters(const std::wstring& str)
{
    int count = 0;
    for (wchar_t ch : str) {
        if (IsCyrillicLetter(ch))
            ++count;
    }
    return count;
}

int CountBasicLatinLetters(const std::wstring& str)
{
    int count = 0;
    for (wchar_t ch : str) {
        if (IsBasicLatinLetter(ch))
            ++count;
    }
    return count;
}

int CountUtf8MojibakeMarkers(const std::wstring& str)
{
    int count = 0;
    for (wchar_t ch : str) {
        if (ch == L'\u00C3' || ch == L'\u00C2' || ch == L'\u00E2')
            ++count;
    }
    return count;
}

bool HasCyrillicInLatinContext(const std::wstring& str)
{
    for (size_t i = 0; i < str.size(); ++i) {
        if (!IsCyrillicLetter(str[i]))
            continue;

        const bool hasLatinBefore = i > 0 && IsBasicLatinLetter(str[i - 1]);
        const bool hasLatinAfter = (i + 1) < str.size() && IsBasicLatinLetter(str[i + 1]);
        const bool hasLatinBeforeApostrophe =
            i > 1 && IsApostropheLike(str[i - 1]) && IsBasicLatinLetter(str[i - 2]);
        const bool hasLatinAfterApostrophe =
            (i + 2) < str.size() && IsApostropheLike(str[i + 1]) && IsBasicLatinLetter(str[i + 2]);

        if (hasLatinBefore || hasLatinAfter || hasLatinBeforeApostrophe || hasLatinAfterApostrophe)
            return true;
    }

    return false;
}

bool HasApostropheBeforeWesternVowel(const std::wstring& str)
{
    for (size_t i = 1; i < str.size(); ++i) {
        if (IsApostropheLike(str[i - 1]) && IsWesternVowel(str[i]))
            return true;
    }
    return false;
}

bool HasApostropheBeforeCaronLetter(const std::wstring& str)
{
    for (size_t i = 1; i < str.size(); ++i) {
        if (IsApostropheLike(str[i - 1]) && IsCaronLetter(str[i]))
            return true;
    }
    return false;
}

int ScoreDecodedText(const std::wstring& decoded, UINT codePage)
{
    if (decoded.empty())
        return -100000;

    int score = 0;
    int latinCount = 0;
    int cyrillicCount = 0;
    int westernAccentCount = 0;
    int caronCount = 0;
    int controlCount = 0;

    for (wchar_t ch : decoded) {
        if (IsBasicLatinLetter(ch)) ++latinCount;
        if (IsCyrillicLetter(ch)) ++cyrillicCount;
        if (IsWesternAccentedLetter(ch)) ++westernAccentCount;
        if (IsCaronLetter(ch)) ++caronCount;
        if ((ch < 0x20 && ch != L'\t' && ch != L'\r' && ch != L'\n') || ch == 0xFFFD)
            ++controlCount;
    }

    score += latinCount;
    score += cyrillicCount * 2;
    score += westernAccentCount * 3;
    score += caronCount * 2;
    score -= controlCount * 20;
    score -= CountUtf8MojibakeMarkers(decoded) * 10;

    if (HasCyrillicInLatinContext(decoded))
        score -= 25;

    if (HasApostropheBeforeWesternVowel(decoded))
        score += 12;

    if (HasApostropheBeforeCaronLetter(decoded))
        score -= 12;

    if (codePage == 1251 && cyrillicCount > 0 && cyrillicCount >= latinCount / 2)
        score += 10;

    if (codePage == 1252 && westernAccentCount > 0)
        score += 4;

    if (codePage == 1250 && caronCount > 0 && !HasApostropheBeforeCaronLetter(decoded))
        score += 4;

    return score;
}

bool TryParseHtmlNumericEntity(const std::wstring& text, size_t ampPos, size_t& endPos, wchar_t& outCh)
{
    if (ampPos + 3 >= text.size() || text[ampPos] != L'&' || text[ampPos + 1] != L'#')
        return false;

    size_t pos = ampPos + 2;
    int base = 10;
    if (pos < text.size() && (text[pos] == L'x' || text[pos] == L'X')) {
        base = 16;
        ++pos;
    }

    if (pos >= text.size())
        return false;

    unsigned value = 0;
    bool hasDigit = false;
    for (; pos < text.size(); ++pos) {
        wchar_t ch = text[pos];
        if (ch == L';')
            break;

        int digit = -1;
        if (ch >= L'0' && ch <= L'9') {
            digit = ch - L'0';
        }
        else if (base == 16 && ch >= L'a' && ch <= L'f') {
            digit = 10 + ch - L'a';
        }
        else if (base == 16 && ch >= L'A' && ch <= L'F') {
            digit = 10 + ch - L'A';
        }
        else {
            return false;
        }

        if (digit >= base)
            return false;

        value = value * base + static_cast<unsigned>(digit);
        hasDigit = true;
    }

    if (!hasDigit || pos >= text.size() || text[pos] != L';' || value == 0 || value > 0xFFFF)
        return false;

    outCh = static_cast<wchar_t>(value);
    endPos = pos;
    return true;
}

void DecodeHtmlNumericEntities(std::wstring& text)
{
    std::wstring result;
    result.reserve(text.size());

    for (size_t i = 0; i < text.size(); ++i) {
        size_t entityEnd = 0;
        wchar_t entityCh = 0;
        if (TryParseHtmlNumericEntity(text, i, entityEnd, entityCh)) {
            result.push_back(entityCh);
            i = entityEnd;
        }
        else {
            result.push_back(text[i]);
        }
    }

    text.swap(result);
}

std::wstring TryRepairUtf8Mojibake(const std::wstring& decoded)
{
    const int markerCount = CountUtf8MojibakeMarkers(decoded);
    if (markerCount <= 0)
        return std::wstring();

    std::string bytes = EncodeTextWithCodepageStrict(decoded, 1252);
    if (bytes.empty())
        return std::wstring();

    std::wstring repaired = DecodeTextWithCodepage(bytes, CP_UTF8, MB_ERR_INVALID_CHARS);
    if (repaired.empty())
        return std::wstring();

    return CountUtf8MojibakeMarkers(repaired) < markerCount ? repaired : std::wstring();
}

std::wstring TryRepairCp1251Mojibake(const std::wstring& decoded)
{
    const int cyrillicCount = CountCyrillicLetters(decoded);
    if (cyrillicCount <= 0 || cyrillicCount > 4)
        return std::wstring();

    if (CountBasicLatinLetters(decoded) < cyrillicCount * 2)
        return std::wstring();

    if (!HasCyrillicInLatinContext(decoded))
        return std::wstring();

    std::string cp1251Bytes = EncodeTextWithCodepage(decoded, 1251);
    if (cp1251Bytes.empty())
        return std::wstring();

    std::wstring cp1252 = DecodeTextWithCodepage(cp1251Bytes, 1252, 0);
    std::wstring cp1250 = DecodeTextWithCodepage(cp1251Bytes, 1250, 0);

    if (!cp1252.empty() && CountCyrillicLetters(cp1252) == 0 &&
        ScoreDecodedText(cp1252, 1252) >= ScoreDecodedText(cp1250, 1250))
        return cp1252;

    if (!cp1250.empty() && CountCyrillicLetters(cp1250) == 0)
        return cp1250;

    return std::wstring();
}

std::wstring DecodeSingleByteMetadata(const std::string& str)
{
    struct Candidate {
        UINT codePage;
        std::wstring text;
        int score;
    };

    Candidate candidates[] = {
        { 1251, DecodeTextWithCodepage(str, 1251, 0), 0 },
        { 1252, DecodeTextWithCodepage(str, 1252, 0), 0 },
        { 1250, DecodeTextWithCodepage(str, 1250, 0), 0 }
    };

    Candidate* best = nullptr;
    for (auto& candidate : candidates) {
        candidate.score = ScoreDecodedText(candidate.text, candidate.codePage);
        if (!best || candidate.score > best->score)
            best = &candidate;
    }

    return best ? best->text : std::wstring();
}

} // namespace

std::wstring DecodeMetadataToWideString(const std::string& str)
{
    if (str.empty())
        return std::wstring();

    std::wstring decoded = DecodeTextWithCodepage(str, CP_UTF8, MB_ERR_INVALID_CHARS);
    if (!decoded.empty()) {
        std::wstring repaired = TryRepairUtf8Mojibake(decoded);
        if (!repaired.empty()) {
            DecodeHtmlNumericEntities(repaired);
            return repaired;
        }

        repaired = TryRepairCp1251Mojibake(decoded);
        if (!repaired.empty()) {
            DecodeHtmlNumericEntities(repaired);
            return repaired;
        }

        DecodeHtmlNumericEntities(decoded);
        return decoded;
    }

    decoded = DecodeSingleByteMetadata(str);
    if (!decoded.empty()) {
        DecodeHtmlNumericEntities(decoded);
        return decoded;
    }

    std::wstring fallback;
    fallback.reserve(str.size());
    for (char ch : str)
        fallback.push_back(static_cast<wchar_t>(static_cast<unsigned char>(ch)));

    DecodeHtmlNumericEntities(fallback);
    return fallback;
}
