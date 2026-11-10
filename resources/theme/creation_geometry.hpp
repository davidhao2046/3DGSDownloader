#pragma once
#include "tokens.hpp"

namespace gsdl::theme::creation {
// Native creation panel, in DIP. Drawing, hit testing, UIA and visual fixtures
// consume this one contract. Rows extend the collapsed panel above its footer.
inline constexpr float width=640, collapsedHeight=392, minimumTop=24;
inline constexpr float inset=24, right=width-inset;
inline constexpr float headerHeight=56, footerHeight=64;
inline constexpr float controlHeight=control::height, controlGap=control::gap;
inline constexpr float actionInset=16, actionGap=12;
inline constexpr float titleTop=16, titleRight=560, titleHeight=controlHeight;
inline constexpr float labelHeight=22, labelFieldGap=4;
inline constexpr float linkLabelTop=72, linkTop=linkLabelTop+labelHeight+labelFieldGap;
inline constexpr float hintTop=136, hintHeight=24;
inline constexpr float directoryLabelTop=170;
inline constexpr float directoryTop=directoryLabelTop+labelHeight+labelFieldGap;
inline constexpr float browseWidth=108, recentWidth=64;
inline constexpr float browseLeft=right-browseWidth;
inline constexpr float recentRight=browseLeft-controlGap, recentLeft=recentRight-recentWidth;
inline constexpr float directoryRight=recentLeft-controlGap;
inline constexpr float previewTop=234, previewHeight=24;
inline constexpr float disclosureTop=272, filesTop=312, fileHeight=30;
inline constexpr float submitWidth=116, submitLeft=right-submitWidth;
inline constexpr float cancelWidth=104, cancelRight=submitLeft-actionGap;
inline constexpr float cancelLeft=cancelRight-cancelWidth;
inline constexpr float closeWidth=control::iconHit, closeLeft=right-closeWidth;
inline constexpr float borderWidth=line, panelRadius=radius;
inline constexpr float panelBorderRadius=panelRadius+borderWidth;
inline constexpr float fieldRadius=radius, fieldInnerRadius=fieldRadius-borderWidth;
inline constexpr float fieldTextInset=8, fileHoverRadius=radius;
inline constexpr float checkboxInset=8, checkboxTop=7, checkboxSize=control::iconSize;
inline constexpr float checkboxRadius=2, checkboxInnerRadius=checkboxRadius-borderWidth;
inline constexpr float fileNameInset=36, fileNameRightReserve=100;
inline constexpr float fileSizeRightReserve=96, fileSizeInset=8;
inline constexpr float titleSize=control::titleSize, bodySize=control::bodySize;
inline constexpr float hintSize=control::hintSize;
inline constexpr unsigned titleWeight=control::emphasisWeight;
inline constexpr auto panelFill=popup, panelBorder=border, separator=border;
inline constexpr auto foreground=text, secondaryForeground=control::muted;
inline constexpr auto errorForeground=error, placeholderForeground=control::placeholder;
inline constexpr auto rowHover=hover, fieldFill=window, fieldBorder=border, fieldFocus=accent;
inline constexpr auto checkboxFill=window, checkboxBorder=border, checkboxMark=text;
inline constexpr graphics::Rgba modalMask{18,18,18,200};
} // namespace gsdl::theme::creation
