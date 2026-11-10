#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

namespace gsdl::net {
struct Error : std::runtime_error {
  std::wstring message;
  unsigned status{}, nativeCode{};
  bool retryable{};
  explicit Error(std::wstring text, unsigned http = 0, unsigned code = 0,
                 bool retry = false)
      : runtime_error("transfer error"), message(std::move(text)), status(http),
        nativeCode(code), retryable(retry) {}
};
struct Cancelled : std::exception {};
using Check = std::function<void()>;
struct Headers {
  unsigned status{};
  std::optional<std::uint64_t> length;
  std::wstring range;
};
using OnHeaders = std::function<bool(const Headers &)>;
using OnBytes = std::function<void(std::string_view)>;
enum class Route { page, asset, loopback_test };
struct Url {
  std::wstring host, path, target;
  unsigned short port{};
  bool secure{};
};
Url parse_url(std::wstring_view, Route);
std::wstring wide(std::string_view);
std::string utf8(std::wstring_view);
std::string hash_hex(std::string_view, bool md5 = false);
// The client is synchronous and must only run on a worker/CLI thread.
// Cancellation is observed between finite-timeout WinHTTP operations; handles
// are exclusively owned by that thread, never closed concurrently by the UI.
class Http {
public:
  virtual ~Http() = default;
  virtual void get(std::wstring_view url, Route route, bool probe,
                   const OnHeaders &, const OnBytes &, const Check &) = 0;
};
class WinHttp final : public Http {
  unsigned testChunkDelayMs_{};
public:
  // Opt-in UI validation only; production callers use the zero default.
  explicit WinHttp(unsigned testChunkDelayMs = 0);
  void get(std::wstring_view, Route, bool, const OnHeaders &, const OnBytes &,
           const Check &) override;
};
void require_ok(const Headers &, unsigned expected = 200);
void backoff(unsigned attempt, const Check &);
} // namespace gsdl::net
