#pragma once

#include "core.hpp"

#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>
#include <wrl/client.h>

#include <functional>

namespace hc {
class Sheet {
public:
    static constexpr std::size_t none = static_cast<std::size_t>(-1);
    struct Position { std::size_t pair = 0; float offset = 0; };
    Sheet(HWND parent, HINSTANCE instance, std::function<void(std::size_t)> choose);
    HWND hwnd() const { return window_; }
    void document(const Result* result);
    void visible(const std::vector<std::size_t>& changes, Mask mask);
    void select(std::size_t change, bool reveal);
    Position position() const;
    void restore(Position pos);
    bool smoke_geometry() const;

private:
    template<class T> using Ptr = Microsoft::WRL::ComPtr<T>;
    struct Run {
        Ptr<IDWriteTextLayout> layout;
        std::vector<UINT32> utf16_offsets;
        std::size_t start = 0;
        float x = 0, y = 0;
        bool note = false;
        bool cell = false;
        D2D1_RECT_F box{};
    };
    struct Side {
        float height = 0;
        std::vector<Run> runs;
    };
    HWND window_ = nullptr;
    Ptr<IDWriteFactory> write_;
    Ptr<IDWriteTextFormat> text_, heading_, annotation_;
    Ptr<ID2D1Factory> drawing_;
    Ptr<ID2D1HwndRenderTarget> target_;
    Ptr<ID2D1SolidColorBrush> brush_;
    const Result* result_ = nullptr;
    std::vector<float> tops_;
    std::vector<std::vector<std::size_t>> change_rows_;
    std::vector<bool> visible_;
    Mask mask_ = all;
    std::size_t selected_ = none;
    float scroll_ = 0;
    float width_ = 0;
    float scale_ = 1;
    std::function<void(std::size_t)> choose_;
    static LRESULT CALLBACK procedure(HWND, UINT, WPARAM, LPARAM);
    LRESULT message(UINT message, WPARAM wparam, LPARAM lparam);
    Run run(View value, std::size_t offset, float width, IDWriteTextFormat* style) const;
    Side layout(const Block& block, float width) const;
    float height(const Run& run) const;
    float sheet_width() const;
    void reflow();
    void scroll(float top);
    float viewport() const;
    void paint();
    void row(std::size_t index, float y, float width, float height);
    void side(const Block* block, std::size_t index, bool left, float x, float y, float width);
    void highlights(const Run& run, const std::vector<Span>& marks, D2D1_COLOR_F color, float x, float y);
    void brush(D2D1_COLOR_F color);
};
} // namespace hc
