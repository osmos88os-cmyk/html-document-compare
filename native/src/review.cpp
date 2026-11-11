#include "core.hpp"

#include <chrono>
#include <ctime>
#include <iomanip>
#include <map>
#include <sstream>
#include <nlohmann/json.hpp>
#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace hc {
namespace {
using Json = nlohmann::json;
const char* mode_key(Mode mode) { return mode == Mode::strict ? "strict" : "structured"; }

void atomic_file(const fs::path& path, const std::string& bytes) {
#ifdef _WIN32
    fs::path temporary;
    HANDLE file = INVALID_HANDLE_VALUE;
    for (unsigned attempt = 0; attempt < 100; ++attempt) {
        temporary = path;
        temporary += L".tmp-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
            std::to_wstring(GetTickCount64()) + L"-" + std::to_wstring(attempt);
        file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file != INVALID_HANDLE_VALUE || GetLastError() != ERROR_FILE_EXISTS) break;
    }
    if (file == INVALID_HANDLE_VALUE) throw Error("Не удалось создать временный файл рядом с проверкой.");
    bool ok = true;
    for (std::size_t pos = 0; pos < bytes.size();) {
        DWORD written = 0;
        auto size = static_cast<DWORD>(std::min<std::size_t>(bytes.size() - pos, 1024 * 1024));
        if (!WriteFile(file, bytes.data() + pos, size, &written, nullptr) || !written) { ok = false; break; }
        pos += written;
    }
    if (ok) ok = FlushFileBuffers(file) != 0;
    if (!CloseHandle(file)) ok = false;
    if (ok) ok = MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
    if (!ok) {
        DeleteFileW(temporary.c_str());
        throw Error("Не удалось сохранить файл. Проверьте доступ к папке и свободное место.");
    }
#else
    std::string name = path.string() + ".tmp-XXXXXX";
    const int file = mkstemp(name.data());
    if (file < 0) throw Error("Не удалось создать временный файл.");
    bool ok = true;
    for (std::size_t pos = 0; pos < bytes.size();) {
        const auto written = write(file, bytes.data() + pos, bytes.size() - pos);
        if (written <= 0) { ok = false; break; }
        pos += static_cast<std::size_t>(written);
    }
    if (ok) ok = fsync(file) == 0;
    if (close(file) != 0) ok = false;
    if (ok) ok = rename(name.c_str(), path.c_str()) == 0;
    if (!ok) {
        unlink(name.c_str());
        throw Error("Не удалось сохранить файл.");
    }
#endif
}

std::string now_utc() {
    const auto timestamp = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm utc{};
#ifdef _WIN32
    gmtime_s(&utc, &timestamp);
#else
    gmtime_r(&timestamp, &utc);
#endif
    std::ostringstream value;
    value << std::put_time(&utc, "%Y-%m-%dT%H:%M:%SZ");
    return value.str();
}

int enum_value(const Json& object, const char* key, int max) {
    const auto& value = object.at(key);
    if (!value.is_number_integer()) throw Error("Некорректный статус или приоритет.");
    const auto number = value.get<std::int64_t>();
    if (number < 0 || number > max) throw Error("Некорректный статус или приоритет.");
    return static_cast<int>(number);
}
Text review_comment(const Json& value) {
    auto result = unicode(value.get<std::string>());
    if (result.size() > 10000 || result.find(U'\0') != Text::npos)
        throw Error("Недопустимый комментарий к решению.");
    return result;
}

std::string escaped(View value) {
    std::string result;
    for (char c : utf8(value)) {
        switch (c) {
        case '&': result += "&amp;"; break;
        case '<': result += "&lt;"; break;
        case '>': result += "&gt;"; break;
        case '"': result += "&quot;"; break;
        case '\'': result += "&#39;"; break;
        default: result += c;
        }
    }
    return result;
}
} // namespace

void review(Change& c, Decision decision, Priority priority, Text comment) {
    if (comment.size() > 10000) throw Error("Комментарий длиннее 10 000 символов.");
    if (c.decision == decision && c.priority == priority && c.comment == comment) return;
    c.history.push_back({decision, priority, comment, now_utc()});
    c.decision = decision;
    c.priority = priority;
    c.comment = std::move(comment);
}

void save_review(const Result& result, const fs::path& path) {
    Json root = {
        {"schema", 2}, {"algorithm", 1}, {"mode", mode_key(result.mode)},
        {"left", {{"name", utf8(result.left.name)}, {"sha256", result.left.hash}}},
        {"right", {{"name", utf8(result.right.name)}, {"sha256", result.right.hash}}},
        {"reviews", Json::object()}
    };
    for (const auto& c : result.changes) {
        if (c.history.empty() && c.decision == Decision::pending && c.comment.empty()) continue;
        Json events = Json::array();
        for (const auto& event : c.history) {
            events.push_back({{"decision", static_cast<int>(event.decision)},
                              {"priority", static_cast<int>(event.priority)}, {"comment", utf8(event.comment)},
                              {"utc", event.utc}});
        }
        root["reviews"][c.id] = {{"decision", static_cast<int>(c.decision)},
                                  {"priority", static_cast<int>(c.priority)}, {"comment", utf8(c.comment)},
                                  {"history", std::move(events)}};
    }
    auto bytes = root.dump(2);
    if (bytes.size() > 50 * 1024 * 1024) throw Error("Файл проверки больше 50 МБ.");
    atomic_file(path, bytes);
}

