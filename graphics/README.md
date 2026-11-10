# Graphics boundary

`gsdl_graphics` contains selected, adapted raster/presentation source files;
`provenance.json` records their read-only reference hashes. No application shell,
scene, renderer, training module, original settings, icons or fonts were copied.

Skia is the hash-locked native C ABI development DLL, not a managed runtime or
GPU widget framework. `SkiaCanvas` draws geometry into owned premultiplied
RGBA8888 pixels. Text layout is a separate DirectWrite concern; the renderer can
blend glyph masks into `mutable_pixels()` in physical coordinates. This access
does not inherit the Skia clip/scale state. The caller must provide the correct
text clip and finish raster work before copying the frame for presentation.

Create, use and destroy `Win32Presenter` on the dedicated render thread. The
HWND can belong to the UI thread. That thread must keep pumping messages and
keep the HWND alive until presenter destruction completes. No presenter method
can race another call. Resource waits remain synchronous on the render thread;
this module does not claim a hung driver can be interrupted. The UI must not
join or synchronously wait for renderer teardown.

The presenter requires the same explicit Vulkan extensions as the selected
reference implementation, including swapchainMaintenance1. There is no GDI or
silent fallback. It validates opaque RGBA and optionally reads GPU bytes back.
A successful presentation/readback does not itself prove compositor visibility.

`gsdl_stage_graphics(target)` copies the locked official NuGet DLL and Skia
notice beside a local executable. Release packaging adds the full SkiaSharp
and bundled third-party notices. The upstream DLL has not been rebuilt
reproducibly from complete sources. Header/notice hashes are checked during
configure; `third_party/dependencies.json` records the distribution source and
hashes, not a machine-specific runtime search path.
