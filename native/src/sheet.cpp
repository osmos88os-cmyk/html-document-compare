#include "sheet.hpp"

#include <algorithm>
#include <cmath>
#include <windowsx.h>

namespace hc {
namespace {
void required(HRESULT hr) {
    if (FAILED(hr)) throw Error("Не удалось инициализировать отображение текста Windows.");
}

D2D1_COLOR_F color(UINT32 rgb, float alpha = 1.f) { return D2D1::ColorF(rgb, alpha); }
D2D1_COLOR_F mark_color(Priority p, bool selected) {
    switch (p) {
    case Priority::high: return color(selected ? 0xfbb6b8 : 0xffd8d9);
    case Priority::medium: return color(selected ? 0xf6cf80 : 0xffecc0);
    case Priority::low: return color(selected ? 0xa8d0ff : 0xdcecff);
    }
    return color(0xdcecff);
}
}

Sheet::Sheet(HWND parent, HINSTANCE instance, std::function<void(std::size_t)> choose)
    : choose_(std::move(choose)) {
    required(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                 reinterpret_cast<IUnknown**>(write_.GetAddressOf())));
    required(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, drawing_.GetAddressOf()));
    HDC dc = GetDC(parent);
    if (dc) {
        scale_ = GetDeviceCaps(dc, LOGPIXELSY) / 96.f;
        ReleaseDC(parent, dc);
    }
    const auto style = [&](float size, DWRITE_FONT_WEIGHT weight, IDWriteTextFormat** format) {
        required(write_->CreateTextFormat(L"Segoe UI", nullptr, weight, DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL, size * scale_, L"ru-RU", format));
        (*format)->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
    };
    style(16.f, DWRITE_FONT_WEIGHT_NORMAL, text_.GetAddressOf());
    style(16.f, DWRITE_FONT_WEIGHT_SEMI_BOLD, heading_.GetAddressOf());
    style(14.f, DWRITE_FONT_WEIGHT_NORMAL, annotation_.GetAddressOf());
    WNDCLASSW type{};
    type.lpfnWndProc = procedure;
    type.hInstance = instance;
    type.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    type.lpszClassName = L"HTMLCompare.Sheets";
    RegisterClassW(&type);
    window_ = CreateWindowExW(0, type.lpszClassName, L"Два листа сравнения",
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_TABSTOP, 0, 0, 0, 0, parent, nullptr, instance, this);
    if (!window_) throw Error("Не удалось создать область сравнения.");
}

LRESULT CALLBACK Sheet::procedure(HWND window, UINT message, WPARAM wp, LPARAM lp) {
    auto self = reinterpret_cast<Sheet*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        self = static_cast<Sheet*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        self->window_ = window;
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    if (!self) return DefWindowProcW(window, message, wp, lp);
    return self->message(message, wp, lp);
}

