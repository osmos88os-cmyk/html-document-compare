#include "core.hpp"

#include <algorithm>
#include <limits>
#include <map>
#include <numeric>
#include <set>
#include <sstream>

namespace hc {
namespace {
// Myers edit script. Store only reached diagonals; bound memory for very different inputs.
template<class Equal>
std::vector<Edit> edits(int left, int right, Equal equal, bool* coarse = nullptr) {
    if (coarse) *coarse = false;
    int prefix = 0;
    while (prefix < left && prefix < right && equal(prefix, prefix)) ++prefix;
    int n = left - prefix, m = right - prefix;
    while (n > 0 && m > 0 && equal(prefix + n - 1, prefix + m - 1)) { --n; --m; }
    auto span = [prefix](int a, int b) {
        return Span{static_cast<std::size_t>(prefix + a), static_cast<std::size_t>(prefix + b), 0};
    };
    if (!n && !m) return {};
    if (!n || !m) return {{span(0, n), span(0, m)}};
    constexpr int budget = 1400;
    const int last = std::min(n + m, budget);
    std::vector<std::vector<int>> trace;
    trace.reserve(static_cast<std::size_t>(last) + 1);
    for (int d = 0; d <= last; ++d) {
        std::vector<int> line(static_cast<std::size_t>(d) + 1);
        for (int k = -d; k <= d; k += 2) {
            const int slot = (k + d) / 2;
            int x = 0;
            if (d) {
                const auto& previous = trace.back();
                if (k == -d || (k != d && previous[slot - 1] < previous[slot]))
                    x = previous[slot];
                else x = previous[slot - 1] + 1;
            }
            int y = x - k;
            while (x < n && y < m && equal(prefix + x, prefix + y)) { ++x; ++y; }
            line[slot] = x;
            if (x < n || y < m) continue;
            std::vector<Edit> reverse;
            x = n; y = m;
            for (int step = d; step > 0; --step) {
                const auto& previous = trace[step - 1];
                const int diagonal = x - y;
                const int index = (diagonal + step) / 2;
                const bool inserted = diagonal == -step ||
                    (diagonal != step && previous[index - 1] < previous[index]);
                const int px = previous[inserted ? index : index - 1];
                const int py = px - (diagonal + (inserted ? 1 : -1));
                reverse.push_back({span(px, px + (inserted ? 0 : 1)), span(py, py + (inserted ? 1 : 0))});
                x = px; y = py;
            }
            std::vector<Edit> result;
            for (auto i = reverse.rbegin(); i != reverse.rend(); ++i) {
                if (!result.empty() && result.back().left.end == i->left.start &&
                    result.back().right.end == i->right.start) {
                    result.back().left.end = i->left.end;
                    result.back().right.end = i->right.end;
                } else result.push_back(*i);
            }
            return result;
        }
        trace.push_back(std::move(line));
    }
    if (coarse) *coarse = true;
    return {{span(0, n), span(0, m)}};
}

Mask character_category(char32_t c) {
    if (digit(c)) return numbers;
    if (letter(c)) return words;
    if (whitespace(c) || c == 0xad) return format;
    return punctuation;
}

std::vector<Span> tokens(View text) {
    std::vector<Span> result;
    for (std::size_t start = 0; start < text.size();) {
        Mask mask = character_category(text[start]);
        auto end = start + 1;
        if (mask == words || mask == format || mask == numbers) {
            while (end < text.size()) {
                const auto c = text[end];
                if (character_category(c) == mask) ++end;
                else if (mask == numbers && View(U".,/-: \u00a0\u202f").find(c) != View::npos &&
                         end + 1 < text.size() && digit(text[end + 1])) end += 2;
                else break;
            }
        }
        result.push_back({start, end, mask});
        start = end;
    }
    return result;
}

Text piece(View text, Span span) { return Text(text.substr(span.start, span.end - span.start)); }
Mask classify(View text) {
    Mask mask = 0;
    for (const auto& token : tokens(text)) mask |= token.mask;
    return mask;
}

Text enumeration(View text) {
    auto value = canonical(text);
    std::size_t end = 0;
    while (end < value.size() && (digit(value[end]) || value[end] == U'^' || value[end] == U'.')) ++end;
    if (end > 0 && (value[end - 1] == U'.' || (end < value.size() && value[end] == U')')))
        return value.substr(0, end + (end < value.size() && value[end] == U')' ? 1 : 0));
    if (value.size() > 1 && letter(value[0]) && value[1] == U')') return value.substr(0, 2);
    return {};
}

struct Anchor {
    Text exact;
    Text scope;
    Text number;
    std::vector<Text> vocabulary;
    Kind kind;
};

std::vector<Anchor> anchors(const std::vector<Block>& blocks, bool strict) {
    std::vector<Anchor> result;
    for (const auto& b : blocks) {
        Anchor a{strict ? b.text : canonical(b.text, b.kind == Kind::heading || b.kind == Kind::signature),
                 canonical(b.section, true), enumeration(b.text), {}, b.kind};
        const auto flat = lower(compact(b.text));
        for (const auto& token : tokens(flat)) {
            if (token.mask & (words | numbers)) a.vocabulary.push_back(piece(flat, token));
            if (a.vocabulary.size() >= 96) break;
        }
        std::sort(a.vocabulary.begin(), a.vocabulary.end());
        a.vocabulary.erase(std::unique(a.vocabulary.begin(), a.vocabulary.end()), a.vocabulary.end());
        result.push_back(std::move(a));
    }
    return result;
}

int similarity(const Anchor& a, const Anchor& b) {
    if (a.kind != b.kind) return 0;
    if (a.exact == b.exact) return 100;
    if (!a.number.empty() && !b.number.empty() && a.number != b.number) return 0;
    if (!a.scope.empty() && !b.scope.empty() && a.scope != b.scope &&
        !a.number.empty() && a.kind != Kind::heading) return 0;
    int common = 0;
    auto i = a.vocabulary.begin(), j = b.vocabulary.begin();
    while (i != a.vocabulary.end() && j != b.vocabulary.end()) {
        if (*i == *j) { ++common; ++i; ++j; }
        else if (*i < *j) ++i;
        else ++j;
    }
    const auto total = a.vocabulary.size() + b.vocabulary.size();
    int score = total ? static_cast<int>(200 * common / total) : 0;
    if (!a.number.empty() && a.number == b.number) score = std::max(score, 62);
    if (a.kind == Kind::table) score = std::max(score, 45);
    return score;
}

void align_gap(const std::vector<Anchor>& a, const std::vector<Anchor>& b, const Edit& gap,
               std::vector<Pair>& out) {
    const auto n = gap.left.end - gap.left.start, m = gap.right.end - gap.right.start;
    const auto left = static_cast<int>(gap.left.start), right = static_cast<int>(gap.right.start);
    if (n == 0 || m == 0 || n * m > 250000) {
        for (std::size_t i = 0; i < n; ++i) out.push_back({left + static_cast<int>(i), -1, m != 0});
        for (std::size_t j = 0; j < m; ++j) out.push_back({-1, right + static_cast<int>(j), n != 0});
        return;
    }
    constexpr int min_score = 45;
    struct Node { int cost = 0; char step = 0; };
    std::vector<Node> dp((n + 1) * (m + 1));
    auto at = [&](std::size_t i, std::size_t j) -> Node& { return dp[i * (m + 1) + j]; };
    std::vector<int> scores(n * m);
    for (std::size_t i = 1; i <= n; ++i) at(i, 0) = {static_cast<int>(i) * 100, 'd'};
    for (std::size_t j = 1; j <= m; ++j) at(0, j) = {static_cast<int>(j) * 100, 'i'};
    for (std::size_t i = 1; i <= n; ++i) for (std::size_t j = 1; j <= m; ++j) {
        int s = similarity(a[gap.left.start + i - 1], b[gap.right.start + j - 1]);
        scores[(i - 1) * m + j - 1] = s;
        Node best{at(i - 1, j).cost + 100, 'd'};
        if (at(i, j - 1).cost + 100 < best.cost) best = {at(i, j - 1).cost + 100, 'i'};
        if (s >= min_score && at(i - 1, j - 1).cost + 180 - s <= best.cost)
            best = {at(i - 1, j - 1).cost + 180 - s, 'p'};
        at(i, j) = best;
    }
    std::vector<Pair> reverse;
    auto i = n, j = m;
    while (i || j) {
        const char step = at(i, j).step;
        if (step == 'p') {
            --i; --j;
            const int s = scores[i * m + j];
            bool uncertain = s < 56;
            for (std::size_t k = 0; k < m && !uncertain; ++k)
                uncertain = k != j && scores[i * m + k] >= std::max(min_score, s - 4);
            for (std::size_t k = 0; k < n && !uncertain; ++k)
                uncertain = k != i && scores[k * m + j] >= std::max(min_score, s - 4);
            reverse.push_back({left + static_cast<int>(i), right + static_cast<int>(j), uncertain});
        } else if (step == 'd') reverse.push_back({left + static_cast<int>(--i), -1, false});
        else reverse.push_back({-1, right + static_cast<int>(--j), false});
    }
    out.insert(out.end(), reverse.rbegin(), reverse.rend());
}

std::vector<Pair> align(const std::vector<Block>& left, const std::vector<Block>& right, bool strict = false) {
    const auto a = anchors(left, strict), b = anchors(right, strict);
    auto equal = [&](int i, int j) {
        if (a[i].kind != b[j].kind || a[i].exact != b[j].exact) return false;
        return a[i].number.empty() || a[i].scope.empty() || b[j].scope.empty() || a[i].scope == b[j].scope;
    };
    auto gaps = edits(static_cast<int>(a.size()), static_cast<int>(b.size()), equal);
    std::vector<Pair> result;
    std::size_t l = 0, r = 0;
    for (const auto& gap : gaps) {
        while (l < gap.left.start) result.push_back({static_cast<int>(l++), static_cast<int>(r++), false});
        align_gap(a, b, gap, result);
        l = gap.left.end; r = gap.right.end;
    }
    while (l < a.size()) result.push_back({static_cast<int>(l++), static_cast<int>(r++), false});
    return result;
}

std::vector<Block> strict_blocks(View text, const std::vector<Script>& scripts) {
    std::vector<Block> blocks;
    std::size_t first_script = 0;
    for (std::size_t start = 0; start < text.size();) {
        const auto nl = text.find(U'\n', start);
        auto end = nl == View::npos ? text.size() : nl + 1;
        // Bound single native text layouts without losing any code point.
        end = std::min(end, start + 20000);
        Block b;
        b.text = Text(text.substr(start, end - start));
        b.strict_offset = start;
        while (first_script < scripts.size() && scripts[first_script].span.end <= start) ++first_script;
        for (std::size_t i = first_script; i < scripts.size() && scripts[i].span.start < end; ++i) {
            auto s = scripts[i];
            if (s.span.end <= start) continue;
            s.span = {std::max(s.span.start, start) - start, std::min(s.span.end, end) - start, format};
            b.scripts.push_back(s);
        }
        blocks.push_back(std::move(b));
        start = end;
    }
    if (blocks.size() > 100000) throw Error("Строгий режим: больше 100 000 строк.");
    return blocks;
}

Text joined_notes(const Block& block) {
    Text text;
    for (const auto& n : block.annotations) {
        if (!text.empty()) text += U'\n';
        text += n.text;
    }
    return text;
}
bool source_anchor(View target) {
    const auto normalized = lower(compact(target));
    return normalized.size() >= 4 && normalized.substr(0, 4) == U"#par";
}

std::vector<std::pair<Text, Text>> comparison_links(const Block& block) {
    std::vector<std::pair<Text, Text>> result;
    for (const auto& link : block.destinations)
        if (!source_anchor(link.second)) result.push_back(link);
    return result;
}

Text joined_links(const std::vector<std::pair<Text, Text>>& links) {
    Text text;
    for (const auto& link : links) {
        if (!text.empty()) text += U'\n';
        text += link.first + U" → " + link.second;
    }
    return text;
}

bool critical_word(View value) {
    auto text = lower(value);
    for (auto t : tokens(text)) {
        const auto word = piece(text, t);
        if (word == U"не" || word == U"ни" || word == U"нет" || word == U"запрещается" ||
            word == U"разрешается" || word == U"обязан" || word == U"вправе") return true;
    }
    return false;
}

Priority priority(Mask mask, View before, View after) {
    if ((mask & numbers) || critical_word(before) || critical_word(after)) return Priority::high;
    if (mask & (words | punctuation | tables)) return Priority::medium;
    return Priority::low;
}

Text join_refs(const std::vector<Text>& refs) {
    Text text;
    for (const auto& ref : refs) {
        if (!text.empty()) text += U", ";
        text += ref;
    }
    return text;
}
Text number_text(int n) { return unicode(std::to_string(n)); }

class Comparison {
public:
    explicit Comparison(Result& result) : result_(result) {}

