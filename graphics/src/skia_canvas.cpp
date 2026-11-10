#include <gsdl/graphics/skia_canvas.hpp>
#include "include/c/sk_canvas.h"
#include "include/c/sk_paint.h"
#include "include/c/sk_path.h"
#include "include/c/sk_surface.h"
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <bcrypt.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <vector>

namespace gsdl::graphics {
namespace {
constexpr std::string_view dll_hash = GSDL_SKIA_SHA256;
void require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
void finite(std::initializer_list<float> values) {
    for (float value : values) require(std::isfinite(value), "Nonfinite drawing coordinate");
}
sk_rect_t native_rect(Rect r) {
    finite({r.left,r.top,r.right,r.bottom});
    require(r.right >= r.left && r.bottom >= r.top, "Inverted drawing rectangle");
    return {r.left,r.top,r.right,r.bottom};
}
sk_color_t color(Rgba c) {
    return (std::uint32_t(c.a)<<24) | (std::uint32_t(c.r)<<16) | (std::uint32_t(c.g)<<8) | c.b;
}
std::string sha256(std::span<const std::uint8_t> bytes) {
    BCRYPT_ALG_HANDLE algorithm{};
    require(BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) >= 0,
            "Cannot open SHA256 provider");
    std::array<std::uint8_t,32> hash{};
    const auto status = BCryptHash(algorithm, nullptr, 0,
        const_cast<PUCHAR>(bytes.data()), static_cast<ULONG>(bytes.size()), hash.data(), static_cast<ULONG>(hash.size()));
    BCryptCloseAlgorithmProvider(algorithm,0);
    require(status >= 0, "Cannot hash locked resource");
    constexpr char hex[] = "0123456789abcdef";
    std::string result; result.reserve(64);
    for (auto b : hash) { result.push_back(hex[b>>4]); result.push_back(hex[b&15]); }
    return result;
}
std::vector<std::uint8_t> locked_file(const std::filesystem::path& path, std::string_view expected) {
    require(path.is_absolute(), "Backend paths must be absolute");
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    require(bool(input), "Cannot open locked backend resource");
    const auto size = input.tellg();
    require(size > 0 && size <= std::streamoff(32*1024*1024), "Locked resource exceeds size bound");
    std::vector<std::uint8_t> data(static_cast<std::size_t>(size)); input.seekg(0);
    require(bool(input.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(data.size()))) &&
            input.peek()==std::char_traits<char>::eof(), "Resource changed during read");
    require(sha256(data) == expected, "Backend SHA256 differs from dependency lock");
    return data;
}
struct Module {
    HMODULE value{};
    ~Module() { if (value) FreeLibrary(value); }
};
template<class T> using Owned = std::unique_ptr<T,void(*)(T*)>;
template<class T> T symbol(HMODULE module, const char* name) {
    const auto address=GetProcAddress(module,name);
    if (!address) throw std::runtime_error(std::string("Missing locked Skia export: ")+name);
    // Windows x64 GetProcAddress returns a generic function pointer. Copying
    // preserves its representation without introducing a calling-convention cast.
    static_assert(sizeof(address)==sizeof(T));
    T result{}; std::memcpy(&result,&address,sizeof(result)); return result;
}
} // namespace

#define SKIA_FUNCTIONS(X) \
    X(sk_surface_new_raster_direct) X(sk_surface_unref) X(sk_surface_get_canvas) \
    X(sk_canvas_clear) X(sk_canvas_draw_rect) X(sk_canvas_draw_round_rect) X(sk_canvas_draw_line) \
    X(sk_canvas_draw_path) X(sk_canvas_save) X(sk_canvas_restore) \
    X(sk_canvas_clip_rect_with_operation) X(sk_canvas_scale) X(sk_canvas_concat) \
    X(sk_paint_new) X(sk_paint_delete) X(sk_paint_set_antialias) X(sk_paint_set_color) \
    X(sk_paint_set_style) X(sk_paint_set_stroke_width) \
    X(sk_path_new) X(sk_path_delete) X(sk_path_move_to) X(sk_path_line_to) X(sk_path_quad_to) \
    X(sk_path_cubic_to) X(sk_path_close) X(sk_path_parse_svg_string) X(sk_path_set_filltype)

struct SkiaCanvas::Impl {
    // Module precedes all Skia resources so it outlives their destructors.
    Module module;
#define DECLARE(name) decltype(&::name) name{};
    SKIA_FUNCTIONS(DECLARE)
#undef DECLARE
    std::vector<std::uint8_t> pixels;
    Owned<sk_surface_t> surface{nullptr,nullptr};
    Owned<sk_paint_t> paint{nullptr,nullptr};
    sk_canvas_t* canvas{}; // Borrowed from surface.
    std::uint32_t width{},height{};
    unsigned saves{};
    std::string version;