LRESULT Sheet::message(UINT message, WPARAM wp, LPARAM lp) {
    switch (message) {
    case WM_ERASEBKGND: return 1;
    case WM_SIZE: {
        const auto anchor = position();
        RECT rect{};
        GetClientRect(window_, &rect);
        if (target_) target_->Resize(D2D1::SizeU(rect.right, rect.bottom));
        if (width_ != static_cast<float>(rect.right)) {
            width_ = static_cast<float>(rect.right);
            reflow();
        }
        restore(anchor);
        return 0;
    }
    case WM_PAINT: paint(); return 0;
    case WM_MOUSEWHEEL: {
        UINT lines = 3;
        SystemParametersInfoW(SPI_GETWHEELSCROLLLINES, 0, &lines, 0);
        const float step = lines == WHEEL_PAGESCROLL ? viewport() : lines * 20.f * scale_;
        scroll(scroll_ - GET_WHEEL_DELTA_WPARAM(wp) / 120.f * step);
        return 0;
    }
    case WM_VSCROLL: {
        SCROLLINFO info{sizeof(info), SIF_TRACKPOS};
        GetScrollInfo(window_, SB_VERT, &info);
        switch (LOWORD(wp)) {
        case SB_THUMBPOSITION: case SB_THUMBTRACK: scroll(static_cast<float>(info.nTrackPos)); break;
        case SB_LINEUP: scroll(scroll_ - 24 * scale_); break;
        case SB_LINEDOWN: scroll(scroll_ + 24 * scale_); break;
        case SB_PAGEUP: scroll(scroll_ - viewport()); break;
        case SB_PAGEDOWN: scroll(scroll_ + viewport()); break;
        case SB_TOP: scroll(0); break;
        case SB_BOTTOM: if (!tops_.empty()) scroll(tops_.back()); break;
        }
        return 0;
    }
    case WM_KEYDOWN:
        if (wp == VK_UP) { scroll(scroll_ - 24 * scale_); return 0; }
        if (wp == VK_DOWN) { scroll(scroll_ + 24 * scale_); return 0; }
        if (wp == VK_PRIOR) { scroll(scroll_ - viewport()); return 0; }
        if (wp == VK_NEXT) { scroll(scroll_ + viewport()); return 0; }
        if (wp == VK_HOME) { scroll(0); return 0; }
        if (wp == VK_END && !tops_.empty()) { scroll(tops_.back()); return 0; }
        break;
    case WM_LBUTTONDOWN:
        SetFocus(window_);
        if (result_ && tops_.size() > 1) {
            const auto y = static_cast<float>(GET_Y_LPARAM(lp)) + scroll_;
            auto next = std::upper_bound(tops_.begin(), tops_.end(), y);
            if (next != tops_.begin() && next != tops_.end()) {
                const auto pair = static_cast<std::size_t>(next - tops_.begin() - 1);
                for (auto change : change_rows_[pair]) {
                    if (visible_[change]) { choose_(change); break; }
                }
            }
        }
        return 0;
    }
    return DefWindowProcW(window_, message, wp, lp);
}

void Sheet::document(const Result* result) {
    result_ = result;
    scroll_ = 0;
    selected_ = none;
    change_rows_.assign(result ? result->pairs.size() : 0, {});
    visible_.assign(result ? result->changes.size() : 0, false);
    if (result) for (std::size_t i = 0; i < result->changes.size(); ++i) {
        change_rows_[result->changes[i].pair].push_back(i);
    }
    reflow();
    scroll(0);
}

void Sheet::visible(const std::vector<std::size_t>& changes, Mask mask) {
    mask_ = mask;
    std::fill(visible_.begin(), visible_.end(), false);
    for (const auto index : changes) visible_[index] = true;
    InvalidateRect(window_, nullptr, FALSE);
}

void Sheet::select(std::size_t change, bool reveal) {
    selected_ = change;
    if (!reveal || !result_ || change >= result_->changes.size()) {
        InvalidateRect(window_, nullptr, FALSE);
        return;
    }
    const auto& item = result_->changes[change];
    const auto& pair = result_->pairs[item.pair];
    const auto locate = [&](const Block& block, const std::vector<Span>& marks) {
        const auto side = layout(block, sheet_width() - 32 * scale_);
        for (auto span : marks) {
            if (span.empty() || !(span.mask & mask_)) continue;
            for (const auto& placed : side.runs) {
                if (span.end <= placed.start || span.start >= placed.start + placed.utf16_offsets.size()) continue;
                const auto offset = std::max(span.start, placed.start) - placed.start;
                FLOAT x = 0, y = 0;
                DWRITE_HIT_TEST_METRICS hit{};
                if (SUCCEEDED(placed.layout->HitTestTextPosition(placed.utf16_offsets[offset], FALSE, &x, &y, &hit)))
                    return 10.f * scale_ + placed.y + y;
            }
        }
        return -1.f;
    };
    float offset = pair.right < 0 ? -1 : locate(result_->right.blocks[pair.right], item.right_marks);
    if (offset < 0 && pair.left >= 0) offset = locate(result_->left.blocks[pair.left], item.left_marks);
    const auto position = tops_[item.pair] + std::max(0.f, offset);
    if (position < scroll_ || position >= scroll_ + viewport() - 32 * scale_)
        scroll(std::max(0.f, position - 24 * scale_));
    else InvalidateRect(window_, nullptr, FALSE);
}

