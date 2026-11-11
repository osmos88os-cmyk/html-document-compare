#include "sheet.hpp"
#include "../resources/resource.h"

#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>
#include <windowsx.h>

#include <algorithm>
#include <array>
#include <cwchar>
#include <memory>
#include <thread>

namespace hc {
namespace {
constexpr bool legacy = HTMLCOMPARE_LEGACY != 0;
constexpr UINT comparison_done = WM_APP + 1;
enum Command {
    open_left = 200, open_right, start_compare, settings, binoculars, previous, next, exit_fullscreen,
    save, load, report, change_list, decision, priority, apply, search, status_filter, next_pending,
    show_history, priority_filter
};

void set(HWND handle, View text) { SetWindowTextW(handle, wide(text).c_str()); }
Text value(HWND handle) {
    std::wstring text(static_cast<std::size_t>(GetWindowTextLengthW(handle)) + 1, L'\0');
    text.resize(static_cast<std::size_t>(GetWindowTextW(handle, text.data(), static_cast<int>(text.size()))));
    return from_wide(text);
}
Text count(std::size_t n) { return unicode(std::to_string(n)); }

struct Preferences {
    Mask categories = all & ~(format | note_position);
    Mode mode = Mode::structured;
};

INT_PTR CALLBACK options(HWND dialog, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_INITDIALOG) {
        const auto* state = reinterpret_cast<const Preferences*>(lp);
        SetWindowLongPtrW(dialog, DWLP_USER, lp);
        CheckRadioButton(dialog, ID_STRUCTURED, ID_STRICT,
            state->mode == Mode::strict ? ID_STRICT : ID_STRUCTURED);
        for (unsigned i = 0; i < categories().size(); ++i)
            CheckDlgButton(dialog, ID_CATEGORY + i, state->categories & categories()[i].first ? BST_CHECKED : BST_UNCHECKED);
        return TRUE;
    }
    if (msg == WM_COMMAND && LOWORD(wp) == IDOK) {
        auto* state = reinterpret_cast<Preferences*>(GetWindowLongPtrW(dialog, DWLP_USER));
        state->mode = IsDlgButtonChecked(dialog, ID_STRICT) == BST_CHECKED ? Mode::strict : Mode::structured;
        state->categories = 0;
        for (unsigned i = 0; i < categories().size(); ++i)
            if (IsDlgButtonChecked(dialog, ID_CATEGORY + i) == BST_CHECKED) state->categories |= categories()[i].first;
        EndDialog(dialog, IDOK);
        return TRUE;
    }
    if ((msg == WM_COMMAND && LOWORD(wp) == IDCANCEL) || msg == WM_CLOSE) {
        EndDialog(dialog, IDCANCEL);
        return TRUE;
    }
    return FALSE;
}

fs::path file_dialog(HWND owner, bool writing, const wchar_t* filter, const wchar_t* extension,
                     const wchar_t* suggested = L"") {
    std::array<wchar_t, 32768> name{};
    const auto size = std::min(std::wcslen(suggested), name.size() - 1);
    std::copy_n(suggested, size, name.data());
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = owner;
    ofn.lpstrFilter = filter;
    ofn.lpstrFile = name.data();
    ofn.nMaxFile = static_cast<DWORD>(name.size());
    ofn.lpstrDefExt = extension;
    ofn.Flags = OFN_EXPLORER | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR |
                (writing ? OFN_OVERWRITEPROMPT : OFN_FILEMUSTEXIST);
    const BOOL accepted = writing ? GetSaveFileNameW(&ofn) : GetOpenFileNameW(&ofn);
    return accepted ? fs::path(name.data()) : fs::path{};
}

struct Completion {
    std::unique_ptr<Result> result;
    Text error;
};

void protect_source(const fs::path& output, const fs::path& left, const fs::path& right) {
    for (const auto& source : {left, right}) {
        std::error_code error;
        if (!source.empty() && fs::equivalent(output, source, error))
            throw Error("Выберите другой файл. Исходный HTML нельзя перезаписать.");
    }
}

class App {
public:
    explicit App(HINSTANCE instance) : instance_(instance) {}
    ~App() {
        if (worker_.joinable()) worker_.join();
        if (font_) DeleteObject(font_);
    }
    HWND hwnd() const { return window_; }
    bool create();
    bool smoke(const fs::path& left, const fs::path& right);

private:
    HINSTANCE instance_;
    HWND window_ = nullptr;
    HFONT font_ = nullptr;
    HWND left_open_, right_open_, left_label_, right_label_, start_, settings_, binoculars_, prev_, next_;
    HWND save_, load_, report_, summary_, list_, detail_, decision_, priority_, apply_;
    HWND search_ = nullptr, status_filter_ = nullptr, priority_filter_ = nullptr, pending_ = nullptr;
    HWND comment_ = nullptr, history_ = nullptr, tooltip_ = nullptr;
    std::vector<HWND> work_controls_;
    std::unique_ptr<Sheet> sheet_;
    std::unique_ptr<Result> result_;
    std::thread worker_;
    fs::path left_, right_;
    Preferences prefs_;
    std::vector<std::size_t> visible_;
    std::size_t selected_ = Sheet::none;
    bool dirty_ = false, busy_ = false, fullscreen_ = false, changing_ = false, history_visible_ = false;
    WINDOWPLACEMENT placement_{sizeof(WINDOWPLACEMENT)};
    LONG_PTR window_style_ = 0;
    int dpi_ = 96;