    void run(bool metadata_only = false) {
        std::size_t left_offset = 0, right_offset = 0;
        for (pair_ = 0; pair_ < result_.pairs.size(); ++pair_) {
            const auto& pair = result_.pairs[pair_];
            const auto* a = pair.left >= 0 ? &result_.left.blocks[pair.left] : nullptr;
            const auto* b = pair.right >= 0 ? &result_.right.blocks[pair.right] : nullptr;
            if (result_.mode == Mode::strict) {
                const View left = a ? View(a->text) : View{};
                const View right = b ? View(b->text) : View{};
                bool coarse = false;
                const auto operations = character_edits(left, right, &coarse);
                for (auto e : operations) {
                    text_change(left, right, e, 0, 0, a && b ? 0 : structure, {});
                    e.left.start += left_offset; e.left.end += left_offset;
                    e.right.start += right_offset; e.right.end += right_offset;
                    result_.strict_operations.push_back(e);
                }
                if (a && b && a->scripts != b->scripts) {
                    std::vector<Span> lm, rm;
                    for (const auto& s : a->scripts) lm.push_back(s.span);
                    for (const auto& s : b->scripts) rm.push_back(s.span);
                    add(format, Priority::low, Text(left), Text(right), {},
                        U"Изменились надстрочные или подстрочные индексы.", std::move(lm), std::move(rm));
                }
                if (coarse) warn(U"Большой заменённый фрагмент показан целиком.");
                left_offset += left.size(); right_offset += right.size();
                continue;
            }
            if (pair.ambiguous && !metadata_only) {
                warn(U"Есть неоднозначные пары блоков. Проверьте их вручную.");
                add(structure, Priority::high, a ? a->text : Text{}, b ? b->text : Text{},
                    U"Неоднозначное сопоставление", U"Нужно проверить соответствие блоков.");
            }
            if (!a || !b) {
                if (metadata_only) continue;
                const auto before = a ? display_text(*a) : Text{};
                const auto after = b ? display_text(*b) : Text{};
                add(structure, Priority::high, before, after, a ? a->section : b->section,
                    U"Блок присутствует только с одной стороны.",
                    {{0, before.size(), structure}}, {{0, after.size(), structure}});
                continue;
            }
            if (a->kind == Kind::table && b->kind == Kind::table) compare_table(*a, *b, metadata_only);
            else if (!metadata_only) compare_text(a->text, b->text, 0, 0, 0, a->section,
                              a->kind == Kind::heading || a->kind == Kind::signature);
            compare_notes(*a, *b);
            const auto left_links = comparison_links(*a);
            const auto right_links = comparison_links(*b);
            if (left_links != right_links)
                add(links, Priority::low, joined_links(left_links), joined_links(right_links),
                    a->section, U"Различаются ссылки.");
        }
    }
private:
    Result& result_;
    std::size_t pair_ = 0;

