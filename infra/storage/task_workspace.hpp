#pragma once
#include "core/application/task_list.hpp"
#include "core/application/download_preferences.hpp"
#include "plan_storage.hpp"

namespace gsdl::storage {
inline constexpr std::size_t max_workspace_bytes = 8 * 1024 * 1024;
inline constexpr std::size_t max_workspace_tasks = 256;

// These captures retain immutable documents. They do not own a second editable
// plan or history, and never include the in-memory source session/signed URLs.
struct WorkspaceSnapshot {
  std::wstring lastDirectory;
  std::vector<TaskSnapshot> tasks;
  std::vector<std::wstring> recentDirectories;
  std::wstring lastUsedBaseDirectory;
  SelectionPreferences selectionPreferences;
};
struct WorkspaceData {
  std::wstring lastDirectory;
  std::vector<TaskRestore> tasks;
  std::vector<std::wstring> recentDirectories;
  std::wstring lastUsedBaseDirectory;
  SelectionPreferences selectionPreferences;
};
std::string encode_workspace(const WorkspaceSnapshot &);
WorkspaceData decode_workspace(std::string_view);
// A missing workspace is a normal first launch; malformed input is an error.
std::optional<WorkspaceData> read_workspace(const std::filesystem::path &);
void save_workspace_atomic(const std::filesystem::path &,
                           const WorkspaceSnapshot &, SaveFault = {});

struct WorkspaceRead {
  std::uint64_t requestId{};
  std::filesystem::path path;
};
struct WorkspaceSave {
  std::uint64_t generation{};
  std::filesystem::path path;
  WorkspaceSnapshot snapshot;
  SaveFault testFault{};
};
struct WorkspaceResult {
  Operation operation{};
  std::uint64_t requestId{}, generation{};
  std::filesystem::path path;
  std::optional<WorkspaceData> data;
  std::optional<Error> error;
  bool missing{};
  bool success() const noexcept { return !error; }
};

// One FIFO serializes reads and writes, including repeated autosaves. The
// generation returned on completion only acknowledges the captured snapshot.
// Stop drains accepted work. The owner keeps callback targets alive until the
// stopped notice; destruction never blocks the UI or joins a worker thread.
class WorkspaceWorker {
public:
  using Notify = std::function<void()>;
  WorkspaceWorker(Notify onResult, Notify onStopped, WorkerOptions = {});
  ~WorkspaceWorker();
  WorkspaceWorker(const WorkspaceWorker &) = delete;
  WorkspaceWorker &operator=(const WorkspaceWorker &) = delete;
  bool submit(WorkspaceRead);
  bool submit(WorkspaceSave);
  std::vector<WorkspaceResult> take_results();
  void request_stop() noexcept;
  bool stopped() const noexcept;

private:
  struct State;
  std::shared_ptr<State> state_;
};
} // namespace gsdl::storage
