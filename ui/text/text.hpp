#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>
#include <windows.h>

namespace gsdl::text {
// An editing session only. The caller owns the authoritative value and applies
// a successful draft through its application command service.
class Editor {
public:
  using Validator = std::function<bool(std::wstring_view)>;
  void begin(std::wstring value);
  bool commit(const Validator &validate = {});
  void cancel();
  bool active() const noexcept { return active_; }
  bool composing() const noexcept { return composing_; }
  bool invalid() const noexcept { return invalid_; }
  const std::wstring &draft() const noexcept { return draft_; }
  std::size_t caret() const noexcept { return caret_; }
  std::size_t anchor() const noexcept { return anchor_; }
  void insert(std::wstring_view text);
  void erase(bool backward = true);
  void move(int direction, bool extend = false);
  void home(bool end, bool extend = false);
  void set_caret(std::size_t position, bool extend = false);
  void select_all();
  std::wstring selection() const;
  void undo();
  void redo();
  void start_composition();
  void update_composition(std::wstring text, std::size_t cursor);
  void result_composition(std::wstring_view text);
  void end_composition();
  std::wstring display_text() const;
  std::size_t display_caret() const;
  std::pair<std::size_t, std::size_t> composition_range() const;

private:
  struct State {
    std::wstring text;
    std::size_t caret{}, anchor{};
  };
  bool active_{}, invalid_{}, composing_{};
  std::wstring baseline_, draft_, composition_;
  std::size_t caret_{}, anchor_{}, composition_cursor_{};
  std::vector<State> undo_, redo_;
  void remember();
  void remove_selection();
};

// Owns no HWND or input context; never disables the user's IME.
std::optional<LRESULT> ime_message(HWND, UINT, WPARAM, LPARAM, Editor &);
void position_ime(HWND, int x_pixels, int y_pixels, int line_height_pixels);

// A single custom-control HWND needs an input context only while its logical
// focus is a text editor. Association is local to this HWND; no IME open state,
// conversion mode or system profile is changed. All calls are UI-thread only.
class ImeBinding {
public:
  explicit ImeBinding(HWND);
  ~ImeBinding();
  ImeBinding(const ImeBinding &) = delete;
  ImeBinding &operator=(const ImeBinding &) = delete;
  // Refuse to detach mid-composition. The host must first let the IME finish
  // (or cancel through its own UI), then apply its normal focus transition.
  bool set_text_focus(bool enabled, bool composition_active = false);

private:
  HWND hwnd_{};
  HIMC saved_{}; // borrowed, never destroyed
  DWORD owner_{};
};

struct Color {
  std::uint8_t r{}, g{}, b{}, a{255};
};
struct Clip {
  int left{}, top{}, right{}, bottom{};
};
struct Metrics {
  float width{}, height{}, baseline{};
};
class Rasterizer {
public:
  Rasterizer();
  ~Rasterizer();
  Rasterizer(const Rasterizer &) = delete;
  Rasterizer &operator=(const Rasterizer &) = delete;
  Metrics measure(std::wstring_view text, float font_size, unsigned weight = 400);
  // Prefix boundary comes from DirectWrite clusters, never a UTF-16 half-pair.
  std::size_t fit_prefix(std::wstring_view text, float font_size, float width,
                         unsigned weight = 400);
  std::size_t hit_test(std::wstring_view text, float font_size, float x);
  float caret_x(std::wstring_view text, float font_size, std::size_t position);
  // Draw into top-to-bottom RGBA8 premultiplied pixels. Caller synchronizes
  // this object and buffer. x/y/font_size are DIP; clipping is physical pixels.
  void draw(std::span<std::uint8_t> pixels, unsigned width, unsigned height,
            std::wstring_view text, float x, float y, float font_size,
            Color color, float scale = 1.f, std::optional<Clip> clip = {},
            unsigned weight = 400);

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
} // namespace gsdl::text
