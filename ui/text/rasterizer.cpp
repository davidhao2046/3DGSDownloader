#include "resources/theme/typography.hpp"
#include "text.hpp"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <dwrite.h>
#include <limits>
#include <stdexcept>
#include <wrl/client.h>

namespace gsdl::text {
using Microsoft::WRL::ComPtr;
namespace {
void check(HRESULT value) {
  if (FAILED(value))
    throw std::runtime_error("DirectWrite operation failed: " +
                             std::to_string(static_cast<unsigned>(value)));
}
void validate(std::wstring_view text, float size) {
  if (text.size() > 32768 || !std::isfinite(size) || size < 1 || size > 256)
    throw std::invalid_argument("Text layout bounds");
}
struct PixelRenderer final : IDWriteTextRenderer {
  std::atomic<ULONG> refs{1};
  IDWriteFactory *factory;
  std::span<std::uint8_t> pixels;
  unsigned width, height;
  float scale;
  Color color;
  Clip clip;
  PixelRenderer(IDWriteFactory *f, std::span<std::uint8_t> p, unsigned w,
                unsigned h, float s, Color c, Clip cl)
      : factory(f), pixels(p), width(w), height(h), scale(s), color(c),
        clip(cl) {}
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void **result) override {
    if (!result)
      return E_POINTER;
    *result = nullptr;
    if (id == __uuidof(IUnknown) || id == __uuidof(IDWritePixelSnapping) ||
        id == __uuidof(IDWriteTextRenderer)) {
      *result = static_cast<IDWriteTextRenderer *>(this);
      AddRef();
      return S_OK;
    }
    return E_NOINTERFACE;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return ++refs; }
  ULONG STDMETHODCALLTYPE Release() override {
    const auto r = --refs;
    if (!r)
      delete this;
    return r;
  }
  HRESULT STDMETHODCALLTYPE IsPixelSnappingDisabled(void *,
                                                    BOOL *out) override {
    *out = FALSE;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE GetCurrentTransform(void *,
                                                DWRITE_MATRIX *out) override {
    *out = {1, 0, 0, 1, 0, 0};
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE GetPixelsPerDip(void *, FLOAT *out) override {
    *out = scale;
    return S_OK;
  }
  void blend(int x, int y, unsigned coverage) {
    if (x < clip.left || y < clip.top || x >= clip.right || y >= clip.bottom ||
        x < 0 || y < 0 || x >= static_cast<int>(width) ||
        y >= static_cast<int>(height))
      return;
    const auto at = (static_cast<std::size_t>(y) * width + x) * 4;
    const unsigned a = (coverage * color.a + 127) / 255;
    const unsigned inv = 255 - a;
    pixels[at] =
        static_cast<std::uint8_t>((color.r * a + pixels[at] * inv + 127) / 255);
    pixels[at + 1] = static_cast<std::uint8_t>(
        (color.g * a + pixels[at + 1] * inv + 127) / 255);
    pixels[at + 2] = static_cast<std::uint8_t>(
        (color.b * a + pixels[at + 2] * inv + 127) / 255);
    pixels[at + 3] =
        static_cast<std::uint8_t>(a + (pixels[at + 3] * inv + 127) / 255);
  }
  HRESULT STDMETHODCALLTYPE DrawGlyphRun(void *, FLOAT x, FLOAT y,
                                         DWRITE_MEASURING_MODE mode,
                                         const DWRITE_GLYPH_RUN *run,
                                         const DWRITE_GLYPH_RUN_DESCRIPTION *,
                                         IUnknown *) override {
    try {
      ComPtr<IDWriteGlyphRunAnalysis> analysis;
      const auto hr = factory->CreateGlyphRunAnalysis(
          run, scale, nullptr, DWRITE_RENDERING_MODE_NATURAL_SYMMETRIC, mode, x,
          y, &analysis);
      if (FAILED(hr))
        return hr;
      RECT bounds{};
      auto result = analysis->GetAlphaTextureBounds(
          DWRITE_TEXTURE_CLEARTYPE_3x1, &bounds);
      if (FAILED(result))
        return result;
      // Intersect before allocating; offscreen/huge text cannot allocate
      // an unbounded texture just to clip it afterwards.
      bounds.left =
          (std::max)(bounds.left, static_cast<LONG>((std::max)(0, clip.left)));
      bounds.top =
          (std::max)(bounds.top, static_cast<LONG>((std::max)(0, clip.top)));
      bounds.right =
          (std::min)(bounds.right,
                     static_cast<LONG>(
                         (std::min)(static_cast<int>(width), clip.right)));
      bounds.bottom =
          (std::min)(bounds.bottom,
                     static_cast<LONG>(
                         (std::min)(static_cast<int>(height), clip.bottom)));
      if (bounds.right <= bounds.left || bounds.bottom <= bounds.top)
        return S_OK;
      const auto w = static_cast<unsigned>(bounds.right - bounds.left),
                 h = static_cast<unsigned>(bounds.bottom - bounds.top);
      const auto size = static_cast<std::size_t>(w) * h * 3;
      if (size > 64 * 1024 * 1024)
        return E_OUTOFMEMORY;
      std::vector<BYTE> mask(size);
      result =
          analysis->CreateAlphaTexture(DWRITE_TEXTURE_CLEARTYPE_3x1, &bounds,
                                       mask.data(), static_cast<UINT32>(size));
      if (FAILED(result))
        return result;
      for (unsigned row = 0; row < h; ++row)
        for (unsigned col = 0; col < w; ++col) {
          const auto at = (static_cast<std::size_t>(row) * w + col) * 3;
          blend(bounds.left + col, bounds.top + row,
                (static_cast<unsigned>(mask[at]) + mask[at + 1] + mask[at + 2] +
                 1) /
                    3);
        }
      return S_OK;
    } catch (...) {
      return E_FAIL;
    }
  }
  HRESULT STDMETHODCALLTYPE DrawUnderline(void *, FLOAT x, FLOAT y,
                                          const DWRITE_UNDERLINE *line,
                                          IUnknown *) override {
    for (int py = static_cast<int>(std::floor((y + line->offset) * scale));
         py < static_cast<int>(
                  std::ceil((y + line->offset + line->thickness) * scale));
         ++py)
      for (int px = static_cast<int>(std::floor(x * scale));
           px < static_cast<int>(std::ceil((x + line->width) * scale)); ++px)
        blend(px, py, 255);
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE DrawStrikethrough(void *, FLOAT x, FLOAT y,
                                              const DWRITE_STRIKETHROUGH *line,
                                              IUnknown *) override {
    DWRITE_UNDERLINE u{};
    u.width = line->width;
    u.thickness = line->thickness;
    u.offset = line->offset;
    return DrawUnderline(nullptr, x, y, &u, nullptr);
  }
  HRESULT STDMETHODCALLTYPE DrawInlineObject(void *, FLOAT, FLOAT,
                                             IDWriteInlineObject *, BOOL, BOOL,
                                             IUnknown *) override {
    return E_NOTIMPL;
  }
};
} // namespace
struct Rasterizer::Impl {
  ComPtr<IDWriteFactory> factory;
  Impl() {
    check(DWriteCreateFactory(
        DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
        reinterpret_cast<IUnknown **>(factory.GetAddressOf())));
  }
  ComPtr<IDWriteTextLayout> layout(std::wstring_view text, float size, unsigned weight = 400) {
    validate(text, size);
    if (weight < 100 || weight > 900)
      throw std::invalid_argument("Invalid font weight");
    ComPtr<IDWriteTextFormat> format;
    check(factory->CreateTextFormat(
        gsdl::theme::bodyFontFamily, nullptr, static_cast<DWRITE_FONT_WEIGHT>(weight),
        DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size,
        gsdl::theme::textLocale, &format));
    check(format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP));
    ComPtr<IDWriteTextLayout> out;
    check(factory->CreateTextLayout(text.data(),
                                    static_cast<UINT32>(text.size()),
                                    format.Get(), 1000000.f, 512.f, &out));
    return out;
  }
};
Rasterizer::Rasterizer() : impl_(std::make_unique<Impl>()) {}
Rasterizer::~Rasterizer() = default;
Metrics Rasterizer::measure(std::wstring_view value, float size, unsigned weight) {
  auto layout = impl_->layout(value, size, weight);
  DWRITE_TEXT_METRICS m{};
  check(layout->GetMetrics(&m));
  DWRITE_LINE_METRICS line{};
  UINT32 count{};
  const auto hr = layout->GetLineMetrics(&line, 1, &count);
  if (FAILED(hr) && hr != E_NOT_SUFFICIENT_BUFFER)
    check(hr);
  return {m.widthIncludingTrailingWhitespace, m.height, line.baseline};
}
std::size_t Rasterizer::hit_test(std::wstring_view value, float size, float x) {
  auto layout = impl_->layout(value, size);
  BOOL trailing{}, inside{};
  DWRITE_HIT_TEST_METRICS m{};
  check(layout->HitTestPoint(x, 0, &trailing, &inside, &m));
  return (std::min)(value.size(),
                    static_cast<std::size_t>(m.textPosition +
                                             (trailing ? m.length : 0)));
}
std::size_t Rasterizer::fit_prefix(std::wstring_view value, float size, float width,
                                  unsigned weight) {
  if (!std::isfinite(width) || width <= 0 || value.empty()) return 0;
  auto layout = impl_->layout(value, size, weight);
  UINT32 count{};
  auto hr=layout->GetClusterMetrics(nullptr,0,&count);
  if(FAILED(hr) && hr!=E_NOT_SUFFICIENT_BUFFER) check(hr);
  std::vector<DWRITE_CLUSTER_METRICS> clusters(count);
  if(count) check(layout->GetClusterMetrics(clusters.data(),count,&count));
  float used=0;
  std::size_t end=0;
  for(const auto &cluster:clusters) {
    if(used+cluster.width>width) break;
    used+=cluster.width;end+=cluster.length;
  }
  return std::min(end,value.size());
}
float Rasterizer::caret_x(std::wstring_view value, float size,
                          std::size_t position) {
  auto layout = impl_->layout(value, size);
  FLOAT x{}, y{};
  DWRITE_HIT_TEST_METRICS m{};
  check(layout->HitTestTextPosition(
      static_cast<UINT32>((std::min)(position, value.size())), FALSE, &x, &y,
      &m));
  return x;
}
void Rasterizer::draw(std::span<std::uint8_t> pixels, unsigned width,
                      unsigned height, std::wstring_view value, float x,
                      float y, float size, Color color, float scale,
                      std::optional<Clip> clip, unsigned weight) {
  if (!width || !height ||
      static_cast<std::size_t>(width) * height > 16777216 ||
      pixels.size() != static_cast<std::size_t>(width) * height * 4 ||
      !std::isfinite(scale) || scale <= 0 || scale > 8 || !std::isfinite(x) ||
      !std::isfinite(y))
    throw std::invalid_argument("Invalid text surface");
  if (value.empty())
    return;
  auto layout = impl_->layout(value, size, weight);
  ComPtr<IDWriteTextRenderer> renderer;
  renderer.Attach(new PixelRenderer(
      impl_->factory.Get(), pixels, width, height, scale, color,
      clip.value_or(
          Clip{0, 0, static_cast<int>(width), static_cast<int>(height)})));
  check(layout->Draw(nullptr, renderer.Get(), x, y));
}
} // namespace gsdl::text