    void warn(const Text& warning) {
        if (std::find(result_.warnings.begin(), result_.warnings.end(), warning) == result_.warnings.end())
            result_.warnings.push_back(warning);
    }
    void add(Mask mask, Priority p, Text before, Text after, Text context, Text reason,
             std::vector<Span> left = {}, std::vector<Span> right = {}) {
        const auto& pair = result_.pairs[pair_];
        std::ostringstream key;
        key << static_cast<int>(result_.mode) << ':' << pair.left << ':' << pair.right
            << ':' << mask << ':' << result_.changes.size() << ':' << utf8(context);
        for (auto s : left) key << ":L" << s.start << '-' << s.end;
        for (auto s : right) key << ":R" << s.start << '-' << s.end;
        Change c;
        c.id = sha256(key.str());
        c.pair = pair_;
        c.mask = mask;
        c.priority = p;
        c.original_priority = p;
        c.before = std::move(before);
        c.after = std::move(after);
        c.context = std::move(context);
        c.reason = std::move(reason);
        c.left_marks = std::move(left);
        c.right_marks = std::move(right);
        result_.changes.push_back(std::move(c));
    }

    void text_change(View a, View b, Edit e, std::size_t lo, std::size_t ro, Mask extra, const Text& context) {
        auto before = piece(a, e.left), after = piece(b, e.right);
        const auto mask = extra | classify(before) | classify(after);
        std::vector<Span> lm, rm;
        for (auto s : tokens(before))
            lm.push_back({lo + e.left.start + s.start, lo + e.left.start + s.end, s.mask | extra});
        for (auto s : tokens(after))
            rm.push_back({ro + e.right.start + s.start, ro + e.right.start + s.end, s.mask | extra});
        const auto level = extra & structure ? Priority::high : priority(mask, before, after);
        add(mask, level, std::move(before), std::move(after), context, U"Изменился текст.", std::move(lm), std::move(rm));
    }

