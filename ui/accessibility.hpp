#pragma once
#include <windows.h>
#include <memory>
#include <string>
#include <vector>

namespace gsdl::ui {
enum class AccessibleKind { edit, button, list, list_item, text };
struct AccessibleNode {
    int id{}; // Stable nonzero control identity, never an index into mutable data.
    std::wstring name;
    std::wstring value;
    RECT screen_bounds{};
    bool focused{};
    bool enabled{true};
    AccessibleKind kind{AccessibleKind::text};
};
enum class AccessibleRequestKind { focus, invoke, set_value };
struct AccessibleRequest {
    AccessibleRequestKind kind{};
    int id{};
    std::wstring value;
};
// UIA may call providers from arbitrary COM threads. Providers read owned
// snapshots and post requests only; they never call application controls.
// Handle requestMessage with unique_ptr<AccessibleRequest>((...*)lParam).
// update/disconnect/destruction belong to HWND's UI thread. Disconnect before
// DestroyWindow: it closes the posting gate and drains pending owned requests.
class AccessibilityHost final {
public:
    AccessibilityHost(HWND window, UINT requestMessage);
    ~AccessibilityHost();
    AccessibilityHost(const AccessibilityHost&) = delete;
    AccessibilityHost& operator=(const AccessibilityHost&) = delete;
    // Publishes a snapshot, then raises only changed structure/properties/focus.
    // Notifications are made outside the snapshot lock to permit UIA reentry.
    void update(std::vector<AccessibleNode> nodes);
    LRESULT get_object(WPARAM wparam, LPARAM lparam);
    void disconnect();
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