Sheet::Position Sheet::position() const {
    if (tops_.size() < 2) return {};
    const auto end = std::upper_bound(tops_.begin(), tops_.end(), scroll_);
    const auto pair = std::min(static_cast<std::size_t>(end - tops_.begin() - 1), tops_.size() - 2);
    return {pair, scroll_ - tops_[pair]};
}

void Sheet::restore(Position anchor) {
    if (tops_.size() < 2 || anchor.pair >= tops_.size() - 1) scroll(0);
    else {
        const auto h = tops_[anchor.pair + 1] - tops_[anchor.pair];
        scroll(tops_[anchor.pair] + std::clamp(anchor.offset, 0.f, std::max(0.f, h - 1)));
    }
}

float Sheet::viewport() const {
    RECT rect{};
    GetClientRect(window_, &rect);
    return static_cast<float>(rect.bottom);
}

void Sheet::scroll(float top) {
    const float content = tops_.empty() ? 0 : tops_.back();
    scroll_ = std::clamp(top, 0.f, std::max(0.f, content - viewport()));
    SCROLLINFO info{sizeof(info), SIF_RANGE | SIF_PAGE | SIF_POS | SIF_DISABLENOSCROLL};
    info.nMax = std::max(0, static_cast<int>(std::ceil(content)) - 1);
    info.nPage = static_cast<UINT>(viewport());
    info.nPos = static_cast<int>(scroll_);
    SetScrollInfo(window_, SB_VERT, &info, TRUE);
    InvalidateRect(window_, nullptr, FALSE);
}

Sheet::Run Sheet::run(View value, std::size_t offset, float width, IDWriteTextFormat* style) const {
    Run result;
    result.start = offset;
    const auto text = wide(value);
    result.utf16_offsets.reserve(value.size() + 1);
    UINT32 position = 0;
    for (auto c : value) {
        result.utf16_offsets.push_back(position);
        position += c >= 0x10000 ? 2 : 1;
    }
    result.utf16_offsets.push_back(position);
    required(write_->CreateTextLayout(text.data(), static_cast<UINT32>(text.size()), style,
        std::max(4.f, width), 2000000.f, result.layout.GetAddressOf()));
    return result;
}

float Sheet::height(const Run& run) const {
    DWRITE_TEXT_METRICS metrics{};
    if (FAILED(run.layout->GetMetrics(&metrics))) return 24 * scale_;
    return std::ceil(metrics.height);
}

Sheet::Side Sheet::layout(const Block& block, float width) const {
    Side side;
    const float padding = 5 * scale_;
    if (block.kind == Kind::table && !block.cells.empty()) {
        int cols = 0, rows = 0;
        for (const auto& c : block.cells) {
            cols = std::max(cols, c.column + c.colspan);
            rows = std::max(rows, c.row + c.rowspan);
        }
        const float column_width = width / cols;
        std::vector<float> heights(rows, 22 * scale_);
        for (const auto& c : block.cells) {
            auto placed = run(c.text, c.display.start, column_width * c.colspan - 2 * padding,
                              c.header ? heading_.Get() : text_.Get());
            const auto last = c.row + c.rowspan - 1;
            float current = 0;
            for (int i = c.row; i <= last; ++i) current += heights[i];
            heights[last] += std::max(0.f, height(placed) + 2 * padding - current);
            side.runs.push_back(std::move(placed));
        }
        std::vector<float> top(rows + 1);
        for (int i = 0; i < rows; ++i) top[i + 1] = top[i] + heights[i];
        for (std::size_t i = 0; i < block.cells.size(); ++i) {
            const auto& c = block.cells[i];
            auto& placed = side.runs[i];
            const auto left = c.column * column_width;
            placed.cell = true;
            placed.box = D2D1::RectF(left, top[c.row], left + c.colspan * column_width, top[c.row + c.rowspan]);
            placed.x = left + padding;
            placed.y = top[c.row] + padding;
        }
        side.height = top.back();
    } else if (!block.text.empty()) {
        auto value = View(block.text);
        // Each strict row includes its delimiter. Display it via its own highlight marker.
        if (!value.empty() && value.back() == U'\n') value.remove_suffix(1);
        auto placed = run(value, 0, width, block.kind == Kind::heading ? heading_.Get() : text_.Get());
        side.height = height(placed);
        side.runs.push_back(std::move(placed));
    }
    const auto offsets = annotation_spans(block);
    for (std::size_t i = 0; i < block.annotations.size(); ++i) {
        auto placed = run(block.annotations[i].text, offsets[i].start, width, annotation_.Get());
        placed.note = true;
        placed.y = side.height + 4 * scale_;
        side.height = placed.y + height(placed);
        side.runs.push_back(std::move(placed));
    }
    return side;
}

