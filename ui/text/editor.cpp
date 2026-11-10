#include "text.hpp"
#include <algorithm>
#include <imm.h>
#include <stdexcept>

namespace gsdl::text {
namespace {
constexpr std::size_t max_length = 32768;
bool high(wchar_t c) { return c >= 0xd800 && c <= 0xdbff; }
bool low(wchar_t c) { return c >= 0xdc00 && c <= 0xdfff; }
std::size_t boundary(std::wstring_view s, std::size_t p) {
  p = (std::min)(p, s.size());
  if (p && p < s.size() && low(s[p]) && high(s[p - 1]))
    --p;
  return p;
}
std::size_t previous(std::wstring_view s, std::size_t p) {
  if (!p)
    return 0;
  --p;
  if (p && low(s[p]) && high(s[p - 1]))
    --p;
  return p;
}
std::size_t next(std::wstring_view s, std::size_t p) {
  if (p >= s.size())
    return s.size();
  return p + ((high(s[p]) && p + 1 < s.size() && low(s[p + 1])) ? 2 : 1);
}
bool valid(std::wstring_view s) {
  for (std::size_t i = 0; i < s.size(); ++i) {
    if (s[i] < 32 || s[i] == 127)
      return false;
    if (high(s[i])) {
      if (i + 1 == s.size() || !low(s[++i]))
        return false;
    } else if (low(s[i]))
      return false;
  }
  return true;
}
std::wstring composition(HIMC context, DWORD kind) {
  const auto n = ImmGetCompositionStringW(context, kind, nullptr, 0);
  if (n < 0 || n > static_cast<LONG>(max_length * 2) || n % 2)
    return {};
  std::wstring value(static_cast<std::size_t>(n) / 2, L'\0');
  if (n && ImmGetCompositionStringW(context, kind, value.data(), n) != n)
    return {};
  return value;
}
} // namespace
void Editor::begin(std::wstring value) {
  if (value.size() > max_length || !valid(value))
    throw std::invalid_argument("Invalid single-line UTF-16 baseline");
  baseline_ = draft_ = std::move(value);
  caret_ = anchor_ = draft_.size();
  active_ = true;
  invalid_ = false;
  end_composition();
  undo_.clear();
  redo_.clear();
}
bool Editor::commit(const Validator &validate) {
  if (!active_)
    return true;
  if (composing_)
    return false;
  if (validate && !validate(draft_)) {
    invalid_ = true;
    return false;
  }
  active_ = false;
  invalid_ = false;
  baseline_ = draft_;
  undo_.clear();
  redo_.clear();
  return true;
}
void Editor::cancel() {
  draft_ = baseline_;
  caret_ = anchor_ = draft_.size();
  active_ = invalid_ = false;
  end_composition();
  undo_.clear();
  redo_.clear();
}
void Editor::remember() {
  undo_.push_back({draft_, caret_, anchor_});
  if (undo_.size() > 128)
    undo_.erase(undo_.begin());
  redo_.clear();
}
void Editor::remove_selection() {
  const auto lo = (std::min)(caret_, anchor_), hi = (std::max)(caret_, anchor_);
  draft_.erase(lo, hi - lo);
  caret_ = anchor_ = lo;
}
void Editor::insert(std::wstring_view value) {
  if (!active_ || composing_ || value.empty() || !valid(value))
    return;
  const auto count = (std::max)(caret_, anchor_) - (std::min)(caret_, anchor_);
  if (draft_.size() - count + value.size() > max_length)
    return;
  remember();
  remove_selection();
  draft_.insert(caret_, value);
  caret_ += value.size();
  anchor_ = caret_;
  invalid_ = false;
}
void Editor::erase(bool backward) {
  if (!active_ || composing_)
    return;
  if (caret_ == anchor_ &&
      ((backward && !caret_) || (!backward && caret_ == draft_.size())))
    return;
  remember();
  if (caret_ == anchor_)
    anchor_ = backward ? previous(draft_, caret_) : next(draft_, caret_);
  remove_selection();
  invalid_ = false;
}
void Editor::move(int direction, bool extend) {
  if (composing_)
    return;
  std::size_t p;
  if (!extend && caret_ != anchor_)
    p = direction < 0 ? (std::min)(caret_, anchor_)
                      : (std::max)(caret_, anchor_);
  else
    p = direction < 0 ? previous(draft_, caret_) : next(draft_, caret_);
  set_caret(p, extend);
}
void Editor::home(bool end, bool extend) {
  set_caret(end ? draft_.size() : 0, extend);
}
void Editor::set_caret(std::size_t p, bool extend) {
  if (!active_ || composing_)
    return;
  caret_ = boundary(draft_, p);
  if (!extend)
    anchor_ = caret_;
}
void Editor::select_all() {
  if (!active_ || composing_)
    return;
  anchor_ = 0;
  caret_ = draft_.size();
}
std::wstring Editor::selection() const {
  const auto lo = (std::min)(caret_, anchor_);
  return draft_.substr(lo, (std::max)(caret_, anchor_) - lo);
}
void Editor::undo() {
  if (!active_ || composing_ || undo_.empty())
    return;
  redo_.push_back({draft_, caret_, anchor_});
  auto s = std::move(undo_.back());
  undo_.pop_back();
  draft_ = std::move(s.text);
  caret_ = s.caret;
  anchor_ = s.anchor;
  invalid_ = false;
}
void Editor::redo() {
  if (!active_ || composing_ || redo_.empty())
    return;
  undo_.push_back({draft_, caret_, anchor_});
  auto s = std::move(redo_.back());
  redo_.pop_back();
  draft_ = std::move(s.text);
  caret_ = s.caret;
  anchor_ = s.anchor;
  invalid_ = false;
}
void Editor::start_composition() {
  if (!active_)
    return;
  composing_ = true;
  composition_.clear();
  composition_cursor_ = 0;
}
void Editor::update_composition(std::wstring value, std::size_t cursor) {
  if (!active_ || value.size() > max_length || !valid(value))
    return;
  composing_ = true;
  composition_ = std::move(value);
  composition_cursor_ = boundary(composition_, cursor);
}
void Editor::result_composition(std::wstring_view value) {
  end_composition();
  insert(value);
}
void Editor::end_composition() {
  composing_ = false;
  composition_.clear();
  composition_cursor_ = 0;
}
std::wstring Editor::display_text() const {
  if (!composing_)
    return draft_;
  auto value = draft_;
  const auto lo = (std::min)(caret_, anchor_), hi = (std::max)(caret_, anchor_);
  value.replace(lo, hi - lo, composition_);
  return value;
}
std::size_t Editor::display_caret() const {
  return composing_ ? (std::min)(caret_, anchor_) + composition_cursor_
                    : caret_;
}
std::pair<std::size_t, std::size_t> Editor::composition_range() const {
  return composing_
             ? std::pair{(std::min)(caret_, anchor_), composition_.size()}
             : std::pair<std::size_t, std::size_t>{0, 0};
}
std::optional<LRESULT> ime_message(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp,
                                   Editor &edit) {
  if (msg == WM_IME_SETCONTEXT)
    return DefWindowProcW(hwnd, msg, wp, lp & ~ISC_SHOWUICOMPOSITIONWINDOW);
  if (!edit.active())
    return {};
  if (msg == WM_IME_STARTCOMPOSITION) {
    edit.start_composition();
    return 0;
  }
  if (msg == WM_IME_ENDCOMPOSITION) {
    edit.end_composition();
    return 0;
  }
  if (msg == WM_IME_CHAR)
    return 0; // Result strings are inserted exactly once below.
  if (msg != WM_IME_COMPOSITION)
    return {};
  const auto context = ImmGetContext(hwnd);
  if (!context)
    return 0;
  if (lp & GCS_RESULTSTR)
    edit.result_composition(composition(context, GCS_RESULTSTR));
  // Some IMEs report only cursor movement when the composition text has not
  // changed. Re-read the current composition to keep caret/candidate placement
  // in sync in that case as well.
  if (lp & (GCS_COMPSTR | GCS_CURSORPOS)) {
    const auto cursor =
        ImmGetCompositionStringW(context, GCS_CURSORPOS, nullptr, 0);
    edit.update_composition(composition(context, GCS_COMPSTR),
                            cursor < 0 ? 0 : static_cast<std::size_t>(cursor));
  }
  if (lp == 0)
    edit.end_composition();
  ImmReleaseContext(hwnd, context);
  return 0;
}
void position_ime(HWND hwnd, int x, int y, int height) {
  const auto context = ImmGetContext(hwnd);
  if (!context)
    return;
  COMPOSITIONFORM form{};
  form.dwStyle = CFS_POINT;
  form.ptCurrentPos = {x, y};
  ImmSetCompositionWindow(context, &form);
  CANDIDATEFORM candidate{};
  candidate.dwIndex = 0;
  candidate.dwStyle = CFS_EXCLUDE;
  candidate.ptCurrentPos = {x, y + height};
  candidate.rcArea = {x, y, x + 1, y + height};
  ImmSetCandidateWindow(context, &candidate);
  ImmReleaseContext(hwnd, context);
}
ImeBinding::ImeBinding(HWND hwnd) : hwnd_(hwnd), owner_(GetCurrentThreadId()) {
  if (GetWindowThreadProcessId(hwnd, nullptr) != owner_)
    throw std::invalid_argument("IME binding belongs to HWND thread");
  saved_ = ImmGetContext(hwnd_);
  if (saved_)
    ImmReleaseContext(hwnd_, saved_);
}
ImeBinding::~ImeBinding() {
  // Typically the host destroys its HWND first. If this binding is removed
  // earlier, restore the borrowed context so it is not left detached.
  if (GetCurrentThreadId() == owner_ && IsWindow(hwnd_) && saved_)
    ImmAssociateContext(hwnd_, saved_);
}
bool ImeBinding::set_text_focus(bool enabled, bool composition_active) {
  if (GetCurrentThreadId() != owner_ || !IsWindow(hwnd_))
    return false;
  if (!enabled && composition_active)
    return false;
  auto current = ImmGetContext(hwnd_);
  if (current)
    ImmReleaseContext(hwnd_, current);
  if (enabled) {
    if (!current) {
      if (saved_)
        ImmAssociateContext(hwnd_, saved_);
      else
        ImmAssociateContextEx(hwnd_, nullptr, IACE_DEFAULT);
      current = ImmGetContext(hwnd_);
      if (current) {
        saved_ = current;
        ImmReleaseContext(hwnd_, current);
      }
    }
    return current != nullptr;
  }
  if (current) {
    // Guard the system's state too; the host's composition flag can lag a
    // synchronous IME callback. Do not force complete/cancel or drop text.
    const auto context = ImmGetContext(hwnd_);
    const auto bytes =
        context ? ImmGetCompositionStringW(context, GCS_COMPSTR, nullptr, 0)
                : 0;
    if (context)
      ImmReleaseContext(hwnd_, context);
    if (bytes > 0)
      return false;
    const auto previous_context = ImmAssociateContext(hwnd_, nullptr);
    if (previous_context)
      saved_ = previous_context;
  }
  current = ImmGetContext(hwnd_);
  if (current)
    ImmReleaseContext(hwnd_, current);
  return current == nullptr;
}
} // namespace gsdl::text
