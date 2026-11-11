#include "core.hpp"

#include <array>
#include <fstream>
#include <regex>
#include <utf8proc.h>

#if defined(_WIN32)
#include <windows.h>
#include <bcrypt.h>
#elif defined(__APPLE__)
#include <CommonCrypto/CommonDigest.h>
#else
#include <openssl/sha.h>
#endif

namespace hc {
namespace {
bool scalar(char32_t c) {
    return c <= 0x10ffff && !(c >= 0xd800 && c <= 0xdfff);
}

Text utf16(std::u16string_view data) {
    Text text;
    for (std::size_t i = 0; i < data.size(); ++i) {
        char32_t c = data[i];
        if (c >= 0xd800 && c <= 0xdbff && i + 1 < data.size()) {
            const char32_t tail = data[i + 1];
            if (tail >= 0xdc00 && tail <= 0xdfff) {
                c = 0x10000 + ((c - 0xd800) << 10) + (tail - 0xdc00);
                ++i;
            }
        }
        if (!scalar(c)) throw Error("Некорректная последовательность UTF-16.");
        text.push_back(c);
    }
    return text;
}

constexpr char32_t cp1251[] = {
    0x0402,0x0403,0x201a,0x0453,0x201e,0x2026,0x2020,0x2021,
    0x20ac,0x2030,0x0409,0x2039,0x040a,0x040c,0x040b,0x040f,
    0x0452,0x2018,0x2019,0x201c,0x201d,0x2022,0x2013,0x2014,
    0,0x2122,0x0459,0x203a,0x045a,0x045c,0x045b,0x045f,
    0x00a0,0x040e,0x045e,0x0408,0x00a4,0x0490,0x00a6,0x00a7,
    0x0401,0x00a9,0x0404,0x00ab,0x00ac,0x00ad,0x00ae,0x0407,
    0x00b0,0x00b1,0x0406,0x0456,0x0491,0x00b5,0x00b6,0x00b7,
    0x0451,0x2116,0x0454,0x00bb,0x0458,0x0405,0x0455,0x0457,
};

Text decode_1251(std::string_view bytes) {
    Text text;
    for (unsigned char byte : bytes) {
        const char32_t c = byte < 128 ? byte : byte >= 192 ? 0x0410 + byte - 192 : cp1251[byte - 128];
        if (!c) throw Error("В файле есть недопустимый символ Windows-1251.");
        text.push_back(c);
    }
    return text;
}
} // namespace

std::string utf8(View text) {
    std::string result;
    for (auto c : text) {
        if (!scalar(c)) throw Error("Недопустимый символ Unicode.");
        if (c < 0x80) result.push_back(static_cast<char>(c));
        else if (c < 0x800) {
            result.push_back(static_cast<char>(0xc0 | (c >> 6)));
            result.push_back(static_cast<char>(0x80 | (c & 63)));
        } else {
            if (c >= 0x10000) {
                result.push_back(static_cast<char>(0xf0 | (c >> 18)));
                result.push_back(static_cast<char>(0x80 | ((c >> 12) & 63)));
            } else result.push_back(static_cast<char>(0xe0 | (c >> 12)));
            result.push_back(static_cast<char>(0x80 | ((c >> 6) & 63)));
            result.push_back(static_cast<char>(0x80 | (c & 63)));
        }
    }
    return result;
}

Text unicode(std::string_view bytes) {
    Text text;
    for (std::size_t i = 0; i < bytes.size();) {
        const auto lead = static_cast<unsigned char>(bytes[i++]);
        if (lead < 0x80) {
            text.push_back(lead);
            continue;
        }
        int extra = lead >= 0xc2 && lead <= 0xdf ? 1 : lead >= 0xe0 && lead <= 0xef ? 2 :
            lead >= 0xf0 && lead <= 0xf4 ? 3 : 0;
        if (!extra || bytes.size() - i < static_cast<std::size_t>(extra))
            throw Error("Некорректная последовательность UTF-8.");
        char32_t c = lead & (0x7f >> (extra + 1));
        for (int k = 0; k < extra; ++k) {
            const auto continuation = static_cast<unsigned char>(bytes[i++]);
            if ((continuation & 0xc0) != 0x80) throw Error("Некорректная последовательность UTF-8.");
            c = (c << 6) | (continuation & 63);
        }
        if (!scalar(c) || (extra == 2 && c < 0x800) || (extra == 3 && c < 0x10000))
            throw Error("Некорректная последовательность UTF-8.");
        text.push_back(c);
    }
    return text;
}

std::wstring wide(View text) {
    std::wstring result;
    for (char32_t c : text) {
        if constexpr (sizeof(wchar_t) == 2) {
            if (c >= 0x10000) {
                c -= 0x10000;
                result.push_back(static_cast<wchar_t>(0xd800 + (c >> 10)));
                c = 0xdc00 + (c & 1023);
            }
        }
        result.push_back(static_cast<wchar_t>(c));
    }
    return result;
}

Text from_wide(std::wstring_view text) {
    if constexpr (sizeof(wchar_t) == 2) {
        return utf16(std::u16string(text.begin(), text.end()));
    } else return Text(text.begin(), text.end());
}

Text decode_html(std::string_view bytes) {
    if (bytes.substr(0, 3) == "\xef\xbb\xbf") return unicode(bytes.substr(3));
    const bool le = bytes.substr(0, 2) == "\xff\xfe";
    const bool be = bytes.substr(0, 2) == "\xfe\xff";
    if (le || be) {
        if (bytes.size() % 2) throw Error("Неполный символ UTF-16 в конце файла.");
        std::u16string data;
        for (std::size_t i = 2; i < bytes.size(); i += 2) {
            auto first = static_cast<unsigned char>(bytes[i]);
            auto second = static_cast<unsigned char>(bytes[i + 1]);
            data.push_back(static_cast<char16_t>(le ? first | (second << 8) : (first << 8) | second));
        }
        return utf16(data);
    }
    static const std::regex meta(R"(<meta\b[^>]*charset\s*=\s*["']?([A-Za-z0-9_-]+))", std::regex::icase);
    const auto head = std::string(bytes.substr(0, 10000));
    std::smatch match;
    if (std::regex_search(head, match, meta)) {
        const auto encoding = lower(unicode(match[1].str()));
        if (encoding == U"utf-8" || encoding == U"utf8") return unicode(bytes);
        if (encoding == U"windows-1251" || encoding == U"cp1251") return decode_1251(bytes);
        if (encoding == U"ascii" || encoding == U"us-ascii") return unicode(bytes);
        throw Error("Кодировка не поддерживается: " + match[1].str() + ". Сохраните HTML в UTF-8.");
    }
    try { return unicode(bytes); }
    catch (const Error&) { return decode_1251(bytes); }
}

std::string read_file(const fs::path& path, std::uintmax_t limit) {
    std::ifstream stream(path, std::ios::binary | std::ios::ate);
    if (!stream) throw Error("Не удалось открыть файл.");
    const auto size = stream.tellg();
    if (size < 0 || static_cast<std::uintmax_t>(size) > limit) throw Error("Файл превышает допустимый размер.");
    std::string bytes(static_cast<std::size_t>(size), '\0');
    stream.seekg(0);
    if (!stream.read(bytes.data(), static_cast<std::streamsize>(bytes.size())))
        throw Error("Не удалось прочитать файл целиком.");
    return bytes;
}

std::string sha256(std::string_view bytes) {
    std::array<unsigned char, 32> digest{};
#if defined(_WIN32)
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0)
        throw Error("Не удалось открыть системный SHA-256.");
    struct Provider { BCRYPT_ALG_HANDLE handle; ~Provider() { BCryptCloseAlgorithmProvider(handle, 0); } } provider{algorithm};
    DWORD size = 0, written = 0;
    if (BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&size), sizeof(size), &written, 0) < 0)
        throw Error("Не удалось подготовить SHA-256.");
    std::vector<unsigned char> buffer(size);
    BCRYPT_HASH_HANDLE hash = nullptr;
    if (BCryptCreateHash(algorithm, &hash, buffer.data(), size, nullptr, 0, 0) < 0)
        throw Error("Не удалось создать SHA-256.");
    struct Hash { BCRYPT_HASH_HANDLE handle; ~Hash() { BCryptDestroyHash(handle); } } handle{hash};
    if (BCryptHashData(hash, reinterpret_cast<PUCHAR>(const_cast<char*>(bytes.data())), static_cast<ULONG>(bytes.size()), 0) < 0 ||
        BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0) < 0)
        throw Error("Не удалось вычислить SHA-256.");