float Sheet::sheet_width() const { return std::max(30.f, (width_ - 30.f * scale_) / 2.f); }

void Sheet::reflow() {
    tops_.assign(1, 0);
    if (!result_ || width_ < 50) return;
    const auto w = sheet_width() - 32 * scale_;
    for (const auto& pair : result_->pairs) {
        const auto l = pair.left < 0 ? 0 : layout(result_->left.blocks[pair.left], w).height;
        const auto r = pair.right < 0 ? 0 : layout(result_->right.blocks[pair.right], w).height;
        tops_.push_back(tops_.back() + std::max({l, r, 20 * scale_}) + 20 * scale_);
    }
}

void Sheet::brush(D2D1_COLOR_F value) { brush_->SetColor(value); }

void Sheet::highlights(const Run& placed, const std::vector<Span>& marks, D2D1_COLOR_F fill, float x, float y) {
    const auto size = placed.utf16_offsets.size() - 1;
    for (const auto s : marks) {
        if (!(s.mask & mask_) || s.empty() || s.end <= placed.start) continue;
        if (s.start >= placed.start + size) {
            // Visualize a modified trailing newline in the strict reader.
            if (result_->mode == Mode::strict && s.start == placed.start + size) {
                FLOAT left = 0, top = 0;
                DWRITE_HIT_TEST_METRICS metric{};
                if (SUCCEEDED(placed.layout->HitTestTextPosition(placed.utf16_offsets.back(), FALSE, &left, &top, &metric))) {
                    brush(fill);
                    target_->FillRectangle(D2D1::RectF(x + left, y + top, x + left + 6 * scale_, y + top + metric.height), brush_.Get());
                }
            }
            continue;
        }
        const auto begin = placed.utf16_offsets[std::max(s.start, placed.start) - placed.start];
        const auto end = placed.utf16_offsets[std::min(s.end - placed.start, size)];
        UINT32 count = 0;
        placed.layout->HitTestTextRange(begin, end - begin, x, y, nullptr, 0, &count);
        if (!count) continue;
        std::vector<DWRITE_HIT_TEST_METRICS> rects(count);
        if (FAILED(placed.layout->HitTestTextRange(begin, end - begin, x, y, rects.data(), count, &count))) continue;
        brush(fill);
        for (const auto& r : rects) {
            target_->FillRectangle(D2D1::RectF(r.left, r.top, r.left + std::max(r.width, 3.f), r.top + r.height), brush_.Get());
        }
    }
}

