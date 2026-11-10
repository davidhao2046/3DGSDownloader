#pragma once
#include "typography.hpp"
#include <gsdl/graphics/skia_canvas.hpp>
namespace gsdl::theme {
using graphics::Rgba;
inline constexpr Rgba window{18, 18, 18}, panel{26, 26, 26}, popup{31, 31, 31},
    form{38, 38, 38};
inline constexpr Rgba text{218, 218, 218}, disabled{103, 103, 103},
    hover{60, 70, 84}, accent{99, 130, 160};
inline constexpr Rgba pressed{125, 165, 200}, error{255, 130, 130},
    selection{60, 70, 84}, border{60, 60, 60};
inline constexpr float body = 13, fontSmall = 12, line = 1, radius = 3,
                       row = 30, fieldHeight = 32;
// Product component colors. Reference accent remains a focus/selection marker;
// solid button surfaces have independent foreground-safe state colors.
namespace control {
inline constexpr Rgba primary{64, 93, 120}, primaryHover{67, 96, 122},
    primaryPressed{52, 77, 99}, onPrimary{218, 218, 218};
inline constexpr Rgba secondary{38, 38, 38}, secondaryHover{52, 60, 70},
    secondaryPressed{43, 51, 61}, disabledFill{32, 32, 32},
    disabledText{117, 117, 117}, placeholder{146, 146, 146},
    muted{161, 161, 161}, running{135, 169, 199},
    success{141, 204, 165}, focus{135, 169, 199},
    selected{43, 53, 64}, edge{64, 64, 64};
inline constexpr float height=32, iconSize=16, iconHit=32, radius=3,
    inset=12, gap=8, focusWidth=1, iconStroke=1.5f;
inline constexpr float bodySize=13, hintSize=12, titleSize=18;
inline constexpr unsigned normalWeight=400, emphasisWeight=600;
}
namespace windowMetrics {
inline constexpr int baseDpi = 96;
inline constexpr int defaultWidth = 1280, defaultHeight = 800;
inline constexpr int minimumWidth = 960, minimumHeight = 640;
// Initial native window placement is in physical screen coordinates.
inline constexpr int initialScreenX = 80, initialScreenY = 60;
} // namespace windowMetrics

// All layout values below are DIP. Drawing, hit testing, caret placement and
// UIA share one set of semantic metrics.
namespace layout {
inline constexpr float fieldLeft = 120, rightMargin = 24;
inline constexpr float linkTop = 20, outputTop = 68;
inline constexpr float outputRightReserve = 132, applyLeftReserve = 116;
inline constexpr float historyLeft = 16, historyTop = 116;
inline constexpr float historyButtonWidth = 64, historyButtonHeight = 28,
                       historyGap = 8;
inline constexpr float resourceHitLeft = 16, resourceHitTop = 190,
                       resourceHitRightReserve = 326, resourceHitBottom = 370;
inline constexpr float formHeight = 156, labelLeft = 20, labelTopInset = 8;
inline constexpr float fieldTextLeftInset = 7, fieldTextTopInset = 7;
inline constexpr float fieldClipHorizontalInset = 5, fieldClipVerticalInset = 2;
inline constexpr float fieldScrollReserve = 16, selectionVerticalInset = 5,
                       caretVerticalInset = 6, imeLineHeight = 25;
inline constexpr float compositionBottomInset = 4;
inline constexpr float buttonTextLeftInset = 17, buttonTextTopInset = 6;
inline constexpr float detailsWidth = 310, statusHeight = 40;
inline constexpr float sectionTitleLeft = 20, sectionTitleTop = 166;
inline constexpr float resourceNameLeft = 24, columnHeaderTop = 202,
                       formatColumnRightOffset = 145,
                       stateColumnRightOffset = 80;
inline constexpr float resourceRowInset = 16, headerRuleTop = 228;
inline constexpr float detailsTextInset = 16, detailsValueTop = 208;
inline constexpr float firstResourceTop = 236, resourceTextTopInset = 7,
                       emptyResourceTop = 248;
inline constexpr float statusTextLeft = 16, statusTextBottomOffset = 28;
} // namespace layout
namespace planLayout {
inline constexpr float margin = 16, gap = 8, toolbarTop = 16, buttonWidth = 80,
                       buttonHeight = 28, linkTop = 60, directoryTop = 108,
                       fieldLeft = 120, contentTop = 160, titleTop = 174,
                       headerTop = 210, firstRow = 240, checkboxSize = 16,
                       checkboxInset = 8, nameLeft = 52, ruleTop = 234,
                       inspectorWidth = 320, inspectorInset = 16,
                       inspectorNameTop = 214, outputLabelTop = 250,
                       outputFieldTop = 274, includedTop = 322,
                       inspectorHelpTop = 364, sizeTop = 390, resultTop = 420,
                       transferButtonWidth = 96, tableBottomReserve = 96,
                       statusHeight = 80;
}
} // namespace gsdl::theme