#elif defined(__APPLE__)
    CC_SHA256(bytes.data(), static_cast<CC_LONG>(bytes.size()), digest.data());
#else
    SHA256(reinterpret_cast<const unsigned char*>(bytes.data()), bytes.size(), digest.data());
#endif
    std::string result;
    const char hex[] = "0123456789abcdef";
    for (auto byte : digest) {
        result += hex[byte >> 4];
        result += hex[byte & 15];
    }
    return result;
}

bool whitespace(char32_t c) {
    return c == U' ' || (c >= U'\t' && c <= U'\r') || c == 0x85 || c == 0xa0 ||
        c == 0x1680 || (c >= 0x2000 && c <= 0x200a) || c == 0x2028 || c == 0x2029 ||
        c == 0x202f || c == 0x205f || c == 0x3000;
}
bool letter(char32_t c) {
    const auto category = utf8proc_category(c);
    return c == U'_' || (category >= UTF8PROC_CATEGORY_LU && category <= UTF8PROC_CATEGORY_ME);
}
bool digit(char32_t c) {
    return utf8proc_category(c) == UTF8PROC_CATEGORY_ND || View(U"⁰¹²³⁴⁵⁶⁷⁸⁹").find(c) != View::npos;
}

Text lower(View text) {
    Text result;
    for (char32_t c : text) result.push_back(static_cast<char32_t>(utf8proc_tolower(c)));
    return result;
}