void load_review(Result& result, const fs::path& path) {
    struct Saved {
        Decision decision;
        Priority priority;
        Text comment;
        std::vector<ReviewEvent> history;
    };
    std::map<std::string, std::size_t> ids;
    for (std::size_t i = 0; i < result.changes.size(); ++i) ids.emplace(result.changes[i].id, i);
    std::map<std::size_t, Saved> staged;
    try {
        const auto root = Json::parse(read_file(path, 50 * 1024 * 1024));
        if (!root.is_object()) throw Error("Файл проверки должен содержать объект JSON.");
        if (root.at("schema") == 1) throw Error("Этот файл создан в v0.1.0. Откройте его в том выпуске.");
        if (root.at("schema") != 2 || root.at("algorithm") != 1) throw Error("Неподдерживаемая версия проверки.");
        if (root.at("mode") != mode_key(result.mode)) throw Error("Файл проверки создан в другом режиме сравнения.");
        if (root.at("left").at("sha256") != result.left.hash || root.at("right").at("sha256") != result.right.hash)
            throw Error("Файл проверки относится к другой паре документов или документы изменены.");
        const auto& reviews = root.at("reviews");
        if (!reviews.is_object()) throw Error("Раздел решений должен содержать объект JSON.");
        for (auto item = reviews.begin(); item != reviews.end(); ++item) {
            const auto index = ids.find(item.key());
            if (index == ids.end()) throw Error("В файле есть решение для неизвестного различия.");
            const auto& record = item.value();
            const auto& events = record.at("history");
            if (!events.is_array()) throw Error("История решения должна содержать список.");
            Saved saved{static_cast<Decision>(enum_value(record, "decision", 3)),
                        static_cast<Priority>(enum_value(record, "priority", 2)),
                        review_comment(record.at("comment")), {}};
            for (const auto& event : events) {
                auto timestamp = event.at("utc").get<std::string>();
                if (timestamp.size() != 20 || timestamp[10] != 'T' || timestamp.back() != 'Z')
                    throw Error("Некорректная дата в истории решения.");
                saved.history.push_back({static_cast<Decision>(enum_value(event, "decision", 3)),
                                         static_cast<Priority>(enum_value(event, "priority", 2)),
                                         review_comment(event.at("comment")), std::move(timestamp)});
            }
            staged.emplace(index->second, std::move(saved));
        }
    } catch (const Json::exception&) {
        throw Error("Не удалось прочитать файл проверки. Проверьте формат JSON.");
    }
    for (auto& entry : staged) {
        auto& c = result.changes[entry.first];
        auto& saved = entry.second;
        c.decision = saved.decision;
        c.priority = saved.priority;
        c.comment = std::move(saved.comment);
        c.history = std::move(saved.history);
    }
}

void export_report(const Result& result, const fs::path& path) {
    std::ostringstream out;
    out << "<!doctype html><html lang=\"ru\"><meta charset=\"utf-8\"><title>Сверка HTML</title>"
        << "<style>body{font:16px system-ui,sans-serif;max-width:1200px;margin:2rem auto;padding:0 1rem}"
        << "table{border-collapse:collapse;width:100%;table-layout:fixed}th,td{border:1px solid #ccc;padding:.65em;"
        << "text-align:left;vertical-align:top;white-space:pre-wrap;overflow-wrap:anywhere}"
        << "small{color:#555}section{margin-top:1.5em}.old{background:#fff0f0}.new{background:#edf8ed}</style>"
        << "<h1>Отчёт сверки HTML</h1><p>" << escaped(result.left.name) << " / " << escaped(result.right.name)
        << "</p><p>Режим: " << (result.mode == Mode::strict ? "строгий" : "по структуре") << "</p>"
        << "<p><small>SHA-256 слева: " << result.left.hash << "<br>SHA-256 справа: " << result.right.hash << "</small></p>";
    for (const auto& warning : result.warnings) out << "<p>" << escaped(warning) << "</p>";
    out << "<p>Различий: " << result.changes.size() << "</p>";
    for (const auto& c : result.changes) {
        out << "<section><strong>" << escaped(category_names(c.mask)) << " · " << escaped(label(c.priority))
            << "</strong><p>" << escaped(c.context) << " " << escaped(c.reason) << "</p>"
            << "<table><thead><tr><th>Было</th><th>Стало</th></tr></thead><tbody><tr><td class=\"old\">"
            << escaped(c.before) << "</td><td class=\"new\">" << escaped(c.after) << "</td></tr></tbody></table>"
            << "<p>" << escaped(label(c.decision)) << " " << escaped(c.comment) << "</p>";
        if (!c.history.empty()) {
            out << "<details><summary>История решений</summary><ul>";
            for (const auto& event : c.history)
                out << "<li>" << escaped(unicode(event.utc)) << " · " << escaped(label(event.decision)) << " · "
                    << escaped(label(event.priority)) << " " << escaped(event.comment) << "</li>";
            out << "</ul></details>";
        }
        out << "</section>";
    }
    out << "</html>";
    atomic_file(path, out.str());
}
} // namespace hc
