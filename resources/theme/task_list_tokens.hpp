#pragma once
#include "tokens.hpp"
#include "creation_geometry.hpp"

namespace gsdl::theme::taskList {
// Original task-list geometry, in device-independent pixels. All interaction
// states share these bounds; focus and pressed feedback only change their fill.
inline constexpr float sidebarWidth = 168, margin = 24, sidebarInset = 16;
inline constexpr float headerHeight = 76, footerHeight = 8, statusHeight = 30;
inline constexpr float rowHeight = 64, rowGap = 4, rowInset = 16;
inline constexpr float buttonHeight = 34, buttonGap = 10;
inline constexpr float primaryTop = 24, primaryHeight = 38;
inline constexpr float filterTop = 108, filterHeight = 40, filterGap = 8;
inline constexpr float titleSize = 18, rowTitleSize = 14;
inline constexpr float titleTop = 25, headerCountTop = 28;
inline constexpr float rowTitleTop = 12, rowDirectoryTop = 35;
inline constexpr float rowProgressTop = 34, progressHeight = 3;
inline constexpr float progressBottom = 10, statusWidth = 132;
inline constexpr float footerInset = 20, buttonTextInset = 12;
inline constexpr float buttonTextTop = 8, statusTextTop = 7;
inline constexpr float emptyTop = 92, emptyLineGap = 32;
inline constexpr float scrollWidth = 3, scrollInset = 8;
inline constexpr graphics::Rgba secondaryText{161, 161, 161};
inline constexpr auto rowSurface = popup;
inline constexpr auto rowSelected = form;
inline constexpr auto sidebarSurface = panel;
inline constexpr auto contentSurface = window;
inline constexpr auto buttonSurface = form;
inline constexpr graphics::Rgba successText{141,204,165};
inline constexpr float fileHeight = 30;
} // namespace gsdl::theme::taskList