Text compact(View text) {
    Text result;
    bool gap = false;
    for (const auto c : text) {
        if (c == 0xad) continue;
        if (whitespace(c)) gap = !result.empty();
        else {
            if (gap) result.push_back(U' ');
            result.push_back(c);
            gap = false;
        }
    }
    return result;
}

Text canonical(View text, bool heading) {
    const auto normalized = compact(text);
    Text result;
    for (std::size_t i = 0; i < normalized.size(); ++i) {
        const auto ch = normalized[i];
        if (ch == U'№' || (ch == U'N' && (i == 0 || !letter(normalized[i - 1])))) {
            auto j = i + 1;
            if (j < normalized.size() && normalized[j] == U' ') ++j;
            if (j < normalized.size() && digit(normalized[j])) {
                result += U"№ ";
                i = j - 1;
                continue;
            }
        }
        result.push_back(ch);
    }
    static const std::regex enum_parens(R"(^(\d+)\((\d+)\)([.)]))");
    static const std::regex enum_decimal(R"(^(\d+)\.(\d+)([.)]))");
    static const std::regex roman_suffix(R"((\d+)-([IVXLCDM]+)\b)");
    static const std::regex scientific(R"((\d+)\s*(?:х|x)\s*(\d+\.[\d-]+))");
    static const std::regex footnote(R"(\s?<([\d*]+)>)");
    auto value = std::regex_replace(std::regex_replace(utf8(result), enum_parens, "$1^$2$3"), enum_decimal, "$1^$2$3");
    const auto roman_value = [](char c) {
        switch (c) {
        case 'I': return 1; case 'V': return 5; case 'X': return 10; case 'L': return 50;
        case 'C': return 100; case 'D': return 500; default: return 1000;
        }
    };
    std::string converted;
    std::size_t pos = 0;
    for (auto i = std::sregex_iterator(value.begin(), value.end(), roman_suffix); i != std::sregex_iterator(); ++i) {
        auto roman = (*i)[2].str();
        if (roman.size() > 15) continue;
        int total = 0;
        for (std::size_t j = 0; j < roman.size(); ++j) {
            const int v = roman_value(roman[j]);
            total += j + 1 < roman.size() && v < roman_value(roman[j + 1]) ? -v : v;
        }
        // Convert only canonical Roman numerals, leaving malformed values visible.
        auto remainder = total;
        std::string standard;
        for (const auto& item : {std::pair{1000,"M"}, {900,"CM"}, {500,"D"}, {400,"CD"},
                                 {100,"C"}, {90,"XC"}, {50,"L"}, {40,"XL"}, {10,"X"},
                                 {9,"IX"}, {5,"V"}, {4,"IV"}, {1,"I"}}) {
            while (remainder >= item.first) { standard += item.second; remainder -= item.first; }
        }
        if (total < 1 || total > 3999 || standard != roman) continue;
        const auto start = static_cast<std::size_t>(i->position());
        converted += value.substr(pos, start - pos) + (*i)[1].str() + "-" + std::to_string(total);
        pos = start + static_cast<std::size_t>(i->length());
    }
    converted += value.substr(pos);
    value = std::regex_replace(std::regex_replace(converted, scientific, "$1 x $2"), footnote, ".$1");
    result = unicode(value);
    return heading ? lower(result) : result;
}

const std::vector<std::pair<Category, Text>>& categories() {
    static const std::vector<std::pair<Category, Text>> labels = {
        {words,U"Слова"}, {numbers,U"Числа и даты"}, {punctuation,U"Пунктуация"},
        {notes,U"Редакционные отметки"}, {note_position,U"Расположение отметок"},
        {links,U"Ссылки"}, {tables,U"Таблицы"}, {structure,U"Структура и пропуски"}, {format,U"Оформление"},
    };
    return labels;
}
Text category_names(Mask mask) {
    Text result;
    for (const auto& item : categories()) if (mask & item.first) {
        if (!result.empty()) result += U" · ";
        result += item.second;
    }
    return result;
}
Text label(Priority value) {
    switch (value) {
    case Priority::high: return U"Высокий";
    case Priority::medium: return U"Средний";
    case Priority::low: return U"Низкий";
    }
    return {};
}
Text label(Decision value) {
    switch (value) {
    case Decision::pending: return U"Не проверено";
    case Decision::confirmed: return U"Подтверждено";
    case Decision::acceptable: return U"Допустимо";
    case Decision::needs_review: return U"Требует уточнения";
    }
    return {};
}
} // namespace hc
