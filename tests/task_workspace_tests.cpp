#include "infra/storage/task_workspace.hpp"
#include <chrono>
#include <condition_variable>
#include <fstream>
#include <iostream>
#include <mutex>
#include <windows.h>

using namespace gsdl;
using namespace gsdl::storage;
namespace {
int checks{};
void require(bool value, const char *why) {
  ++checks;
  if (!value)
    throw std::runtime_error(why);
}
template <class F> void rejects(F action, const char *why) {
  try {
    action();
  } catch (const StorageError &) {
    ++checks;
    return;
  }
  throw std::runtime_error(why);
}
std::string contents(const std::filesystem::path &path) {
  std::ifstream stream(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(stream), {}};
}
std::string replace(std::string text, std::string_view from,
                    std::string_view to) {
  auto i = text.find(from);
  require(i != text.npos, "missing fixture substitution");
  text.replace(i, from.size(), to);
  return text;
}
struct TempDirectory {
  std::filesystem::path path;
  TempDirectory() {
    wchar_t temp[MAX_PATH]{};
    require(GetTempPathW(MAX_PATH, temp) != 0, "temp root");
    path = std::filesystem::path(temp) /
           (L"gsdl-workspace-test-" + std::to_wstring(GetCurrentProcessId()) +
            L"-" + std::to_wstring(GetTickCount64()));
    require(std::filesystem::create_directory(path), "create temp dir");
  }
  ~TempDirectory() {
    std::error_code ignored;
    std::filesystem::remove_all(path, ignored);
  }
};
WorkspaceSnapshot fixture() {
  DocumentStore store;
  auto plan = make_internal_fixture();
  plan.outputDirectory = L"D:\\下载测试\\任务一";
  plan.resources.front().outputName = L"正面模型.sog";
  require(bool(store.replaceLoaded(plan, L"D:\\下载测试\\计划.3dgsdl.json")),
          "fixture document");
  transfer::Report report;
  report.files.push_back(
      {plan.resources.front().id, L"D:\\下载测试\\任务一\\正面模型.sog", 123,
       std::string(32, 'a'), std::string(64, 'b'), std::string(32, 'a')});
  report.receiptPath = L"D:\\下载测试\\任务一\\.3dgsdl-receipts\\1.json";
  WorkspaceSnapshot s;
  s.lastDirectory = L"D:\\下载测试";
  s.recentDirectories={L"D:\\其他目录",L"D:\\下载测试"};
  s.lastUsedBaseDirectory=L"D:\\确认下载目录";
  s.selectionPreferences={true,{{"primaryModel",false},{"cameraData",true},{"voxelArchive",false},{"effectVideo1",true},{"effectVideo2",false}}};
  s.tasks.push_back({"task-1",
                     store.snapshot(),
                     store.target(),
                     TaskState::Succeeded,
                     report,
                     {}});
  s.tasks.push_back({"task-2",
                     store.snapshot(),
                     {},
                     TaskState::Downloading,
                     {},
                     L"等待网络"});
  return s;
}
void codec() {
  auto s = fixture();
  auto encoded = encode_workspace(s);
  auto decoded = decode_workspace(encoded);
  require(decoded.lastDirectory == s.lastDirectory && decoded.tasks.size() == 2,
          "Unicode directory/tasks roundtrip");
  require(decoded.recentDirectories==s.recentDirectories,"recent directories independent roundtrip");
  require(decoded.lastUsedBaseDirectory==s.lastUsedBaseDirectory &&
          decoded.selectionPreferences==s.selectionPreferences,"confirmed directory and checked/unchecked preferences roundtrip");
  auto v2=replace(encoded,"\"schemaVersion\":3","\"schemaVersion\":2");
  const auto preferenceBegin=v2.find(",\"lastUsedBaseDirectory\":");
  const auto preferenceEnd=v2.find(",\"recentDirectories\":",preferenceBegin);
  v2.erase(preferenceBegin,preferenceEnd-preferenceBegin);
  auto legacy2=decode_workspace(v2);
  require(legacy2.lastUsedBaseDirectory.empty() && !legacy2.selectionPreferences.remembered &&
          legacy2.recentDirectories==s.recentDirectories,"v2 does not mistake last browsed directory for confirmed destination");
  auto v1=replace(v2,"\"schemaVersion\":2","\"schemaVersion\":1");
  const auto recentBegin=v1.find(",\"recentDirectories\":[");
  const auto recentEnd=v1.find(']',recentBegin);
  v1.erase(recentBegin,recentEnd+1-recentBegin);
  auto legacy=decode_workspace(v1);
  require(legacy.lastDirectory==s.lastDirectory && legacy.recentDirectories.empty(),"legacy directory migrates without recent list");
  require(legacy.lastUsedBaseDirectory.empty() && !legacy.selectionPreferences.remembered,"v1 gets no invented preference history");
  require(decoded.tasks[0].plan == s.tasks[0].document->plan,
          "strict plan roundtrip");
  require(decoded.tasks[0].id == "task-1" &&
              decoded.tasks[0].planPath == s.tasks[0].planPath,
          "task identity/path roundtrip");
  require(decoded.tasks[0].state == TaskState::Succeeded &&
              decoded.tasks[1].state == TaskState::Downloading,
          "saved state roundtrip");
  require(decoded.tasks[0].report.files.front().path ==
                  s.tasks[0].report.files.front().path &&
              decoded.tasks[0].report.files.front().bytes == 123 &&
              decoded.tasks[0].report.files.front().sha256 ==
                  std::string(64, 'b'),
          "receipt roundtrip");
  TaskListService restored;
  require(bool(restored.restore(std::move(decoded.tasks))), "restore list");
  require(restored.find("task-2")->state == TaskState::Cancelled,
          "active becomes interrupted after restart");
  require(!restored.locked("task-1") && !restored.locked("task-2"),
          "restored tasks do not run");
  require(restored.find("task-1")->plans.undo_count() == 0,
          "history is not persisted");
  s.tasks[0].error = L"https://asset.example/a?Signature=DO_NOT_PERSIST";
  s.tasks[0].report.warning = L"Signature=HIDDEN_SIGNATURE";
  auto sanitized = encode_workspace(s);
  require(sanitized.find("DO_NOT_PERSIST") == sanitized.npos &&
              sanitized.find("HIDDEN_SIGNATURE") == sanitized.npos,
          "signed URLs never persisted as error/warning");
  require(sanitized.find("signedUrl") == sanitized.npos &&
              sanitized.find("resolved") == sanitized.npos,
          "no source session storage");
  rejects(
      [&] {
        decode_workspace(
            replace(encoded, "\"schemaVersion\":3", "\"schemaVersion\":9"));
      },
      "future schema accepted");
  rejects(
      [&] {
        decode_workspace(
            replace(encoded, "\"tasks\":[", "\"unknown\":0,\"tasks\":["));
      },
      "unknown field accepted");
  rejects(
      [&] {
        decode_workspace(replace(encoded, "\"tasks\":[",
                                 "\"lastDirectory\":\"\",\"tasks\":["));
      },
      "duplicate field accepted");
  rejects([&] {decode_workspace(replace(encoded,"\"cameraData\":true","\"cameraData\":1"));},"numeric preference accepted");
  rejects([&] {decode_workspace(replace(encoded,"\"cameraData\":true","\"unknownData\":true"));},"unknown preference accepted");
  rejects([&] {decode_workspace(replace(encoded,"\"remembered\":true","\"remembered\":false"));},"uncommitted choices accepted");
  rejects(
      [&] { decode_workspace(replace(encoded, "\"task-2\"", "\"task-1\"")); },
      "duplicate task accepted");
  rejects(
      [&] {
        decode_workspace(replace(encoded, "\"succeeded\"", "\"mystery\""));
      },
      "unknown state accepted");
  rejects(
      [&] {
        decode_workspace(replace(encoded, "\"bytes\":123", "\"bytes\":1.25"));
      },
      "fractional byte count accepted");
  rejects(
      [&] {
        decode_workspace(replace(encoded, "\"bytes\":123",
                                 "\"bytes\":18446744073709551616"));
      },
      "overflow accepted");
  rejects([&] { decode_workspace(std::string("\xff")); },
          "invalid UTF-8 accepted");
  rejects([&] { decode_workspace(encoded.substr(0, encoded.size() / 2)); },
          "truncated JSON accepted");
  rejects([&] { decode_workspace(std::string(max_workspace_bytes + 1, ' ')); },
          "oversize input accepted");
  auto invalid = s;
  invalid.tasks[0].document.reset();
  rejects([&] { encode_workspace(invalid); }, "null document accepted");
  invalid = s;
  invalid.tasks[0].report.files[0].path =
      L"https://asset.example/a?Signature=bad";
  rejects([&] { encode_workspace(invalid); }, "nonlocal report path accepted");
  invalid = s;
  invalid.tasks[0].report.files[0].sha256 = "not-a-sha256";
  rejects([&] { encode_workspace(invalid); }, "invalid hash accepted");
  invalid = s;
  invalid.tasks[0].id = "https://bad";
  rejects([&] { encode_workspace(invalid); }, "invalid ID accepted");
  invalid = s;
  invalid.tasks.resize(max_workspace_tasks + 1);
  rejects([&] { encode_workspace(invalid); }, "task bound ignored");
  invalid=s;invalid.lastUsedBaseDirectory=L"relative/path";
  rejects([&] {encode_workspace(invalid);},"invalid confirmed directory accepted");
  invalid=s;invalid.selectionPreferences.choices["https://secret"]=true;
  rejects([&] {encode_workspace(invalid);},"arbitrary preference key accepted");
}
void selection_preferences() {
  std::vector<Resource> files{
    {"share-a-model",L"1_3DGS.sog",L"SOG",L"我的模型.sog",false,{}},
    {"share-a-camera",L"2_cameras.json",L"JSON",L"参数.json",false,{}},
    {"share-a-voxel",L"3_3DGS.voxel.zip",L"ZIP",L"体素.zip",true,{}},
    {"share-a-effect-1",L"4_effect_1.mp4",L"MP4",L"效果一.mp4",true,{}},
    {"share-a-effect-2",L"5_effect_2.mp4",L"MP4",L"效果二.mp4",true,{}},
    {"share-a-metadata",L"6_metadata.json",L"JSON",L"cameras.json",true,{}}};
  SelectionPreferences preferences;
  apply_selection_preferences(files,preferences);
  require(files[0].included && files[1].included,"recommended model and camera selected");
  require(!files[2].included && !files[3].included && !files[4].included && !files[5].included,
          "optional/unknown resources not automatically selected");
  require(!preferences.remembered && preferences.choices.empty(),"preview application must not commit preferences");
  files[0].included=false;files[3].included=true;
  remember_selection_preferences(preferences,files);
  require(preferences.remembered && !preferences.choices.at("primaryModel") &&
          preferences.choices.at("cameraData") && preferences.choices.at("effectVideo1"),"explicit check and uncheck retained");
  const auto committed=preferences;
  auto next=files;
  std::reverse(next.begin(),next.end());
  for(auto &resource:next){resource.id="share-b-"+resource.id;resource.included=false;resource.outputName=L"改名.bin";}
  apply_selection_preferences(next,preferences);
  require(next[2].included && next[4].included && !next[5].included,"new share reordered and renamed resources use semantic preferences");
  require(preferences==committed,"reading preferences cannot change memory");
  std::vector<Resource> missing{files[2]};
  missing[0].included=true;
  remember_selection_preferences(preferences,missing);
  require(preferences.choices.at("cameraData") && preferences.choices.at("effectVideo1") &&
          preferences.choices.at("voxelArchive") && !preferences.choices.at("primaryModel"),"missing categories retain their previous choices");
  std::vector<Resource> none{files[0],files[4],files[5]};
  apply_selection_preferences(none,preferences);
  require(std::none_of(none.begin(),none.end(),[](const auto &resource){return resource.included;}),"no applicable choice does not silently reselect model");
  auto renamed=files[0];renamed.displayName=L"9_CaMeRaS.JsOn";renamed.format=L"json";
  require(resource_preference_key(renamed)=="cameraData","ordinal and casing are not semantic identity");
  renamed.displayName=L"cameras.json";renamed.format=L"mp4";
  require(resource_preference_key(renamed).empty(),"mismatching format not treated as camera data");
  renamed.displayName=L"metadata.json";renamed.format=L"JSON";
  require(resource_preference_key(renamed).empty(),"ordinary json not treated as camera data");
}
void atomic(const std::filesystem::path &root) {
  const auto path = root / L"自动保存" / L"任务列表.json";
  require(!read_workspace(path), "first launch should be missing");
  auto before = fixture();
  save_workspace_atomic(path, before);
  const auto original = contents(path);
  auto after = before;
  after.lastDirectory = L"E:\\新目录";
  after.lastUsedBaseDirectory=L"E:\\新确认目录";
  after.selectionPreferences.choices["cameraData"]=false;
  for (auto fault : {SaveFault::before_write, SaveFault::after_write,
                     SaveFault::before_publish}) {
    rejects([&] { save_workspace_atomic(path, after, fault); },
            "injected save should fail");
    require(contents(path) == original, "failed save replaced original");
    std::size_t count{};
    for (const auto &unused :
         std::filesystem::directory_iterator(path.parent_path())) {
      (void)unused;
      ++count;
    }
    require(count == 1, "failed save leaked temporary");
  }
  save_workspace_atomic(path, after);
  require(read_workspace(path)->lastDirectory == after.lastDirectory,
          "successful replacement missing");
  require(read_workspace(path)->lastUsedBaseDirectory==after.lastUsedBaseDirectory &&
          read_workspace(path)->selectionPreferences==after.selectionPreferences,"confirmed preferences survive disk reopen");
  require(before.lastDirectory != after.lastDirectory,
          "snapshot fixture mutated");
  rejects([&] { save_workspace_atomic(path.parent_path(), after); },
          "directory target accepted");
  require(read_workspace(path)->lastDirectory == after.lastDirectory,
          "invalid target damaged workspace");
  // Untrusted persisted data does not cause any referenced media I/O.
  require(!std::filesystem::exists(root / L"正面模型.sog"),
          "workspace write created media");
}
void worker(const std::filesystem::path &root) {
  std::mutex mutex;
  std::condition_variable cv;
  bool stopped{};
  auto path = root / L"worker" / L"workspace.json";
  WorkspaceWorker io({},
                     [&] {
                       std::lock_guard lock(mutex);
                       stopped = true;
                       cv.notify_all();
                     },
                     {8, 20});
  auto s = fixture();
  require(io.submit(WorkspaceSave{11, path, s}), "save first rejected");
  s.lastDirectory = L"E:\\最后目录";
  require(io.submit(WorkspaceSave{12, path, s}), "save second rejected");
  require(io.submit(WorkspaceRead{13, path}), "read rejected");
  require(io.submit(WorkspaceSave{14, path, s, SaveFault::before_publish}),
          "failure request rejected");
  io.request_stop();
  require(!io.submit(WorkspaceRead{15, path}), "stop accepted new read");
  {
    std::unique_lock lock(mutex);
    require(cv.wait_for(lock, std::chrono::seconds(5), [&] { return stopped; }),
            "workspace worker stop timeout");
  }
  auto results = io.take_results();
  require(io.stopped() && results.size() == 4, "drain lost accepted requests");
  require(results[0].generation == 11 && results[1].generation == 12 &&
              results[2].requestId == 13 && results[3].generation == 14,
          "generations/FIFO order lost");
  require(results[0].success() && results[1].success() &&
              results[2].success() && !results[3].success(),
          "worker results wrong");
  require(results[2].data->lastDirectory == s.lastDirectory &&
              read_workspace(path)->lastDirectory == s.lastDirectory,
          "older save overwrote latest snapshot");
  bool detachedStopped{};
  {
    WorkspaceWorker detached({},
                             [&] {
                               std::lock_guard lock(mutex);
                               detachedStopped = true;
                               cv.notify_all();
                             },
                             {1, 100});
    require(detached.submit(WorkspaceRead{1, path}), "detached read rejected");
    require(!detached.submit(WorkspaceRead{2, path}), "queue bound ignored");
  }
  {
    std::unique_lock lock(mutex);
    require(cv.wait_for(lock, std::chrono::seconds(5),
                        [&] { return detachedStopped; }),
            "destructor failed to drain asynchronously");
  }
}
} // namespace
int main() {
  try {
    TempDirectory temp;
    selection_preferences();
    codec();
    atomic(temp.path);
    worker(temp.path);
    std::cout << "workspace strict_codec atomic_preservation fifo_generation "
                 "async_drain passed; checks="
              << checks << '\n';
    return 0;
  } catch (const StorageError &e) {
    std::wcerr << e.detail().message << '\n';
    return 1;
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
