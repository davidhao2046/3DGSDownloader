#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <string_view>

namespace gsdl::graphics {
struct Rgba { std::uint8_t r{}, g{}, b{}, a{255}; };
struct Rect { float left{}, top{}, right{}, bottom{}; };
enum class PathOp { move, line, quad, cubic, close };
struct PathCommand {
    PathOp op{PathOp::move};
    // move/line: x,y; quad: control x,y,end x,y; cubic: c1,c2,end.
    std::array<float,6> points{};
};

// Replaceable development raster backend, not the recovered native UI ABI.
// C++ controls own layout and interaction; no managed/system controls are used.
// DLL must be an absolute hash-locked path. Loaded with DLL-directory + System32.
// Font shaping/rasterization is supplied separately by DirectWrite; no font files.
// Calls require caller synchronization. Pixels are RGBA8888, premultiplied,
// top-to-bottom, with row_bytes()==width()*4; they remain owned by this object.
class SkiaCanvas final {
public:
    explicit SkiaCanvas(const std::filesystem::path& absolute_dll);
    ~SkiaCanvas();
    SkiaCanvas(SkiaCanvas&&) noexcept;
    SkiaCanvas& operator=(SkiaCanvas&&) noexcept;
    SkiaCanvas(const SkiaCanvas&) = delete;
    SkiaCanvas& operator=(const SkiaCanvas&) = delete;

    void resize(std::uint32_t width, std::uint32_t height);
    void clear(Rgba color);
    // Bounded opaque viewport copy into this raster surface, in physical pixels.
    // Does not change the Skia transform/clip; caller supplies the viewport rect.
    void write_rgba(std::span<const std::uint8_t> rgba,std::uint32_t source_width,
                    std::uint32_t source_height,std::uint32_t x,std::uint32_t y,
                    std::uint32_t width,std::uint32_t height);
    void rect(Rect bounds, Rgba color);
    void rounded_rect(Rect bounds, float radius_x, float radius_y, Rgba color);
    void line(float x1, float y1, float x2, float y2, Rgba color, float width = 1);
    void path(std::span<const PathCommand> commands, Rgba color, float stroke_width = 0);
    // SVG transform order [a,b,c,d,e,f]: x'=a*x+c*y+e, y'=b*x+d*y+f.
    void svg_path(std::string_view d, const std::array<float,6>& affine,
                  Rgba color, bool even_odd = true, float stroke_width = 0);
    void save();
    void clip(Rect bounds);
    void scale(float factor);
    void restore();

    std::span<const std::uint8_t> pixels() const noexcept;
    // Rendering-thread-only access for DirectWrite glyph alpha composition.
    // Physical RGBA premultiplied; no Skia transforms/clips applied by this access.
    std::span<std::uint8_t> mutable_pixels() noexcept;
    std::uint32_t width() const noexcept;
    std::uint32_t height() const noexcept;
    std::size_t row_bytes() const noexcept;
    std::string backend_version() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace gsdl::graphics