    Impl(const std::filesystem::path& dll) {
        (void)locked_file(dll,dll_hash);
        module.value=LoadLibraryExW(dll.c_str(),nullptr,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_SYSTEM32);
        require(module.value!=nullptr,"Cannot load locked native Skia backend");
        const auto get_version=symbol<const char*(*)()>(module.value,"sk_version_get_string");
        const auto milestone=symbol<int(*)()>(module.value,"sk_version_get_milestone");
        const auto increment=symbol<int(*)()>(module.value,"sk_version_get_increment");
        const char* found=get_version();
        require(found && std::string_view(found)=="88.1" && milestone()==88 && increment()==1,
                "Skia native ABI version differs from lock");
        version=found;
#define LOAD(name) name=symbol<decltype(name)>(module.value,#name);
        SKIA_FUNCTIONS(LOAD)
#undef LOAD
        paint=Owned<sk_paint_t>(sk_paint_new(),sk_paint_delete);
        require(bool(paint),"Cannot create Skia paint");
        sk_paint_set_antialias(paint.get(),true);
    }
    void ready() const { require(canvas!=nullptr,"Call resize before drawing"); }
    sk_paint_t* brush(Rgba c,float stroke_width=0) {
        sk_paint_set_color(paint.get(),color(c));
        sk_paint_set_style(paint.get(),stroke_width>0?STROKE_SK_PAINT_STYLE:FILL_SK_PAINT_STYLE);
        sk_paint_set_stroke_width(paint.get(),stroke_width);
        return paint.get();
    }
    Owned<sk_path_t> new_path() {
        Owned<sk_path_t> value(sk_path_new(),sk_path_delete);
        require(bool(value),"Cannot allocate Skia path"); return value;
    }
};
#undef SKIA_FUNCTIONS

SkiaCanvas::SkiaCanvas(const std::filesystem::path& dll)
    : impl_(std::make_unique<Impl>(dll)) {}