    void compare_text(View a, View b, std::size_t lo, std::size_t ro, Mask extra,
                      const Text& context, bool heading = false) {
        if (a == b) return;
        // Act titles and document headings are routinely exported with different
        // capitalization by ТЕХН and К+. Treat case alone as equivalent there.
        if (heading && lower(compact(a)) == lower(compact(b))) return;
        if (canonical(a, heading) == canonical(b, heading)) {
            for (auto e : character_edits(a, b)) {
                add(format | extra, Priority::low, piece(a, e.left), piece(b, e.right), context, U"Различается оформление.",
                    {{lo + e.left.start, lo + e.left.end, format | extra}},
                    {{ro + e.right.start, ro + e.right.end, format | extra}});
            }
            return;
        }
        const auto x = tokens(a), y = tokens(b);
        auto original = [](Span s, const std::vector<Span>& spans, std::size_t length) {
            const auto first = s.start == spans.size() ? length : spans[s.start].start;
            return Span{first, s.empty() ? first : spans[s.end - 1].end, 0};
        };
        bool coarse = false;
        for (const auto& e : edits(static_cast<int>(x.size()), static_cast<int>(y.size()),
            [&](int i, int j) {
                return a.substr(x[i].start, x[i].end - x[i].start) == b.substr(y[j].start, y[j].end - y[j].start);
            }, &coarse)) {
            text_change(a, b, {original(e.left, x, a.size()), original(e.right, y, b.size())}, lo, ro, extra, context);
        }
        if (coarse) warn(U"Большой заменённый фрагмент показан целиком.");
    }

