#include "core.hpp"

#include <chrono>
#include <fstream>
#include <functional>
#include <iostream>
#include <random>
#include <set>

using namespace hc;

namespace {
void check(bool passed, const char* message) {
    if (!passed) throw Error(message);
}

Document doc(View body, Text name = U"УП 110 от 26.01.2012.html") {
    return parse(utf8(U"<html><head><meta charset='utf-8'></head><body>" + Text(body) + U"</body></html>"),
                 std::move(name));
}
Result diff(View left, View right, Mode mode = Mode::structured) { return compare(doc(left), doc(right), mode); }
bool has(const Result& result, Mask mask) {
    for (const auto& c : result.changes) if ((c.mask & mask) == mask) return true;
    return false;
}

struct TempDirectory {
    fs::path path = fs::temp_directory_path() / ("htmlcompare-test-" +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    TempDirectory() { fs::create_directory(path); }
    ~TempDirectory() { std::error_code ignored; fs::remove_all(path, ignored); }
};

void edits_round_trip() {
    std::mt19937 generator(7843);
    auto random_text = [&] {
        Text text;
        for (unsigned i = generator() % 41; i > 0; --i) text += U"abcя1 🙂!"[generator() % 8];
        return text;
    };
    for (int i = 0; i < 2000; ++i) {
        auto a = random_text(), b = random_text();
        auto edits = character_edits(a, b);
        check(apply_edits(a, b, edits) == b, "Myers reconstruction");
        std::size_t cost = 0;
        for (auto e : edits) cost += e.left.end - e.left.start + e.right.end - e.right.start;
        std::vector<std::size_t> lcs(b.size() + 1);
        for (char32_t ch : a) {
            std::size_t previous = 0;
            for (std::size_t j = 1; j <= b.size(); ++j) {
                const auto saved = lcs[j];
                lcs[j] = ch == b[j-1] ? previous + 1 : std::max(lcs[j], lcs[j-1]);
                previous = saved;
            }
        }
        check(cost == a.size() + b.size() - 2 * lcs.back(), "minimal edit script");
    }
    bool coarse = false;
    Text a(3000, U'а'), b(3000, U'б');
    const auto bounded = character_edits(a, b, &coarse);
    check(coarse && apply_edits(a, b, bounded) == b, "bounded fallback reconstruction");
}

void ordinary_xhtml() {
    const auto result = parse(u8"<html xmlns='http://www.w3.org/1999/xhtml'><body>"
                              u8"<p>Изменённая вводная часть.</p><p>В целях проверки</p></body></html>");
    check(result.source == U"HTML", "xmlns is not a provider signature");
    check(result.blocks.size() == 2 && result.blocks.front().text == U"Изменённая вводная часть.",
          "XHTML introductory paragraph survives");
    check(parse(u8"<html xmlns='http://www.w3.org/1999/xhtml'><p>Документ.</p></html>").blocks.size() == 1,
          "ordinary XHTML without markers is supported");
}

void strict_text() {
    auto result = diff(U"<p>А  Б: 10&nbsp;рублей.</p><p>Обычный 1 и <sup>¹</sup></p>",
                       U"<p>А Б: 11 рублей!</p><p>Обычный 1 и <sup>²</sup></p><p>Вставка</p>", Mode::strict);
    check(has(result, numbers) && has(result, format) && has(result, punctuation), "strict categories");
    check(apply_edits(result.left.strict_text, result.right.strict_text, result.strict_operations) ==
          result.right.strict_text, "strict result reconstructs right text");
    check(result.left.strict_text == U"А  Б: 10\u00a0рублей.\nОбычный 1 и ¹\n", "strict extraction");
    auto single = diff(U"<p>А  Б.</p>", U"<p>А Б.</p>", Mode::strict);
    check(single.changes.size() == 1 && single.changes.front().before == U" " &&
          single.changes.front().after.empty(), "one extra space is localized");
    auto index = diff(U"<p>Число 10<sup>2</sup>.</p>", U"<p>Число 102.</p>", Mode::strict);
    check(index.strict_operations.empty() && has(index, format), "strict mode notices superscript markup");
    auto target = diff(U"<p><a href='/first'>Документ</a></p>", U"<p><a href='/second'>Документ</a></p>", Mode::strict);
    check(target.strict_operations.empty() && has(target, links), "strict mode compares link destinations");
    auto merged = diff(U"<table><tr><td colspan='2'>Заголовок</td></tr></table>",
                       U"<table><tr><td>Заголовок</td></tr></table>", Mode::strict);
    check(merged.strict_operations.empty() && has(merged, tables), "strict mode compares table geometry");
}

void categories_and_ranges() {
    auto r = diff(U"<p>1. Срок пять дней.</p>", U"<p>1. Срок 10 дней!</p>");
    check(has(r, words | numbers), "word-to-number is visible in both categories");
    check(has(r, punctuation), "punctuation change is separate");
    for (const auto& c : r.changes) {
        for (auto mark : c.right_marks) {
            if (mark.mask & words) check(r.right.blocks[0].text.substr(mark.start, mark.end - mark.start) != U"10",
                                          "word filter doesn't highlight number");
        }
    }
    auto digit_result = diff(U"<p>1. Выплата 650 рублей.</p>", U"<p>1. Выплата 850 рублей.</p>");
    check(digit_result.changes.size() == 1 && digit_result.changes[0].before == U"650" &&
          digit_result.changes[0].priority == Priority::high, "number replacement");
    auto denial = diff(U"<p>Передача разрешается.</p>", U"<p>Передача не разрешается.</p>");
    check(!denial.changes.empty() && denial.changes.front().priority == Priority::high, "negation priority");
    auto mixed = diff(U"<p>Заявление подаётся.</p>", U"<p>Заявление подаётся с 23.10.2026 года.</p>");
    check(has(mixed, words | numbers), "date inside an inserted phrase");
}

void heading_case_is_ignored() {
    const auto same_title = diff(U"<h1>УКАЗ ПРЕЗИДЕНТА РОССИЙСКОЙ ФЕДЕРАЦИИ</h1><p>Текст.</p>",
                                 U"<h1>Указ Президента Российской Федерации</h1><p>Текст.</p>");
    check(same_title.changes.empty(), "heading capitalization is ignored");
    const auto body_case = diff(U"<p>Текст правила.</p>", U"<p>текст правила.</p>");
    check(!body_case.changes.empty() && has(body_case, words), "body capitalization remains visible");
    const auto title_typo = diff(U"<h1>Указ Президента</h1>", U"<h1>Указ Призидента</h1>");
    check(!title_typo.changes.empty() && has(title_typo, words), "actual title typo remains visible");
}

void aligned_insertion() {
    auto r = diff(U"<p>1. Первый общий пункт.</p><p>2. Второй общий пункт.</p>",
                  U"<p>1. Первый общий пункт.</p><p>1.1. Длинная вставка с дополнительным текстом.</p><p>2. Второй общий пункт.</p>");
    check(r.pairs.size() == 3 && r.pairs[1].left == -1 && r.pairs[2].left == 1 && r.pairs[2].right == 2,
          "following paragraph stays aligned");
    check(r.changes.size() == 1 && r.changes[0].mask == structure, "insertion doesn't cascade");
}

void reference_rules() {
    const auto kp = doc(U"<div>Документ предоставлен КонсультантПлюс</div><div>Оглавление</div><div class='s2B'>УКАЗ</div>"
                        U"<p>6. Выплатить.</p><p>часть 6.1 в ред. Федерального закона от 01.07.2014 N 483</p>");
    const auto tech = doc(U"<div class='fragments'><div class='T'>УКАЗ</div><p>6. Выплатить."
                          U"<span class='mark'>(В редакции </span><span class='mark'>Указа от 01.07.2014 № 483)</span></p></div>");
    check(kp.source == U"К+" && tech.source == U"Комплекс / ТЕХН", "provider signatures");
    check(kp.blocks.size() == 2 && kp.blocks.back().annotations.size() == 1 &&
          tech.blocks.back().annotations.size() == 1, "reference notes extracted");
    check(kp.strict_text.find(U"Оглавление") != Text::npos, "strict text keeps provider preamble");
    auto r = compare(tech, kp, Mode::structured);
    check(has(r, notes) && has(r, note_position) && !has(r, words), "note movement and text remain separate");
    const auto phrasing = diff(
        U"<p>Пункт.<span class='mark'>(В редакции Указа Президента от 01.07.2014 № 483)</span></p>",
        U"<p>Пункт.<span class='mark'>(в ред. Указа Президента от 01.07.2014 N 483)</span></p>");
    const auto note = std::find_if(phrasing.changes.begin(), phrasing.changes.end(),
        [](const Change& change) { return change.mask & notes; });
    check(note != phrasing.changes.end() && note->before.size() > 20 &&
          !note->left_marks.empty() && note->left_marks.front().end - note->left_marks.front().start < note->before.size(),
          "editorial phrasing remains visible but highlights only changed words");
    auto coloured = doc(U"<p>Правило.<span style='background: #f4f3f8'>Отметка</span></p>");
    check(coloured.blocks[0].text == U"Правило." && coloured.blocks[0].annotations[0].text == U"Отметка",
          "reference shaded note");
    auto repealed = diff(U"<p>абзац; <span class='mark'>(Утратил силу)</span></p>",
                         U"<p><span class='mark'>абзац; (Утратил силу)</span></p>");
    check(repealed.left.blocks[0].text == repealed.right.blocks[0].text, "repeal status stays in body");
    auto formatting = diff(U"<p>27<span class='W9'>1</span>) Текст.</p>", U"<p>27(1)) Текст.</p>");
    check(!has(formatting, words) && !has(formatting, numbers) && has(formatting, format), "equivalent numbering");
    auto refs = diff(U"<p>Пункт. <span class='mark'>(от 01.07.2014 N 483)</span></p>",
                     U"<p>Пункт. <span class='mark'>(от 01.07.2014 N 483, от 23.10.2026 № 775)</span></p>");
    check(refs.changes[0].reason.find(U"Добавлено: 23.10.2026 № 775") != Text::npos, "new reference shown");
}

Result changed_cells() {
    return diff(U"<table><tr><td>10</td><td>10</td></tr><tr><td>Итого</td><td>20</td></tr></table>",
                U"<table><tr><td>11</td><td>11</td></tr><tr><td>Итого</td><td>20</td></tr></table>");
}

void tables_and_independent_decisions() {
    auto result = changed_cells();
    std::set<std::string> unique;
    for (const auto& c : result.changes) {
        check(c.mask & tables, "table change classification");
        check(unique.insert(c.id).second, "separate table cell IDs");
    }
    check(result.changes.size() == 2, "two cell changes");
    check(!result.changes[0].left_marks.empty() && !result.changes[1].left_marks.empty() &&
          result.changes[0].context != result.changes[1].context &&
          result.changes[0].left_marks[0].start != result.changes[1].left_marks[0].start, "cell offsets");
    auto merged = diff(U"<table><tr><td colspan='2'>Название</td></tr><tr><td>А</td><td>Б</td></tr></table>",
                       U"<table><tr><td>Название</td></tr><tr><td>А</td><td>Б</td></tr></table>");
    check(has(merged, tables), "colspan change detected");
    auto rowspan = doc(U"<table><tr><td rowspan='2'>А</td><td>10</td></tr><tr><td>20</td></tr></table>");
    check(rowspan.blocks[0].cells[2].row == 1 && rowspan.blocks[0].cells[2].column == 1, "rowspan geometry");
    auto inserted = diff(U"<table><tr><td>Первый</td></tr><tr><td>Последний</td></tr></table>",
                         U"<table><tr><td>Первый</td></tr><tr><td>Новая строка</td></tr><tr><td>Последний</td></tr></table>");
    check(inserted.changes.size() == 1 && inserted.changes[0].after == U"Новая строка", "inserted row");

    TempDirectory temp;
    auto file = temp.path / "review.json";
    review(result.changes[0], Decision::confirmed, Priority::high, U"<проверено>");
    review(result.changes[1], Decision::acceptable, Priority::low, U"Отдельное решение");
    save_review(result, file);
    auto reopened = changed_cells();
    load_review(reopened, file);
    check(reopened.changes[0].decision == Decision::confirmed &&
          reopened.changes[1].decision == Decision::acceptable &&
          reopened.changes[0].history[0].comment == U"<проверено>", "JSON preserves distinct decisions and history");
    auto empty = temp.path / "empty.json";
    save_review(changed_cells(), empty);
    auto reset = reopened;
    load_review(reset, empty);
    check(reset.changes[0].decision == Decision::pending && reset.changes[0].history.empty() &&
          reset.changes[1].priority == reset.changes[1].original_priority, "a saved review replaces prior decisions");
    auto report = temp.path / "report.html";
    export_report(reopened, report);
    check(read_file(report, 100000).find(u8"&lt;проверено&gt;") != std::string::npos, "report escapes comments");

    auto malformed = read_file(file, 100000);
    auto pos = malformed.rfind("\"decision\": 2");
    if (pos != std::string::npos) malformed.replace(pos, 13, "\"decision\": 9");
    std::ofstream(file) << malformed;
    auto untouched = changed_cells();
    bool rejected = false;
    try { load_review(untouched, file); }
    catch (const Error&) { rejected = true; }
    check(rejected && untouched.changes[0].decision == Decision::pending, "invalid record is atomic");
    std::ofstream(file) << "[]";
    rejected = false;
    try { load_review(untouched, file); }
    catch (const Error&) { rejected = true; }
    check(rejected, "JSON array reports validation error");
}

void encoding_and_links() {
    check(sha256("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", "SHA-256");
    check(unicode(utf8(U"Русский 🙂")) == U"Русский 🙂" && from_wide(wide(U"Русский 🙂")) == U"Русский 🙂", "Unicode conversion");
    auto value = parse("<meta charset='windows-1251'><p>\xcf\xf0\xe8\xe2\xe5\xf2</p>");
    check(value.blocks[0].text == U"Привет", "Windows-1251");
    auto links = diff(U"<p><a href='http://example.org/a'>Документ</a></p>",
                      U"<p><a href='https://example.org/a'>Документ</a></p><script>ignored()</script>");
    check(links.changes.size() == 1 && links.changes.front().mask == hc::links, "link protocol change");
    auto anchors = diff(U"<p>См. пункт 1.</p>",
                        U"<p><a href='#Par18'>См. пункт 1.</a></p>");
    check(!has(anchors, hc::links), "provider-specific internal paragraph links are ignored");
    auto linked_text = diff(U"<p><a href='#Par18'>См. пункт 1.</a></p>",
                            U"<p><a href='#Par39'>См. пункт 1.</a></p>");
    check(!has(linked_text, hc::links), "different internal paragraph anchors are ignored");
    check(links.right.strict_text.find(U"ignored") == Text::npos, "scripts excluded from strict text");
}
} // namespace

int sample_pairs(const fs::path& tech, const fs::path& gpu, const fs::path& kplus) {
    std::size_t pairs = 0;
    bool passed = true;
    for (const auto& entry : fs::directory_iterator(gpu)) {
        if (!entry.is_regular_file() || entry.path().extension() != ".html") continue;
        const auto left = parse_file(tech / entry.path().filename());
        std::vector<std::pair<std::string, fs::path>> versions{{"GPU", entry.path()}};
        for (const auto& item : fs::directory_iterator(kplus)) {
            if (item.is_regular_file() && item.path().extension() == ".html" &&
                item.path().filename().u8string().find(" N " + utf8(left.act_number) + " ") != std::string::npos)
                versions.emplace_back("K+", item.path());
        }
        if (versions.size() != 2) throw Error("Expected one matching K+ file per act.");
        for (const auto& version : versions) {
            const auto right = parse_file(version.second);
            const auto result = compare(left, right, Mode::structured);
            std::size_t content = 0;
            for (const auto& c : result.changes) if (c.mask & (words | numbers | structure | punctuation | tables)) {
                if (++content <= 16)
                    std::cout << "  " << utf8(category_names(c.mask)) << ": "
                              << utf8(c.before.substr(0, 90)) << " → " << utf8(c.after.substr(0, 90)) << '\n';
            }
            const auto strict = compare(left, right, Mode::strict);
            std::cout << utf8(left.act_number) << ' ' << version.first << " · "
                      << left.blocks.size() << '/' << right.blocks.size() << " blocks · "
                      << content << " content differences · " << strict.strict_operations.size()
                      << " verified strict operations\n";
            passed = passed && content == 0;
            ++pairs;
        }
    }
    return passed && pairs == 6 ? 0 : 1;
}

int main(int argc, char** argv) {
    if (argc == 5 && std::string_view(argv[1]) == "--samples") {
        try { return sample_pairs(fs::u8path(argv[2]), fs::u8path(argv[3]), fs::u8path(argv[4])); }
        catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
    }
    const std::vector<std::pair<const char*, std::function<void()>>> cases = {
        {"character edit scripts", edits_round_trip}, {"XHTML", ordinary_xhtml}, {"strict text", strict_text},
        {"categories and ranges", categories_and_ranges}, {"alignment", aligned_insertion},
        {"heading capitalization", heading_case_is_ignored}, {"reference rules", reference_rules},
        {"tables and review persistence", tables_and_independent_decisions},
        {"encoding and links", encoding_and_links},
    };
    int failures = 0;
    for (const auto& test : cases) {
        try { test.second(); std::cout << "PASS " << test.first << '\n'; }
        catch (const std::exception& e) {
            ++failures;
            std::cerr << "FAIL " << test.first << ": " << e.what() << '\n';
        }
    }
    return failures ? 1 : 0;
}
