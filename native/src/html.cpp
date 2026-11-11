#include "core.hpp"

#include <algorithm>
#include <memory>
#include <regex>
#include <set>
#include <libxml/HTMLparser.h>
#include <libxml/tree.h>

namespace hc {
namespace {
bool starts(View text, View prefix) { return text.substr(0, prefix.size()) == prefix; }
bool has(View text, View part) { return text.find(part) != View::npos; }
std::string_view tag(const xmlNode* n) {
    return n && n->type == XML_ELEMENT_NODE
        ? std::string_view(reinterpret_cast<const char*>(n->name)) : std::string_view{};
}
Text attribute(const xmlNode* n, const char* key) {
    auto value = xmlGetProp(n, reinterpret_cast<const xmlChar*>(key));
    if (!value) return {};
    std::unique_ptr<xmlChar, decltype(xmlFree)> owned(value, xmlFree);
    return unicode(reinterpret_cast<const char*>(value));
}
Text node_text(const xmlNode* n) {
    return n->content ? unicode(reinterpret_cast<const char*>(n->content)) : Text{};
}
bool class_is(const xmlNode* n, View value) {
    return has(U" " + compact(attribute(n, "class")) + U" ", U" " + Text(value) + U" ");
}
bool mark(const xmlNode* n) {
    const auto value = U" " + compact(attribute(n, "class"));
    return has(value, U" mark");
}
bool skip(std::string_view name) {
    return name == "script" || name == "style" || name == "head" || name == "noscript" ||
           name == "svg" || name == "template" || name == "input";
}
bool container(std::string_view name) {
    return name == "body" || name == "div" || name == "p" || name == "section" ||
           name == "article" || name == "pre" || name == "ul" || name == "ol" ||
           name == "li" || name == "table" || name == "tr" || name == "td" ||
           name == "th" || (name.size() == 2 && name[0] == 'h' && name[1] >= '1' && name[1] <= '6');
}

const xmlNode* find(const xmlNode* node, std::string_view name) {
    if (!node) return nullptr;
    if (tag(node) == name) return node;
    for (auto child = node->children; child; child = child->next)
        if (const auto* found = find(child, name)) return found;
    return nullptr;
}
bool source_class(const xmlNode* n, View value) {
    if (!n || n->type != XML_ELEMENT_NODE || skip(tag(n))) return false;
    if (class_is(n, value)) return true;
    for (auto child = n->children; child; child = child->next)
        if (source_class(child, value)) return true;
    return false;
}

void strict_content(const xmlNode* n, Text& out) {
    if (n->type == XML_TEXT_NODE || n->type == XML_CDATA_SECTION_NODE) {
        out += node_text(n);
        return;
    }
    const auto name = tag(n);
    if (name.empty() || skip(name)) return;
    if (name == "br") {
        out += U'\n';
        return;
    }
    for (auto child = n->children; child; child = child->next) strict_content(child, out);
    if (container(name) && name != "body") out += U'\n';
}

struct Parts {
    Text body;
    Text editorial;
    std::vector<std::pair<Text, Text>> links;
};

void flatten(const xmlNode* n, Parts& out, bool marked = false) {
    if (n->type == XML_TEXT_NODE || n->type == XML_CDATA_SECTION_NODE) {
        (marked ? out.editorial : out.body) += node_text(n);
        return;
    }
    auto name = tag(n);
    if (name.empty() || skip(name) || name == "label") return;
    const auto style = lower(compact(attribute(n, "style")));
    marked = marked || mark(n) || has(style, U"background: #f4f3f8") ||
        has(style, U"background:#f4f3f8") || has(style, U"background-color: #f4f3f8") ||
        has(style, U"background-color:#f4f3f8");
    auto& text = marked ? out.editorial : out.body;
    if (name == "br") {
        text += U' ';
        return;
    }
    if (name == "sup" || class_is(n, U"W9")) text += U'^';
    if (name == "sub" || class_is(n, U"W8")) text += U'_';
    auto begin = text.size();
    for (auto child = n->children; child; child = child->next) flatten(child, out, marked);
    if (!marked) {
        auto address = attribute(n, "href");
        auto command = attribute(n, "cmdprm");
        if (!command.empty()) address = U"команда:" + command;
        if (!address.empty() && (name == "a" || name == "span")) {
            out.links.emplace_back(compact(View(text).substr(begin)), std::move(address));
        }
    }
    if (name == "p" || name == "div" || name == "li" || name == "td") text += U' ';
}

std::vector<const xmlNode*> table_rows(const xmlNode* n) {
    std::vector<const xmlNode*> rows;
    for (auto child = n->children; child; child = child->next) {
        const auto name = tag(child);
        if (name == "tr") rows.push_back(child);
        else if (name == "thead" || name == "tbody" || name == "tfoot") {
            auto group = table_rows(child);
            rows.insert(rows.end(), group.begin(), group.end());
        }
    }
    return rows;
}

int extent(const xmlNode* n, const char* name) {
    const auto value = attribute(n, name);
    if (value.empty()) return 1;
    int count = 0;
    for (auto c : value) {
        if (c < U'0' || c > U'9') return 1;
        count = count * 10 + static_cast<int>(c - U'0');
        if (count > 1000) throw Error("Таблица содержит слишком большое объединение ячеек.");
    }
    return std::max(1, count);
}

void table(Block& block, const xmlNode* node, Document& document) {
    const auto rows = table_rows(node);
    std::set<std::pair<int, int>> occupied;
    block.row_text.reserve(rows.size());
    block.text.clear();
    for (std::size_t i = 0; i < rows.size(); ++i) {
        int row = static_cast<int>(i), column = 0;
        const auto start = block.text.size();
        for (auto td = rows[i]->children; td; td = td->next) {
            if (tag(td) != "td" && tag(td) != "th") continue;
            const auto h = extent(td, "rowspan"), w = extent(td, "colspan");
            while (occupied.count({row, column})) ++column;
            if (occupied.size() + static_cast<std::size_t>(h) * w > 100000)
                throw Error("Таблица содержит больше 100 000 позиций ячеек.");
            bool overlap = false;
            for (int r = row; r < row + h; ++r)
                for (int c = column; c < column + w; ++c)
                    overlap = !occupied.emplace(r, c).second || overlap;
            if (overlap) document.warnings.push_back(U"Перекрывающиеся объединения ячеек требуют ручной проверки.");
            if (find(td, "table")) document.warnings.push_back(U"Вложенная таблица требует ручной проверки.");
            Parts cell;
            flatten(td, cell);
            auto text = compact(cell.body);
            if (!compact(cell.editorial).empty()) text += U" " + compact(cell.editorial);
            if (block.text.size() > start) block.text += U" | ";
            const auto offset = block.text.size();
            block.text += text;
            block.cells.push_back({std::move(text), row, column, h, w, tag(td) == "th",
                                   {offset, block.text.size(), tables}});
            column += w;
        }
        block.row_text.push_back(block.text.substr(start));
        block.row_display.push_back({start, block.text.size(), tables});
        if (i + 1 != rows.size()) block.text += U'\n';
    }
}

bool block_child(const xmlNode* n) {
    for (auto c = n->children; c; c = c->next) {
        if (container(tag(c))) return true;
        if (c->type == XML_ELEMENT_NODE && !skip(tag(c)) && block_child(c)) return true;
    }
    return false;
}

bool note_start(View value) {
    auto text = lower(value);
    if (!text.empty() && text.front() == U'(') text = compact(View(text).substr(1));
    for (View prefix : {U"в ред.", U"в редакции", U"дополнен", U"дополнение", U"введен", U"введён",
                        U"пункт введ", U"подпункт введ", U"абзац введ", U"примечание", U"примечания", U"см. текст"})
        if (starts(text, prefix)) return true;
    static const std::regex short_note(R"(^пп?\.\s*\S+\s+(введен|в ред\.))");
    return std::regex_search(utf8(text), short_note);
}

bool federal_note(View text) {
    // Reference exports also put editorial history into unmarked standalone lines.
    static const std::regex pattern(
        R"(^(?:\(|[;,]\s*)?(?:(?:часть|п\.)\s+\d+(?:\.\d+)*\s+)?(?:в ред\.\s+федеральн(?:ого|ых).*?\s+закон(?:а|ов)|введен[а]?\s+федеральным.*?\s+законом)\s)");
    return std::regex_search(utf8(lower(text)), pattern);
}

bool heading(const xmlNode* node, View text) {
    const auto name = tag(node);
    if (name.size() == 2 && name[0] == 'h' && name[1] >= '1' && name[1] <= '6') return true;
    if (class_is(node, U"T") || class_is(node, U"Z") || class_is(node, U"s2B")) return true;
    static const std::regex roman(R"(^[IVXLCDM]+\.\s)");
    return std::regex_search(utf8(text), roman);
}

void append_refs(Document& document, View text) {
    const auto references = act_references(text);
    document.amendments.insert(document.amendments.end(), references.begin(), references.end());
}

class Collector {
public:
    explicit Collector(Document& doc) : doc_(doc) {}
    void collect(const xmlNode* n) {
        const auto name = tag(n);
        if (skip(name)) return;
        if (n->type == XML_TEXT_NODE) {
            Parts loose;
            loose.body = node_text(n);
            add(n, std::move(loose));
        } else if (n->type == XML_ELEMENT_NODE) {
            if (name != "table" && block_child(n)) {
                for (auto child = n->children; child; child = child->next) collect(child);
            } else {
                Parts p;
                flatten(n, p);
                add(n, std::move(p));
            }
        }
    }
private:
    Document& doc_;
    Text section_;
    void add(const xmlNode* n, Parts p) {
        auto text = compact(p.body);
        auto note = compact(p.editorial);
        const auto name = tag(n);
        const auto raw = compact(p.body + U" " + p.editorial);
        if (text.empty() && note.empty() && name != "table") return;
        const bool provider = doc_.source != U"HTML";
        if (provider && starts(raw, U"Список изменяющих документов")) {
            append_refs(doc_, raw);
            return;
        }
        if (provider) {
            for (View prefix : {U"Документ предоставлен КонсультантПлюс", U"Дата сохранения:",
                                U"Недействующая редакция", U"По состоянию на", U"См. последующие изменения"})
                if (starts(raw, prefix)) return;
            if (has(attribute(n, "class"), U"ihint")) return;
            if (doc_.source == U"ГПУ" && doc_.blocks.empty() && starts(raw, U"Указ Президента Российской Федерации от"))
                return;
        }
        static const std::regex status_prefix(R"(^(?:(\d+(?:\^\d+|\(\d+\))?[.)]?)\s*)?(?:абзац[;.]?)?\s*$)");
        std::smatch prefix;
        const auto body_bytes = utf8(lower(text));
        if (repealed(note) && std::regex_match(body_bytes, prefix, status_prefix))
            text = (prefix[1].matched ? unicode(prefix[1].str()) : U"абзац") + U" [Утратил силу]";
        if (doc_.source == U"К+" && repealed(text)) {
            static const std::regex start_repeal(R"(^(?:(\d+(?:\(\d+\))?[.)]?)\s*|абзац\s+)?утратил)");
            if (std::regex_search(body_bytes, prefix, start_repeal)) {
                note = text;
                text = (prefix[1].matched ? unicode(prefix[1].str()) : U"абзац") + U" [Утратил силу]";
            }
        }
        if (text.empty() && !note.empty() && !doc_.blocks.empty() && name != "table") {
            doc_.blocks.back().annotations.push_back({std::move(note), true});
            return;
        }
        if (doc_.source == U"К+" && !doc_.blocks.empty() && (note_start(raw) || federal_note(raw)) && name != "table") {
            doc_.blocks.back().annotations.push_back({raw, true});
            return;
        }
        Block b;
        b.text = std::move(text);
        b.source_line = xmlGetLineNo(n);
        b.destinations = std::move(p.links);
        if (!note.empty()) b.annotations.push_back({std::move(note), false});
        if (name == "table" && !table_rows(n).empty()) {
            static const std::regex date_layout(R"(^\d{1,2}\s+[^ ]+\s+\d{4}\s+года(?:\s+(?:N|№).*)?$)");
            if (table_rows(n).size() == 1 && std::regex_match(utf8(raw), date_layout)) return;
            b.kind = Kind::table;
            table(b, n, doc_);
        } else if (class_is(n,U"aR") || class_is(n,U"Y") || class_is(n,U"S")) b.kind = Kind::signature;
        else if (heading(n, b.text)) {
            b.kind = Kind::heading;
            section_ = b.text;
        }
        b.section = section_;
        if (!doc_.blocks.empty() && (b.kind == Kind::heading || b.kind == Kind::signature) &&
            doc_.blocks.back().kind == b.kind) {
            auto& previous = doc_.blocks.back();
            previous.text += U" " + b.text;
            previous.annotations.insert(previous.annotations.end(), b.annotations.begin(), b.annotations.end());
            previous.destinations.insert(previous.destinations.end(), b.destinations.begin(), b.destinations.end());
            if (b.kind == Kind::heading) section_ = previous.section = previous.text;
        } else doc_.blocks.push_back(std::move(b));
        if (doc_.blocks.size() > 6000) throw Error("В документе больше 6000 блоков.");
    }
};
} // namespace

std::vector<Text> act_references(View text) {
    static const std::regex ref(R"((\d{2}\.\d{2}\.\d{4})\s*(?:N|№)\s*(\d+(?:(?:-|–)[^ ,;.)]+)?))", std::regex::icase);
    const auto bytes = utf8(text);
    std::vector<Text> refs;
    for (auto i = std::sregex_iterator(bytes.begin(), bytes.end(), ref); i != std::sregex_iterator(); ++i)
        refs.push_back(unicode((*i)[1].str() + " № " + (*i)[2].str()));
    std::sort(refs.begin(), refs.end());
    refs.erase(std::unique(refs.begin(), refs.end()), refs.end());
    return refs;
}
bool repealed(View text) {
    const auto normalized = lower(compact(text));
    for (View form : {U"утратил силу", U"утратила силу", U"утратили силу", U"утратило силу"})
        if (has(normalized, form)) return true;
    return false;
}

Document parse(std::string_view raw, Text name) {
    if (raw.size() > 50 * 1024 * 1024) throw Error("HTML-файл больше 50 МБ.");
    auto decoded = decode_html(raw);
    if (decoded.find(U'\0') != Text::npos || compact(decoded).empty())
        throw Error("Пустой HTML-файл или недопустимый нулевой символ.");
    const auto bytes = utf8(decoded);
    const int flags = HTML_PARSE_NONET | HTML_PARSE_NOERROR | HTML_PARSE_NOWARNING | HTML_PARSE_IGNORE_ENC;
    std::unique_ptr<xmlDoc, decltype(&xmlFreeDoc)> tree(
        htmlReadMemory(bytes.data(), static_cast<int>(bytes.size()), nullptr, "UTF-8", flags), xmlFreeDoc);
    if (!tree) throw Error("Не удалось разобрать HTML.");
    const auto* root = xmlDocGetRootElement(tree.get());
    if (!root) throw Error("HTML не содержит документа.");
    const auto* body = find(root, "body");
    if (!body) body = root;
    Document doc;
    doc.name = std::move(name);
    doc.hash = sha256(raw);
    strict_content(body, doc.strict_text);
    const auto full = compact(doc.strict_text);
    doc.source = U"HTML";
    if (has(full, U"Документ предоставлен КонсультантПлюс") ||
        (source_class(body, U"s2B") && (has(lower(full), U"консультантплюс") ||
         has(full, U"Название документа"))))
        doc.source = U"К+";
    else if (source_class(body, U"fragments")) doc.source = U"Комплекс / ТЕХН";
    if (has(View(full).substr(0, 600), U"Недействующая редакция")) doc.source = U"ГПУ";
    static const std::regex filename(R"((?:УП|Указ.*?)\s+(\d+)\s+от\s+(\d{2}\.\d{2}\.\d{4}))");
    static const std::regex revision(R"((?:По состоянию на|ред\.\s*от|\sна)\s*(\d{2}\.\d{2}\.\d{4}))");
    std::smatch match;
    const auto filename_bytes = utf8(doc.name);
    const auto prelude = utf8(View(full).substr(0, 1800)) + " " + filename_bytes;
    if (std::regex_search(filename_bytes, match, filename)) {
        doc.act_number = unicode(match[1].str());
        doc.act_date = unicode(match[2].str());
    } else if (doc.source == U"К+" || doc.source == U"ГПУ") {
        static const std::regex act(R"((\d{2}\.\d{2}\.\d{4})\s*(?:N|№)\s*(\d+))");
        if (std::regex_search(prelude, match, act)) {
            doc.act_date = unicode(match[1].str());
            doc.act_number = unicode(match[2].str());
        }
    }
    if (std::regex_search(prelude, match, revision)) doc.revision = unicode(match[1].str());
    Collector(doc).collect(body);
    if (doc.blocks.empty() && full.empty()) throw Error("В HTML не найден текст документа.");
    if (doc.blocks.empty()) doc.warnings.push_back(U"Текст доступен в строгом режиме.");
    if (doc.amendments.empty()) {
        for (const auto& b : doc.blocks) {
            if (b.kind != Kind::heading && b.annotations.empty()) break;
            for (const auto& n : b.annotations)
                if (note_start(n.text)) append_refs(doc, n.text);
        }
    }
    std::sort(doc.amendments.begin(), doc.amendments.end());
    doc.amendments.erase(std::unique(doc.amendments.begin(), doc.amendments.end()), doc.amendments.end());
    return doc;
}

Document parse_file(const fs::path& path) {
    return parse(read_file(path, 50 * 1024 * 1024), unicode(path.filename().u8string()));
}

Text display_text(const Block& block) {
    Text result = block.text;
    for (const auto& note : block.annotations) {
        if (!result.empty()) result += U'\n';
        result += note.text;
    }
    return result;
}
std::vector<Span> annotation_spans(const Block& block) {
    std::vector<Span> spans;
    auto pos = block.text.size();
    for (const auto& n : block.annotations) {
        if (pos) ++pos;
        spans.push_back({pos, pos + n.text.size(), notes});
        pos += n.text.size();
    }
    return spans;
}
} // namespace hc