    void compare_notes(const Block& a, const Block& b) {
        auto x = joined_notes(a), y = joined_notes(b);
        const auto ar = act_references(x), br = act_references(y);
        if (canonical(x) != canonical(y)) {
            std::vector<Text> added, removed;
            std::set_difference(br.begin(), br.end(), ar.begin(), ar.end(), std::back_inserter(added));
            std::set_difference(ar.begin(), ar.end(), br.begin(), br.end(), std::back_inserter(removed));
            Text detail;
            if (!added.empty()) detail += U"Добавлено: " + join_refs(added) + U". ";
            if (!removed.empty()) detail += U"Удалено: " + join_refs(removed) + U". ";
            if (detail.empty()) detail = U"Изменился текст редакционной отметки.";
            auto level = ar != br || repealed(x) || repealed(y) ? Priority::high : Priority::medium;
            const auto left_tokens = tokens(x), right_tokens = tokens(y);
            const auto token_edits = edits(static_cast<int>(left_tokens.size()), static_cast<int>(right_tokens.size()),
                [&](int i, int j) {
                    return piece(x, left_tokens[i]) == piece(y, right_tokens[j]);
                });
            const auto map_note_span = [](const Block& block, Span range) {
                std::vector<Span> mapped;
                const auto displayed = annotation_spans(block);
                std::size_t joined_start = 0;
                for (std::size_t i = 0; i < block.annotations.size(); ++i) {
                    const auto joined_end = joined_start + block.annotations[i].text.size();
                    const auto start = std::max(range.start, joined_start);
                    const auto end = std::min(range.end, joined_end);
                    if (start < end)
                        mapped.push_back({displayed[i].start + start - joined_start,
                                          displayed[i].start + end - joined_start, notes});
                    joined_start = joined_end + (i + 1 < block.annotations.size() ? 1 : 0);
                }
                return mapped;
            };
            const auto character_range = [](View text, const std::vector<Span>& spans, Span range) {
                const auto start = range.start == spans.size() ? text.size() : spans[range.start].start;
                const auto end = range.start == range.end ? start : spans[range.end - 1].end;
                return Span{start, end, notes};
            };
            std::vector<Span> left_marks, right_marks;
            for (const auto& edit : token_edits) {
                auto lm = map_note_span(a, character_range(x, left_tokens, edit.left));
                auto rm = map_note_span(b, character_range(y, right_tokens, edit.right));
                left_marks.insert(left_marks.end(), lm.begin(), lm.end());
                right_marks.insert(right_marks.end(), rm.begin(), rm.end());
            }
            add(notes, level, x, y, a.section.empty() ? a.text.substr(0, 140) : a.section,
                std::move(detail), std::move(left_marks), std::move(right_marks));
        }
        std::vector<bool> ap, bp;
        for (const auto& n : a.annotations) ap.push_back(n.after);
        for (const auto& n : b.annotations) bp.push_back(n.after);
        if (!x.empty() && !y.empty() && ap != bp) {
            auto lm = annotation_spans(a), rm = annotation_spans(b);
            for (auto& s : lm) s.mask = note_position;
            for (auto& s : rm) s.mask = note_position;
            add(note_position, Priority::low, x, y, a.section, U"Изменилось расположение отметки.",
                std::move(lm), std::move(rm));
        }
    }

