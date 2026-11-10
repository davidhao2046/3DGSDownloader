#include "core/application/task_list.hpp"
#include <iostream>
#include <stdexcept>

using namespace gsdl;
namespace {
int checks{};
void check(bool value, const char *expression, int line) {
  ++checks;
  if (!value)
    throw std::runtime_error(std::string("line ") + std::to_string(line) +
                             ": " + expression);
}
#define CHECK(...)                                                             \
  check(static_cast<bool>((__VA_ARGS__)), #__VA_ARGS__, __LINE__)
const std::wstring link =
    L"https://app.insta360.com/3dspace/detail/GS3DtaskTest";
const std::wstring directory = L"D:\\下载结果";
struct FakeTransfer {
  transfer::View current;
  Snapshot request;
  std::optional<transfer::Completion> completion;
  int resolveCalls{}, downloadCalls{}, cancelCalls{};
  bool rejectResolve{}, rejectDownload{};
  std::uint64_t nextJob{1};
  std::vector<std::uint64_t> resolvedSessions, downloadedSessions;
  Result submit(Snapshot snapshot, transfer::Kind kind) {
    CHECK(!current.busy && !completion);
    if ((kind == transfer::Kind::resolve && rejectResolve) ||
        (kind == transfer::Kind::download && rejectDownload))
      return {false, false, L"测试启动失败"};
    request = std::move(snapshot);
    current = {};
    current.jobId = nextJob++;
    current.sessionId = request->sessionId;
    current.inputRevision = request->inputRevision;
    current.kind = kind;
    current.busy = true;
    current.progress.stage = L"测试运行中";
    if (kind == transfer::Kind::resolve) {
      ++resolveCalls;
      resolvedSessions.push_back(request->sessionId);
    } else {
      ++downloadCalls;
      downloadedSessions.push_back(request->sessionId);
    }
    return {true, true, {}};
  }
  TaskTransferPort port() {
    return {[this](Snapshot snapshot) {
              return submit(std::move(snapshot), transfer::Kind::resolve);
            },
            [this](Snapshot snapshot,
                   std::shared_ptr<const sources::ResolvedSource> source) {
              CHECK(source &&
                    source->request->sessionId == snapshot->sessionId);
              CHECK(source->request->plan.planId == snapshot->plan.planId);
              return submit(std::move(snapshot), transfer::Kind::download);
            },
            [this] {
              ++cancelCalls;
              current.cancelRequested = true;
            },
            [this] { return current; },
            [this] {
              auto value = std::move(completion);
              completion.reset();
              return value;
            }};
  }
  void finish(transfer::Terminal terminal = transfer::Terminal::succeeded) {
    CHECK(current.busy);
    current.busy = false;
    current.terminal = terminal;
    current.elapsedMs = 21;
    if (terminal == transfer::Terminal::failed)
      current.error = L"连接失败，原文件保留，请重试";
    completion = transfer::Completion{request, current, {}, {}};
    if (current.kind == transfer::Kind::resolve &&
        terminal == transfer::Terminal::succeeded) {
      auto source = std::make_shared<sources::ResolvedSource>();
      source->request = request;
      source->resources.push_back(
          {Resource{"sog", L"模型", L"SOG", L"model.sog", true, 123},
           L"https://fixture.invalid/model.sog?Signature=never-persist",
           {}});
      source->resources.push_back(
          {Resource{"video", L"视频", L"MP4", L"video.mp4", false, 456},
           L"https://fixture.invalid/video.mp4?Signature=never-persist",
           {}});
      completion->resolved = std::move(source);
    }
  }
};
std::string add(TaskListService &tasks, bool start = false) {
  std::string id;
  CHECK(tasks.add(link, directory, start, id));
  CHECK(!id.empty());
  return id;
}
TaskRestore restore_record(const TaskSnapshot &item) {
  return {item.id,    item.document->plan, item.planPath,
          item.state, item.report,         item.error};
}
void ownership_and_serial_queue() {
  FakeTransfer worker;
  TaskListService tasks;
  tasks.bind(worker.port());
  const auto a = add(tasks, true), b = add(tasks, true), c = add(tasks);
  CHECK(tasks.entries().size() == 3);
  CHECK(tasks.find(a)->state == TaskState::Resolving &&
        tasks.find(b)->state == TaskState::Queued);
  CHECK(tasks.find(c)->state == TaskState::Draft);
  CHECK(worker.resolveCalls == 1 && tasks.locked(a) && tasks.locked(b) &&
        !tasks.locked(c));
  CHECK(tasks.find(a)->plans.snapshot()->sessionId !=
        tasks.find(b)->plans.snapshot()->sessionId);
  CHECK(!tasks.enqueue(a) && !tasks.remove(b));
  CHECK(tasks.find(c)->plans.edit("outputDirectory", {}, L"D:\\单独目录"));
  CHECK(tasks.find(c)->plans.undo_count() == 1 &&
        tasks.find(a)->plans.undo_count() == 0);
  CHECK(tasks.find(c)->plans.undo());
  worker.current.progress.bytes = 17;
  CHECK(!tasks.poll());
  CHECK(tasks.find(a)->progress.bytes == 17 &&
        tasks.find(b)->progress.bytes == 0);
  worker.finish();
  tasks.poll();
  CHECK(tasks.find(a)->state == TaskState::Downloading &&
        worker.downloadCalls == 1);
  CHECK(tasks.find(b)->state == TaskState::Queued && worker.resolveCalls == 1);
  CHECK(tasks.find(a)->plans.snapshot()->plan.resources.size() == 2);
  CHECK(tasks.find(a)->plans.snapshot()->plan.resources[0].included);
  CHECK(!tasks.find(a)->plans.snapshot()->plan.resources[1].included);
  CHECK(tasks.find(b)->plans.snapshot()->plan.resources.empty());
  CHECK(worker.request->revision ==
        1); // Download captures the accepted metadata revision.
  worker.finish();
  tasks.poll();
  CHECK(tasks.find(a)->state == TaskState::Succeeded &&
        tasks.find(b)->state == TaskState::Resolving);
  CHECK(worker.resolveCalls == 2 && !tasks.locked(a));
  worker.finish(transfer::Terminal::failed);
  tasks.poll();
  CHECK(tasks.find(b)->state == TaskState::Failed && !tasks.busy());
  CHECK(tasks.find(b)->error.find(L"连接失败") != std::wstring::npos);
  CHECK(tasks.resolve(c));
  worker.finish();
  tasks.poll();
  CHECK(tasks.find(c)->state == TaskState::Ready && worker.downloadCalls == 1 &&
        !tasks.busy());
  CHECK(worker.resolvedSessions[0] ==
        tasks.find(a)->plans.snapshot()->sessionId);
  CHECK(worker.downloadedSessions[0] ==
        tasks.find(a)->plans.snapshot()->sessionId);
}
void cancellation_and_retry() {
  FakeTransfer worker;
  TaskListService tasks;
  tasks.bind(worker.port());
  const auto a = add(tasks, true), b = add(tasks, true);
  CHECK(tasks.cancel(b));
  CHECK(tasks.find(b)->state == TaskState::Cancelled &&
        worker.cancelCalls == 0);
  CHECK(tasks.cancel(a));
  CHECK(tasks.find(a)->state == TaskState::Cancelling && tasks.locked(a));
  CHECK(tasks.cancel(a) && worker.cancelCalls == 1);
  // Completion already won at resolve, but user cancellation prevents phase 2.
  worker.finish();
  tasks.poll();
  CHECK(tasks.find(a)->state == TaskState::Cancelled &&
        worker.downloadCalls == 0 && !tasks.busy());
  CHECK(tasks.enqueue(a));
  CHECK(worker.resolveCalls == 2);
  worker.finish();
  tasks.poll();
  CHECK(tasks.find(a)->state == TaskState::Downloading);
  CHECK(tasks.cancel(a));
  worker.finish();
  worker.completion->report.files.push_back(
      {"sog",
       std::filesystem::path(directory) / L"model.sog",
       123,
       {},
       {},
       {}});
  tasks.poll(); // Publication won: successful original file remains successful,
                // not cancelled.
  CHECK(tasks.find(a)->state == TaskState::Succeeded);
  CHECK(tasks.find(a)->report.files.size() == 1);
  CHECK(tasks.enqueue(a));
  CHECK(worker.resolveCalls == 3 && tasks.find(a)->attempt == 3);
  worker.finish(transfer::Terminal::cancelled);
  tasks.poll();
  CHECK(tasks.find(a)->state == TaskState::Cancelled);
  CHECK(tasks.find(a)->report.files.size() ==
        1); // Retry never erases completed-file facts.
  CHECK(tasks.remove(a));
  CHECK(!tasks.find(a));
}
void stale_results_and_edits() {
  FakeTransfer worker;
  TaskListService tasks;
  tasks.bind(worker.port());
  const auto a = add(tasks, true), b = add(tasks, true);
  const auto real = worker.current;
  // Unrelated completion must never follow current selection or current row.
  auto foreignRequest = tasks.find(b)->plans.snapshot();
  worker.completion = transfer::Completion{foreignRequest, real, {}, {}};
  worker.completion->view.busy = false;
  worker.completion->view.terminal = transfer::Terminal::succeeded;
  tasks.poll();
  CHECK(tasks.find(a)->state == TaskState::Resolving &&
        tasks.find(b)->state == TaskState::Queued);
  CHECK(tasks.find(a)->plans.snapshot()->plan.resources.empty());
  // Programmatic edit bypassing GUI lock is still rejected at the queue
  // boundary.
  CHECK(tasks.find(b)->plans.edit("outputDirectory", {}, L"D:\\更新目录"));
  worker.finish(transfer::Terminal::failed);
  tasks.poll();
  CHECK(tasks.find(b)->state == TaskState::Failed && worker.resolveCalls == 1);
  CHECK(tasks.find(b)->error.find(L"配置发生变化") != std::wstring::npos);
  CHECK(tasks.enqueue(a));
  CHECK(tasks.find(a)->plans.edit("outputDirectory", {}, L"D:\\运行中修改"));
  worker.finish();
  tasks.poll();
  CHECK(tasks.find(a)->state == TaskState::Failed && worker.downloadCalls == 0);
  CHECK(tasks.find(a)->plans.snapshot()->plan.resources.empty());
  CHECK(!tasks.busy());
}
void snapshots_restore_and_shutdown() {
  FakeTransfer worker;
  TaskListService tasks;
  tasks.bind(worker.port());
  const auto a = add(tasks, true), b = add(tasks, true), c = add(tasks);
  const auto captured = tasks.exportSnapshot();
  CHECK(captured.size() == 3 && captured[0].state == TaskState::Resolving);
  CHECK(!tasks.restore({}));
  tasks.stopScheduling();
  CHECK(tasks.find(a)->state == TaskState::Cancelling &&
        tasks.find(b)->state == TaskState::Cancelled);
  CHECK(worker.cancelCalls == 1);
  std::string unused;
  CHECK(!tasks.add(link, directory, false, unused));
  CHECK(!tasks.enqueue(c));
  worker.finish();
  tasks.poll();
  CHECK(worker.downloadCalls == 0 && !tasks.busy());
  FakeTransfer newWorker;
  TaskListService reopened;
  reopened.bind(newWorker.port());
  std::vector<TaskRestore> records;
  for (const auto &item : captured)
    records.push_back(restore_record(item));
  CHECK(reopened.restore(std::move(records)));
  CHECK(reopened.find(a)->state == TaskState::Cancelled &&
        reopened.find(b)->state == TaskState::Cancelled);
  CHECK(reopened.find(c)->state == TaskState::Draft &&
        newWorker.resolveCalls == 0);
  CHECK(reopened.find(a)->plans.snapshot()->sessionId !=
        captured[0].document->sessionId);
  CHECK(reopened.find(a)->plans.undo_count() == 0);
  auto duplicate = restore_record(captured[0]);
  CHECK(!reopened.restore({duplicate, duplicate}));
  CHECK(reopened.entries().size() == 3);
  duplicate.plan.schemaVersion = 999;
  CHECK(!reopened.restore({duplicate}));
  CHECK(reopened.entries().size() == 3);
  CHECK(reopened.enqueue(a));
  CHECK(newWorker.resolveCalls == 1);
  CHECK(captured[0].document->plan.resources.empty());
}
void validation_filter_and_failures() {
  FakeTransfer worker;
  TaskListService tasks;
  tasks.bind(worker.port());
  std::string id;
  CHECK(!tasks.add(L"https://evil.invalid/test", directory, true, id));
  CHECK(!tasks.add(link, L"", true, id));
  CHECK(tasks.entries().empty());
  CHECK(tasks.add({}, directory, false, id));
  CHECK(!tasks.enqueue(id));
  CHECK(task_matches(TaskState::Draft, TaskFilter::All));
  CHECK(!task_matches(TaskState::Draft, TaskFilter::Active));
  CHECK(task_matches(TaskState::Queued, TaskFilter::Active));
  CHECK(task_matches(TaskState::Cancelling, TaskFilter::Active));
  CHECK(!task_matches(TaskState::Ready, TaskFilter::Completed));
  CHECK(task_matches(TaskState::Succeeded, TaskFilter::Completed));
  CHECK(task_matches(TaskState::Draft, TaskFilter::Attention));
  CHECK(task_matches(TaskState::Ready, TaskFilter::Attention));
  CHECK(task_matches(TaskState::Failed, TaskFilter::Attention));
  CHECK(task_matches(TaskState::Cancelled, TaskFilter::Attention));
  CHECK(!task_matches(TaskState::Queued, TaskFilter::Attention));
  CHECK(!task_matches(TaskState::Resolving, TaskFilter::Attention));
  CHECK(!task_matches(TaskState::Downloading, TaskFilter::Attention));
  CHECK(!task_matches(TaskState::Cancelling, TaskFilter::Attention));
  CHECK(!task_matches(TaskState::Succeeded, TaskFilter::Attention));
  CHECK(task_state_label(TaskState::Cancelling) == L"正在取消");
  worker.rejectResolve = true;
  const auto failed = add(tasks, true);
  CHECK(tasks.find(failed)->state == TaskState::Failed && !tasks.busy());
  worker.rejectResolve = false;
  worker.rejectDownload = true;
  CHECK(tasks.enqueue(failed));
  worker.finish();
  tasks.poll();
  CHECK(tasks.find(failed)->state == TaskState::Failed && !tasks.busy());
  CHECK(tasks.find(failed)->plans.snapshot()->plan.resources.size() == 2);
  CHECK(!tasks.cancel("missing") && !tasks.remove("missing"));
}
void selection_and_incremental_publication() {
  FakeTransfer worker;
  TaskListService tasks;
  tasks.bind(worker.port());
  const auto id = add(tasks);
  CHECK(tasks.resolve(id));
  worker.finish();
  tasks.poll();
  CHECK(tasks.find(id)->plans.edit("resource.included", "sog", L"false"));
  CHECK(!tasks.enqueue(id));
  CHECK(worker.resolveCalls == 1 && worker.downloadCalls == 0 && !tasks.busy());
  CHECK(tasks.find(id)->plans.edit("resource.included", "sog", L"true"));
  CHECK(tasks.enqueue(id));
  worker.finish();
  tasks.poll();
  worker.current.progress.published.push_back(
      {"sog", std::filesystem::path(directory) / L"model.sog", 123, {}, {}, {}});
  worker.current.progress.completedFiles = 1;
  tasks.poll();
  CHECK(tasks.busy() && tasks.find(id)->report.files.size() == 1);
  const auto session = worker.current.sessionId;
  ++worker.current.sessionId;
  worker.current.progress.published.push_back(
      {"foreign", std::filesystem::path(directory) / L"foreign.mp4", 456, {}, {}, {}});
  tasks.poll();
  CHECK(tasks.find(id)->report.files.size() == 1);
  worker.current.sessionId = session;
  worker.current.progress.published.resize(1);
  worker.finish(transfer::Terminal::failed);
  tasks.poll();
  CHECK(tasks.find(id)->state == TaskState::Failed);
  CHECK(tasks.find(id)->report.files.size() == 1);
}
void task_limit() {
  TaskListService tasks;
  const auto plan = make_empty_plan();
  std::string id;
  // Small in-memory plans only: no network, filesystem, or large fixtures.
  for (std::size_t i = 0; i < max_task_entries; ++i)
    CHECK(tasks.addPlan(plan, {}, id));
  CHECK(tasks.entries().size() == 256);
  const auto last = id;
  id = "unchanged-on-rejection";
  const auto result = tasks.addPlan(plan, {}, id);
  CHECK(!result && !result.changed &&
        result.error.find(L"先移除") != std::wstring::npos);
  CHECK(id == "unchanged-on-rejection" && tasks.entries().size() == 256);
  CHECK(tasks.remove(last));
  CHECK(tasks.addPlan(plan, {}, id) && tasks.entries().size() == 256);
  CHECK(tasks.find(id) && !tasks.find(last));
  std::vector<TaskRestore> tooMany(max_task_entries + 1);
  CHECK(!tasks.restore(std::move(tooMany)) && tasks.entries().size() == 256);
}
} // namespace
int main() {
  try {
    ownership_and_serial_queue();
    cancellation_and_retry();
    stale_results_and_edits();
    snapshots_restore_and_shutdown();
    validation_filter_and_failures();
    selection_and_incremental_publication();
    task_limit();
    std::cout << "task_list_tests: " << checks << " checks passed\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "task_list_tests failed: " << error.what() << '\n';
    return 1;
  }
}
