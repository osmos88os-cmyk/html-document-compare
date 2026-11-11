#pragma once
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace hc {
using Text = std::u32string;
using View = std::u32string_view;
namespace fs = std::filesystem;
using Error = std::runtime_error;

std::string utf8(View text);
Text unicode(std::string_view text);
std::wstring wide(View text);
Text from_wide(std::wstring_view text);
Text decode_html(std::string_view bytes);
std::string sha256(std::string_view bytes);
std::string read_file(const fs::path& path, std::uintmax_t limit);
bool whitespace(char32_t ch);
bool letter(char32_t ch);
bool digit(char32_t ch);
Text lower(View text);
Text compact(View text);
Text canonical(View text, bool heading = false);

enum Category : std::uint32_t {
    words = 1 << 0, numbers = 1 << 1, punctuation = 1 << 2,
    notes = 1 << 3, note_position = 1 << 4, links = 1 << 5,
    tables = 1 << 6, structure = 1 << 7, format = 1 << 8,
    all = (1 << 9) - 1
};
using Mask = std::uint32_t;
const std::vector<std::pair<Category, Text>>& categories();
Text category_names(Mask mask);
enum class Mode { structured, strict };
enum class Kind { paragraph, heading, signature, table };
enum class Priority { high, medium, low };
enum class Decision { pending, confirmed, acceptable, needs_review };
Text label(Priority priority);
Text label(Decision decision);

struct Span {
    std::size_t start = 0;
    std::size_t end = 0;
    Mask mask = 0;
    bool empty() const { return start == end; }
};
struct Edit { Span left; Span right; };
struct Script {
    Span span;
    bool raised;
    bool operator==(const Script& other) const {
        return span.start == other.span.start && span.end == other.span.end && raised == other.raised;
    }
};
struct Note { Text text; bool after = false; };
struct Cell {
    Text text;
    int row = 0;
    int column = 0;
    int rowspan = 1;
    int colspan = 1;
    bool header = false;
    Span display;
};
struct Block {
    Kind kind = Kind::paragraph;
    Text text;
    Text section;
    std::vector<Note> annotations;
    std::vector<std::pair<Text, Text>> destinations;
    std::vector<Cell> cells;
    std::vector<Text> row_text;
    std::vector<Span> row_display;
    long source_line = 0;
    std::size_t strict_offset = 0;
    std::vector<Script> scripts;
};
struct Document {
    Text name;
    std::string hash;
    Text source;
    Text act_date;
    Text act_number;
    Text revision;
    std::vector<Text> amendments;
    std::vector<Text> warnings;
    std::vector<Block> blocks;
    Text strict_text;
    std::vector<Script> strict_scripts;
};

Document parse(std::string_view raw, Text name = U"Документ.html");
Document parse_file(const fs::path& path);
std::vector<Text> act_references(View text);
bool repealed(View text);
Text display_text(const Block& block);
std::vector<Span> annotation_spans(const Block& block);

struct Pair { int left = -1; int right = -1; bool ambiguous = false; };
struct ReviewEvent {
    Decision decision = Decision::pending;
    Priority priority = Priority::low;
    Text comment;
    std::string utc;
};
struct Change {
    std::string id;
    std::size_t pair = 0;
    Mask mask = 0;
    Priority priority = Priority::low;
    Priority original_priority = Priority::low;
    Decision decision = Decision::pending;
    Text before;
    Text after;
    Text context;
    Text reason;
    std::vector<Span> left_marks;
    std::vector<Span> right_marks;
    Text comment;
    std::vector<ReviewEvent> history;
};
struct Result {
    Document left;
    Document right;
    Mode mode = Mode::structured;
    std::vector<Pair> pairs;
    std::vector<Change> changes;
    std::vector<Text> warnings;
    std::vector<Edit> strict_operations;
};

std::vector<Edit> character_edits(View left, View right, bool* coarse = nullptr);
Text apply_edits(View left, View right, const std::vector<Edit>& edits);
Result compare(Document left, Document right, Mode mode);
void review(Change& change, Decision decision, Priority priority, Text comment);
void save_review(const Result& result, const fs::path& path);
void load_review(Result& result, const fs::path& path);
void export_report(const Result& result, const fs::path& path);
} // namespace hc