    void compare_table(const Block& a, const Block& b, bool metadata_only) {
        auto rows = [](const Block& block) {
            std::vector<Block> result;
            for (const auto& text : block.row_text) {
                Block row;
                row.kind = Kind::table;
                row.text = text;
                result.push_back(std::move(row));
            }
            return result;
        };
        std::map<int, std::map<int, const Cell*>> ac, bc;
        for (const auto& c : a.cells) ac[c.row][c.column] = &c;
        for (const auto& c : b.cells) bc[c.row][c.column] = &c;
        for (const auto& row : align(rows(a), rows(b))) {
            const auto location = U"Строка слева " + (row.left < 0 ? U"—" : number_text(row.left + 1)) +
                                  U", справа " + (row.right < 0 ? U"—" : number_text(row.right + 1));
            if (row.left < 0 || row.right < 0) {
                add(tables, Priority::high, row.left < 0 ? Text{} : a.row_text[row.left],
                    row.right < 0 ? Text{} : b.row_text[row.right], location, U"Добавлена или удалена строка таблицы.",
                    row.left < 0 ? std::vector<Span>{} : std::vector<Span>{a.row_display[row.left]},
                    row.right < 0 ? std::vector<Span>{} : std::vector<Span>{b.row_display[row.right]});
                continue;
            }
            if (row.ambiguous) warn(U"Есть неоднозначные строки таблицы. Проверьте их вручную.");
            std::set<int> columns;
            for (const auto& c : ac[row.left]) columns.insert(c.first);
            for (const auto& c : bc[row.right]) columns.insert(c.first);
            for (int column : columns) {
                const auto* l = ac[row.left][column];
                const auto* r = bc[row.right][column];
                const auto position = location + U", столбец " + number_text(column + 1);
                if (!l || !r) add(tables, Priority::high, l ? l->text : Text{}, r ? r->text : Text{},
                    position, U"Добавлена или удалена ячейка.", l ? std::vector<Span>{l->display} : std::vector<Span>{},
                    r ? std::vector<Span>{r->display} : std::vector<Span>{});
                else {
                    if (!metadata_only) compare_text(l->text, r->text, l->display.start, r->display.start, tables, position);
                    if (l->rowspan != r->rowspan || l->colspan != r->colspan || l->header != r->header)
                        add(tables, Priority::medium, number_text(l->rowspan) + U" × " + number_text(l->colspan),
                            number_text(r->rowspan) + U" × " + number_text(r->colspan), position,
                            U"Изменилась структура ячейки.", {l->display}, {r->display});
                }
            }
        }
    }
};
} // namespace

std::vector<Edit> character_edits(View left, View right, bool* coarse) {
    return edits(static_cast<int>(left.size()), static_cast<int>(right.size()),
                 [&](int a, int b) { return left[a] == right[b]; }, coarse);
}

Text apply_edits(View left, View right, const std::vector<Edit>& edits) {
    Text result;
    std::size_t l = 0, r = 0;
    for (const auto& e : edits) {
        if (e.left.start < l || e.right.start < r || e.left.end < e.left.start || e.right.end < e.right.start ||
            e.left.end > left.size() || e.right.end > right.size() ||
            left.substr(l, e.left.start - l) != right.substr(r, e.right.start - r))
            throw Error("Нарушена последовательность операций сравнения.");
        result += left.substr(l, e.left.start - l);
        result += right.substr(e.right.start, e.right.end - e.right.start);
        l = e.left.end; r = e.right.end;
    }
    if (left.substr(l) != right.substr(r)) throw Error("Операции сравнения не покрывают изменения.");
    result += left.substr(l);
    return result;
}

Result compare(Document left, Document right, Mode mode) {
    Result result;
    result.left = std::move(left); result.right = std::move(right); result.mode = mode;
    auto& a = result.left; auto& b = result.right;
    result.warnings = a.warnings;
    result.warnings.insert(result.warnings.end(), b.warnings.begin(), b.warnings.end());
    if (!a.act_number.empty() && !b.act_number.empty() && (a.act_date != b.act_date || a.act_number != b.act_number))
        result.warnings.push_back(U"Реквизиты актов различаются. Проверьте выбранные документы.");
    if (a.revision != b.revision) result.warnings.push_back(U"Различаются даты редакций или состояния документа.");
    if (a.amendments != b.amendments) result.warnings.push_back(U"Различаются списки изменяющих актов.");
    std::vector<std::pair<Change, std::pair<bool, std::size_t>>> metadata;
    if (mode == Mode::strict) {
        result.mode = Mode::structured;
        result.pairs = align(a.blocks, b.blocks);
        Comparison(result).run(true);
        for (auto& change : result.changes) {
            const auto& pair = result.pairs[change.pair];
            const bool right = pair.right >= 0;
            const auto offset = right ? b.blocks[pair.right].strict_offset : a.blocks[pair.left].strict_offset;
            metadata.emplace_back(std::move(change), std::make_pair(right, offset));
        }
        result.changes.clear();
        a.blocks = strict_blocks(a.strict_text, a.strict_scripts);
        b.blocks = strict_blocks(b.strict_text, b.strict_scripts);
        result.mode = Mode::strict;
    }
    result.pairs = align(a.blocks, b.blocks, mode == Mode::strict);
    Comparison(result).run();
    if (mode == Mode::strict) {
        if (apply_edits(a.strict_text, b.strict_text, result.strict_operations) != b.strict_text)
            throw Error("Не удалось проверить полноту строгой сверки.");
        std::vector<std::size_t> lp(a.blocks.size()), rp(b.blocks.size());
        for (std::size_t i = 0; i < result.pairs.size(); ++i) {
            auto pair = result.pairs[i];
            if (pair.left >= 0) lp[pair.left] = i;
            if (pair.right >= 0) rp[pair.right] = i;
        }
        for (auto& item : metadata) {
            auto& c = item.first;
            const bool right = item.second.first;
            const auto offset = item.second.second;
            const auto& blocks = right ? b.blocks : a.blocks;
            const auto anchor = std::upper_bound(blocks.begin(), blocks.end(), offset,
                [](std::size_t value, const Block& block) { return value < block.strict_offset; });
            if (anchor == blocks.begin()) continue;
            c.pair = (right ? rp : lp)[anchor - blocks.begin() - 1];
            c.id = sha256("strict-metadata:" + c.id);
            c.left_marks.clear();
            c.right_marks.clear();
            result.changes.push_back(std::move(c));
        }
        std::stable_sort(result.changes.begin(), result.changes.end(), [](const Change& l, const Change& r) {
            return l.pair < r.pair;
        });
    }
    return result;
}
} // namespace hc
