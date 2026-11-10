#pragma once

#include <windows.h>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace gsdl::graphics {
// New internal display contract; no saved-camera, PSHT or original display ABI.
// Tightly packed, top row first, R/G/B encoded as sRGB, alpha exactly 255.
struct Rgba8Frame {
    std::uint32_t width{}, height{};
    std::vector<std::uint8_t> rgba;
};
inline constexpr std::uint64_t max_frame_pixels = 16'777'216;
enum class ErrorCode {
    invalid_input, unsupported_environment, wrong_thread,
    device_lost, surface_lost, vulkan_failure, readback_mismatch
};
class PresentationError final : public std::runtime_error {
public:
    PresentationError(ErrorCode code, std::string message, std::int32_t native_result = 0)
        : std::runtime_error(std::move(message)), code_(code), native_result_(native_result) {}
    [[nodiscard]] ErrorCode code() const noexcept { return code_; }
    [[nodiscard]] std::int32_t native_result() const noexcept { return native_result_; }
private:
    ErrorCode code_;
    std::int32_t native_result_;
};
enum class PresentStatus { presented, minimized, retry };
struct PresentResult {
    PresentStatus status{PresentStatus::retry};
    std::uint32_t width{}, height{};
    std::uint64_t swapchain_generation{}, submission_index{};
    bool readback_verified{};
};
struct Diagnostics {
    std::string device_name, loader_path, swapchain_format;
    std::uint32_t api_version{}, driver_version{}, vendor_id{}, device_id{};
    bool presentation_fences{}, readback_supported{};
    std::uint64_t swapchain_generation{}, submitted_frames{}, accepted_presents{};
};
class Win32Presenter final {
public:
    // HWND may belong to the UI thread and MUST outlive this presenter.
    // Create/call/destroy this object exclusively on the render thread. Requires
    // Vulkan 1.1, Win32 surface, swapchain and EXT_swapchain_maintenance1.
    // Loads only the OS System32 Vulkan loader; no original application modules.
    explicit Win32Presenter(HWND window);
    ~Win32Presenter() noexcept;
    Win32Presenter(const Win32Presenter&) = delete;
    Win32Presenter& operator=(const Win32Presenter&) = delete;
    Win32Presenter(Win32Presenter&&) = delete;
    Win32Presenter& operator=(Win32Presenter&&) = delete;
    // All calls and destruction belong to the construction thread and must not
    // race. No input references survive this synchronous call. CPU nearest-
    // neighbor resampling maps the whole frame to the current client extent.
    // A presented result means Vulkan accepted the presentation and its fence
    // completed, not that a human/compositor saw a hidden or occluded window.
    // verify_readback requires TRANSFER_SRC support and verifies all copied GPU
    // bytes before returning success. No GDI/CPU presentation fallback exists.
    // retry did NOT present the frame: retain/re-render the newest input and
    // retry with a bounded, shutdown-interruptible delay on the rendering thread.
    // minimized is also not a successful frame; wait for a restore/size update.
    // Result dimensions are swapchain dimensions; the host should supersede
    // stale-layout frames when a concurrent DPI/size change arrives.
    [[nodiscard]] PresentResult present(const Rgba8Frame& frame, bool verify_readback = false);
    // Mark swapchain dirty after WM_SIZE/DPI change; zero-sized/minimized HWNDs
    // defer recreation. present() also detects client-size and out-of-date changes.
    void resize();
    [[nodiscard]] Diagnostics diagnostics() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace gsdl::graphics
