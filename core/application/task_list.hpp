#pragma once
#include "plan_service.hpp"
#include "transfer_service.hpp"
#include <deque>

namespace gsdl {
inline constexpr std::size_t max_task_entries = 256;
enum class TaskState {
  Draft,
  Queued,
  Resolving,
  Ready,
  Downloading,
  Cancelling,
  Succeeded,
  Failed,
  Cancelled
};
enum class TaskFilter { All, Active, Completed, Attention };
bool task_matches(TaskState, TaskFilter);
std::wstring_view task_state_label(TaskState);

struct TaskEntry {
  std::string id;
  PlanService
      plans; // Its DocumentStore is the only editable business model/history.
  TaskState state{TaskState::Draft};
  transfer::Progress progress;
  std::uint64_t elapsedMs{}, attempt{};
  std::wstring error;
  transfer::Report report;

private:
  friend class TaskListService;
  std::shared_ptr<const sources::ResolvedSource> resolved;
};

// Immutable captures for the owner's asynchronous workspace writer. No signed
// URL or editable model is exported. Restoring never resumes a network
// operation.
struct TaskSnapshot {
  std::string id;
  Snapshot document;
  std::wstring planPath;
  TaskState state{TaskState::Draft};
  transfer::Report report;
  std::wstring error;
};
struct TaskRestore {
  std::string id;
  DownloadPlan plan;
  std::wstring planPath;
  TaskState state{TaskState::Draft};
  transfer::Report report;
  std::wstring error;
};

// Injection seam for deterministic queue tests. The production adapter
// delegates to one existing transfer::Service; callbacks must be nonblocking.
struct TaskTransferPort {
  std::function<Result(Snapshot)> resolve;
  std::function<Result(Snapshot,
                       std::shared_ptr<const sources::ResolvedSource>)>
      start;
  std::function<void()> cancel;
  std::function<transfer::View()> view;
  std::function<std::optional<transfer::Completion>()> takeCompletion;
};

// Single coordinator/UI-thread owner. Does not own a thread, block, persist
// files, or inspect downloaded files. Selection is deliberately outside this
// service.
class TaskListService {
public:
  void bind(transfer::Service &);
  void bind(TaskTransferPort);
  Result add(std::wstring_view link, std::wstring_view outputDirectory,
             bool startAfterResolve, std::string &outId);
  Result addPlan(DownloadPlan, std::wstring path, std::string &outId);
  const std::vector<std::unique_ptr<TaskEntry>> &entries() const {
    return entries_;
  }
  TaskEntry *find(std::string_view);
  const TaskEntry *find(std::string_view) const;
  bool locked(std::string_view) const;
  bool busy() const { return active_.has_value() || !queue_.empty(); }
  Result enqueue(std::string_view id, bool download = true);
  Result resolve(std::string_view id) { return enqueue(id, false); }
  Result cancel(std::string_view id);
  Result remove(std::string_view id);
  // Returns true when durable task facts or a plan changed. Transient byte/time
  // progress still updates entries but does not request a workspace disk write.
  // Call on worker messages; enqueue also polls to start an idle worker.
  bool poll();
  // Stops queue advancement and requests active cancellation; the owner still
  // calls Service::stop() and waits asynchronously for its stopped callback.
  void stopScheduling();
  std::vector<TaskSnapshot> exportSnapshot() const;
  Result restore(std::vector<TaskRestore>);

private:
  struct Queued {
    std::string id;
    Snapshot request;
    bool download{};
  };
  struct Active {
    std::string id;
    Snapshot request;
    std::uint64_t jobId{};
    transfer::Kind kind{};
    bool download{};
  };
  std::vector<std::unique_ptr<TaskEntry>> entries_;
  std::deque<Queued> queue_;
  std::optional<Active> active_;
  TaskTransferPort port_;
  bool stopping_{};
  bool portReady() const;
  bool dispatch();
  void complete(transfer::Completion);
  bool startDownload(TaskEntry &);
};
} // namespace gsdl
