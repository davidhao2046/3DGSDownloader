#include "insta360.hpp"
#include "core/application/download_preferences.hpp"
#include "infra/net/bounded_json.hpp"
#include <algorithm>
#include <charconv>
#include <chrono>
#include <set>

namespace gsdl::sources {
namespace {
std::string script(std::string_view html) {
  if (html.size() > 8 * 1024 * 1024)
    throw net::Error(L"公开页面超过 8 MiB 限制");
  std::size_t p{};
  std::string found;
  while ((p = html.find("<script", p)) != html.npos) {
    auto end = html.find('>', p);
    if (end == html.npos)
      break;
    auto attrs = html.substr(p, end - p);
    auto close = html.find("</script>", end + 1);
    if (close == html.npos)
      break;
    if (attrs.find("id=\"__NEXT_DATA__\"") != attrs.npos ||
        attrs.find("id='__NEXT_DATA__'") != attrs.npos) {
      if (!found.empty())
        throw net::Error(L"公开页面有重复的来源数据");
      found = html.substr(end + 1, close - end - 1);
    }
    p = close + 9;
  }
  if (found.empty())
    throw net::Error(
        L"公开页面没有可识别的资源清单；可能分享失效或页面结构改变");
  return found;
}
std::wstring filename(std::wstring_view path) {
  auto name = net::utf8(path.substr(path.find_last_of(L'/') + 1));
  std::string decoded;
  for (std::size_t i = 0; i < name.size(); ++i) {
    if (name[i] != '%') {
      decoded += name[i];
      continue;
    }
    if (i + 2 >= name.size())
      throw net::Error(L"资源文件名转义无效");
    unsigned n{};
    auto r = std::from_chars(name.data() + i + 1, name.data() + i + 3, n, 16);
    if (r.ec != std::errc{} || r.ptr != name.data() + i + 3)
      throw net::Error(L"资源文件名转义无效");
    decoded += static_cast<char>(n);
    i += 2;
  }
  auto out = net::wide(decoded);
  if (auto r = validate_output_name(out); !r)
    throw net::Error(L"远程文件名不能安全保存：" + r.error);
  return out;
}
template <class F> void retry(F f, const net::Check &check) {
  for (unsigned attempt = 0;; ++attempt) {
    check();
    try {
      f();
      return;
    } catch (const net::Error &e) {
      check();
      if (!e.retryable || attempt >= 2)
        throw;
      net::backoff(attempt, check);
    }
  }
}
std::optional<std::uint64_t> total(const net::Headers &h) {
  if (h.status == 200)
    return h.length;
  net::require_ok(h, 206);
  auto text = net::utf8(h.range);
  constexpr std::string_view prefix = "bytes 0-0/";
  if (!text.starts_with(prefix))
    throw net::Error(L"资源探测返回了不匹配的字节范围");
  auto number = std::string_view(text).substr(prefix.size());
  if (number == "*")
    return {};
  std::uint64_t n{};
  auto r = std::from_chars(number.data(), number.data() + number.size(), n);
  if (r.ec != std::errc{} || r.ptr != number.data() + number.size() || n == 0)
    throw net::Error(L"资源探测返回了无效总长度");
  return n;
}
} // namespace
std::vector<RemoteResource> parse_insta360(std::string_view html,
                                           std::string_view captureId) {
  auto root = net::json::Parser(script(html)).parse();
  const auto &props = root.at("props").at("pageProps");
  if (!props.at("detailErrorCode").number("0"))
    throw net::Error(L"分享不存在或当前无法公开访问；请检查分享链接");
  const auto &detail = props.at("taskDetail");
  if (detail.at("taskOrderNo").text() != captureId)
    throw net::Error(L"页面项目编号与分享链接不匹配");
  if (!detail.at("isPrivate").number("0"))
    throw net::Error(L"此分享未公开，首版仅支持无需登录的公开链接");
  if (!detail.at("taskStatus").number("3"))
    throw net::Error(L"作品尚未就绪或状态不受支持，请稍后重试");
  const auto &outputs = detail.at("outputs").array();
  if (outputs.empty() || outputs.size() > 64)
    throw net::Error(L"公开资源为空或超过 64 项限制");
  std::vector<RemoteResource> result;
  std::set<std::string> ids;
  bool primary{};
  for (const auto &o : outputs) {
    auto url = net::wide(o.at("url").text());
    auto u = net::parse_url(url, net::Route::asset);
    const auto prefix = L"/3dgs/" + net::wide(captureId) + L"/";
    if (!u.path.starts_with(prefix) ||
        u.path.substr(prefix.size()).find(L'/') != std::wstring::npos)
      throw net::Error(L"资源路径不属于当前分享项目");
    auto format = o.at("fileFormat").text();
    if (format != "sog" && format != "json" && format != "zip" &&
        format != "mp4")
      throw net::Error(L"来源出现尚未验证的原文件格式，请更新适配器");
    auto md5 = o.at("md5").text();
    std::transform(md5.begin(), md5.end(), md5.begin(), [](char c) {
      return c >= 'A' && c <= 'F' ? static_cast<char>(c + 32) : c;
    });
    if (!md5.empty() && (md5.size() != 32 ||
                         md5.find_first_not_of("0123456789abcdef") != md5.npos))
      throw net::Error(L"页面声明的 MD5 格式无效");
    auto id = "res-" + net::hash_hex(net::utf8(u.host + u.path) + "|" + md5);
    if (!ids.insert(id).second)
      throw net::Error(L"来源清单出现重复资源");
    auto name = filename(u.path);
    bool included = format == "sog" && !primary;
    if (included)
      primary = true;
    auto displayFormat = net::wide(format);
    std::transform(displayFormat.begin(), displayFormat.end(),
                   displayFormat.begin(), [](wchar_t c) {
                     return static_cast<wchar_t>(c >= L'a' && c <= L'z' ? c - 32
                                                                        : c);
                   });
    result.push_back({{id, name, displayFormat, name, included, {}},
                      std::move(url),
                      std::move(md5)});
    if(resource_preference_key(result.back().descriptor)=="cameraData")
      result.back().descriptor.included=true;
  }
  return result;
}
ResolvedSource resolve(Snapshot request, net::Http &http,
                       const net::Check &originalCheck) {
  const auto started = std::chrono::steady_clock::now();
  const net::Check check = [&] {
    originalCheck();
    if (std::chrono::steady_clock::now() - started > std::chrono::seconds(90))
      throw net::Error(L"解析超过时间上限，请稍后重试");
  };
  if (!request || request->plan.internalFixture ||
      request->plan.source.shareUrl.empty())
    throw net::Error(L"请先提交有效的公开分享链接");
  if (auto r = validate_plan(request->plan); !r)
    throw net::Error(r.error);
  for (unsigned refresh = 0;; ++refresh) {
    try {
      std::string html;
      retry(
          [&] {
            html.clear();
            http.get(
                request->plan.source.shareUrl, net::Route::page, false,
                [&](const net::Headers &h) {
                  net::require_ok(h);
                  if (h.length && *h.length > 8 * 1024 * 1024)
                    throw net::Error(L"公开页面超过 8 MiB 限制");
                  return true;
                },
                [&](auto bytes) {
                  if (html.size() + bytes.size() > 8 * 1024 * 1024)
                    throw net::Error(L"公开页面超过 8 MiB 限制");
                  html += bytes;
                },
                check);
          },
          check);
      ResolvedSource result{
          request, parse_insta360(html, request->plan.source.captureId)};
      for (auto &r : result.resources) {
        check();
        retry(
            [&] {
              http.get(
                  r.signedUrl, net::Route::asset, true,
                  [&](const net::Headers &h) {
                    r.descriptor.expectedBytes = total(h);
                    return false;
                  },
                  [](auto) {}, check);
            },
            check);
      }
      return result;
    } catch (const net::Error &e) {
      check();
      if (refresh == 0 && (e.status == 401 || e.status == 403))
        continue;
      throw;
    }
  }
}
} // namespace gsdl::sources
