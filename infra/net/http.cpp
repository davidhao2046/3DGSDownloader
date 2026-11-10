#include "http.hpp"
// clang-format off
#include <windows.h> // WinHTTP / BCrypt require Windows base types first.
// clang-format on
#include <algorithm>
#include <array>
#include <bcrypt.h>
#include <charconv>
#include <chrono>
#include <thread>
#include <vector>
#include <windows.h>
#include <winhttp.h>

namespace gsdl::net {
namespace {
struct Handle {
  HINTERNET h{};
  ~Handle() {
    if (h)
      WinHttpCloseHandle(h);
  }
};
[[noreturn]] void failure() {
  auto code = GetLastError();
  throw Error(L"网络请求失败（Windows " + std::to_wstring(code) +
                  L"）；请检查网络后重试",
              0, code,
              code == ERROR_WINHTTP_TIMEOUT ||
                  code == ERROR_WINHTTP_CONNECTION_ERROR ||
                  code == ERROR_WINHTTP_CANNOT_CONNECT ||
                  code == ERROR_WINHTTP_NAME_NOT_RESOLVED);
}
std::wstring header(HINTERNET h, DWORD key) {
  DWORD count{};
  WinHttpQueryHeaders(h, key, WINHTTP_HEADER_NAME_BY_INDEX, nullptr, &count,
                      WINHTTP_NO_HEADER_INDEX);
  if (GetLastError() == ERROR_WINHTTP_HEADER_NOT_FOUND)
    return {};
  if (count > 65536)
    throw Error(L"响应头超过限制");
  std::wstring value(count / sizeof(wchar_t), L'\0');
  if (!WinHttpQueryHeaders(h, key, WINHTTP_HEADER_NAME_BY_INDEX, value.data(),
                           &count, WINHTTP_NO_HEADER_INDEX))
    failure();
  value.resize(count / sizeof(wchar_t));
  return value;
}
} // namespace
std::wstring wide(std::string_view v) {
  if (v.size() > INT_MAX)
    throw Error(L"文字超过限制");
  if (v.empty())
    return {};
  int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, v.data(),
                              static_cast<int>(v.size()), nullptr, 0);
  if (!n)
    throw Error(L"来源文字不是有效 UTF-8");
  std::wstring out(n, 0);
  MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, v.data(),
                      static_cast<int>(v.size()), out.data(), n);
  return out;
}
std::string utf8(std::wstring_view v) {
  if (v.size() > INT_MAX)
    throw Error(L"文字超过限制");
  if (v.empty())
    return {};
  int n = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, v.data(),
                              static_cast<int>(v.size()), nullptr, 0, nullptr,
                              nullptr);
  if (!n)
    throw Error(L"文字包含非法 Unicode");
  std::string out(n, 0);
  WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, v.data(),
                      static_cast<int>(v.size()), out.data(), n, nullptr,
                      nullptr);
  return out;
}
std::string hash_hex(std::string_view v, bool md5) {
  BCRYPT_ALG_HANDLE a{};
  if (BCryptOpenAlgorithmProvider(
          &a, md5 ? BCRYPT_MD5_ALGORITHM : BCRYPT_SHA256_ALGORITHM, nullptr,
          0) < 0)
    throw Error(L"无法初始化校验算法");
  std::array<unsigned char, 32> digest{};
  auto status = BCryptHash(
      a, nullptr, 0, reinterpret_cast<PUCHAR>(const_cast<char *>(v.data())),
      static_cast<ULONG>(v.size()), digest.data(), md5 ? 16 : 32);
  BCryptCloseAlgorithmProvider(a, 0);
  if (status < 0)
    throw Error(L"无法计算校验值");
  std::string s;
  for (unsigned i = 0; i < (md5 ? 16u : 32u); ++i) {
    s += "0123456789abcdef"[digest[i] >> 4];
    s += "0123456789abcdef"[digest[i] & 15];
  }
  return s;
}
Url parse_url(std::wstring_view value, Route route) {
  if (value.empty() || value.size() > 32768 ||
      value.find_first_of(L"\r\n\t\\#") != value.npos ||
      value.find(L'\0') != value.npos)
    throw Error(L"来源地址格式不受支持");
  URL_COMPONENTS c{};
  c.dwStructSize = sizeof(c);
  c.dwHostNameLength = c.dwUrlPathLength = c.dwExtraInfoLength =
      c.dwUserNameLength = c.dwPasswordLength = static_cast<DWORD>(-1);
  if (!WinHttpCrackUrl(value.data(), static_cast<DWORD>(value.size()), 0, &c))
    throw Error(L"来源地址无法解析");
  Url u{std::wstring(c.lpszHostName, c.dwHostNameLength),
        std::wstring(c.lpszUrlPath, c.dwUrlPathLength),
        {},
        c.nPort,
        c.nScheme == INTERNET_SCHEME_HTTPS};
  std::transform(u.host.begin(), u.host.end(), u.host.begin(), [](wchar_t x) {
    return static_cast<wchar_t>(x >= L'A' && x <= L'Z' ? x + 32 : x);
  });
  if (c.dwUserNameLength || c.dwPasswordLength)
    throw Error(L"来源地址不能包含登录信息");
  if (route == Route::loopback_test) {
    if (u.host != L"127.0.0.1" || c.nScheme != INTERNET_SCHEME_HTTP)
      throw Error(L"测试地址必须为本机回环 HTTP");
  } else if (!u.secure || u.port != 443 ||
             (route == Route::page
                  ? u.host != L"app.insta360.com"
                  : u.host != L"insta360-app-hz.oss-cn-hangzhou.aliyuncs.com"))
    throw Error(L"来源域名或协议不在已验证范围，请更新适配器");
  u.target = u.path;
  if (c.dwExtraInfoLength)
    u.target.append(c.lpszExtraInfo, c.dwExtraInfoLength);
  return u;
}
void require_ok(const Headers &h, unsigned expected) {
  if (h.status != expected)
    throw Error(L"服务器返回 HTTP " + std::to_wstring(h.status) +
                    L"；未取得可用资源",
                h.status, 0, h.status == 429 || h.status >= 500);
}
void backoff(unsigned attempt, const Check &check) {
  for (unsigned i = 0; i < (attempt + 1) * 5; ++i) {
    check();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  check();
}
WinHttp::WinHttp(unsigned testChunkDelayMs)
    : testChunkDelayMs_(std::min(2000u, testChunkDelayMs)) {}
void WinHttp::get(std::wstring_view requested, Route route, bool probe,
                  const OnHeaders &onHeaders, const OnBytes &onBytes,
                  const Check &check) {
  Handle session{
      WinHttpOpen(L"3DGSDownloader/0.2", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                  WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0)};
  if (!session.h)
    failure();
  if (!WinHttpSetTimeouts(session.h, 5000, 5000, 5000, 10000))
    failure();
  std::wstring current(requested);
  for (unsigned redirects = 0;; ++redirects) {
    check();
    auto u = parse_url(current, route);
    Handle connection{WinHttpConnect(session.h, u.host.c_str(), u.port, 0)};
    if (!connection.h)
      failure();
    Handle request{WinHttpOpenRequest(
        connection.h, L"GET", u.target.c_str(), nullptr, WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES, u.secure ? WINHTTP_FLAG_SECURE : 0)};
    if (!request.h)
      failure();
    DWORD disable = WINHTTP_DISABLE_REDIRECTS | WINHTTP_DISABLE_COOKIES |
                    WINHTTP_DISABLE_AUTHENTICATION;
    if (!WinHttpSetOption(request.h, WINHTTP_OPTION_DISABLE_FEATURE, &disable,
                          sizeof disable))
      failure();
    const wchar_t *headers =
        probe ? L"Accept-Encoding: identity\r\nRange: bytes=0-0\r\n"
              : L"Accept-Encoding: identity\r\n";
    if (!WinHttpSendRequest(request.h, headers, static_cast<DWORD>(-1),
                            WINHTTP_NO_REQUEST_DATA, 0, 0, 0))
      failure();
    check();
    if (!WinHttpReceiveResponse(request.h, nullptr))
      failure();
    check();
    Headers h;
    DWORD count = sizeof(DWORD);
    if (!WinHttpQueryHeaders(
            request.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX, &h.status, &count,
            WINHTTP_NO_HEADER_INDEX))
      failure();
    if (h.status == 301 || h.status == 302 || h.status == 303 ||
        h.status == 307 || h.status == 308) {
      if (redirects >= 4)
        throw Error(L"来源重定向超过限制");
      auto next = header(request.h, WINHTTP_QUERY_LOCATION);
      if (next.starts_with(L"/") && !next.starts_with(L"//"))
        next = (u.secure ? L"https://" : L"http://") + u.host +
               (route == Route::loopback_test ? L":" + std::to_wstring(u.port)
                                              : L"") +
               next;
      parse_url(next, route);
      current = std::move(next);
      continue;
    }
    auto length = header(request.h, WINHTTP_QUERY_CONTENT_LENGTH);
    if (!length.empty()) {
      auto s = utf8(length);
      std::uint64_t n{};
      auto r = std::from_chars(s.data(), s.data() + s.size(), n);
      if (r.ec != std::errc{} || r.ptr != s.data() + s.size())
        throw Error(L"服务器文件长度无效");
      h.length = n;
    }
    h.range = header(request.h, WINHTTP_QUERY_CONTENT_RANGE);
    auto encoding = header(request.h, WINHTTP_QUERY_CONTENT_ENCODING);
    if (!encoding.empty() && encoding != L"identity")
      throw Error(L"服务器返回了不支持的内容编码");
    if (!onHeaders(h))
      return;
    std::array<char, 65536> data{};
    std::uint64_t received{};
    auto start = std::chrono::steady_clock::now();
    for (;;) {
      check();
      if (std::chrono::steady_clock::now() - start > std::chrono::hours(1))
        throw Error(L"下载超过本次任务时间上限，请重试");
      DWORD read{};
      if (!WinHttpReadData(request.h, data.data(),
                           static_cast<DWORD>(data.size()), &read))
        failure();
      check();
      if (!read)
        break;
      received += read;
      if (h.length && received > *h.length)
        throw Error(L"响应超过声明长度", 0, 0, true);
      onBytes(std::string_view(data.data(), read));
      if (!probe && route == Route::asset) {
        for (unsigned left = testChunkDelayMs_; left;) {
          check();
          auto step = std::min(20u, left);
          std::this_thread::sleep_for(std::chrono::milliseconds(step));
          left -= step;
        }
      }
    }
    if (h.length && received != *h.length)
      throw Error(L"服务器连接提前结束；响应长度不完整", 0, 0, true);
    return;
  }
}
} // namespace gsdl::net