    static LRESULT CALLBACK procedure(HWND, UINT, WPARAM, LPARAM);
    LRESULT message(UINT, WPARAM, LPARAM);
    HWND widget(const wchar_t* type, const wchar_t* title, DWORD style, int id = 0, DWORD ex = 0);
    HWND button(const wchar_t* label, int id, DWORD style = BS_PUSHBUTTON);
    void initialize();
    void layout();
    void command(int id, int notification);
    void status();
    void enable();
    void choose(bool left);
    void compare_now();
    void complete(std::unique_ptr<Completion> payload);
    void display(std::unique_ptr<Result> result);
    void filter();
    void select(std::size_t id, bool reveal);
    void details(bool reset_comment);
    void navigate(int direction, bool pending_only = false);
    void apply_review();
    bool confirm_discard() const;
    void toggle_fullscreen();
    void update_tooltip();
    void draw_icon(const DRAWITEMSTRUCT&);
    void preferences();
    int px(int units) const { return MulDiv(units, dpi_, 96); }
};

bool App::create() {
    WNDCLASSEXW type{sizeof(type)};
    type.lpfnWndProc = procedure;
    type.hInstance = instance_;
    type.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    type.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    type.hbrBackground = GetSysColorBrush(COLOR_BTNFACE);
    type.lpszClassName = L"HTMLCompare.Main";
    if (!RegisterClassExW(&type)) return false;
    const int w = std::min(1380, GetSystemMetrics(SM_CXSCREEN) - 48);
    const int h = std::min(900, GetSystemMetrics(SM_CYSCREEN) - 70);
    window_ = CreateWindowExW(WS_EX_CONTROLPARENT, type.lpszClassName,
        legacy ? L"Сверка HTML · Windows 7" : L"Сверка HTML · Windows 10/11",
        WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT, CW_USEDEFAULT, w, h,
        nullptr, nullptr, instance_, this);
    return window_ != nullptr;
}

LRESULT CALLBACK App::procedure(HWND window, UINT msg, WPARAM wp, LPARAM lp) {
    auto self = reinterpret_cast<App*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (msg == WM_NCCREATE) {
        self = static_cast<App*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        self->window_ = window;
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    if (!self) return DefWindowProcW(window, msg, wp, lp);
    try { return self->message(msg, wp, lp); }
    catch (const std::exception& error) {
        std::wstring reason;
        try { reason = wide(unicode(error.what())); }
        catch (...) { reason = L"Не удалось завершить операцию."; }
        MessageBoxW(window, reason.c_str(), L"Сверка HTML", MB_ICONERROR | MB_OK);
        return msg == WM_CREATE ? -1 : 0;
    }
}

LRESULT App::message(UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE: initialize(); return 0;
    case WM_SIZE: if (sheet_) layout(); return 0;
    case WM_GETMINMAXINFO: {
        auto* size = reinterpret_cast<MINMAXINFO*>(lp);
        size->ptMinTrackSize.x = px(840);
        size->ptMinTrackSize.y = px(550);
        return 0;
    }
    case WM_COMMAND: command(LOWORD(wp), HIWORD(wp)); return 0;
    case WM_NOTIFY: {
        const auto* header = reinterpret_cast<NMHDR*>(lp);
        if (header->idFrom == change_list && header->code == LVN_ITEMCHANGED && !changing_) {
            const auto* notice = reinterpret_cast<NMLISTVIEW*>(lp);
            if ((notice->uNewState & LVIS_SELECTED) && !(notice->uOldState & LVIS_SELECTED) &&
                notice->iItem >= 0 && static_cast<std::size_t>(notice->iItem) < visible_.size())
                select(visible_[notice->iItem], true);
        }
        return 0;
    }
    case WM_DRAWITEM: if (wp == binoculars) { draw_icon(*reinterpret_cast<DRAWITEMSTRUCT*>(lp)); return TRUE; } break;
    case comparison_done: complete(std::unique_ptr<Completion>(reinterpret_cast<Completion*>(lp))); return 0;
    case WM_CLOSE:
        if (busy_) {
            MessageBoxW(window_, L"Дождитесь завершения сравнения.", L"Сверка HTML", MB_OK | MB_ICONINFORMATION);
            return 0;
        }
        if (confirm_discard()) DestroyWindow(window_);
        return 0;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(window_, msg, wp, lp);
}

HWND App::widget(const wchar_t* type, const wchar_t* title, DWORD style, int id, DWORD ex) {
    auto result = CreateWindowExW(ex, type, title, WS_CHILD | WS_VISIBLE | style,
        0, 0, 0, 0, window_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), instance_, nullptr);
    if (!result) throw Error("Не удалось создать элемент окна.");
    SendMessageW(result, WM_SETFONT, reinterpret_cast<WPARAM>(font_), FALSE);
    return result;
}
HWND App::button(const wchar_t* label, int id, DWORD style) {
    return widget(L"BUTTON", label, WS_TABSTOP | style, id);
}

void App::initialize() {
    HDC dc = GetDC(window_);
    if (dc) {
        dpi_ = GetDeviceCaps(dc, LOGPIXELSY);
        ReleaseDC(window_, dc);
    }
    font_ = CreateFontW(-MulDiv(10, dpi_, 72), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                        DEFAULT_PITCH, L"Segoe UI");
    left_open_ = button(L"Левый HTML…", open_left);
    right_open_ = button(L"Правый HTML…", open_right);
    left_label_ = widget(L"STATIC", L"Исходный документ · ТЕХН", SS_LEFTNOWORDWRAP | SS_ENDELLIPSIS);
    right_label_ = widget(L"STATIC", L"Документ для сравнения", SS_LEFTNOWORDWRAP | SS_ENDELLIPSIS);
    start_ = button(L"Сравнить", start_compare);
    settings_ = button(L"Настройки сравнения", settings);
    binoculars_ = button(L"Полноэкранное сравнение (F11)", binoculars, BS_OWNERDRAW);
    prev_ = button(L"← F7", previous);
    next_ = button(L"F7 →", next);
    set(prev_, U"← Назад");
    set(next_, U"Вперёд →");
    save_ = button(L"Сохранить…", save);
    load_ = button(L"Открыть проверку…", load);
    report_ = button(L"HTML-отчёт…", report);
    summary_ = widget(L"STATIC", L"Выберите два документа", SS_LEFTNOWORDWRAP | SS_ENDELLIPSIS);
    list_ = widget(WC_LISTVIEWW, L"Различия", WS_TABSTOP | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
                   change_list, WS_EX_CLIENTEDGE);
    ListView_SetExtendedListViewStyle(list_, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_LABELTIP);
    LVCOLUMNW col{};
    col.mask = LVCF_WIDTH | LVCF_TEXT;
    col.cx = px(180);
    wchar_t categories_title[] = L"Различие";
    col.pszText = categories_title;
    ListView_InsertColumn(list_, 0, &col);
    col.cx = px(112);
    wchar_t decision_title[] = L"Решение";
    col.pszText = decision_title;
    ListView_InsertColumn(list_, 1, &col);
    detail_ = widget(L"EDIT", L"", WS_TABSTOP | ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY | WS_VSCROLL, 0, WS_EX_CLIENTEDGE);
    decision_ = widget(L"COMBOBOX", L"", WS_TABSTOP | CBS_DROPDOWNLIST, decision);
    priority_ = widget(L"COMBOBOX", L"", WS_TABSTOP | CBS_DROPDOWNLIST, priority);
    for (int i = 0; i <= 3; ++i) SendMessageW(decision_, CB_ADDSTRING, 0,
        reinterpret_cast<LPARAM>(wide(label(static_cast<Decision>(i))).c_str()));
    for (int i = 0; i <= 2; ++i) SendMessageW(priority_, CB_ADDSTRING, 0,
        reinterpret_cast<LPARAM>(wide(label(static_cast<Priority>(i))).c_str()));
    apply_ = button(L"Применить решение", apply);
    work_controls_ = {left_open_, right_open_, left_label_, right_label_, start_, save_, load_, report_,
                      list_, detail_, decision_, priority_, apply_};
    if (!legacy) {
        search_ = widget(L"EDIT", L"", WS_TABSTOP | ES_AUTOHSCROLL, search, WS_EX_CLIENTEDGE);
        SendMessageW(search_, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(L"Поиск по различиям и комментариям"));
        status_filter_ = widget(L"COMBOBOX", L"", WS_TABSTOP | CBS_DROPDOWNLIST, status_filter);
        SendMessageW(status_filter_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Все решения"));
        for (int i = 0; i <= 3; ++i) SendMessageW(status_filter_, CB_ADDSTRING, 0,
            reinterpret_cast<LPARAM>(wide(label(static_cast<Decision>(i))).c_str()));
        SendMessageW(status_filter_, CB_SETCURSEL, 0, 0);
        priority_filter_ = widget(L"COMBOBOX", L"", WS_TABSTOP | CBS_DROPDOWNLIST, priority_filter);
        SendMessageW(priority_filter_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Все приоритеты"));
        for (int i = 0; i <= 2; ++i) SendMessageW(priority_filter_, CB_ADDSTRING, 0,
            reinterpret_cast<LPARAM>(wide(label(static_cast<Priority>(i))).c_str()));
        SendMessageW(priority_filter_, CB_SETCURSEL, 0, 0);
        pending_ = button(L"Следующее непроверенное", next_pending);
        comment_ = widget(L"EDIT", L"", WS_TABSTOP | ES_AUTOHSCROLL, 0, WS_EX_CLIENTEDGE);
        SendMessageW(comment_, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(L"Комментарий к решению"));
        SendMessageW(comment_, EM_LIMITTEXT, 10000, 0);
        history_ = button(L"История", show_history);
        for (auto control : {search_, status_filter_, priority_filter_, pending_, comment_, history_})
            work_controls_.push_back(control);
    }
    tooltip_ = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr, WS_POPUP | TTS_ALWAYSTIP,
                              CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
                              window_, nullptr, instance_, nullptr);
    TOOLINFOW tip{sizeof(tip)};
    tip.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
    tip.hwnd = window_;
    tip.uId = reinterpret_cast<UINT_PTR>(binoculars_);
    tip.lpszText = const_cast<wchar_t*>(L"Полноэкранное сравнение (F11)");
    SendMessageW(tooltip_, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&tip));
    sheet_ = std::make_unique<Sheet>(window_, instance_, [this](std::size_t id) { select(id, false); });
    enable();
    layout();
}

void App::layout() {
    RECT r{};
    GetClientRect(window_, &r);
    int w = r.right, h = r.bottom;
    if (!w || !h) return;
    const int margin = px(10), gap = px(6), row = px(30);
    auto move = [](HWND handle, int x, int y, int width, int height) {
        MoveWindow(handle, x, y, std::max(0, width), std::max(0, height), TRUE);
    };
    auto anchor = sheet_->position();
    for (auto control : work_controls_) ShowWindow(control, fullscreen_ ? SW_HIDE : SW_SHOWNA);
    int y = margin;
    if (!fullscreen_) {
        const int half = (w - margin * 2 - gap) / 2;
        move(left_open_, margin, y, px(130), row);
        move(left_label_, margin + px(138), y + px(6), half - px(138), row);
        move(right_open_, margin + half + gap, y, px(130), row);
        move(right_label_, margin + half + gap + px(138), y + px(6), half - px(138), row);
        y += row + gap;
    }
    int x = margin;
    auto tool = [&](HWND control, int width) {
        if (x + px(width) > w - margin && x > margin) { x = margin; y += row + gap; }
        move(control, x, y, px(width), row);
        x += px(width) + gap;
    };
    if (!fullscreen_) tool(start_, 86);
    tool(settings_, 162);
    tool(binoculars_, 40);
    tool(prev_, 73);
    tool(next_, 73);
    if (!fullscreen_) {
        tool(save_, 100);
        tool(load_, 138);
        tool(report_, 106);
    }
    y += row + gap;
    move(summary_, margin, y, w - 2 * margin, px(22));
    y += px(24);
    if (fullscreen_) move(sheet_->hwnd(), 0, y, w, h - y);
    else {
        const int panel = std::clamp(w / (legacy ? 4 : 3), px(260), px(430));
        const int sidebar = w - panel - margin;
        move(sheet_->hwnd(), 0, y, sidebar - gap, h - y);
        int top = y;
        if (!legacy) {
            move(search_, sidebar, top, panel, row);
            top += row + gap;
            const int half = (panel - gap) / 2;
            move(status_filter_, sidebar, top, half, px(180));
            move(priority_filter_, sidebar + half + gap, top, panel - half - gap, px(150));
            top += row + gap;
            move(pending_, sidebar, top, panel, row);
            top += row + gap;
        }
        const int bottom = h - margin - (legacy ? 2 : 3) * (row + gap);
        const int detail_y = top + std::max(px(80), (bottom - top) / 2);
        move(list_, sidebar, top, panel, detail_y - top - gap);
        ListView_SetColumnWidth(list_, 0, panel - px(119));
        move(detail_, sidebar, detail_y, panel, bottom - detail_y);
        const int half = (panel - gap) / 2;
        move(decision_, sidebar, bottom + gap, half, px(150));
        move(priority_, sidebar + half + gap, bottom + gap, panel - half - gap, px(150));
        int edit_y = bottom + row + 2 * gap;
        if (!legacy) {
            move(comment_, sidebar, edit_y, panel, row);
            edit_y += row + gap;
            move(history_, sidebar + panel - px(82), edit_y, px(82), row);
        }
        move(apply_, sidebar, edit_y, panel - (legacy ? 0 : px(82) + gap), row);
    }
    sheet_->restore(anchor);
}

void App::enable() {
    const bool selected = result_ && selected_ < result_->changes.size();
    EnableWindow(start_, !busy_ && !left_.empty() && !right_.empty());
    EnableWindow(left_open_, !busy_); EnableWindow(right_open_, !busy_);
    for (auto control : {save_, load_, report_, binoculars_}) EnableWindow(control, !busy_ && result_ != nullptr);
    EnableWindow(settings_, !busy_);
    for (auto control : {prev_, next_}) EnableWindow(control, !busy_ && !visible_.empty());
    for (auto control : {decision_, priority_, apply_}) EnableWindow(control, !busy_ && selected);
    if (!legacy) {
        EnableWindow(pending_, !busy_ && !visible_.empty());
        EnableWindow(comment_, !busy_ && selected);
        EnableWindow(history_, !busy_ && selected);
    }
}

void App::status() {
    if (busy_) { set(summary_, U"Выполняется сверка…"); return; }
    if (!result_) {
        set(summary_, left_.empty() || right_.empty() ? U"Выберите два документа" : U"Документы выбраны. Нажмите «Сравнить».");
        return;
    }
    const auto checked = std::count_if(result_->changes.begin(), result_->changes.end(),
        [](const Change& c) { return c.decision != Decision::pending; });
    Text text = U"Показано " + count(visible_.size()) + U" из " + count(result_->changes.size()) +
        U"   ·   Проверено " + count(checked) + (result_->mode == Mode::strict ? U"   ·   Строгий режим" : U"   ·   По структуре");
    if (fullscreen_) text += U"   ·   " + result_->left.name + U" / " + result_->right.name;
    if (!result_->warnings.empty()) text += U"   ·   " + result_->warnings.front();
    set(summary_, text);
}

bool App::confirm_discard() const {
    return !dirty_ || MessageBoxW(window_, L"Решения изменены. Продолжить без сохранения?",
        L"Несохранённая проверка", MB_YESNO | MB_DEFBUTTON2 | MB_ICONQUESTION) == IDYES;
}

void App::choose(bool left) {
    const auto path = file_dialog(window_, false, L"Документы HTML\0*.html;*.htm;*.xhtml\0Все файлы\0*.*\0", L"html");
    if (path.empty() || !confirm_discard()) return;
    auto& file = left ? left_ : right_;
    file = path;
    set(left ? left_label_ : right_label_, unicode(path.filename().u8string()));
    dirty_ = false;
    result_.reset();
    selected_ = Sheet::none;
    sheet_->document(nullptr);
    filter();
}

void App::compare_now() {
    if (busy_ || left_.empty() || right_.empty() || !confirm_discard()) return;
    const auto left = left_, right = right_;
    const auto mode = prefs_.mode;
    const auto owner = window_;
    busy_ = true;
    enable(); status();
    try {
        worker_ = std::thread([left, right, mode, owner] {
            auto payload = std::make_unique<Completion>();
            try {
                auto before = parse_file(left);
                auto after = parse_file(right);
                payload->result = std::make_unique<Result>(compare(std::move(before), std::move(after), mode));
            } catch (const std::exception& error) {
                try { payload->error = unicode(error.what()); }
                catch (...) { payload->error = U"Не удалось разобрать документы."; }
            }
            auto* message = payload.release();
            if (!PostMessageW(owner, comparison_done, 0, reinterpret_cast<LPARAM>(message))) delete message;
        });
    } catch (...) {
        busy_ = false;
        enable(); status();
        throw;
    }
}

void App::complete(std::unique_ptr<Completion> payload) {
    if (worker_.joinable()) worker_.join();
    busy_ = false;
    if (payload->result) display(std::move(payload->result));
    else {
        if (result_) prefs_.mode = result_->mode;
        enable(); status();
        MessageBoxW(window_, wide(payload->error).c_str(), L"Ошибка сравнения", MB_OK | MB_ICONERROR);
    }
}

void App::display(std::unique_ptr<Result> result) {
    result_ = std::move(result);
    dirty_ = false;
    selected_ = Sheet::none;
    set(left_label_, result_->left.name + U" · " + result_->left.source);
    set(right_label_, result_->right.name + U" · " + result_->right.source);
    sheet_->document(result_.get());
    filter();
    if (!visible_.empty()) select(visible_.front(), false);
}

void App::filter() {
    changing_ = true;
    SendMessageW(list_, WM_SETREDRAW, FALSE, 0);
    ListView_DeleteAllItems(list_);
    visible_.clear();
    if (result_) {
        const auto query = !legacy ? lower(compact(value(search_))) : Text{};
        const int status = !legacy ? static_cast<int>(SendMessageW(status_filter_, CB_GETCURSEL, 0, 0)) : 0;
        const int priority = !legacy ? static_cast<int>(SendMessageW(priority_filter_, CB_GETCURSEL, 0, 0)) : 0;
        for (std::size_t i = 0; i < result_->changes.size(); ++i) {
            const auto& c = result_->changes[i];
            if (!(c.mask & prefs_.categories) || (status > 0 && status - 1 != static_cast<int>(c.decision)) ||
                (priority > 0 && priority - 1 != static_cast<int>(c.priority))) continue;
            if (!query.empty() && lower(c.before + U" " + c.after + U" " + c.context + U" " + c.comment).find(query) == Text::npos)
                continue;
            const auto row = static_cast<int>(visible_.size());
            visible_.push_back(i);
            auto caption = wide(count(i + 1) + U" · " + category_names(c.mask));
            LVITEMW item{};
            item.mask = LVIF_TEXT;
            item.iItem = row;
            item.pszText = caption.data();
            ListView_InsertItem(list_, &item);
            auto state = wide(label(c.decision));
            ListView_SetItemText(list_, row, 1, state.data());
        }
    }
    if (std::find(visible_.begin(), visible_.end(), selected_) == visible_.end()) selected_ = Sheet::none;
    else {
        const auto i = static_cast<int>(std::find(visible_.begin(), visible_.end(), selected_) - visible_.begin());
        ListView_SetItemState(list_, i, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
        ListView_EnsureVisible(list_, i, FALSE);
    }
    SendMessageW(list_, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(list_, nullptr, TRUE);
    changing_ = false;
    sheet_->visible(visible_, prefs_.categories);
    sheet_->select(selected_, false);
    details(true);
    enable(); status();
}

void App::select(std::size_t id, bool reveal) {
    const auto match = std::find(visible_.begin(), visible_.end(), id);
    if (match == visible_.end()) return;
    const bool changed = id != selected_;
    selected_ = id;
    changing_ = true;
    const auto row = static_cast<int>(match - visible_.begin());
    ListView_SetItemState(list_, row, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
    ListView_EnsureVisible(list_, row, FALSE);
    changing_ = false;
    sheet_->select(id, reveal);
    details(changed);
    enable();
}

void App::details(bool reset_comment) {
    if (!result_ || selected_ >= result_->changes.size()) {
        set(detail_, result_ ? U"Выберите различие." : U"Здесь появится карточка различия.");
        if (comment_ && reset_comment) set(comment_, {});
        return;
    }
    const auto& c = result_->changes[selected_];
    Text text = category_names(c.mask) + U"\r\n" + c.context + U"\r\n" + c.reason + U"\r\n\r\n";
    text += U"Было\r\n" + (c.before.empty() ? U"—" : c.before) + U"\r\n\r\n";
    text += U"Стало\r\n" + (c.after.empty() ? U"—" : c.after);
    if (history_visible_) {
        text += U"\r\n\r\nИстория решений";
        for (const auto& event : c.history)
            text += U"\r\n" + unicode(event.utc) + U" · " + label(event.decision) + U" · " +
                    label(event.priority) + U"\r\n" + event.comment;
        if (c.history.empty()) text += U"\r\nРешений пока нет.";
    }
    set(detail_, text);
    SendMessageW(decision_, CB_SETCURSEL, static_cast<int>(c.decision), 0);
    SendMessageW(priority_, CB_SETCURSEL, static_cast<int>(c.priority), 0);
    if (comment_ && reset_comment) set(comment_, c.comment);
}

void App::navigate(int direction, bool pending_only) {
    if (visible_.empty() || !result_) return;
    auto it = std::find(visible_.begin(), visible_.end(), selected_);
    int size = static_cast<int>(visible_.size());
    int pos = it == visible_.end() ? (direction > 0 ? -1 : 0) : static_cast<int>(it - visible_.begin());
    for (int n = 0; n < size; ++n) {
        pos = (pos + direction + size) % size;
        auto id = visible_[pos];
        if (!pending_only || result_->changes[id].decision == Decision::pending) { select(id, true); return; }
    }
    set(summary_, U"Среди показанных различий нет непроверенных.");
}

void App::apply_review() {
    if (!result_ || selected_ >= result_->changes.size()) return;
    auto& c = result_->changes[selected_];
    const auto d = SendMessageW(decision_, CB_GETCURSEL, 0, 0);
    const auto p = SendMessageW(priority_, CB_GETCURSEL, 0, 0);
    if (d < 0 || p < 0) return;
    const auto before = c.history.size();
    review(c, static_cast<Decision>(d), static_cast<Priority>(p), comment_ ? value(comment_) : c.comment);
    dirty_ = dirty_ || c.history.size() != before;
    filter();
}

void App::update_tooltip() {
    const auto* label = fullscreen_ ? L"Выйти из полноэкранного режима (Esc)" : L"Полноэкранное сравнение (F11)";
    SetWindowTextW(binoculars_, label);
    TOOLINFOW tip{sizeof(tip)};
    tip.hwnd = window_;
    tip.uFlags = TTF_IDISHWND;
    tip.uId = reinterpret_cast<UINT_PTR>(binoculars_);
    tip.lpszText = const_cast<wchar_t*>(label);
    SendMessageW(tooltip_, TTM_UPDATETIPTEXTW, 0, reinterpret_cast<LPARAM>(&tip));
    InvalidateRect(binoculars_, nullptr, TRUE);
}

void App::toggle_fullscreen() {
    if (!result_) return;
    const auto anchor = sheet_->position();
    if (!fullscreen_) {
        GetWindowPlacement(window_, &placement_);
        window_style_ = GetWindowLongPtrW(window_, GWL_STYLE);
        MONITORINFO monitor{sizeof(monitor)};
        if (!GetMonitorInfoW(MonitorFromWindow(window_, MONITOR_DEFAULTTONEAREST), &monitor)) return;
        fullscreen_ = true;
        SetWindowLongPtrW(window_, GWL_STYLE, window_style_ & ~WS_OVERLAPPEDWINDOW);
        const auto& r = monitor.rcMonitor;
        SetWindowPos(window_, HWND_TOP, r.left, r.top, r.right - r.left, r.bottom - r.top, SWP_FRAMECHANGED);
    } else {
        fullscreen_ = false;
        SetWindowLongPtrW(window_, GWL_STYLE, window_style_);
        SetWindowPlacement(window_, &placement_);
        SetWindowPos(window_, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
    }
    update_tooltip();
    layout();
    sheet_->restore(anchor);
    status();
    SetFocus(sheet_->hwnd());
}

void App::draw_icon(const DRAWITEMSTRUCT& item) {
    auto r = item.rcItem;
    UINT style = DFCS_BUTTONPUSH;
    if (item.itemState & ODS_SELECTED) style |= DFCS_PUSHED;
    if (item.itemState & ODS_DISABLED) style |= DFCS_INACTIVE;
    DrawFrameControl(item.hDC, &r, DFC_BUTTON, style);
    const int x = (r.left + r.right) / 2, y = (r.top + r.bottom) / 2, radius = px(5);
    auto pen = CreatePen(PS_SOLID, std::max(1, px(2)),
        GetSysColor((item.itemState & ODS_DISABLED) ? COLOR_GRAYTEXT : COLOR_BTNTEXT));
    auto previous_pen = SelectObject(item.hDC, pen);
    auto previous_brush = SelectObject(item.hDC, GetStockObject(HOLLOW_BRUSH));
    Ellipse(item.hDC, x - 2 * radius - 1, y - radius + 2, x - 1, y + radius + 2);
    Ellipse(item.hDC, x + 1, y - radius + 2, x + 2 * radius + 1, y + radius + 2);
    MoveToEx(item.hDC, x - radius - 4, y - radius + 4, nullptr);
    LineTo(item.hDC, x - radius - 2, y - radius - 3);
    LineTo(item.hDC, x + radius + 2, y - radius - 3);
    LineTo(item.hDC, x + radius + 4, y - radius + 4);
    SelectObject(item.hDC, previous_brush);
    SelectObject(item.hDC, previous_pen);
    DeleteObject(pen);
    if (item.itemState & ODS_FOCUS) { InflateRect(&r, -3, -3); DrawFocusRect(item.hDC, &r); }
}

void App::preferences() {
    auto staged = prefs_;
    if (DialogBoxParamW(instance_, MAKEINTRESOURCEW(ID_SETTINGS), window_, options,
                        reinterpret_cast<LPARAM>(&staged)) != IDOK) return;
    const auto old_mode = prefs_.mode;
    prefs_ = staged;
    filter();
    if (result_ && old_mode != prefs_.mode) {
        compare_now();
        if (!busy_) prefs_.mode = old_mode;
    }
}

void App::command(int id, int notification) {
    if (busy_) return;
    if (id == search && notification == EN_CHANGE) { filter(); return; }
    if ((id == status_filter || id == priority_filter) && notification == CBN_SELCHANGE) { filter(); return; }
    if (notification > 1) return;
    switch (id) {
    case open_left: choose(true); break;
    case open_right: choose(false); break;
    case start_compare: compare_now(); break;
    case settings: preferences(); break;
    case binoculars: toggle_fullscreen(); break;
    case exit_fullscreen: if (fullscreen_) toggle_fullscreen(); break;
    case previous: navigate(-1); break;
    case next: navigate(1); break;
    case next_pending: navigate(1, true); break;
    case apply: apply_review(); break;
    case show_history: history_visible_ = !history_visible_; details(false); break;
    case save: if (result_) {
        const auto path = file_dialog(window_, true, L"Проверка JSON\0*.json\0", L"json", L"Проверка.json");
        if (!path.empty()) {
            protect_source(path, left_, right_); save_review(*result_, path); dirty_ = false;
            set(summary_, U"Проверка сохранена.");
        }
        break;
    } break;
    case load: if (result_) {
        const auto path = file_dialog(window_, false, L"Проверка JSON\0*.json\0", L"json");
        if (!path.empty() && confirm_discard()) {
            load_review(*result_, path); dirty_ = false; filter();
            set(summary_, U"Проверка открыта.");
        }
        break;
    } break;
    case report: if (result_) {
        const auto path = file_dialog(window_, true, L"Отчёт HTML\0*.html\0", L"html", L"Отчёт сверки.html");
        if (!path.empty()) {
            protect_source(path, left_, right_); export_report(*result_, path);
            set(summary_, U"Отчёт сохранён.");
        }
        break;
    } break;
    }
}

bool App::smoke(const fs::path& left, const fs::path& right) {
    left_ = left; right_ = right;
    auto before = parse_file(left);
    auto after = parse_file(right);
    ShowWindow(window_, SW_SHOWNOACTIVATE);
    display(std::make_unique<Result>(compare(std::move(before), std::move(after), Mode::structured)));
    if (result_->changes.empty() || !sheet_->smoke_geometry()) return false;
    const auto original = selected_;
    SendMessageW(window_, WM_COMMAND, binoculars, 0);
    UpdateWindow(sheet_->hwnd());
    if (!fullscreen_ || selected_ != original || !sheet_->smoke_geometry()) return false;
    SendMessageW(window_, WM_COMMAND, exit_fullscreen, 0);
    if (fullscreen_ || selected_ != original || !sheet_->smoke_geometry()) return false;
    SendMessageW(window_, WM_COMMAND, next, 0);
    SendMessageW(window_, WM_COMMAND, previous, 0);
    if (selected_ != original) return false;
    const auto location = sheet_->position();
    prefs_.categories = notes;
    filter();
    const auto new_location = sheet_->position();
    return new_location.pair == location.pair && new_location.offset == location.offset;
}
} // namespace
} // namespace hc

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show) {
    using namespace hc;
    int argc = 0;
    auto args = CommandLineToArgvW(GetCommandLineW(), &argc);
    struct Arguments { LPWSTR* p; ~Arguments() { LocalFree(p); } } owned{args};
    const bool smoke = argc == 4 && std::wstring_view(args[1]) == L"--smoke-test";
    const bool report_only = (argc == 5 || argc == 6) && std::wstring_view(args[1]) == L"--report";
    try {
        if (report_only) {
            const auto mode = argc == 6 && std::wstring_view(args[5]) == L"--strict" ? Mode::strict : Mode::structured;
            protect_source(args[4], args[2], args[3]);
            auto left = parse_file(args[2]);
            auto right = parse_file(args[3]);
            export_report(compare(std::move(left), std::move(right), mode), args[4]);
            return 0;
        }
        INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_LISTVIEW_CLASSES | ICC_STANDARD_CLASSES | ICC_WIN95_CLASSES};
        InitCommonControlsEx(&controls);
        App app(instance);
        if (!app.create()) return 1;
        if (smoke) return app.smoke(args[2], args[3]) ? 0 : 2;
        ShowWindow(app.hwnd(), show);
        UpdateWindow(app.hwnd());
        ACCEL keys[] = {
            {FVIRTKEY, VK_F11, binoculars}, {FVIRTKEY, VK_ESCAPE, exit_fullscreen},
            {FVIRTKEY, VK_F7, next}, {FVIRTKEY | FSHIFT, VK_F7, previous},
        };
        const auto shortcuts = CreateAcceleratorTableW(keys, ARRAYSIZE(keys));
        MSG message{};
        BOOL result = 0;
        while ((result = GetMessageW(&message, nullptr, 0, 0)) > 0) {
            if (!TranslateAcceleratorW(app.hwnd(), shortcuts, &message) && !IsDialogMessageW(app.hwnd(), &message)) {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
        }
        if (shortcuts) DestroyAcceleratorTable(shortcuts);
        return result < 0 ? 1 : static_cast<int>(message.wParam);
    } catch (const std::exception& error) {
        if (!smoke && !report_only) {
            try { MessageBoxW(nullptr, wide(unicode(error.what())).c_str(), L"Сверка HTML", MB_OK | MB_ICONERROR); }
            catch (...) { MessageBoxW(nullptr, L"Не удалось запустить программу.", L"Сверка HTML", MB_OK | MB_ICONERROR); }
        }
        return 1;
    }
}
