#pragma once
#include "adapters/sources/insta360.hpp"
#include <filesystem>
#include <mutex>

namespace gsdl::transfer {
class CancelGate {
  std::atomic<int> phase_{}; // active, cancel won, publishing, finished
  std::atomic<bool> requested_{};

public:
  void request() noexcept {
    requested_ = true;
    int active = 0;
    phase_.compare_exchange_strong(active, 1);
  }
  bool requested() const noexcept { return requested_; }
  void check() const {
    if (requested_)
      throw net::Cancelled{};
  }
  bool begin_publish() noexcept {
    int active = 0;
    return phase_.compare_exchange_strong(active, 2);
  }
  void published(bool last) noexcept { phase_ = last ? 3 : 0; }
};
enum class LocalState { available, missing, changed, unchecked };
struct FileReceipt {
  std::string resourceId;
  std::filesystem::path path;
  std::uint64_t bytes{};
  std::string md5, sha256, expectedMd5;
  LocalState localState{LocalState::available};
};
// Worker-only; never call from UI or serialization.
LocalState check_local(const FileReceipt &) noexcept;
struct Progress {
  std::wstring stage, resourceName;
  std::uint64_t bytes{};
  std::optional<std::uint64_t> total;
  std::size_t completedFiles{}, fileCount{};
  std::vector<FileReceipt> published;
};
using NotifyProgress = std::function<void(const Progress &)>;
struct Report {
  std::vector<FileReceipt> files;
  std::filesystem::path receiptPath;
  std::wstring warning;
};
bool compatible(Snapshot, const sources::ResolvedSource &);
void download(Snapshot, const sources::ResolvedSource &, net::Http &,
              CancelGate &, const NotifyProgress &, Report &,
              const std::vector<FileReceipt> &previous = {});
enum class Kind { resolve, download };
enum class Terminal { none, succeeded, failed, cancelled };
struct View {
  std::uint64_t jobId{}, sessionId{}, inputRevision{};
  Kind kind{Kind::resolve};
  Terminal terminal{Terminal::none};
  bool busy{}, cancelRequested{};
  Progress progress;
  std::uint64_t elapsedMs{};
  std::wstring error;
};
struct Completion {
  Snapshot request;
  View view;
  std::shared_ptr<const sources::ResolvedSource> resolved;
  Report report;
};
// One bounded background executor, shared by GUI and CLI. Destruction requests
// stop; the owner keeps its callback target alive until onStopped is delivered.
class Service {
public:
  using Notify = std::function<void()>;
  Service(Notify onUpdate, Notify onStopped, unsigned testChunkDelayMs = 0);
  ~Service();
  Result resolve(Snapshot);
  Result start(Snapshot, std::shared_ptr<const sources::ResolvedSource>,
               std::vector<FileReceipt> previous = {});
  void cancel() noexcept;
  View view() const;
  std::optional<Completion> take_completion();
  void stop() noexcept;

private:
  struct State;
  std::shared_ptr<State> state_;
  Result submit(Snapshot, std::shared_ptr<const sources::ResolvedSource>, Kind,
                std::vector<FileReceipt> previous = {});
};
std::string encode_report(const Completion &);
} // namespace gsdl::transfer
