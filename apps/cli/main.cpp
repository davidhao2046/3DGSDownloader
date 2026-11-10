#include "core/application/plan_service.hpp"
#include "infra/storage/plan_storage.hpp"
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <vector>
#include <windows.h>

namespace fs = std::filesystem;
namespace storage = gsdl::storage;
namespace {
struct CliError {
  int exitCode;
  std::string code;
  std::wstring message;
};
enum class Operation {
  outputDirectory,
  rename,
  included,
  shareUrl,
  undo,
  redo
};
struct Edit {
  Operation operation;
  std::wstring value;
  std::string resourceId;
  unsigned count{};
  bool included{};
};
struct Arguments {
  std::wstring command;
  fs::path input, output;
  bool fixture{}, overwrite{};
  std::vector<Edit> edits;
};
std::string utf8(std::wstring_view value) {
  if (value.empty())
    return {};
  const auto size = WideCharToMultiByte(
      CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
      static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
  if (!size)
    throw CliError{2, "invalid_unicode", L"参数包含无效 Unicode 文本"};
  std::string output(size, '\0');
  WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
                      static_cast<int>(value.size()), output.data(), size,
                      nullptr, nullptr);
  return output;
}
std::string quote(std::string_view value) {
  constexpr char hex[] = "0123456789abcdef";
  std::string output = "\"";
  for (unsigned char c : value) {
    if (c == '"' || c == '\\') {
      output += '\\';
      output += static_cast<char>(c);
    } else if (c < 32) {
      output += "\\u00";
      output += hex[c >> 4];
      output += hex[c & 15];
    } else
      output += static_cast<char>(c);
  }
  return output + '"';
}
std::string quote(std::wstring_view value) { return quote(utf8(value)); }
[[noreturn]] void usage(std::wstring message) {
  throw CliError{2, "usage", std::move(message)};
}
unsigned count(std::wstring_view value) {
  if (value.empty())
    usage(L"撤销/重做次数必须是 0 到 100000 的整数");
  unsigned result = 0;
  for (auto c : value) {
    if (c < L'0' || c > L'9' || result > 10000)
      usage(L"撤销/重做次数必须是 0 到 100000 的整数");
    result = result * 10 + static_cast<unsigned>(c - L'0');
    if (result > 100000)
      usage(L"撤销/重做次数必须是 0 到 100000 的整数");
  }
  return result;
}
Arguments parse(int argc, wchar_t **argv) {
  if (argc < 2)
    usage(L"需要 new、inspect 或 edit 子命令；使用 --help 查看语法");
  Arguments result;
  result.command = argv[1];
  if (result.command != L"new" && result.command != L"inspect" &&
      result.command != L"edit")
    usage(L"未知子命令");
  int i = 2;
  if (result.command != L"new") {
    if (i == argc || std::wstring_view(argv[i]).starts_with(L"--"))
      usage(L"需要输入计划文件路径");
    result.input = argv[i++];
  }
  bool hasOutput = false, hasFixture = false, hasOverwrite = false;
  const auto next = [&]() -> std::wstring {
    if (i == argc)
      usage(L"选项缺少参数值");
    return argv[i++];
  };
  while (i < argc) {
    const std::wstring option = argv[i++];
    if (result.command == L"inspect")
      usage(L"inspect 只接收一个计划路径");
    if (option == L"--out") {
      if (hasOutput)
        usage(L"--out 不能重复");
      hasOutput = true;
      result.output = next();
      if (result.output.empty())
        usage(L"输出计划路径不能为空");
    } else if (option == L"--overwrite") {
      if (hasOverwrite)
        usage(L"--overwrite 不能重复");
      hasOverwrite = result.overwrite = true;
    } else if (option == L"--fixture" && result.command == L"new") {
      if (hasFixture)
        usage(L"--fixture 不能重复");
      hasFixture = result.fixture = true;
    } else if (result.command == L"edit") {
      if (option == L"--output-dir")
        result.edits.push_back({Operation::outputDirectory, next()});
      else if (option == L"--link")
        result.edits.push_back({Operation::shareUrl, next()});
      else if (option == L"--rename") {
        const auto id = utf8(next());
        result.edits.push_back({Operation::rename, next(), id});
      } else if (option == L"--include") {
        const auto id = utf8(next());
        const auto value = next();
        if (value != L"true" && value != L"false")
          usage(L"--include 的值必须是 true 或 false");
        result.edits.push_back(
            {Operation::included, {}, id, 0, value == L"true"});
      } else if (option == L"--undo" || option == L"--redo")
        result.edits.push_back(
            {option == L"--undo" ? Operation::undo : Operation::redo,
             {},
             {},
             count(next())});
      else
        usage(L"未知 edit 选项: " + option);
    } else
      usage(L"未知 new 选项: " + option);
  }
  if (result.command == L"new" && !hasOutput)
    usage(L"new 必须指定 --out");
  if (result.command == L"edit" && !hasOutput)
    result.output = result.input;
  if (!result.input.empty())
    result.input = fs::absolute(result.input).lexically_normal();
  if (!result.output.empty())
    result.output = fs::absolute(result.output).lexically_normal();
  return result;
}
void require(const gsdl::Result &result, int exitCode = 3,
             std::string code = "validation") {
  if (!result)
    throw CliError{exitCode, std::move(code), result.error};
}
void execute(gsdl::PlanService &service, const Edit &edit) {
  switch (edit.operation) {
  case Operation::outputDirectory:
    require(service.edit("outputDirectory", {}, edit.value));
    break;
  case Operation::shareUrl:
    require(service.edit("source.shareUrl", {}, edit.value));
    break;
  case Operation::rename:
    require(service.edit("resource.outputName", edit.resourceId, edit.value));
    break;
  case Operation::included: {
    const std::string id[] = {edit.resourceId};
    require(service.setIncluded(id, edit.included));
    break;
  }
  case Operation::undo:
  case Operation::redo:
    for (unsigned i = 0; i < edit.count; ++i)
      require(edit.operation == Operation::undo ? service.undo()
                                                : service.redo());
    break;
  }
}
std::string state(const gsdl::PlanService &service) {
  const auto snapshot = service.snapshot();
  return "{\"sessionId\":" + std::to_string(snapshot->sessionId) +
         ",\"revision\":" + std::to_string(snapshot->revision) +
         ",\"inputRevision\":" + std::to_string(snapshot->inputRevision) +
         ",\"dirty\":" + (service.dirty() ? "true" : "false") +
         ",\"undoCount\":" + std::to_string(service.undo_count()) +
         ",\"redoCount\":" + std::to_string(service.redo_count()) +
         ",\"target\":" + quote(service.target()) + "}";
}
void failure(int exitCode, std::string_view code, std::wstring_view message,
             const gsdl::PlanService *service,
             const storage::Error *detail = nullptr) {
  std::cout << "{\"ok\":false,\"exitCode\":" << exitCode
            << ",\"error\":{\"code\":" << quote(code)
            << ",\"message\":" << quote(message);
  if (detail)
    std::cout << ",\"stage\":" << quote(detail->stage)
              << ",\"nativeCode\":" << detail->nativeCode
              << ",\"recoveryPath\":" << quote(detail->recoveryPath.wstring());
  std::cout << '}';
  if (service)
    std::cout << ",\"state\":" << state(*service);
  std::cout << "}\n";
}
void help() {
  std::cout << "3DGSDownloaderCli - plans and public Insta360 downloads\n"
               "  resolve URL --out PLAN [--output-dir DIR] [--overwrite]\n"
               "  download PLAN [--output-dir DIR]\n"
               "  new [--fixture] --out PATH [--overwrite]\n"
               "  inspect PATH\n"
               "  edit PATH [--output-dir DIR] [--link URL]\n"
               "    [--rename ID NAME] [--include ID true|false]\n"
               "    [--undo N] [--redo N] [--out PATH] [--overwrite]\n"
               "Edit operations run in argument order; no files are saved if "
               "one fails.\n"
               "Default edit output is its input. Other existing outputs need "
               "--overwrite.\n"
               "Downloads never overwrite existing resource files. Ctrl+C cancels.\n"
               "Transfer exit codes: 7 failed, 8 cancelled. UTF-8 JSON output.\n"
               "Fixture IDs: model, cameras, voxel (local tests only).\n";
}
} // namespace

