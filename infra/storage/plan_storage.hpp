#pragma once
#include <core/domain/plan.hpp>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace gsdl::storage {
inline constexpr std::size_t max_plan_bytes = 8 * 1024 * 1024;
inline constexpr std::size_t max_resources = 10000;

struct Error {
    std::wstring message;
    std::string stage;
    std::uint32_t nativeCode{};
    // A rare OS publication/rollback failure can retain the original here.
    // Never delete this recovery copy merely because the operation failed.
    std::filesystem::path recoveryPath;
};
class StorageError : public std::runtime_error {
public:
    explicit StorageError(Error);
    const Error& detail() const noexcept { return detail_; }
private:
    Error detail_;
};

// Default is no overwrite. replace_or_create is only for an explicitly
// authorized target. All publishers in one process must share one IoWorker.
enum class PublishMode { create_new, replace_or_create };
// Deterministic verification seams. Product callers leave these at defaults.
enum class SaveFault { none, before_write, after_write, before_publish };
struct SaveOptions { PublishMode mode{PublishMode::create_new}; SaveFault fault{}; };

std::string encode_plan(const DownloadPlan&);
DownloadPlan decode_plan(std::string_view utf8);
DownloadPlan read_plan(const std::filesystem::path&);
void save_plan_atomic(const std::filesystem::path&, const DownloadPlan&, SaveOptions = {});

struct ReadRequest {
    std::uint64_t sessionId{}, requestId{};
    std::uint64_t capturedRevision{};
    std::filesystem::path path;
};
struct SaveRequest {
    SaveCapture capture;
    std::uint64_t requestId{};
    PublishMode mode{PublishMode::create_new};
    SaveFault testFault{};
};
enum class Operation { read, save };
struct IoResult {
    Operation operation{};
    std::uint64_t sessionId{}, requestId{};
    std::uint64_t capturedRevision{};
    std::filesystem::path path;
    SaveCapture capture;
    std::optional<DownloadPlan> plan;
    std::optional<Error> error;
    bool success() const noexcept { return !error; }
};
struct WorkerOptions {
    std::size_t maxPending{8};
    std::uint32_t testIoDelayMs{};
};

// One FIFO serializes all accepted reads/saves, including aliases of a target.
// No HWND dependency. Callbacks run on the I/O thread and must only post a
// lifetime-safe notification, never touch application state or perform a join.
// request_stop rejects new requests and drains already accepted work. stopped
// is set before onStopped; keep the callback receiver alive until that notice.
// Destructor requests stop and never joins; detached state owns its lifetime.
class IoWorker {
public:
    using Notify = std::function<void()>;
    explicit IoWorker(Notify onResult, Notify onStopped, WorkerOptions = {});
    ~IoWorker();
    IoWorker(const IoWorker&) = delete;
    IoWorker& operator=(const IoWorker&) = delete;
    bool submit(ReadRequest);
    bool submit(SaveRequest);
    std::vector<IoResult> take_results();
    void request_stop() noexcept;
    bool stopped() const noexcept;
private:
    struct State;
    std::shared_ptr<State> state_;
};
} // namespace gsdl::storage