void Sheet::side(const Block* block, std::size_t index, bool left, float x, float y, float width) {
    if (!block) return;
    const auto content = layout(*block, width - 32 * scale_);
    for (const auto& placed : content.runs) {
        const float run_x = x + 16 * scale_ + placed.x;
        const float run_y = y + 10 * scale_ + placed.y;
        if (placed.cell) {
            auto box = placed.box;
            box.left += x + 16 * scale_; box.right += x + 16 * scale_;
            box.top += y + 10 * scale_; box.bottom += y + 10 * scale_;
            brush(color(0xd9e0ea));
            target_->DrawRectangle(box, brush_.Get());
        }
        const auto draw = [&](std::size_t id) {
            const auto& change = result_->changes[id];
            highlights(placed, left ? change.left_marks : change.right_marks,
                mark_color(change.priority, id == selected_), run_x, run_y);
        };
        for (auto id : change_rows_[index]) if (visible_[id] && id != selected_) draw(id);
        if (selected_ < result_->changes.size() && result_->changes[selected_].pair == index && visible_[selected_])
            draw(selected_);
        brush(color(placed.note ? 0x475c76 : 0x172438));
        target_->DrawTextLayout(D2D1::Point2F(run_x, run_y), placed.layout.Get(), brush_.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
    }
}

void Sheet::row(std::size_t index, float y, float width, float h) {
    const auto& p = result_->pairs[index];
    const bool selected = selected_ < result_->changes.size() && result_->changes[selected_].pair == index;
    const auto x = 10 * scale_, other = x + width + 10 * scale_;
    brush(color(0xffffff));
    target_->FillRectangle(D2D1::RectF(x, y, x + width, y + h), brush_.Get());
    target_->FillRectangle(D2D1::RectF(other, y, other + width, y + h), brush_.Get());
    if (selected || p.ambiguous) {
        brush(color(selected ? 0x166cbb : 0xb88628));
        target_->FillRectangle(D2D1::RectF(x, y, x + 3 * scale_, y + h), brush_.Get());
        target_->FillRectangle(D2D1::RectF(other, y, other + 3 * scale_, y + h), brush_.Get());
    }
    side(p.left < 0 ? nullptr : &result_->left.blocks[p.left], index, true, x, y, width);
    side(p.right < 0 ? nullptr : &result_->right.blocks[p.right], index, false, other, y, width);
}

void Sheet::paint() {
    PAINTSTRUCT ps{};
    BeginPaint(window_, &ps);
    try {
        RECT bounds{};
        GetClientRect(window_, &bounds);
        if (bounds.right && bounds.bottom) {
            if (!target_) {
                const auto props = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT,
                    D2D1::PixelFormat(), 96, 96);
                required(drawing_->CreateHwndRenderTarget(props, D2D1::HwndRenderTargetProperties(
                    window_, D2D1::SizeU(bounds.right, bounds.bottom)), target_.GetAddressOf()));
                required(target_->CreateSolidColorBrush(color(0), brush_.GetAddressOf()));
            }
            target_->BeginDraw();
            target_->Clear(color(0xeaf0f7));
            if (!result_ || tops_.size() < 2) {
                auto placeholder = run(U"Выберите два HTML-документа и нажмите «Сравнить».", 0, width_ - 30, text_.Get());
                brush(color(0x475c76));
                target_->DrawTextLayout(D2D1::Point2F(24, 24), placeholder.layout.Get(), brush_.Get());
            } else {
                const auto first = position().pair;
                for (auto i = first; i + 1 < tops_.size() && tops_[i] - scroll_ < bounds.bottom; ++i)
                    row(i, tops_[i] - scroll_, sheet_width(), tops_[i + 1] - tops_[i]);
            }
            if (target_->EndDraw() == D2DERR_RECREATE_TARGET) {
                target_.Reset(); brush_.Reset();
                InvalidateRect(window_, nullptr, FALSE);
            }
        }
    } catch (const std::exception&) {
        target_.Reset(); brush_.Reset();
        FillRect(ps.hdc, &ps.rcPaint, GetSysColorBrush(COLOR_WINDOW));
        const wchar_t message[] = L"Не удалось отобразить документы.";
        TextOutW(ps.hdc, 12, 12, message, ARRAYSIZE(message) - 1);
    }
    EndPaint(window_, &ps);
}

bool Sheet::smoke_geometry() const {
    if (!result_ || tops_.size() != result_->pairs.size() + 1) return false;
    for (std::size_t i = 0; i < result_->pairs.size(); ++i) {
        const auto& p = result_->pairs[i];
        const auto w = sheet_width() - 32 * scale_;
        const auto l = p.left < 0 ? 0 : layout(result_->left.blocks[p.left], w).height;
        const auto r = p.right < 0 ? 0 : layout(result_->right.blocks[p.right], w).height;
        if (tops_[i + 1] - tops_[i] < std::max(l, r)) return false;
    }
    return true;
}
} // namespace hc
