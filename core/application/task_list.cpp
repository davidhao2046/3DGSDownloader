#include "task_list.hpp"
#include <algorithm>
#include <stdexcept>
#include <unordered_set>

namespace gsdl {
namespace {
bool same_request(const Snapshot &a, const Snapshot &b) {
  return a && b && a->sessionId == b->sessionId && a->revision == b->revision &&
         a->inputRevision == b->inputRevision &&
         a->plan.planId == b->plan.planId;
}
bool running(TaskState state) {
  return state == TaskState::Queued || state == TaskState::Resolving ||
         state == TaskState::Downloading || state == TaskState::Cancelling;
}
void merge_report(transfer::Report &into, transfer::Report from) {
  for (auto &file : from.files) {
    const auto existing = std::find_if(
        into.files.begin(), into.files.end(), [&](const auto &value) {
          return value.path == file.path && value.resourceId == file.resourceId;
        });
    if (existing == into.files.end())
      into.files.push_back(std::move(file));
    else
      *existing = std::move(file);
  }
  if (!from.receiptPath.empty())
    into.receiptPath = std::move(from.receiptPath);
  into.warning = std::move(from.warning);
}
} // namespace

bool task_matches(TaskState state, TaskFilter filter) {
  switch (filter) {
  case TaskFilter::All:
    return true;
  case TaskFilter::Active:
    return running(state);
  case TaskFilter::Completed:
    return state == TaskState::Succeeded;
  case TaskFilter::Attention:
    return state == TaskState::Draft || state == TaskState::Ready ||
           state == TaskState::Failed || state == TaskState::Cancelled;
  }
  return false;
}
std::wstring_view task_state_label(TaskState state) {
  switch (state) {
  case TaskState::Draft:
    return L"待开始";
  case TaskState::Queued:
    return L"排队中";
  case TaskState::Resolving:
    return L"解析链接";
  case TaskState::Ready:
    return L"待下载";
  case TaskState::Downloading:
    return L"下载中";
  case TaskState::Cancelling:
    return L"正在取消";
  case TaskState::Succeeded:
    return L"已完成";
  case TaskState::Failed:
    return L"失败";
  case TaskState::Cancelled:
    return L"已取消";
  }
  return L"未知状态";
}
void TaskListService::bind(transfer::Service &service) {
  bind(TaskTransferPort{
      [&service](Snapshot request) {
        return service.resolve(std::move(request));
      },
      [this, &service](Snapshot request,
                 std::shared_ptr<const sources::ResolvedSource> source) {
        std::vector<transfer::FileReceipt> previous;
        for (const auto &entry : entries_)
          if (entry->plans.snapshot()->sessionId == request->sessionId)
            previous = entry->report.files;
        return service.start(std::move(request), std::move(source), std::move(previous));
      },
      [&service] { service.cancel(); }, [&service] { return service.view(); },
      [&service] { return service.take_completion(); }});
}
void TaskListService::bind(TaskTransferPort port) {
  if (busy())
    throw std::logic_error("Cannot replace an active task transfer port");
  port_ = std::move(port);
}
bool TaskListService::portReady() const {
  return port_.resolve && port_.start && port_.cancel && port_.view &&
         port_.takeCompletion;
}
TaskEntry *TaskListService::find(std::string_view id) {
  const auto it =
      std::find_if(entries_.begin(), entries_.end(),
                   [&](const auto &entry) { return entry->id == id; });
  return it == entries_.end() ? nullptr : it->get();
}
const TaskEntry *TaskListService::find(std::string_view id) const {
  const auto it =
      std::find_if(entries_.begin(), entries_.end(),
                   [&](const auto &entry) { return entry->id == id; });
  return it == entries_.end() ? nullptr : it->get();
}
bool TaskListService::locked(std::string_view id) const {
  if (active_ && active_->id == id)
    return true;
  return std::any_of(queue_.begin(), queue_.end(),
                     [&](const auto &item) { return item.id == id; });
}
Result TaskListService::add(std::wstring_view link, std::wstring_view directory,
                            bool start, std::string &outId) {
  if (stopping_)
    return {false, false, L"程序正在退出，暂时不能添加任务"};
  auto plan = make_empty_plan();
  if (!link.empty()) {
    if (auto result = canonical_share_url(link, plan.source); !result)
      return result;
  }
  plan.outputDirectory = directory;
  if (start && plan.source.shareUrl.empty())
    return {false, false, L"请输入影石 3D 时光舱公开分享链接"};
  if (start && directory.empty())
    return {false, false, L"请选择下载文件夹"};
  if (start && !portReady())
    return {false, false, L"下载服务尚未就绪"};
  auto result = addPlan(std::move(plan), {}, outId);
  if (!result || !start)
    return result;
  return enqueue(outId);
}
Result TaskListService::addPlan(DownloadPlan plan, std::wstring path,
                                std::string &outId) {
  if (stopping_)
    return {false, false, L"程序正在退出，暂时不能添加任务"};
  if (entries_.size() >= max_task_entries)
    return {false, false,
            L"任务列表已达到 256 "
            L"条上限；请先移除不再需要的任务记录，原文件不会删除"};
  auto entry = std::make_unique<TaskEntry>();
  if (auto result =
          entry->plans.replaceLoaded(std::move(plan), std::move(path));
      !result)
    return result;
  entry->id = make_empty_plan().planId;
  outId = entry->id;
  entries_.push_back(std::move(entry));
  return {true, true, {}};
}
Result TaskListService::enqueue(std::string_view id, bool download) {
  if (stopping_)
    return {false, false, L"程序正在退出，暂时不能开始任务"};
  auto *entry = find(id);
  if (!entry)
    return {false, false, L"任务不存在"};
  if (locked(id))
    return {false, false, L"该任务已在队列中"};
  if (!portReady())
    return {false, false, L"下载服务尚未就绪"};
  const auto request = entry->plans.snapshot();
  if (request->plan.internalFixture || request->plan.source.shareUrl.empty())
    return {false, false, L"请输入真实的影石 3D 时光舱公开分享链接"};
  if (download && request->plan.outputDirectory.empty())
    return {false, false, L"请选择下载文件夹"};
  if (auto valid = validate_plan(request->plan); !valid)
    return valid;
  if (download && !request->plan.resources.empty() &&
      std::none_of(request->plan.resources.begin(), request->plan.resources.end(),
                   [](const auto &r) { return r.included; }))
    return {false, false, L"请勾选需要下载的文件"};
  queue_.push_back({entry->id, request, download});
  entry->state = TaskState::Queued;
  entry->error.clear();
  entry->resolved.reset(); // Retry always refreshes short-lived signed URLs.
  entry->progress = {};
  entry->progress.stage = L"等待前面的任务完成";
  entry->elapsedMs = 0;
  ++entry->attempt;
  poll();
  return {true, true, {}};
}
Result TaskListService::cancel(std::string_view id) {
  auto *entry = find(id);
  if (!entry)
    return {false, false, L"任务不存在"};
  if (active_ && active_->id == id) {
    if (entry->state == TaskState::Cancelling)
      return {};
    port_.cancel();
    entry->state = TaskState::Cancelling;
    entry->progress.stage = L"正在取消；已完成的原文件会保留";
    return {true, true, {}};
  }
  const auto queued =
      std::find_if(queue_.begin(), queue_.end(),
                   [&](const auto &item) { return item.id == id; });
  if (queued == queue_.end())
    return {false, false, L"该任务当前没有运行"};
  queue_.erase(queued);
  entry->state = TaskState::Cancelled;
  entry->progress.stage = L"已取消排队";
  return {true, true, {}};
}
Result TaskListService::remove(std::string_view id) {
  if (locked(id))
    return {false, false, L"请先取消任务，等待取消完成后再移除"};
  const auto entry =
      std::find_if(entries_.begin(), entries_.end(),
                   [&](const auto &value) { return value->id == id; });
  if (entry == entries_.end())
    return {false, false, L"任务不存在"};
  entries_.erase(
      entry); // Original files are never deleted by a task-list edit.
  return {true, true, {}};
}
bool TaskListService::dispatch() {
  if (stopping_ || active_ || queue_.empty() || !portReady() ||
      port_.view().busy)
    return false;
  bool changed = false;
  while (!queue_.empty() && !active_) {
    auto item = std::move(queue_.front());
    queue_.pop_front();
    auto *entry = find(item.id);
    if (!entry)
      continue;
    changed = true;
    if (!same_request(item.request, entry->plans.snapshot())) {
      entry->state = TaskState::Failed;
      entry->error = L"排队后任务配置发生变化，未使用旧配置下载；请重新开始";
      continue;
    }
    const auto result = port_.resolve(item.request);
    if (!result) {
      entry->state = TaskState::Failed;
      entry->error = result.error;
      continue;
    }
    const auto view = port_.view();
    active_ = Active{entry->id, item.request, view.jobId,
                     transfer::Kind::resolve, item.download};
    entry->state = TaskState::Resolving;
    entry->progress = view.progress;
  }
  return changed;
}
bool TaskListService::startDownload(TaskEntry &entry) {
  const auto request = entry.plans.snapshot();
  const auto result = port_.start(request, entry.resolved);
  if (!result) {
    entry.state = TaskState::Failed;
    entry.error = result.error;
    return false;
  }
  const auto view = port_.view();
  active_ =
      Active{entry.id, request, view.jobId, transfer::Kind::download, true};
  entry.state = TaskState::Downloading;
  entry.progress = view.progress;
  entry.elapsedMs = 0;
  return true;
}
void TaskListService::complete(transfer::Completion completion) {
  if (!active_ || completion.view.jobId != active_->jobId ||
      completion.view.kind != active_->kind ||
      !same_request(completion.request, active_->request) ||
      completion.view.sessionId != active_->request->sessionId ||
      completion.view.inputRevision != active_->request->inputRevision ||
      completion.view.busy ||
      completion.view.terminal == transfer::Terminal::none)
    return;
  const auto job = *active_;
  auto *entry = find(job.id);
  active_.reset();
  if (!entry)
    return;
  const bool cancelledBeforeTransition =
      entry->state == TaskState::Cancelling || stopping_;
  entry->progress = completion.view.progress;
  entry->elapsedMs = completion.view.elapsedMs;
  entry->error = completion.view.error;
  merge_report(entry->report, std::move(completion.report));
  if (completion.view.terminal == transfer::Terminal::cancelled) {
    entry->state = TaskState::Cancelled;
    return;
  }
  if (completion.view.terminal == transfer::Terminal::failed) {
    entry->state = TaskState::Failed;
    return;
  }
  if (!same_request(job.request, entry->plans.snapshot())) {
    entry->state = TaskState::Failed;
    entry->error = L"运行期间任务配置发生变化，旧结果未写入当前计划；已完成的原"
                   L"文件保留，请重新开始";
    return;
  }
  if (job.kind == transfer::Kind::download) {
    entry->state = TaskState::Succeeded;
    return;
  }
  // A resolve completion can race with a cancel click. Do not launch a new
  // download after that click even if resolution itself already succeeded.
  if (cancelledBeforeTransition) {
    entry->state = TaskState::Cancelled;
    return;
  }
  if (!completion.resolved ||
      !same_request(completion.resolved->request, job.request)) {
    entry->state = TaskState::Failed;
    entry->error = L"解析结果与该任务不一致，请重新解析";
    return;
  }
  std::vector<Resource> resources;
  for (const auto &remote : completion.resolved->resources)
    resources.push_back(remote.descriptor);
  const auto accepted = entry->plans.acceptResolved(
      job.request, std::move(resources), sources::adapter_version);
  if (!accepted) {
    entry->state = TaskState::Failed;
    entry->error = accepted.error;
    return;
  }
  entry->resolved = std::move(completion.resolved);
  entry->state = TaskState::Ready;
  if (job.download)
    startDownload(*entry);
}
bool TaskListService::poll() {
  if (!portReady())
    return false;
  bool changed = false;
  if (auto completion = port_.takeCompletion()) {
    complete(std::move(*completion));
    changed = true;
  }
  if (active_) {
    const auto view = port_.view();
    if (view.jobId == active_->jobId &&
        view.sessionId == active_->request->sessionId &&
        view.inputRevision == active_->request->inputRevision &&
        view.kind == active_->kind) {
      if (auto *entry = find(active_->id)) {
        entry->progress = view.progress;
        entry->elapsedMs = view.elapsedMs;
        if(!view.progress.published.empty()) {
          const auto before=entry->report.files.size();
          transfer::Report published; published.files=view.progress.published;
          merge_report(entry->report,std::move(published));
          changed |= before!=entry->report.files.size();
        }
        if (view.cancelRequested && entry->state != TaskState::Cancelling) {
          entry->state = TaskState::Cancelling;
          changed = true;
        }
      }
    }
  }
  return dispatch() || changed;
}
void TaskListService::stopScheduling() {
  stopping_ = true;
  for (const auto &item : queue_)
    if (auto *entry = find(item.id)) {
      entry->state = TaskState::Cancelled;
      entry->progress.stage = L"程序退出，排队已取消";
    }
  queue_.clear();
  if (active_)
    cancel(active_->id);
}
std::vector<TaskSnapshot> TaskListService::exportSnapshot() const {
  std::vector<TaskSnapshot> result;
  result.reserve(entries_.size());
  for (const auto &entry : entries_)
    result.push_back({entry->id, entry->plans.snapshot(), entry->plans.target(),
                      entry->state, entry->report, entry->error});
  return result;
}
Result TaskListService::restore(std::vector<TaskRestore> records) {
  if (busy())
    return {false, false, L"请等待运行中的任务结束后再恢复任务列表"};
  if (stopping_)
    return {false, false, L"程序正在退出，暂时不能恢复任务列表"};
  if (records.size() > max_task_entries)
    return {false, false, L"任务记录超过 256 条上限，当前列表未改变"};
  std::vector<std::unique_ptr<TaskEntry>> restored;
  std::unordered_set<std::string> ids;
  for (auto &record : records) {
    if (record.id.empty() || record.id.size() > 128 ||
        !ids.insert(record.id).second)
      return {false, false,
              L"任务记录包含无效或重复的任务标识，当前列表未改变"};
    if (static_cast<unsigned>(record.state) >
        static_cast<unsigned>(TaskState::Cancelled))
      return {false, false, L"任务记录包含未知状态，当前列表未改变"};
    auto entry = std::make_unique<TaskEntry>();
    if (auto result = entry->plans.replaceLoaded(std::move(record.plan),
                                                 std::move(record.planPath));
        !result)
      return result;
    entry->id = std::move(record.id);
    entry->state = record.state;
    entry->report = std::move(record.report);
    entry->error = std::move(record.error);
    if (running(entry->state)) {
      entry->state = TaskState::Cancelled;
      entry->error =
          L"上次任务已中断；已完成的文件保留。点击开始后会重新解析链接";
    }
    restored.push_back(std::move(entry));
  }
  entries_ = std::move(restored);
  return {true, true, {}};
}
} // namespace gsdl
