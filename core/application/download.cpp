#include "transfer_service.hpp"
// clang-format off
#include <windows.h> // BCrypt requires Windows base types first.
// clang-format on
#include <algorithm>
#include <array>
#include <bcrypt.h>
#include <vector>
#include <windows.h>

namespace gsdl::transfer {
namespace {
struct Handle {
  HANDLE h{INVALID_HANDLE_VALUE};
  ~Handle() {
    if (h != INVALID_HANDLE_VALUE)
      CloseHandle(h);
  }
  void close() {
    auto value = h;
    h = INVALID_HANDLE_VALUE;
    if (value != INVALID_HANDLE_VALUE && !CloseHandle(value))
      throw net::Error(L"关闭临时资源文件失败");
  }
};
[[noreturn]] void file_error(std::wstring what) {
  throw net::Error(std::move(what) + L"（Windows " +
                       std::to_wstring(GetLastError()) +
                       L"）；已有文件保持，请检查目录或更换文件名",
                   0, GetLastError());
}
std::wstring native(const std::filesystem::path &p) {
  auto s = p.wstring();
  if (s.starts_with(L"\\\\?\\"))
    return s;
  if (s.starts_with(L"\\\\"))
    return L"\\\\?\\UNC\\" + s.substr(2);
  return L"\\\\?\\" + s;
}
struct Hash {
  BCRYPT_ALG_HANDLE algorithm{};
  BCRYPT_HASH_HANDLE hash{};
  std::vector<unsigned char> object;
  ULONG length{};
  explicit Hash(const wchar_t *name) {
    if (BCryptOpenAlgorithmProvider(&algorithm, name, nullptr, 0) < 0)
      throw net::Error(L"无法初始化文件校验");
    DWORD bytes{}, size{};
    if (BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH,
                          reinterpret_cast<PUCHAR>(&size), sizeof size, &bytes,
                          0) < 0 ||
        BCryptGetProperty(algorithm, BCRYPT_HASH_LENGTH,
                          reinterpret_cast<PUCHAR>(&length), sizeof length,
                          &bytes, 0) < 0) {
      BCryptCloseAlgorithmProvider(algorithm, 0);
      algorithm = nullptr;
      throw net::Error(L"无法读取校验算法属性");
    }
    object.resize(size);
    if (BCryptCreateHash(algorithm, &hash, object.data(), size, nullptr, 0, 0) <
        0) {
      BCryptCloseAlgorithmProvider(algorithm, 0);
      algorithm = nullptr;
      throw net::Error(L"无法创建文件校验上下文");
    }
  }
  ~Hash() {
    if (hash)
      BCryptDestroyHash(hash);
    if (algorithm)
      BCryptCloseAlgorithmProvider(algorithm, 0);
  }
  void add(std::string_view data) {
    if (BCryptHashData(
            hash, reinterpret_cast<PUCHAR>(const_cast<char *>(data.data())),
            static_cast<ULONG>(data.size()), 0) < 0)
      throw net::Error(L"计算文件校验失败");
  }
  std::string finish() {
    std::vector<unsigned char> digest(length);
    if (BCryptFinishHash(hash, digest.data(), length, 0) < 0)
      throw net::Error(L"完成文件校验失败");
    std::string out;
    for (auto c : digest) {
      out += "0123456789abcdef"[c >> 4];
      out += "0123456789abcdef"[c & 15];
    }
    return out;
  }
};
struct Temporary {
  std::filesystem::path path;
  bool owned{};
  ~Temporary() {
    if (owned)
      DeleteFileW(native(path).c_str());
  }
  void cleanup() {
    if (owned && !DeleteFileW(native(path).c_str()))
      throw net::Error(L"未完成资源未发布，但临时文件清理失败，请手动移除：" +
                       path.wstring());
    owned = false;
  }
};
std::wstring random_name() {
  std::array<unsigned char, 16> b{};
  if (BCryptGenRandom(nullptr, b.data(), static_cast<ULONG>(b.size()),
                      BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0)
    throw net::Error(L"无法生成临时文件标识");
  std::wstring s = L".3dgsdl-";
  for (auto c : b) {
    s += L"0123456789abcdef"[c >> 4];
    s += L"0123456789abcdef"[c & 15];
  }
  return s + L".part";
}
FileReceipt one(const Resource &resource, const sources::RemoteResource &remote,
                const std::filesystem::path &directory, net::Http &http,
                CancelGate &gate, Progress &progress,
                const NotifyProgress &notify, bool last) {
  auto target = directory / resource.outputName;
  Temporary temporary{directory / random_name()};
  const auto targetName = native(target);
  DWORD attr = GetFileAttributesW(targetName.c_str());
  if (attr != INVALID_FILE_ATTRIBUTES)
    throw net::Error(L"目标文件已存在，未覆盖：" + resource.outputName +
                     L"；请打开文件夹将同名文件移走或改名后重试，或取消勾选此文件");
  if (GetLastError() != ERROR_FILE_NOT_FOUND)
    file_error(L"无法检查输出目标");
  Handle file{CreateFileW(
      native(temporary.path).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
      FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr)};
  if (file.h == INVALID_HANDLE_VALUE)
    file_error(L"无法创建临时资源文件");
  temporary.owned = true;
  try {
    Hash md5(BCRYPT_MD5_ALGORITHM), sha(BCRYPT_SHA256_ALGORITHM);
    std::uint64_t bytes{};
    std::optional<std::uint64_t> responseLength;
    http.get(
        remote.signedUrl, net::Route::asset, false,
        [&](const net::Headers &h) {
          net::require_ok(h);
          responseLength = h.length;
          if (h.length && remote.descriptor.expectedBytes &&
              h.length != remote.descriptor.expectedBytes)
            throw net::Error(L"文件长度已改变，请重新解析来源");
          progress.total =
              h.length ? h.length : remote.descriptor.expectedBytes;
          return true;
        },
        [&](auto chunk) {
          gate.check();
          if (UINT64_MAX - bytes < chunk.size())
            throw net::Error(L"文件超过可处理长度");
          bytes += chunk.size();
          if ((responseLength && bytes > *responseLength) ||
              (remote.descriptor.expectedBytes &&
               bytes > *remote.descriptor.expectedBytes))
            throw net::Error(L"接收到的数据超过声明长度；未发布文件");
          std::size_t offset{};
          while (offset < chunk.size()) {
            DWORD written{};
            if (!WriteFile(file.h, chunk.data() + offset,
                           static_cast<DWORD>(chunk.size() - offset), &written,
                           nullptr) ||
                written == 0)
              file_error(L"写入资源失败");
            offset += written;
          }
          md5.add(chunk);
          sha.add(chunk);
          progress.bytes = bytes;
          progress.stage = L"正在下载";
          notify(progress);
        },
        [&] { gate.check(); });
    gate.check();
    progress.stage = L"正在校验";
    notify(progress);
    if ((responseLength && bytes != *responseLength) ||
        (remote.descriptor.expectedBytes &&
         bytes != *remote.descriptor.expectedBytes))
      throw net::Error(L"资源传输不完整；临时文件未发布", 0, 0, true);
    auto digest = md5.finish(), sha256 = sha.finish();
    if (!remote.md5.empty() && digest != remote.md5)
      throw net::Error(
          L"文件 MD5 与公开页面声明不符；临时文件未发布，请重新解析后重试");
    if (!FlushFileBuffers(file.h))
      file_error(L"刷新资源文件失败");
    file.close();
    FileReceipt completed{
        resource.id,       target,    bytes, std::move(digest),
        std::move(sha256), remote.md5};
    progress.stage = L"正在发布";
    notify(progress);
    if (!gate.begin_publish())
      throw net::Cancelled{};
    // This CAS is the cancellation/publication linearization point. Once won,
    // cancellation cannot turn this published file into a cancelled result.
    if (!MoveFileExW(native(temporary.path).c_str(), targetName.c_str(),
                     MOVEFILE_WRITE_THROUGH))
      file_error(L"发布文件失败（不会覆盖已有目标）");
    temporary.owned = false;
    gate.published(last);
    return completed;
  } catch (...) {
    auto error = std::current_exception();
    file.close();
    temporary.cleanup();
    std::rethrow_exception(error);
  }
}
} // namespace
LocalState check_local(const FileReceipt &receipt) noexcept {
  try {
    Handle file{CreateFileW(native(receipt.path).c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_FLAG_SEQUENTIAL_SCAN,nullptr)};
    if(file.h==INVALID_HANDLE_VALUE) return GetLastError()==ERROR_FILE_NOT_FOUND || GetLastError()==ERROR_PATH_NOT_FOUND ? LocalState::missing : LocalState::unchecked;
    LARGE_INTEGER size{};
    if(!GetFileSizeEx(file.h,&size)) return LocalState::unchecked;
    if(size.QuadPart<0 || std::uint64_t(size.QuadPart)!=receipt.bytes) return LocalState::changed;
    if(receipt.sha256.empty()) return LocalState::unchecked;
    Hash sha(BCRYPT_SHA256_ALGORITHM);
    std::array<char,65536> buffer;
    DWORD count{};
    for(;;) {
      if(!ReadFile(file.h,buffer.data(),DWORD(buffer.size()),&count,nullptr)) return LocalState::unchecked;
      if(!count) break;
      sha.add({buffer.data(),count});
    }
    return sha.finish()==receipt.sha256 ? LocalState::available : LocalState::changed;
  } catch(...) {return LocalState::unchecked;}
}
bool compatible(Snapshot s, const sources::ResolvedSource &source) {
  if (!s || !source.request || s->sessionId != source.request->sessionId ||
      s->plan.planId != source.request->plan.planId ||
      s->plan.source.shareUrl != source.request->plan.source.shareUrl ||
      s->plan.internalFixture)
    return false;
  for (const auto &r : s->plan.resources)
    if (r.included &&
        std::none_of(source.resources.begin(), source.resources.end(),
                     [&](const auto &v) { return v.descriptor.id == r.id; }))
      return false;
  return !s->plan.resources.empty();
}
void download(Snapshot s, const sources::ResolvedSource &source,
              net::Http &http, CancelGate &gate, const NotifyProgress &notify,
              Report &report, const std::vector<FileReceipt> &previous) {
  if (!compatible(s, source))
    throw net::Error(L"当前计划没有匹配的资源会话，请重新解析");
  if (auto r = validate_plan(s->plan); !r)
    throw net::Error(r.error);
  if (s->plan.outputDirectory.empty())
    throw net::Error(L"请先设置保存目录");
  std::vector<Resource> selected;
  for (const auto &r : s->plan.resources)
    if (r.included)
      selected.push_back(r);
  if (selected.empty())
    throw net::Error(L"请至少勾选一个资源");
  report.files.reserve(selected.size());
  gate.check();
  std::filesystem::path directory(s->plan.outputDirectory);
  std::error_code ec;
  std::filesystem::create_directories(directory, ec);
  if (ec)
    throw net::Error(L"无法创建保存目录；请检查路径和写入权限");
  // Pin the selected directory against replacement while publishing leaf names.
  Handle folder{CreateFileW(native(directory).c_str(), FILE_READ_ATTRIBUTES,
                            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                            OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS,
                            nullptr)};
  if (folder.h == INVALID_HANDLE_VALUE)
    file_error(L"无法固定保存目录");
  std::vector<wchar_t> actual(32768);
  auto count = GetFinalPathNameByHandleW(
      folder.h, actual.data(), static_cast<DWORD>(actual.size()),
      FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
  if (!count || count >= actual.size())
    file_error(L"无法确定保存目录边界");
  std::wstring canonical(actual.data(), count);
  if (canonical.starts_with(L"\\\\?\\UNC\\"))
    canonical = L"\\\\" + canonical.substr(8);
  else if (canonical.starts_with(L"\\\\?\\"))
    canonical = canonical.substr(4);
  directory = canonical;
  sources::ResolvedSource active = source;
  bool refreshed{};
  Progress progress;
  progress.fileCount = selected.size();
  for (const auto &resource : selected) {
    gate.check();
    progress.bytes = 0;
    progress.total = resource.expectedBytes;
    progress.resourceName = resource.outputName;
    progress.stage = L"准备下载";
    notify(progress);
    unsigned retries{};
    for (;;) {
      gate.check();
      auto remote = std::find_if(
          active.resources.begin(), active.resources.end(),
          [&](const auto &x) { return x.descriptor.id == resource.id; });
      if (remote == active.resources.end())
        throw net::Error(L"资源版本已改变，请重新解析并确认下载选择");
      try {
        std::optional<FileReceipt> reused;
        auto target = directory / resource.outputName;
        for (const auto &old : previous) {
          if (old.resourceId != resource.id || old.sha256.empty() ||
              _wcsicmp(old.path.lexically_normal().c_str(), target.lexically_normal().c_str()))
            continue;
          Handle existing{CreateFileW(native(target).c_str(), GENERIC_READ,
                                     FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                     FILE_FLAG_SEQUENTIAL_SCAN, nullptr)};
          if (existing.h == INVALID_HANDLE_VALUE) {
            if (GetLastError() == ERROR_FILE_NOT_FOUND) break;
            file_error(L"无法核对已下载文件");
          }
          progress.stage = L"核对已有文件";
          notify(progress);
          Hash hash(BCRYPT_SHA256_ALGORITHM), md5(BCRYPT_MD5_ALGORITHM);
          std::array<char, 65536> buffer;
          std::uint64_t bytes{};
          for (;;) {
            gate.check();
            DWORD read{};
            if (!ReadFile(existing.h, buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr))
              file_error(L"读取已下载文件失败");
            if (!read) break;
            bytes += read;
            hash.add({buffer.data(), read}); md5.add({buffer.data(), read});
          }
          const auto sha = hash.finish(), digest = md5.finish();
          if (bytes != old.bytes || sha != old.sha256 ||
              (resource.expectedBytes && bytes != *resource.expectedBytes) ||
              (!remote->md5.empty() && digest != remote->md5))
            throw net::Error(L"已有文件与本次资源不一致，未覆盖：" + resource.outputName + L"；请改名或选择其他目录");
          gate.check();
          reused = FileReceipt{resource.id, target, bytes, digest, sha, remote->md5};
          break;
        }
        auto receipt = reused ? *reused :
            one(resource, *remote, directory, http, gate, progress, notify,
                progress.completedFiles + 1 == selected.size());
        if (receipt.expectedMd5.empty())
          report.warning =
              L"部分资源未提供来源 MD5；仅核对可知长度并记录本地哈希";
        report.files.push_back(std::move(receipt));
        progress.published=report.files;
        ++progress.completedFiles;
        progress.stage = L"文件已完成";
        notify(progress);
        break;
      } catch (const net::Error &e) {
        gate.check();
        if ((e.status == 401 || e.status == 403) && !refreshed) {
          refreshed = true;
          progress.stage = L"刷新公开资源地址";
          notify(progress);
          active = sources::resolve(s, http, [&] { gate.check(); });
          continue;
        }
        if (!e.retryable || retries >= 2)
          throw;
        progress.stage = L"等待重试（从头下载）";
        notify(progress);
        net::backoff(retries++, [&] { gate.check(); });
        progress.bytes = 0;
      }
    }
  }
}
} // namespace gsdl::transfer
