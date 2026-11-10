#pragma once
#include "resources/theme/menu_tokens.hpp"
#include <algorithm>
#include <memory>
#include <string>
#include <vector>
#include <windows.h>
#include <oleacc.h>

// Native menu hosting retains Windows navigation, dismissal and accessibility.
// Only HMENU item painting uses its supplied HDC; application content continues
// to be drawn by the Skia / DirectWrite / Vulkan pipeline.
namespace themed_menu {
namespace style = gsdl::theme::menu;
inline COLORREF color(gsdl::graphics::Rgba c) { return RGB(c.r, c.g, c.b); }
struct Item {
  // Documented MSAAMENUINFO prefix gives owner-drawn menu items spoken text.
  MSAAMENUINFO accessible{};
  std::wstring label, shortcut;
  HFONT font{};
  UINT dpi = 96;
  bool separator{}, enabled = true;
  int px(int dip) const { return MulDiv(dip, int(dpi), 96); }
};
inline Item *item(ULONG_PTR data) {
  if (!data) return nullptr;
  auto *value = reinterpret_cast<Item *>(data);
  return value->accessible.dwMSAASignature == MSAA_MENU_SIG ? value : nullptr;
}
inline bool measure(MEASUREITEMSTRUCT *measure) {
  if (!measure || measure->CtlType != ODT_MENU) return false;
  const auto *value = item(measure->itemData);
  if (!value) return false;
  measure->itemHeight = value->px(value->separator ? style::separatorHeight : style::rowHeight);
  auto dc = GetDC(nullptr);
  auto old = SelectObject(dc, value->font);
  SIZE text{}, shortcut{};
  GetTextExtentPoint32W(dc, value->label.c_str(), int(value->label.size()), &text);
  GetTextExtentPoint32W(dc, value->shortcut.c_str(), int(value->shortcut.size()), &shortcut);
  SelectObject(dc, old);
  ReleaseDC(nullptr, dc);
  const int width = text.cx + 2 * value->px(style::inset) +
      (value->shortcut.empty() ? 0 : shortcut.cx + value->px(style::shortcutGap));
  measure->itemWidth = UINT(std::clamp(width, value->px(style::minimumWidth), value->px(style::maximumWidth)));
  return true;
}
inline bool draw(DRAWITEMSTRUCT *draw) {
  if (!draw || draw->CtlType != ODT_MENU) return false;
  const auto *value = item(draw->itemData);
  if (!value) return false;
  const bool enabled = value->enabled && !(draw->itemState & (ODS_DISABLED | ODS_GRAYED));
  const auto background = enabled && !value->separator && (draw->itemState & ODS_SELECTED)
      ? style::hover : style::surface;
  auto brush = CreateSolidBrush(color(background));
  FillRect(draw->hDC, &draw->rcItem, brush);
  DeleteObject(brush);
  RECT bounds = draw->rcItem;
  bounds.left += value->px(style::inset);
  bounds.right -= value->px(style::inset);
  if (value->separator) {
    bounds.top = (bounds.top + bounds.bottom) / 2;
    bounds.bottom = bounds.top + 1; // crisp one-physical-pixel separator
    brush = CreateSolidBrush(color(style::rule));
    FillRect(draw->hDC, &bounds, brush);
    DeleteObject(brush);
    return true;
  }
  auto oldFont = SelectObject(draw->hDC, value->font);
  const auto oldMode = SetBkMode(draw->hDC, TRANSPARENT);
  const auto oldColor = SetTextColor(draw->hDC, color(enabled ? style::foreground : style::disabled));
  if (!value->shortcut.empty()) {
    SIZE size{};
    GetTextExtentPoint32W(draw->hDC, value->shortcut.c_str(), int(value->shortcut.size()), &size);
    RECT right = bounds;
    right.left = right.right - size.cx;
    SetTextColor(draw->hDC, color(enabled ? style::shortcut : style::disabled));
    DrawTextW(draw->hDC, value->shortcut.c_str(), -1, &right,
              DT_SINGLELINE | DT_VCENTER | DT_RIGHT | DT_NOPREFIX);
    bounds.right = right.left - value->px(style::shortcutGap);
    SetTextColor(draw->hDC, color(enabled ? style::foreground : style::disabled));
  }
  DrawTextW(draw->hDC, value->label.c_str(), -1, &bounds,
            DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_NOPREFIX | DT_END_ELLIPSIS);
  SetTextColor(draw->hDC, oldColor);
  SetBkMode(draw->hDC, oldMode);
  SelectObject(draw->hDC, oldFont);
  return true;
}
class Popup {
public:
  explicit Popup(HWND owner) : owner_(owner), dpi_(GetDpiForWindow(owner)), menu_(CreatePopupMenu()) {
    background_ = CreateSolidBrush(color(style::surface));
    MENUINFO info{sizeof info};
    info.fMask = MIM_BACKGROUND | MIM_STYLE;
    info.hbrBack = background_;
    info.dwStyle = MNS_NOCHECK;
    SetMenuInfo(menu_, &info);
    font_ = CreateFontW(-MulDiv(style::fontSize, int(dpi_), 96), 0, 0, 0,
        FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
        CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH,
        gsdl::theme::bodyFontFamily);
  }
  ~Popup() {
    DestroyMenu(menu_);
    DeleteObject(font_);
    DeleteObject(background_);
  }
  Popup(const Popup &) = delete;
  Popup &operator=(const Popup &) = delete;
  void add(UINT id, std::wstring label, bool enabled = true, std::wstring shortcut = {}) {
    append(id, std::move(label), enabled, std::move(shortcut), false);
  }
  void separator() { append(0, {}, false, {}, true); }
  UINT show(POINT screenPoint) {
    return UINT(TrackPopupMenuEx(menu_, TPM_RETURNCMD | TPM_RIGHTBUTTON,
                                screenPoint.x, screenPoint.y, owner_, nullptr));
  }
private:
  void append(UINT id, std::wstring label, bool enabled, std::wstring shortcut, bool separator) {
    auto value = std::make_unique<Item>();
    value->label = std::move(label);
    value->shortcut = std::move(shortcut);
    value->enabled = enabled;
    value->separator = separator;
    value->font = font_;
    value->dpi = dpi_;
    value->accessible = {MSAA_MENU_SIG, DWORD(value->label.size()), value->label.data()};
    MENUITEMINFOW info{sizeof info};
    info.fMask = MIIM_FTYPE | MIIM_ID | MIIM_STATE | MIIM_DATA | MIIM_STRING;
    info.fType = MFT_OWNERDRAW | (separator ? MFT_SEPARATOR : 0);
    info.fState = enabled ? MFS_ENABLED : MFS_DISABLED;
    info.wID = id;
    info.dwItemData = reinterpret_cast<ULONG_PTR>(value.get());
    info.dwTypeData = value->label.data();
    info.cch = UINT(value->label.size());
    InsertMenuItemW(menu_, UINT(items_.size()), TRUE, &info);
    items_.push_back(std::move(value));
  }
  HWND owner_{};
  UINT dpi_{};
  HMENU menu_{};
  HBRUSH background_{};
  HFONT font_{};
  std::vector<std::unique_ptr<Item>> items_;
};
} // namespace themed_menu
