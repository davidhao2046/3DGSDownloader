#pragma once
#include <windows.h>
#include <shellapi.h>
#include <objbase.h>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace gsdl::desktop {
struct FolderOpenResult {
  std::string taskId;
  std::filesystem::path path;
  std::wstring error;
  DWORD win32Error{};
  bool success() const noexcept { return error.empty(); }
};

// The UI submits an immutable task/path capture. Directory checks and Shell
// activation run on one worker, never from hover/layout or the UI thread.
// Callbacks run on that worker and must only post a lifetime-safe notification.
// request_stop rejects new work and drains accepted requests. Keep the receiver
// alive until onStopped. Destruction requests stop but never joins the UI thread;
// the detached worker owns its shared State until COM teardown is complete.
class FolderOpener {
public:
  using Notify = std::function<void()>;
  explicit FolderOpener(Notify onResult, Notify onStopped,
                        std::size_t maxPending = 8)
      : state_(std::make_shared<State>()) {
    if (!maxPending || maxPending > 64)
      throw std::invalid_argument("invalid folder-open queue limit");
    state_->onResult = std::move(onResult);
    state_->onStopped = std::move(onStopped);
    state_->maxPending = maxPending;
    std::thread([state = state_] { state->run(); }).detach();
  }
  ~FolderOpener() { request_stop(); }
  FolderOpener(const FolderOpener &) = delete;
  FolderOpener &operator=(const FolderOpener &) = delete;

  bool open(std::string taskId, std::filesystem::path path) {
    {
      std::lock_guard lock(state_->mutex);
      if (state_->stopping || state_->outstanding >= state_->maxPending)
        return false;
      state_->requests.push_back({std::move(taskId), std::move(path)});
      ++state_->outstanding;
    }
    state_->changed.notify_one();
    return true;
  }
  std::vector<FolderOpenResult> take_results() {
    std::lock_guard lock(state_->mutex);
    std::vector<FolderOpenResult> results;
    results.swap(state_->results);
    state_->outstanding -= results.size();
    return results;
  }
  void request_stop() noexcept {
    {
      std::lock_guard lock(state_->mutex);
      state_->stopping = true;
    }
    state_->changed.notify_one();
  }
  bool stopped() const noexcept {
    std::lock_guard lock(state_->mutex);
    return state_->done;
  }

private:
  struct Request {
    std::string taskId;
    std::filesystem::path path;
  };
  struct State {
    std::mutex mutex;
    std::condition_variable changed;
    std::deque<Request> requests;
    std::vector<FolderOpenResult> results;
    Notify onResult, onStopped;
    std::size_t maxPending{}, outstanding{};
    bool stopping{}, done{};

    static void notify(const Notify &callback) noexcept {
      try { if (callback) callback(); } catch (...) {}
    }
    static FolderOpenResult execute(const Request &request, HRESULT comResult) {
      FolderOpenResult result{request.taskId, request.path, {}, 0};
      if (FAILED(comResult)) {
        result.error = L"无法初始化文件夹打开服务；原文件保留，请重试。";
        return result;
      }
      const auto &name = request.path.native();
      if (!request.path.is_absolute() || name.empty() ||
          name.size() > 32767 || name.find(L'\0') != std::wstring::npos) {
        result.win32Error = ERROR_INVALID_NAME;
        result.error = L"文件夹路径无效；原文件保留，请检查任务保存位置。";
        return result;
      }
      std::error_code error;
      if (!std::filesystem::is_directory(request.path, error)) {
        result.win32Error = error ? DWORD(error.value()) : ERROR_PATH_NOT_FOUND;
        result.error = L"文件夹不存在或无法访问；未创建目录、未改动文件，请检查保存位置。";
        return result;
      }
      SHELLEXECUTEINFOW info{sizeof info};
      // NOASYNC keeps this worker's COM apartment alive through Shell's launch
      // handoff. NO_UI suppresses ordinary Shell error dialogs; Windows security
      // prompts are not bypassed. There is no process wait or UI-thread join.
      info.fMask = SEE_MASK_FLAG_NO_UI | SEE_MASK_NOASYNC;
      info.lpVerb = L"open";
      info.lpFile = name.c_str();
      info.nShow = SW_SHOWNORMAL;
      if (!ShellExecuteExW(&info)) {
        result.win32Error = GetLastError();
        result.error = L"无法打开文件夹；原文件保留，请检查路径或稍后重试。";
      }
      return result;
    }
    void run() noexcept {
      const HRESULT comResult =
          CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
      for (;;) {
        Request capture;
        {
          std::unique_lock lock(mutex);
          changed.wait(lock, [&] { return stopping || !requests.empty(); });
          if (requests.empty() && stopping) break;
          capture = std::move(requests.front());
          requests.pop_front();
        }
        FolderOpenResult result;
        try {
          result = execute(capture, comResult);
        } catch (...) {
          result = {capture.taskId, capture.path,
                    L"打开文件夹时发生异常；原文件保留，请稍后重试。", 0};
        }
        {
          std::lock_guard lock(mutex);
          results.push_back(std::move(result));
        }
        notify(onResult);
      }
      if (SUCCEEDED(comResult)) CoUninitialize();
      {
        std::lock_guard lock(mutex);
        done = true;
      }
      notify(onStopped);
    }
  };
  std::shared_ptr<State> state_;
};
} // namespace gsdl::desktop
