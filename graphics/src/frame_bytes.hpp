#pragma once
#include <gsdl/graphics/win32_presenter.hpp>

namespace gsdl::graphics::detail {
inline std::size_t checked_frame_bytes(std::uint32_t width, std::uint32_t height) {
    const auto count = static_cast<std::uint64_t>(width) * height;
    if (!width || !height || count > max_frame_pixels)
        throw PresentationError(ErrorCode::invalid_input, "Presentation extent exceeds the nonzero 16777216-pixel budget");
    return static_cast<std::size_t>(count) * 4;
}
inline void validate_frame(const Rgba8Frame& frame) {
    const auto size = checked_frame_bytes(frame.width, frame.height);
    if (frame.rgba.size() != size)
        throw PresentationError(ErrorCode::invalid_input, "RGBA frame byte count differs from dimensions");
    for (std::size_t i = 3; i < size; i += 4)
        if (frame.rgba[i] != 255) throw PresentationError(ErrorCode::invalid_input, "Display frame must have opaque alpha");
}
// Internal, shared with an independent golden unit test. Preconditions: validated
// source. Pixel centers determine nearest source texel; no Y flip or filtering.
inline std::vector<std::uint8_t> resample(const Rgba8Frame& frame, std::uint32_t width, std::uint32_t height, bool bgra) {
    std::vector<std::uint8_t> result(checked_frame_bytes(width, height));
    for (std::uint32_t y = 0; y < height; ++y) {
        const auto sy = static_cast<std::uint32_t>((2ULL * y + 1) * frame.height / (2ULL * height));
        for (std::uint32_t x = 0; x < width; ++x) {
            const auto sx = static_cast<std::uint32_t>((2ULL * x + 1) * frame.width / (2ULL * width));
            const auto from = (static_cast<std::size_t>(sy) * frame.width + sx) * 4;
            const auto to = (static_cast<std::size_t>(y) * width + x) * 4;
            result[to] = frame.rgba[from + (bgra ? 2 : 0)]; result[to + 1] = frame.rgba[from + 1];
            result[to + 2] = frame.rgba[from + (bgra ? 0 : 2)]; result[to + 3] = 255;
        }
    }
    return result;
}
} // namespace gsdl::graphics::detail
