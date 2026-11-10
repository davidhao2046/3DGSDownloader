#pragma once
#include "tokens.hpp"

namespace gsdl::theme::menu {
inline constexpr auto surface = popup;
inline constexpr auto hover = gsdl::theme::hover;
inline constexpr auto foreground = text;
inline constexpr auto disabled = gsdl::theme::disabled;
inline constexpr auto rule = border;
inline constexpr graphics::Rgba shortcut{161, 161, 161};
inline constexpr int rowHeight = 32, separatorHeight = 9, inset = 12;
inline constexpr int minimumWidth = 208, maximumWidth = 480, shortcutGap = 32;
inline constexpr int fontSize = 13;
} // namespace gsdl::theme::menu
