#include "task_workspace.hpp"
#include "infra/net/bounded_json.hpp"
#include <algorithm>
#include <array>
#include <windows.h> // Must precede bcrypt.h for Windows SDK types.

#include <bcrypt.h>
#include <charconv>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <set>
#include <thread>
#include <variant>

namespace gsdl::storage {
namespace {
using Json = net::json::Value;
[[noreturn]] void
bad(std::wstring message =
        L"任务记录格式错误或超出读取限制；现有任务和原文件保持不变。") {
  throw StorageError({std::move(message), "workspace", 0, {}});
}
[[noreturn]] void os_error(std::string stage, DWORD code = GetLastError(),
                           std::filesystem::path recovery = {}) {
  throw StorageError({L"无法保存或读取任务记录（Windows 错误 " +
                          std::to_wstring(code) +
                          L"）；当前任务仍保留在内存中，请检查目录权限后重试。",
                      std::move(stage), code, std::move(recovery)});
}
void check_directory(std::wstring_view text) {
  if (!text.empty() && !validate_output_directory(text))
    bad(L"任务记录中的下载目录无效；现有任务保持不变。");
}
void check_path(std::wstring_view text, bool allowEmpty = true) {
  if (text.empty() && allowEmpty)
    return;
  if (text.empty() || text.size() > 32000 || text.find(L'\0') != text.npos ||
      !std::filesystem::path(text).is_absolute() ||
      text.find(L"://") != text.npos)
    bad(L"任务记录中的本地文件路径无效。");
}
bool valid_id(std::string_view text) {
  return !text.empty() && text.size() <= 128 &&
         std::all_of(text.begin(), text.end(), [](unsigned char c) {
           return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                  (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.';
         });
}
void check_hash(std::string_view text, std::size_t count) {
  if (text.empty())
    return;
  if (text.size() != count ||
      !std::all_of(text.begin(), text.end(), [](unsigned char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
               (c >= 'A' && c <= 'F');
      }))
    bad(L"任务记录中的文件校验值无效。");
}
std::wstring safe_message(std::wstring_view value) {
  if (value.size() > 4096)
    return L"上次操作未完成，请重新解析后重试。";
  std::wstring lower(value);
  std::transform(lower.begin(), lower.end(), lower.begin(), [](wchar_t c) {
    return c >= L'A' && c <= L'Z' ? static_cast<wchar_t>(c + 32) : c;
  });
  // Error messages are normally already sanitized by the transfer service.
  // Enforce the persistence boundary independently so future callers cannot
  // accidentally turn a raw response/signed resource URL into a saved log.
  for (auto token : {L"://", L"signature=", L"credential=", L"security-token=",
                     L"accesskeyid="})
    if (lower.find(token) != lower.npos)
      return L"上次操作未完成，详细远程地址未保存；请重新解析后重试。";
  if (value.find(L'\0') != value.npos)
    bad();
  return std::wstring(value);
}
void quote(std::string &out, std::string_view value) {
  if (value.size() > max_workspace_bytes)
    bad();
  net::wide(value); // strict UTF-8
  constexpr char hex[] = "0123456789abcdef";
  out += '"';
  for (unsigned char c : value) {
    if (c == '"' || c == '\\') {
      out += '\\';
      out += static_cast<char>(c);
    } else if (c < 32) {
      out += "\\u00";
      out += hex[c >> 4];
      out += hex[c & 15];
    } else
      out += static_cast<char>(c);
  }
  out += '"';
}
void prop(std::string &out, std::string_view name, std::string_view value) {
  quote(out, name);
  out += ':';
  quote(out, value);
}
void prop(std::string &out, std::string_view name, std::wstring_view value) {
  prop(out, name, net::utf8(value));
}
const Json::Object &object(const Json &value,
                           std::initializer_list<std::string_view> fields) {
  const auto p = std::get_if<Json::Object>(&value.data);
  if (!p || p->size() != fields.size())
    bad();
  for (auto field : fields)
    if (!p->contains(field))
      bad();
  return *p;
}
std::uint64_t number(const Json &v) {
  const auto p = std::get_if<net::json::Number>(&v.data);
  if (!p)
    bad();
  std::uint64_t out{};
  auto r =
      std::from_chars(p->text.data(), p->text.data() + p->text.size(), out);
  if (r.ec != std::errc{} || r.ptr != p->text.data() + p->text.size())
    bad();
  return out;
}
bool boolean(const Json &value) {
  if(const auto result=std::get_if<bool>(&value.data))return *result;
  bad(L"任务记录中的下载偏好必须是布尔值。");
}
void encode_preferences(std::string &out,const SelectionPreferences &preferences) {
  if(preferences.choices.size()>selection_preference_keys.size() ||
      (!preferences.remembered && !preferences.choices.empty()))bad();
  out += preferences.remembered?"{\"remembered\":true,\"choices\":{":"{\"remembered\":false,\"choices\":{";
  bool first=true;
  for(const auto &[key,included]:preferences.choices) {
    if(!valid_selection_preference_key(key))bad(L"任务记录含有未知下载偏好类别。");
    if(!first)out+=',';
    first=false;quote(out,key);out+=included?":true":":false";
  }
  out+="}}";
}
SelectionPreferences decode_preferences(const Json &value) {
  object(value,{"remembered","choices"});
  SelectionPreferences preferences;
  preferences.remembered=boolean(value.at("remembered"));
  const auto choices=std::get_if<Json::Object>(&value.at("choices").data);
  if(!choices || choices->size()>selection_preference_keys.size())bad();
  for(const auto &[key,selected]:*choices) {
    if(!valid_selection_preference_key(key))bad(L"任务记录含有未知下载偏好类别。");
    preferences.choices.emplace(key,boolean(selected));
  }
  if(!preferences.remembered && !preferences.choices.empty())bad();
  return preferences;
}
void render_json(std::string &out, const Json &value) {
  if (std::holds_alternative<std::nullptr_t>(value.data))
    out += "null";
  else if (const auto boolean = std::get_if<bool>(&value.data))
    out += *boolean ? "true" : "false";
  else if (const auto numberValue = std::get_if<net::json::Number>(&value.data))
    out += numberValue->text;
  else if (const auto textValue = std::get_if<std::string>(&value.data))
    quote(out, *textValue);
  else if (const auto array = std::get_if<Json::Array>(&value.data)) {
    out += '[';
    bool first = true;
    for (const auto &v : *array) {
      if (!first)
        out += ',';
      first = false;
      render_json(out, v);
    }
    out += ']';
  } else {
    out += '{';
    bool first = true;
    for (const auto &[key, v] : std::get<Json::Object>(value.data)) {
      if (!first)
        out += ',';
      first = false;
      quote(out, key);
      out += ':';
      render_json(out, v);
    }
    out += '}';
  }
}
constexpr std::array<std::string_view, 9> state_names{
    "draft",      "queued",    "resolving", "ready",    "downloading",
    "cancelling", "succeeded", "failed",    "cancelled"};
std::string_view state_text(TaskState state) {
  auto index = static_cast<std::size_t>(state);
  if (index >= state_names.size())
    bad();
  return state_names[index];
}
TaskState read_state(std::string_view text) {
  for (std::size_t i = 0; i < state_names.size(); ++i)
    if (state_names[i] == text)
      return static_cast<TaskState>(i);
  bad(L"任务记录包含未知状态；不会自动转换或启动下载。");
}
void encode_report(std::string &out, const transfer::Report &report) {
  if (report.files.size() > 4096)
    bad();
  check_path(report.receiptPath.native());
  out += '{';
  prop(out, "receiptPath", report.receiptPath.native());
  out += ',';
  prop(out, "warning", safe_message(report.warning));
  out += ",\"files\":[";
  bool first = true;
  for (const auto &file : report.files) {
    if (!valid_id(file.resourceId))
      bad();
    check_path(file.path.native(), false);
    check_hash(file.md5, 32);
    check_hash(file.expectedMd5, 32);
    check_hash(file.sha256, 64);
    if (!first)
      out += ',';
    first = false;
    out += '{';
    prop(out, "resourceId", file.resourceId);
    out += ',';
    prop(out, "path", file.path.native());
    out += ",\"bytes\":" + std::to_string(file.bytes) + ',';
    prop(out, "md5", file.md5);
    out += ',';
    prop(out, "sha256", file.sha256);
    out += ',';
    prop(out, "expectedMd5", file.expectedMd5);
    out += '}';
  }
  out += "]}";
}
transfer::Report decode_report(const Json &value) {
  object(value, {"receiptPath", "warning", "files"});
  transfer::Report report;
  report.receiptPath = net::wide(value.at("receiptPath").text());
  report.warning = safe_message(net::wide(value.at("warning").text()));
  const auto &files = value.at("files").array();
  if (files.size() > 4096)
    bad();
  for (const auto &v : files) {
    object(v, {"resourceId", "path", "bytes", "md5", "sha256", "expectedMd5"});
    report.files.push_back({v.at("resourceId").text(),
                            net::wide(v.at("path").text()),
                            number(v.at("bytes")), v.at("md5").text(),
                            v.at("sha256").text(), v.at("expectedMd5").text()});
  }
  std::string validated;
  encode_report(validated, report);
  return report;
}
std::filesystem::path absolute_path(const std::filesystem::path &path) {
  if (path.empty() || path.native().find(L'\0') != path.native().npos ||
      path.native().size() > 32000)
    bad(L"任务记录文件路径无效。");
  std::error_code ec;
  auto out = std::filesystem::absolute(path, ec).lexically_normal();
  if (ec || out.filename().empty() || out.filename() == L"." ||
      out.filename() == L"..")
    bad();
  return out;
}
std::wstring native_path(const std::filesystem::path &path) {
  auto value = path.native();
  if (value.starts_with(L"\\\\?\\"))
    return value;
  if (value.starts_with(L"\\\\"))
    return L"\\\\?\\UNC\\" + value.substr(2);
  return L"\\\\?\\" + value;
}
struct Handle {
  HANDLE value{INVALID_HANDLE_VALUE};
  ~Handle() {
    if (value != INVALID_HANDLE_VALUE)
      CloseHandle(value);
  }
  bool valid() const { return value != INVALID_HANDLE_VALUE; }
  bool close() {
    auto old = value;
    value = INVALID_HANDLE_VALUE;
    return CloseHandle(old) != FALSE;
  }
};
std::wstring suffix() {
  std::array<unsigned char, 16> bytes{};
  if (BCryptGenRandom(nullptr, bytes.data(), static_cast<ULONG>(bytes.size()),
                      BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0)
    bad(L"无法创建任务记录临时文件名；未写入文件。");
  std::wstring result;
  for (auto c : bytes) {
    result += L"0123456789abcdef"[c >> 4];
    result += L"0123456789abcdef"[c & 15];
  }
  return result;
}
struct Temporary {
  std::filesystem::path path;
  bool owned{};
  ~Temporary() {
    if (owned)
      DeleteFileW(native_path(path).c_str());
  }
};
void inject(SaveFault point, SaveFault wanted) {
  if (point == wanted)
    os_error("workspace_test_injection", ERROR_DISK_FULL);
}
} // namespace

std::string encode_workspace(const WorkspaceSnapshot &snapshot) {
  try {
    if (snapshot.tasks.size() > max_workspace_tasks)
      bad(L"任务列表超过 256 条保存上限。");
    check_directory(snapshot.lastDirectory);
    check_directory(snapshot.lastUsedBaseDirectory);
    std::string out = "{\"schemaVersion\":3,";
    prop(out, "lastDirectory", snapshot.lastDirectory);
    out+=',';
    prop(out,"lastUsedBaseDirectory",snapshot.lastUsedBaseDirectory);
    out+=",\"selectionPreferences\":";
    encode_preferences(out,snapshot.selectionPreferences);
    out += ",\"recentDirectories\":[";
    if(snapshot.recentDirectories.size()>8) bad();
    for(std::size_t i=0;i<snapshot.recentDirectories.size();++i) {
      check_directory(snapshot.recentDirectories[i]);
      if(i) out += ',';
      quote(out, net::utf8(snapshot.recentDirectories[i]));
    }
    out += "],\"tasks\":[";
    bool first = true;
    std::set<std::string> ids;
    for (const auto &task : snapshot.tasks) {
      if (!valid_id(task.id) || !ids.insert(task.id).second || !task.document)
        bad();
      check_path(task.planPath);
      if (!first)
        out += ',';
      first = false;
      out += '{';
      prop(out, "id", task.id);
      out += ',';
      prop(out, "planPath", task.planPath);
      out += ',';
      prop(out, "state", state_text(task.state));
      out += ',';
      prop(out, "error", safe_message(task.error));
      out += ",\"plan\":";
      out += encode_plan(task.document->plan);
      out += ",\"report\":";
      encode_report(out, task.report);
      out += '}';
      if (out.size() > max_workspace_bytes)
        bad();
    }
    out += "]}\n";
    if (out.size() > max_workspace_bytes)
      bad();
    // Combined documents must also fit the parser's node/string limits. Never
    // publish a workspace which this exact version could not reopen.
    (void)decode_workspace(out);
    return out;
  } catch (const net::Error &) {
    bad(L"任务记录包含无效的文字编码；未保存任务记录。");
  }
}
WorkspaceData decode_workspace(std::string_view bytes) {
  try {
    if (bytes.size() > max_workspace_bytes)
      bad();
    auto root = net::json::Parser(bytes).parse();
    const bool v3=root.at("schemaVersion").number("3");
    const bool v2=root.at("schemaVersion").number("2");
    const bool v1=root.at("schemaVersion").number("1");
    if (!v3 && !v2 && !v1)
      bad(L"任务记录版本不受支持；原文件保持不变。");
    if(v3)object(root,{"schemaVersion","lastDirectory","lastUsedBaseDirectory","selectionPreferences","recentDirectories","tasks"});
    else if(v2) object(root, {"schemaVersion", "lastDirectory", "recentDirectories", "tasks"});
    else object(root, {"schemaVersion", "lastDirectory", "tasks"});
    WorkspaceData result;
    result.lastDirectory = net::wide(root.at("lastDirectory").text());
    check_directory(result.lastDirectory);
    if(v3) {
      result.lastUsedBaseDirectory=net::wide(root.at("lastUsedBaseDirectory").text());
      check_directory(result.lastUsedBaseDirectory);
      result.selectionPreferences=decode_preferences(root.at("selectionPreferences"));
    }
    // Older recent-directory lists include abandoned chooser drafts. They must
    // not be reinterpreted as a confirmed last-used download destination.
    if(v2 || v3) {
      const auto &recent=root.at("recentDirectories").array();
      if(recent.size()>8) bad();
      for(const auto &value:recent) { auto path=net::wide(value.text()); check_directory(path); result.recentDirectories.push_back(std::move(path)); }
    }
    const auto &tasks = root.at("tasks").array();
    if (tasks.size() > max_workspace_tasks)
      bad();
    std::set<std::string> ids;
    for (const auto &v : tasks) {
      object(v, {"id", "planPath", "state", "error", "plan", "report"});
      TaskRestore task;
      task.id = v.at("id").text();
      if (!valid_id(task.id) || !ids.insert(task.id).second)
        bad();
      task.planPath = net::wide(v.at("planPath").text());
      check_path(task.planPath);
      task.state = read_state(v.at("state").text());
      task.error = safe_message(net::wide(v.at("error").text()));
      std::string plan;
      render_json(plan, v.at("plan"));
      task.plan = decode_plan(plan);
      task.report = decode_report(v.at("report"));
      result.tasks.push_back(std::move(task));
    }
    return result;
  } catch (const net::Error &) {
    bad();
  }
}
std::optional<WorkspaceData>
read_workspace(const std::filesystem::path &requested) {
  const auto path = absolute_path(requested);
  Handle file{CreateFileW(native_path(path).c_str(), GENERIC_READ,
                          FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                          FILE_FLAG_SEQUENTIAL_SCAN, nullptr)};
  if (!file.valid()) {
    const auto error = GetLastError();
    if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND)
      return {};
    os_error("workspace_read", error);
  }
  LARGE_INTEGER size{};
  if (!GetFileSizeEx(file.value, &size))
    os_error("workspace_read_size");
  if (size.QuadPart < 0 || size.QuadPart > max_workspace_bytes)
    bad();
  std::string bytes(static_cast<std::size_t>(size.QuadPart), '\0');
  std::size_t offset{};
  while (offset < bytes.size()) {
    DWORD count{};
    if (!ReadFile(file.value, bytes.data() + offset,
                  static_cast<DWORD>(
                      std::min<std::size_t>(65536, bytes.size() - offset)),
                  &count, nullptr))
      os_error("workspace_read");
    if (!count)
      bad(L"任务记录在读取中被截断；现有任务保持不变。");
    offset += count;
  }
  char extra{};
  DWORD count{};
  if (!ReadFile(file.value, &extra, 1, &count, nullptr))
    os_error("workspace_read_tail");
  if (count)
    bad(L"任务记录在读取中发生变化；请重试。");
  auto decoded=decode_workspace(bytes);
  for(auto &task:decoded.tasks) for(auto &receipt:task.report.files) receipt.localState=transfer::check_local(receipt);
  return decoded;
}
void save_workspace_atomic(const std::filesystem::path &requested,
                           const WorkspaceSnapshot &snapshot, SaveFault fault) {
  const auto bytes =
      encode_workspace(snapshot); // validate before filesystem mutation
  const auto path = absolute_path(requested);
  std::error_code ec;
  std::filesystem::create_directories(path.parent_path(), ec);
  if (ec)
    os_error("workspace_directory", static_cast<DWORD>(ec.value()));
  const auto target = native_path(path);
  const DWORD attr = GetFileAttributesW(target.c_str());
  const DWORD attrError =
      attr == INVALID_FILE_ATTRIBUTES ? GetLastError() : ERROR_SUCCESS;
  const bool exists = attr != INVALID_FILE_ATTRIBUTES;
  if (exists && (attr & FILE_ATTRIBUTE_DIRECTORY))
    os_error("workspace_target", ERROR_DIRECTORY);
  if (!exists && attrError != ERROR_FILE_NOT_FOUND &&
      attrError != ERROR_PATH_NOT_FOUND)
    os_error("workspace_target", attrError);
  Temporary temporary;
  Handle file;
  for (int attempt = 0; attempt < 8; ++attempt) {
    temporary.path =
        path.parent_path() / (L".3dgsdl-workspace-" + suffix() + L".tmp");
    file.value =
        CreateFileW(native_path(temporary.path).c_str(), GENERIC_WRITE, 0,
                    nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file.valid()) {
      temporary.owned = true;
      break;
    }
    if (GetLastError() != ERROR_FILE_EXISTS &&
        GetLastError() != ERROR_ALREADY_EXISTS)
      os_error("workspace_temporary");
  }
  if (!file.valid())
    os_error("workspace_temporary", ERROR_FILE_EXISTS);
  inject(SaveFault::before_write, fault);
  std::size_t offset{};
  while (offset < bytes.size()) {
    DWORD count{};
    if (!WriteFile(file.value, bytes.data() + offset,
                   static_cast<DWORD>(
                       std::min<std::size_t>(65536, bytes.size() - offset)),
                   &count, nullptr))
      os_error("workspace_write");
    if (!count)
      os_error("workspace_write", ERROR_WRITE_FAULT);
    offset += count;
  }
  inject(SaveFault::after_write, fault);
  if (!FlushFileBuffers(file.value))
    os_error("workspace_flush");
  if (!file.close())
    os_error("workspace_close");
  inject(SaveFault::before_publish, fault);
  if (exists) {
    const auto backup =
        path.parent_path() / (L".3dgsdl-workspace-" + suffix() + L".rollback");
    if (GetFileAttributesW(native_path(backup).c_str()) !=
        INVALID_FILE_ATTRIBUTES)
      os_error("workspace_backup", ERROR_FILE_EXISTS);
    if (!ReplaceFileW(target.c_str(), native_path(temporary.path).c_str(),
                      native_path(backup).c_str(), 0, nullptr, nullptr)) {
      const auto error = GetLastError();
      const bool retained = GetFileAttributesW(native_path(backup).c_str()) !=
                            INVALID_FILE_ATTRIBUTES;
      if (retained &&
          GetFileAttributesW(target.c_str()) == INVALID_FILE_ATTRIBUTES) {
        if (!MoveFileExW(native_path(backup).c_str(), target.c_str(),
                         MOVEFILE_WRITE_THROUGH))
          os_error("workspace_rollback", GetLastError(), backup);
      } else if (retained)
        os_error("workspace_replace", error, backup);
      os_error("workspace_replace", error);
    }
    temporary.owned = false;
    DeleteFileW(
        native_path(backup)
            .c_str()); // a retained old backup does not negate publication
  } else {
    if (!MoveFileExW(native_path(temporary.path).c_str(), target.c_str(),
                     MOVEFILE_WRITE_THROUGH))
      os_error("workspace_publish");
    temporary.owned = false;
  }
}

struct WorkspaceWorker::State {
  using Request = std::variant<WorkspaceRead, WorkspaceSave>;
  std::mutex mutex;
  std::condition_variable changed;
  std::deque<Request> requests;
  std::vector<WorkspaceResult> results;
  Notify onResult, onStopped;
  WorkerOptions options;
  bool stopping{}, done{};
  std::size_t outstanding{};
  static void notify(const Notify &callback) noexcept {
    try {
      if (callback)
        callback();
    } catch (...) {
    }
  }
  void run() noexcept {
    for (;;) {
      Request request;
      {
        std::unique_lock lock(mutex);
        changed.wait(lock, [&] { return stopping || !requests.empty(); });
        if (requests.empty() && stopping)
          break;
        request = std::move(requests.front());
        requests.pop_front();
        if (options.testIoDelayMs)
          changed.wait_for(lock,
                           std::chrono::milliseconds(options.testIoDelayMs),
                           [&] { return stopping; });
      }
      WorkspaceResult result;
      try {
        if (const auto read = std::get_if<WorkspaceRead>(&request)) {
          result.operation = Operation::read;
          result.requestId = read->requestId;
          result.path = read->path;
          result.data = read_workspace(read->path);
          result.missing = !result.data;
        } else {
          const auto &save = std::get<WorkspaceSave>(request);
          result.operation = Operation::save;
          result.generation = save.generation;
          result.path = save.path;
          save_workspace_atomic(save.path, save.snapshot, save.testFault);
        }
      } catch (const StorageError &e) {
        result.error = e.detail();
      } catch (...) {
        result.error =
            Error{L"任务记录操作发生异常；当前任务仍保留在内存中，请重试。",
                  "workspace_exception",
                  0,
                  {}};
      }
      {
        std::lock_guard lock(mutex);
        results.push_back(std::move(result));
      }
      notify(onResult);
    }
    {
      std::lock_guard lock(mutex);
      done = true;
    }
    notify(onStopped);
  }
};
WorkspaceWorker::WorkspaceWorker(Notify onResult, Notify onStopped,
                                 WorkerOptions options)
    : state_(std::make_shared<State>()) {
  if (!options.maxPending || options.maxPending > 1024)
    throw std::invalid_argument("invalid workspace queue limit");
  state_->onResult = std::move(onResult);
  state_->onStopped = std::move(onStopped);
  state_->options = options;
  std::thread([state = state_] { state->run(); }).detach();
}
WorkspaceWorker::~WorkspaceWorker() { request_stop(); }
bool WorkspaceWorker::submit(WorkspaceRead request) {
  {
    std::lock_guard lock(state_->mutex);
    if (state_->stopping || state_->outstanding >= state_->options.maxPending)
      return false;
    state_->requests.emplace_back(std::move(request));
    ++state_->outstanding;
  }
  state_->changed.notify_one();
  return true;
}
bool WorkspaceWorker::submit(WorkspaceSave request) {
  {
    std::lock_guard lock(state_->mutex);
    if (state_->stopping || state_->outstanding >= state_->options.maxPending)
      return false;
    state_->requests.emplace_back(std::move(request));
    ++state_->outstanding;
  }
  state_->changed.notify_one();
  return true;
}
std::vector<WorkspaceResult> WorkspaceWorker::take_results() {
  std::lock_guard lock(state_->mutex);
  std::vector<WorkspaceResult> out;
  out.swap(state_->results);
  state_->outstanding -= out.size();
  return out;
}
void WorkspaceWorker::request_stop() noexcept {
  {
    std::lock_guard lock(state_->mutex);
    state_->stopping = true;
  }
  state_->changed.notify_one();
}
bool WorkspaceWorker::stopped() const noexcept {
  std::lock_guard lock(state_->mutex);
  return state_->done;
}
} // namespace gsdl::storage
