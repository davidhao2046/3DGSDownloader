#include "transfer_service.hpp"
#include <chrono>
#include <condition_variable>
#include <fstream>
#include <thread>
#include <windows.h>

namespace gsdl::transfer {
namespace {
std::string quote(std::wstring_view value) {
  std::string out = "\"";
  for (unsigned char c : net::utf8(value)) {
    if (c == '"' || c == '\\') {
      out += '\\';
      out += c;
    } else if (c < 32) {
      out += "\\u00";
      out += "0123456789abcdef"[c >> 4];
      out += "0123456789abcdef"[c & 15];
    } else
      out += c;
  }
  return out + '"';
}
std::wstring terminal(Terminal t) {
  switch (t) {
  case Terminal::succeeded:
    return L"succeeded";
  case Terminal::cancelled:
    return L"cancelled";
  case Terminal::failed:
    return L"failed";
  default:
    return L"running";
  }
}
void receipt(Completion &c) {
  if (c.view.kind != Kind::download || c.request->plan.outputDirectory.empty())
    return;
  try {
    std::filesystem::path dir = c.request->plan.outputDirectory;
    dir /= L".3dgsdl-receipts";
    std::filesystem::create_directories(dir);
    auto suffix = std::to_wstring(GetCurrentProcessId()) + L"-" +
                  std::to_wstring(GetTickCount64()) + L"-" +
                  std::to_wstring(c.view.jobId);
    auto target = dir / (suffix + L".json"), part = dir / (suffix + L".tmp");
    auto text = encode_report(c);
    HANDLE f = CreateFileW(part.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE)
      throw std::runtime_error("receipt");
    DWORD n{};
    bool ok = WriteFile(f, text.data(), static_cast<DWORD>(text.size()), &n,
                        nullptr) &&
              n == text.size() && FlushFileBuffers(f);
    CloseHandle(f);
    if (!ok ||
        !MoveFileExW(part.c_str(), target.c_str(), MOVEFILE_WRITE_THROUGH)) {
      DeleteFileW(part.c_str());
      throw std::runtime_error("receipt");
    }
    c.report.receiptPath = target;
  } catch (...) {
    c.report.warning =
        L"文件结果保持，但任务收据写入失败；请保留本次显示的路径和哈希";
  }
}
} // namespace
std::string encode_report(const Completion &c) {
  std::string out =
      "{\"jobId\":" + std::to_string(c.view.jobId) +
      ",\"sessionId\":" + std::to_string(c.request->sessionId) +
      ",\"inputRevision\":" + std::to_string(c.request->inputRevision) +
      ",\"planId\":" + quote(net::wide(c.request->plan.planId)) +
      ",\"terminal\":" + quote(terminal(c.view.terminal)) +
      ",\"error\":" + quote(c.view.error) +
      ",\"warning\":" + quote(c.report.warning) + ",\"files\":[";
  bool first = true;
  for (const auto &f : c.report.files) {
    if (!first)
      out += ',';
    first = false;
    out += "{\"resourceId\":" + quote(net::wide(f.resourceId)) +
           ",\"path\":" + quote(f.path.wstring()) +
           ",\"bytes\":" + std::to_string(f.bytes) +
           ",\"md5\":" + quote(net::wide(f.md5)) +
           ",\"sha256\":" + quote(net::wide(f.sha256)) +
           ",\"expectedMd5\":" + quote(net::wide(f.expectedMd5)) + "}";
  }
  return out + "]}";
}
struct Service::State {
  mutable std::mutex mutex;
  std::condition_variable wake;
  Notify update, stopped;
  bool stopping{};
  unsigned testChunkDelayMs{};
  std::uint64_t nextId{1};
  View view;
  struct Job {
    Snapshot request;
    std::shared_ptr<const sources::ResolvedSource> source;
    std::shared_ptr<CancelGate> gate;
    Kind kind;
    std::vector<FileReceipt> previous;
  };
  std::optional<Job> job;
  std::optional<Completion> result;
  std::shared_ptr<CancelGate> active;
  void run() {
    for (;;) {
      Job work;
      {
        std::unique_lock lock(mutex);
        wake.wait(lock, [&] { return stopping || job.has_value(); });
        if (!job && stopping)
          break;
        work = std::move(*job);
        job.reset();
      }
      Completion done;
      done.request = work.request;
      auto start = std::chrono::steady_clock::now(),
           last = start - std::chrono::seconds(1);
      auto progress = [&](const Progress &p) {
        auto now = std::chrono::steady_clock::now();
        bool send{};
        {
          std::lock_guard lock(mutex);
          send = p.stage != view.progress.stage ||
                 p.completedFiles != view.progress.completedFiles ||
                 now - last > std::chrono::milliseconds(100);
          view.progress = p;
          view.elapsedMs =
              std::chrono::duration_cast<std::chrono::milliseconds>(now - start)
                  .count();
        }
        if (send) {
          last = now;
          update();
        }
      };
      try {
        net::WinHttp http(testChunkDelayMs);
        if (work.kind == Kind::resolve) {
          progress({L"正在解析公开分享"});
          done.resolved =
              std::make_shared<const sources::ResolvedSource>(sources::resolve(
                  work.request, http, [&] { work.gate->check(); }));
          work.gate->check();
        } else
          download(work.request, *work.source, http, *work.gate, progress,
                   done.report, work.previous);
        done.view.terminal = Terminal::succeeded;
      } catch (const net::Cancelled &) {
        done.view.terminal = Terminal::cancelled;
        done.view.error = L"已取消；已完成文件保留，当前未完成文件未发布";
      } catch (const net::Error &e) {
        done.view.terminal = Terminal::failed;
        done.view.error = e.message;
      } catch (...) {
        done.view.terminal = Terminal::failed;
        done.view.error = L"处理失败；已完成文件保持，请检查目录和来源后重试";
      }
      {
        std::lock_guard lock(mutex);
        auto end = done.view.terminal;
        auto error = std::move(done.view.error);
        done.view = view;
        done.view.terminal = end;
        done.view.error = std::move(error);
        done.view.elapsedMs =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - start)
                .count();
      }
      receipt(done);
      {
        std::lock_guard lock(mutex);
        done.view.busy = false;
        done.view.cancelRequested = work.gate->requested();
        view = done.view;
        result = std::move(done);
        active.reset();
      }
      update();
    }
    stopped();
  }
};
Service::Service(Notify update, Notify stopped, unsigned testChunkDelayMs)
    : state_(std::make_shared<State>()) {
  state_->update = std::move(update);
  state_->stopped = std::move(stopped);
  state_->testChunkDelayMs = testChunkDelayMs;
  std::thread([s = state_] { s->run(); }).detach();
}
Service::~Service() { stop(); }
Result Service::submit(Snapshot request,
                       std::shared_ptr<const sources::ResolvedSource> source,
                       Kind kind, std::vector<FileReceipt> previous) {
  if (!request)
    return {false, false, L"当前计划不存在"};
  if (kind == Kind::resolve &&
      (request->plan.internalFixture || request->plan.source.shareUrl.empty()))
    return {false, false, L"请先提交公开分享链接"};
  if (kind == Kind::download && (!source || !compatible(request, *source) ||
                                 request->plan.outputDirectory.empty()))
    return {false, false, L"请先解析资源并设置保存目录"};
  {
    std::lock_guard lock(state_->mutex);
    if (state_->stopping || state_->view.busy || state_->result)
      return {false, false, L"请先等待当前任务结果"};
    auto gate = std::make_shared<CancelGate>();
    state_->active = gate;
    state_->view = {state_->nextId++,
                    request->sessionId,
                    request->inputRevision,
                    kind,
                    Terminal::none,
                    true,
                    false,
                    {L"排队中"},
                    0,
                    {}};
    state_->job = State::Job{std::move(request), std::move(source), gate, kind, std::move(previous)};
  }
  state_->wake.notify_one();
  return {true, false, {}};
}
Result Service::resolve(Snapshot request) {
  return submit(std::move(request), {}, Kind::resolve);
}
Result Service::start(Snapshot request,
                      std::shared_ptr<const sources::ResolvedSource> source,
                      std::vector<FileReceipt> previous) {
  return submit(std::move(request), std::move(source), Kind::download, std::move(previous));
}
void Service::cancel() noexcept {
  std::shared_ptr<CancelGate> gate;
  {
    std::lock_guard lock(state_->mutex);
    gate = state_->active;
    if (gate)
      state_->view.cancelRequested = true;
  }
  if (gate)
    gate->request();
}
View Service::view() const {
  std::lock_guard lock(state_->mutex);
  return state_->view;
}
std::optional<Completion> Service::take_completion() {
  std::lock_guard lock(state_->mutex);
  auto out = std::move(state_->result);
  state_->result.reset();
  return out;
}
void Service::stop() noexcept {
  {
    std::lock_guard lock(state_->mutex);
    state_->stopping = true;
    if (state_->active)
      state_->active->request();
  }
  state_->wake.notify_one();
}
} // namespace gsdl::transfer