SkiaCanvas::~SkiaCanvas()=default;
SkiaCanvas::SkiaCanvas(SkiaCanvas&&) noexcept=default;
SkiaCanvas& SkiaCanvas::operator=(SkiaCanvas&&) noexcept=default;
void SkiaCanvas::resize(std::uint32_t width,std::uint32_t height) {
    require(width && height && width<=16384 && height<=16384 && std::uint64_t(width)*height<=64*1024*1024,
            "Raster dimensions exceed backend bounds");
    auto& p=*impl_;
    if(p.width==width && p.height==height)return;
    std::vector<std::uint8_t> next(static_cast<std::size_t>(width)*height*4);
    const sk_imageinfo_t info{nullptr,static_cast<std::int32_t>(width),static_cast<std::int32_t>(height),
                             RGBA_8888_SK_COLORTYPE,PREMUL_SK_ALPHATYPE};
    Owned<sk_surface_t> surface(p.sk_surface_new_raster_direct(&info,next.data(),std::size_t(width)*4,
                                                            nullptr,nullptr,nullptr),p.sk_surface_unref);
    require(bool(surface),"Cannot create Skia raster surface");
    auto* canvas=p.sk_surface_get_canvas(surface.get());
    require(canvas!=nullptr,"Cannot obtain Skia raster canvas");
    p.surface.reset(); // Release old surface before replacing its backing bytes.
    p.pixels.swap(next);p.surface=std::move(surface);p.canvas=canvas;
    p.width=width;p.height=height;p.saves=0;
}
void SkiaCanvas::clear(Rgba c) { auto& p=*impl_;p.ready();p.sk_canvas_clear(p.canvas,color(c)); }
void SkiaCanvas::rect(Rect r,Rgba c) {
    const auto bounds=native_rect(r);auto& p=*impl_;p.ready();p.sk_canvas_draw_rect(p.canvas,&bounds,p.brush(c));
}
void SkiaCanvas::rounded_rect(Rect r,float rx,float ry,Rgba c) {
    const auto bounds=native_rect(r);finite({rx,ry});require(rx>=0&&ry>=0,"Negative corner radius");
    auto& p=*impl_;p.ready();p.sk_canvas_draw_round_rect(p.canvas,&bounds,rx,ry,p.brush(c));
}
void SkiaCanvas::line(float x1,float y1,float x2,float y2,Rgba c,float width) {
    finite({x1,y1,x2,y2,width});require(width>0,"Line width must be positive");
    auto& p=*impl_;p.ready();p.sk_canvas_draw_line(p.canvas,x1,y1,x2,y2,p.brush(c,width));
}
void SkiaCanvas::path(std::span<const PathCommand> commands,Rgba c,float stroke_width) {
    finite({stroke_width});require(stroke_width>=0,"Negative path stroke width");
    require(commands.size()<=65536,"Path exceeds command bound");
    auto& p=*impl_;p.ready();auto path=p.new_path();
    for(const auto& command:commands){
        const auto& x=command.points;
        for(float v:x)require(std::isfinite(v),"Nonfinite path coordinate");
        switch(command.op){
        case PathOp::move:p.sk_path_move_to(path.get(),x[0],x[1]);break;
        case PathOp::line:p.sk_path_line_to(path.get(),x[0],x[1]);break;
        case PathOp::quad:p.sk_path_quad_to(path.get(),x[0],x[1],x[2],x[3]);break;
        case PathOp::cubic:p.sk_path_cubic_to(path.get(),x[0],x[1],x[2],x[3],x[4],x[5]);break;
        case PathOp::close:p.sk_path_close(path.get());break;
        default:throw std::runtime_error("Unknown path operation");
        }
    }
    p.sk_canvas_draw_path(p.canvas,path.get(),p.brush(c,stroke_width));
}
void SkiaCanvas::svg_path(std::string_view d,const std::array<float,6>& a,Rgba c,bool even_odd,float stroke_width) {
    require(d.size()<=1024*1024 && d.find('\0')==std::string_view::npos,"Invalid SVG path byte extent");
    require(std::isfinite(stroke_width)&&stroke_width>=0,"Invalid SVG stroke width");
    for(float v:a)require(std::isfinite(v),"Nonfinite SVG transform");
    auto& p=*impl_;p.ready();auto path=p.new_path();const std::string terminated(d);
    require(p.sk_path_parse_svg_string(path.get(),terminated.c_str()),"Invalid SVG path data");
    p.sk_path_set_filltype(path.get(),even_odd?EVENODD_SK_PATH_FILLTYPE:WINDING_SK_PATH_FILLTYPE);
    const sk_matrix_t matrix{a[0],a[2],a[4],a[1],a[3],a[5],0,0,1};
    p.sk_canvas_save(p.canvas);p.sk_canvas_concat(p.canvas,&matrix);
    p.sk_canvas_draw_path(p.canvas,path.get(),p.brush(c,stroke_width));p.sk_canvas_restore(p.canvas);
}
void SkiaCanvas::save() {
    auto& p=*impl_;p.ready();require(p.saves<256,"Canvas save depth exceeded");p.sk_canvas_save(p.canvas);++p.saves;
}
void SkiaCanvas::clip(Rect r) {
    const auto bounds=native_rect(r);auto& p=*impl_;p.ready();
    p.sk_canvas_clip_rect_with_operation(p.canvas,&bounds,INTERSECT_SK_CLIPOP,true);
}
void SkiaCanvas::scale(float factor) {
    require(std::isfinite(factor)&&factor>0,"Invalid drawing scale");auto& p=*impl_;p.ready();p.sk_canvas_scale(p.canvas,factor,factor);
}
void SkiaCanvas::restore() {
    auto& p=*impl_;p.ready();require(p.saves!=0,"Unbalanced canvas restore");p.sk_canvas_restore(p.canvas);--p.saves;
}
std::span<std::uint8_t> SkiaCanvas::mutable_pixels() noexcept { return impl_?std::span<std::uint8_t>(impl_->pixels):std::span<std::uint8_t>{}; }
std::span<const std::uint8_t> SkiaCanvas::pixels() const noexcept { return impl_?std::span<const std::uint8_t>(impl_->pixels):std::span<const std::uint8_t>{}; }
void SkiaCanvas::write_rgba(std::span<const std::uint8_t> source,std::uint32_t sw,std::uint32_t sh,std::uint32_t x,std::uint32_t y,std::uint32_t w,std::uint32_t h){
    auto& p=*impl_;p.ready();require(sw&&sh&&w&&h&&source.size()==std::uint64_t(sw)*sh*4,"Invalid viewport RGBA frame");
    require(x<=p.width&&w<=p.width-x&&y<=p.height&&h<=p.height-y,"Viewport copy outside raster surface");
    for(std::uint32_t dy=0;dy<h;++dy){const auto sy=std::uint64_t(dy)*sh/h;for(std::uint32_t dx=0;dx<w;++dx){const auto sx=std::uint64_t(dx)*sw/w;
        const auto src=(sy*sw+sx)*4,dst=(std::uint64_t(y+dy)*p.width+x+dx)*4;std::memcpy(p.pixels.data()+dst,source.data()+src,4);}}
}
std::uint32_t SkiaCanvas::width() const noexcept { return impl_?impl_->width:0; }
std::uint32_t SkiaCanvas::height() const noexcept { return impl_?impl_->height:0; }
std::size_t SkiaCanvas::row_bytes() const noexcept { return std::size_t(width())*4; }
std::string SkiaCanvas::backend_version() const { return impl_?impl_->version:std::string{}; }
} // namespace gsdl::graphics