int run_transfer_cli(int argc,wchar_t** argv);
int wmain(int argc, wchar_t **argv) {
  SetConsoleOutputCP(CP_UTF8);
  if(argc>1&&(std::wstring_view(argv[1])==L"resolve"||std::wstring_view(argv[1])==L"download"))return run_transfer_cli(argc,argv);
  std::unique_ptr<gsdl::PlanService> service;
  std::optional<gsdl::SaveCapture> pendingSave;
  int ioExitCode = 4;
  try {
    if (argc == 2 && (std::wstring_view(argv[1]) == L"--help" ||
                      std::wstring_view(argv[1]) == L"-h")) {
      help();
      return 0;
    }
    const auto arguments = parse(argc, argv);
    service = std::make_unique<gsdl::PlanService>();
    if (arguments.command == L"new")
      require(service->newPlan(arguments.fixture));
    else
      require(service->replaceLoaded(storage::read_plan(arguments.input),
                                     arguments.input.wstring()),
              4, "open");
    for (const auto &edit : arguments.edits)
      execute(*service, edit);
    if (arguments.command != L"inspect") {
      ioExitCode = 5;
      const auto capture = service->captureSave(arguments.output.wstring());
      pendingSave = capture;
      const bool inputTarget =
          !arguments.input.empty() &&
          _wcsicmp(arguments.input.c_str(), arguments.output.c_str()) == 0;
      storage::save_plan_atomic(arguments.output, capture.snapshot->plan,
                                {arguments.overwrite || inputTarget
                                     ? storage::PublishMode::replace_or_create
                                     : storage::PublishMode::create_new});
      require(service->acceptSaved(capture), 5, "save_confirmation");
      pendingSave.reset();
    }
    std::cout << "{\"ok\":true,\"command\":" << quote(arguments.command)
              << ",\"state\":" << state(*service)
              << ",\"plan\":" << storage::encode_plan(service->snapshot()->plan)
              << "}\n";
    return 0;
  } catch (const CliError &error) {
    if (pendingSave && service)
      service->rejectSave(*pendingSave);
    failure(error.exitCode, error.code, error.message, service.get());
    return error.exitCode;
  } catch (const storage::StorageError &error) {
    if (pendingSave && service)
      service->rejectSave(*pendingSave);
    failure(ioExitCode, ioExitCode == 4 ? "open" : "save",
            error.detail().message, service.get(), &error.detail());
    return ioExitCode;
  } catch (const std::exception &) {
    if (pendingSave && service)
      service->rejectSave(*pendingSave);
    failure(6, "internal", L"命令执行失败；计划未被确认保存", service.get());
    return 6;
  }
}
