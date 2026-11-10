#include "core/application/plan_service.hpp"
#include "core/application/transfer_service.hpp"
#include "infra/storage/plan_storage.hpp"
#include <condition_variable>
#include <iostream>
#include <windows.h>

namespace {
std::atomic<bool> interrupted{};
BOOL WINAPI handler(DWORD event) {
  if (event == CTRL_C_EVENT || event == CTRL_BREAK_EVENT) {
    interrupted = true;
    return TRUE;
  }
  return FALSE;
}
struct Handler {
  Handler() {
    interrupted = false;
    SetConsoleCtrlHandler(handler, TRUE);
  }
  ~Handler() { SetConsoleCtrlHandler(handler, FALSE); }
};
struct Signal {
  std::mutex mutex;
  std::condition_variable changed;
  bool notified{};
};
std::string quote(std::wstring_view v) {
  std::string out = "\"";
  for (unsigned char c : gsdl::net::utf8(v)) {
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
void require(gsdl::Result r) {
  if (!r)
    throw gsdl::net::Error(r.error);
}
} // namespace
int run_transfer_cli(int argc, wchar_t **argv) {
  try {
    if (argc < 3)
      throw gsdl::net::Error(
          L"用法：resolve URL --out PLAN [--output-dir DIR] "
          L"[--overwrite]；download PLAN [--output-dir DIR]");
    const bool resolving = std::wstring_view(argv[1]) == L"resolve";
    std::filesystem::path output;
    std::wstring directory;
    bool overwrite{};
    for (int i = 3; i < argc; ++i) {
      std::wstring_view arg = argv[i];
      if (arg == L"--overwrite" && resolving)
        overwrite = true;
      else if ((arg == L"--out" && resolving) || arg == L"--output-dir") {
        if (++i == argc)
          throw gsdl::net::Error(L"选项缺少参数");
        if (arg == L"--out")
          output = argv[i];
        else
          directory = argv[i];
      } else
        throw gsdl::net::Error(L"无法识别的下载命令参数");
    }
    if (resolving && output.empty())
      throw gsdl::net::Error(L"resolve 必须指定 --out 计划文件");
    gsdl::PlanService plan;
    if (resolving)
      require(plan.edit("source.shareUrl", {}, argv[2]));
    else
      require(plan.replaceLoaded(gsdl::storage::read_plan(argv[2]), argv[2]));
    if (!directory.empty())
      require(plan.edit("outputDirectory", {}, directory));
    auto signal = std::make_shared<Signal>();
    gsdl::transfer::Service service(
        [signal] {
          std::lock_guard lock(signal->mutex);
          signal->notified = true;
          signal->changed.notify_one();
        },
        [] {});
    Handler control;
    auto wait = [&] {
      for (;;) {
        if (interrupted)
          service.cancel();
        if (auto c = service.take_completion())
          return std::move(*c);
        std::unique_lock lock(signal->mutex);
        signal->changed.wait_for(lock, std::chrono::milliseconds(100),
                                 [&] { return signal->notified; });
        signal->notified = false;
      }
    };
    require(service.resolve(plan.snapshot()));
    auto parsed = wait();
    if (parsed.view.terminal != gsdl::transfer::Terminal::succeeded) {
      std::cout << "{\"ok\":false,\"report\":"
                << gsdl::transfer::encode_report(parsed) << "}\n";
      return parsed.view.terminal == gsdl::transfer::Terminal::cancelled ? 8
                                                                         : 7;
    }
    if (!resolving &&
        !gsdl::transfer::compatible(plan.snapshot(), *parsed.resolved))
      throw gsdl::net::Error(
          L"计划资源版本已改变；请先重新解析并确认选择，未开始下载");
    std::vector<gsdl::Resource> descriptors;
    for (const auto &r : parsed.resolved->resources)
      descriptors.push_back(r.descriptor);
    require(plan.acceptResolved(parsed.request, std::move(descriptors),
                                gsdl::sources::adapter_version));
    if (resolving) {
      auto capture = plan.captureSave(output.wstring());
      gsdl::storage::save_plan_atomic(
          output, capture.snapshot->plan,
          {overwrite ? gsdl::storage::PublishMode::replace_or_create
                     : gsdl::storage::PublishMode::create_new});
      require(plan.acceptSaved(capture));
      std::cout << "{\"ok\":true,\"command\":\"resolve\",\"plan\":"
                << gsdl::storage::encode_plan(plan.snapshot()->plan)
                << ",\"checks\":[";
      bool first = true;
      for (const auto &r : parsed.resolved->resources) {
        if (!first)
          std::cout << ',';
        first = false;
        std::cout << "{\"id\":" << quote(gsdl::net::wide(r.descriptor.id))
                  << ",\"expectedMd5\":" << quote(gsdl::net::wide(r.md5))
                  << "}";
      }
      std::cout << "]}\n";
      return 0;
    }
    require(service.start(plan.snapshot(), parsed.resolved));
    auto result = wait();
    std::cout << "{\"ok\":"
              << (result.view.terminal == gsdl::transfer::Terminal::succeeded
                      ? "true"
                      : "false")
              << ",\"report\":" << gsdl::transfer::encode_report(result)
              << ",\"receiptPath\":"
              << quote(result.report.receiptPath.wstring()) << "}\n";
    return result.view.terminal == gsdl::transfer::Terminal::succeeded   ? 0
           : result.view.terminal == gsdl::transfer::Terminal::cancelled ? 8
                                                                         : 7;
  } catch (const gsdl::net::Error &e) {
    std::cout << "{\"ok\":false,\"error\":" << quote(e.message) << "}\n";
    return 7;
  } catch (const gsdl::storage::StorageError &e) {
    std::cout << "{\"ok\":false,\"error\":" << quote(e.detail().message)
              << "}\n";
    return 5;
  } catch (...) {
    std::cout << "{\"ok\":false,\"error\":\"Transfer failed; existing files "
                 "retained\"}\n";
    return 7;
  }
}
