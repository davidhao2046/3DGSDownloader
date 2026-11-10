#pragma once

namespace gsdl::theme {
// Text uses the Windows system collection and DirectWrite locale fallback.
// These names do not imply that a font file is bundled or hash-locked.
// This leaf header has no graphics dependency, so text layout can share it.
inline constexpr wchar_t bodyFontFamily[] = L"Segoe UI";
inline constexpr wchar_t textLocale[] = L"zh-cn";
} // namespace gsdl::theme
