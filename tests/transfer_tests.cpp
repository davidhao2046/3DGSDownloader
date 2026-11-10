// clang-format off
#include <winsock2.h>
#include <windows.h>
// clang-format on
#include "core/application/plan_service.hpp"
#include "core/application/transfer_service.hpp"
#include "infra/net/bounded_json.hpp"
#include "infra/storage/plan_storage.hpp"
#include <condition_variable>
#include <fstream>
#include <iostream>
#include <thread>

using namespace gsdl;
namespace {
int checks{};
void expect(bool value, const char *label) {
  ++checks;
  if (!value)
    throw std::runtime_error(label);
}
template <class F> void rejects(F &&f, const char *label) {
  bool thrown{};
  try {
    f();
  } catch (const net::Error &) {
    thrown = true;
  }
  expect(thrown, label);
}
const std::string md5 = "5d41402abc4b2a76b9719d911017c592";
const std::string sha =
    "2cf24dba5fb0a30e26e83b2ac5b9e29e1b161e5c1fa7425e73043362938b9824";
const std::wstring link = L"https://app.insta360.com/3dspace/detail/GS3Dtest";
std::string page(std::string checksum = md5) {
  return "<html><script id=\"__NEXT_DATA__\" "
         "type=\"application/"
         "json\">{\"props\":{\"pageProps\":{\"detailErrorCode\":0,"
         "\"taskDetail\":{\"taskOrderNo\":\"GS3Dtest\",\"isPrivate\":0,"
         "\"taskStatus\":3,\"outputs\":[{\"fileFormat\":\"sog\",\"md5\":\"" +
         checksum +
         "\",\"url\":\"https://insta360-app-hz.oss-cn-hangzhou.aliyuncs.com/"
         "3dgs/GS3Dtest/model.sog?Signature=fixture-only\"}]}}}}</script></"
         "html>";
}
std::string replace(std::string s, std::string_view a, std::string_view b) {
  auto p = s.find(a);
  if (p == s.npos)
    throw std::runtime_error("fixture marker");
  s.replace(p, a.size(), b);
  return s;
}
enum class Mode {
  good,
  unknown,
  probe200,
  badHash,
  truncated,
  authOnce,
  authAlways,
  changed,
  limited,
  unavailable
};
struct Fake final : net::Http {
  Mode mode{Mode::good};
  int pages{}, probes{}, full{};
  void get(std::wstring_view, net::Route route, bool probe,
           const net::OnHeaders &headers, const net::OnBytes &bytes,
           const net::Check &check) override {
    check();
    if (route == net::Route::page) {
      ++pages;
      auto body =
          page(mode == Mode::changed && pages > 1 ? std::string(32, '0') : md5);
      if (headers({200, body.size(), {}}))
        bytes(body);
      return;
    }
    if (probe) {
      ++probes;
      net::Headers h{206, 1, L"bytes 0-0/5"};
      if (mode == Mode::probe200)
        h = {200, 5, {}};
      if (mode == Mode::unknown)
        h = {200, {}, {}};
      if (headers(h))
        bytes("h");
      return;
    }
    ++full;
    unsigned status =
        mode == Mode::authAlways ||
                ((mode == Mode::authOnce || mode == Mode::changed) && full == 1)
            ? 403
        : mode == Mode::unavailable         ? 503
        : mode == Mode::limited && full < 3 ? 429
                                            : 200;
    if (!headers({status,
                  mode == Mode::unknown ? std::optional<uint64_t>{}
                                        : std::optional<uint64_t>{5},
                  {}}))
      return;
    std::string body = mode == Mode::badHash     ? "hullo"
                       : mode == Mode::truncated ? "hell"
                                                 : "hello";
    check();
    bytes(body.substr(0, 2));
    check();
    bytes(body.substr(2));
    check();
  }
};
struct Fixture {
  PlanService plan;
  Fake http;
  sources::ResolvedSource resolved;
  std::filesystem::path directory;
  Fixture(std::filesystem::path base, std::string name, Mode mode = Mode::good)
      : directory(base / net::wide(name)) {
    http.mode = mode;
    expect(plan.edit("source.shareUrl", {}, link).success, "fixture link");
    expect(plan.edit("outputDirectory", {}, directory.wstring()).success,
           "fixture directory");
    resolved = sources::resolve(plan.snapshot(), http, [] {});
    std::vector<Resource> rows;
    for (const auto &r : resolved.resources)
      rows.push_back(r.descriptor);
    expect(plan.acceptResolved(resolved.request, std::move(rows),
                               sources::adapter_version)
               .success,
           "accept fixture");
  }
  std::filesystem::path target() const { return directory / L"model.sog"; }
  void run(
      transfer::CancelGate &gate, transfer::Report &report,
      const transfer::NotifyProgress &progress = [](const auto &) {}) {
    transfer::download(plan.snapshot(), resolved, http, gate, progress, report);
  }
  void no_part() {
    if (!std::filesystem::exists(directory))
      return;
    for (const auto &e : std::filesystem::directory_iterator(directory))
      expect(e.path().extension() != L".part", "no abandoned part");
  }
};
std::string read(const std::filesystem::path &path) {
  std::ifstream f(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(f), {}};
}
struct Server {
  SOCKET listener{INVALID_SOCKET};
  std::thread thread;
  unsigned short port{};
  std::vector<std::string> requests;
  explicit Server(std::vector<std::string> replies) {
    listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listener == INVALID_SOCKET)
      throw std::runtime_error("socket");
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(listener, reinterpret_cast<sockaddr *>(&address),
             sizeof address) ||
        listen(listener, 8))
      throw std::runtime_error("bind");
    int length = sizeof address;
    getsockname(listener, reinterpret_cast<sockaddr *>(&address), &length);
    port = ntohs(address.sin_port);
    thread = std::thread([this, replies = std::move(replies)] {
      for (const auto &reply : replies) {
        auto client = accept(listener, nullptr, nullptr);
        if (client == INVALID_SOCKET)
          return;
        DWORD timeout = 3000;
        setsockopt(client, SOL_SOCKET, SO_RCVTIMEO,
                   reinterpret_cast<char *>(&timeout), sizeof timeout);
        std::string request;
        char buffer[2048];
        for (;;) {
          int count = recv(client, buffer, sizeof buffer, 0);
          if (count <= 0)
            break;
          request.append(buffer, count);
          if (request.find("\r\n\r\n") != request.npos ||
              request.size() > 16384)
            break;
        }
        requests.push_back(std::move(request));
        std::size_t offset{};
        while (offset < reply.size()) {
          int sent = send(client, reply.data() + offset,
                          static_cast<int>(reply.size() - offset), 0);
          if (sent <= 0)
            break;
          offset += sent;
        }
        shutdown(client, SD_BOTH);
        closesocket(client);
      }
    });
  }
  ~Server() {
    closesocket(listener);
    if (thread.joinable())
      thread.join();
  }
  std::wstring url() const {
    return L"http://127.0.0.1:" + std::to_wstring(port) + L"/file";
  }
  void done() {
    if (thread.joinable())
      thread.join();
  }
};
void network_tests() {
  WSADATA data{};
  expect(WSAStartup(MAKEWORD(2, 2), &data) == 0, "winsock");
  net::WinHttp client;
  {
    Server server({"HTTP/1.1 200 OK\r\nContent-Length: 5\r\nConnection: "
                   "close\r\n\r\nhello"});
    std::string bytes;
    client.get(
        server.url(), net::Route::loopback_test, false,
        [&](const auto &h) {
          expect(h.status == 200 && h.length == 5, "real headers");
          return true;
        },
        [&](auto b) { bytes += b; }, [] {});
    server.done();
    expect(bytes == "hello", "real body");
    expect(server.requests[0].find("Cookie:") == std::string::npos &&
               server.requests[0].find("Authorization:") == std::string::npos,
           "anonymous headers");
  }
  {
    Server server(
        {"HTTP/1.1 206 Partial Content\r\nContent-Length: 1\r\nContent-Range: "
         "bytes 0-0/5\r\nConnection: close\r\n\r\nh"});
    std::string bytes;
    client.get(
        server.url(), net::Route::loopback_test, true,
        [&](const auto &h) {
          expect(h.status == 206 && h.range == L"bytes 0-0/5", "real range");
          return false;
        },
        [&](auto b) { bytes += b; }, [] {});
    server.done();
    expect(bytes.empty() &&
               server.requests[0].find("Range: bytes=0-0") != std::string::npos,
           "bounded GET probe no consumed prefix");
  }
  {
    Server server({"HTTP/1.1 200 OK\r\nConnection: close\r\n\r\nhello"});
    std::string bytes;
    client.get(
        server.url(), net::Route::loopback_test, false,
        [&](const auto &h) {
          expect(!h.length, "unknown total");
          return true;
        },
        [&](auto b) { bytes += b; }, [] {});
    expect(bytes == "hello", "unknown body");
  }
  {
    Server server({"HTTP/1.1 200 OK\r\nContent-Length: 10\r\nConnection: "
                   "close\r\n\r\nhello"});
    rejects(
        [&] {
          client.get(
              server.url(), net::Route::loopback_test, false,
              [](const auto &) { return true; }, [](auto) {}, [] {});
        },
        "real disconnect");
  }
  {
    Server server({"HTTP/1.1 302 Found\r\nLocation: "
                   "https://example.com/private\r\nContent-Length: "
                   "0\r\nConnection: close\r\n\r\n"});
    rejects(
        [&] {
          client.get(
              server.url(), net::Route::loopback_test, false,
              [](const auto &) { return true; }, [](auto) {}, [] {});
        },
        "redirect policy enforced before request");
  }
  {
    std::vector<std::string> responses(
        5, "HTTP/1.1 302 Found\r\nLocation: /loop\r\nContent-Length: "
           "0\r\nConnection: close\r\n\r\n");
    Server server(responses);
    rejects(
        [&] {
          client.get(
              server.url(), net::Route::loopback_test, false,
              [](const auto &) { return true; }, [](auto) {}, [] {});
        },
        "redirect limit");
    server.done();
    expect(server.requests.size() == 5, "bounded redirects");
  }
  WSACleanup();
}
} // namespace
int wmain(int argc, wchar_t **argv) {
  try {
    auto root =
        std::filesystem::absolute(argc > 1 ? argv[1] : L"transfer-tests") /
        std::to_wstring(GetTickCount64());
    std::filesystem::create_directories(root);
    expect(net::hash_hex("hello", true) == md5 && net::hash_hex("hello") == sha,
           "known hash vectors");
    auto parsed = sources::parse_insta360(page(), "GS3Dtest");
    expect(parsed.size() == 1 && parsed[0].descriptor.included &&
               parsed[0].descriptor.format == L"SOG",
           "parse real-shaped fixture");
    auto cameraPage=replace(replace(page(),"\"fileFormat\":\"sog\"","\"fileFormat\":\"json\""),"model.sog","2_cameras.json");
    auto cameraParsed=sources::parse_insta360(cameraPage,"GS3Dtest");
    expect(cameraParsed.size()==1 && cameraParsed[0].descriptor.included,"camera data is recommended for new plans");
    auto otherJson=sources::parse_insta360(replace(cameraPage,"2_cameras.json","metadata.json"),"GS3Dtest");
    expect(!otherJson[0].descriptor.included,"arbitrary json is not automatically selected");
    expect(sources::parse_insta360(
               replace(page(), "fixture-only", "different-signature"),
               "GS3Dtest")[0]
                   .descriptor.id == parsed[0].descriptor.id,
           "signature-independent identity");
    rejects(
        [&] {
          sources::parse_insta360(
              replace(page(), "\"isPrivate\":0", "\"isPrivate\":1"),
              "GS3Dtest");
        },
        "private rejected");
    rejects(
        [&] {
          sources::parse_insta360(
              replace(page(), "\"taskStatus\":3", "\"taskStatus\":1"),
              "GS3Dtest");
        },
        "not ready rejected");
    rejects([&] { sources::parse_insta360(page(), "GS3Dother"); },
            "capture mismatch rejected");
    rejects(
        [&] {
          sources::parse_insta360(replace(page(), "model.sog", "..%2fevil.sog"),
                                  "GS3Dtest");
        },
        "encoded traversal rejected");
    rejects(
        [&] {
          sources::parse_insta360(replace(page(), "model.sog", "CON.sog"),
                                  "GS3Dtest");
        },
        "reserved remote name rejected");
    rejects(
        [&] {
          sources::parse_insta360(
              replace(page(), "\"detailErrorCode\":0",
                      "\"detailErrorCode\":0,\"detailErrorCode\":0"),
              "GS3Dtest");
        },
        "duplicate JSON key rejected");
    rejects(
        [&] {
          sources::parse_insta360("<html>not a share</html>", "GS3Dtest");
        },
        "missing next data rejected");
    rejects([&] { net::json::Parser("{\"x\":\"\\ud800\"}").parse(); },
            "bad surrogate rejected");
    rejects(
        [&] {
          net::json::Parser(std::string(8 * 1024 * 1024 + 1, ' ')).parse();
        },
        "JSON bounded");
    expect(net::json::Parser("[-1.25e+2,null,true,false]")
                   .parse()
                   .array()
                   .size() == 4,
           "general JSON numbers supported");
    rejects(
        [&] {
          net::parse_url(
              L"https://insta360-app-hz.oss-cn-hangzhou.aliyuncs.com.evil.test/"
              L"x",
              net::Route::asset);
        },
        "host suffix rejected");
    rejects(
        [&] { net::parse_url(L"http://app.insta360.com/x", net::Route::page); },
        "HTTPS required");
    rejects(
        [&] {
          net::parse_url(L"https://user:pass@app.insta360.com/x",
                         net::Route::page);
        },
        "credentials rejected");
    {
      Fixture f(root, "stale");
      auto request = f.plan.snapshot();
      auto rows = request->plan.resources;
      f.plan.edit("outputDirectory", {}, (root / L"other").wstring());
      expect(!f.plan.acceptResolved(request, rows, sources::adapter_version),
             "stale revision rejected");
      f.plan.newPlan();
      expect(!f.plan.acceptResolved(request, rows, sources::adapter_version),
             "stale session rejected");
    }
    for (auto mode : {Mode::good, Mode::unknown, Mode::probe200, Mode::authOnce,
                      Mode::limited}) {
      Fixture f(root, "success-" + std::to_string(static_cast<int>(mode)),
                mode);
      transfer::CancelGate gate;
      transfer::Report result;
      bool unknownSeen{};
      f.run(gate, result, [&](const auto &p) {
        if (p.bytes && !p.total)
          unknownSeen = true;
      });
      expect(result.files.size() == 1 && read(f.target()) == "hello",
             "download full original bytes");
      expect(result.files[0].md5 == md5 && result.files[0].sha256 == sha &&
                 result.files[0].expectedMd5 == md5,
             "verified receipt hashes");
      if (mode == Mode::unknown)
        expect(unknownSeen, "unknown length not invented");
      if (mode == Mode::authOnce)
        expect(f.http.pages == 2 && f.http.full == 2, "one address refresh");
      if (mode == Mode::limited)
        expect(f.http.full == 3, "bounded 429 retry");
      expect(storage::encode_plan(f.plan.snapshot()->plan).find("Signature") ==
                 std::string::npos,
             "signature not persisted");
      f.no_part();
    }
    for (auto mode : {Mode::badHash, Mode::truncated, Mode::authAlways,
                      Mode::changed, Mode::unavailable}) {
      Fixture f(root, "failure-" + std::to_string(static_cast<int>(mode)),
                mode);
      transfer::CancelGate gate;
      transfer::Report result;
      rejects([&] { f.run(gate, result); }, "failed transfer rejected");
      expect(result.files.empty() && !std::filesystem::exists(f.target()),
             "failed transfer not published");
      if (mode == Mode::truncated || mode == Mode::unavailable)
        expect(f.http.full == 3, "retry capped at three");
      if (mode == Mode::authAlways)
        expect(f.http.pages == 2 && f.http.full == 2, "auth refresh capped");
      f.no_part();
    }
    for (int boundary = 0; boundary < 3; ++boundary) {
      Fixture f(root, "cancel-" + std::to_string(boundary));
      transfer::CancelGate gate;
      transfer::Report result;
      bool cancelled{};
      try {
        f.run(gate, result, [&](const auto &p) {
          if ((boundary == 0 && p.bytes == 2) ||
              (boundary == 1 && p.stage == L"正在发布") ||
              (boundary == 2 && p.stage == L"文件已完成"))
            gate.request();
        });
      } catch (const net::Cancelled &) {
        cancelled = true;
      }
      expect(boundary == 2 ? !cancelled : cancelled, "cancel commit ordering");
      expect(std::filesystem::exists(f.target()) == (boundary == 2),
             "publication matches cancellation winner");
      expect(result.files.size() == (boundary == 2 ? 1u : 0u),
             "completed file never reported cancelled");
      f.no_part();
    }
    {
      Fixture f(root, "conflict");
      std::filesystem::create_directories(f.directory);
      std::ofstream(f.target()) << "original";
      transfer::CancelGate gate;
      transfer::Report result;
      rejects([&] { f.run(gate, result); }, "collision rejected");
      expect(read(f.target()) == "original" && f.http.full == 0,
             "existing file never overwritten or downloaded over");
      f.no_part();
    }
    {
      transfer::CancelGate gate;
      expect(gate.begin_publish(), "commit wins");
      gate.request();
      gate.published(true);
      expect(!gate.begin_publish(), "no second publication");
    }
    {
      Fixture f(root, "batch-cancel");
      auto second = f.resolved.resources[0];
      second.descriptor.id = "second";
      second.descriptor.outputName = L"second.sog";
      second.descriptor.displayName = L"second.sog";
      f.resolved.resources.push_back(second);
      auto rows = f.plan.snapshot()->plan.resources;
      rows.push_back(second.descriptor);
      expect(
          f.plan
              .acceptResolved(f.plan.snapshot(), rows, sources::adapter_version)
              .success,
          "batch setup");
      transfer::CancelGate gate;
      transfer::Report report;
      bool cancelled{};
      try {
        f.run(gate, report, [&](const auto &p) {
          if (p.completedFiles == 1)
            gate.request();
        });
      } catch (const net::Cancelled &) {
        cancelled = true;
      }
      expect(cancelled && report.files.size() == 1 &&
                 std::filesystem::exists(f.target()) &&
                 !std::filesystem::exists(f.directory / L"second.sog"),
             "batch cancellation preserves first completed file");
      f.no_part();
    }
    {
      Fixture f(root, "verified-reuse");
      transfer::CancelGate first;
      transfer::Report saved;
      f.run(first, saved);
      expect(transfer::check_local(saved.files.front())==transfer::LocalState::available,"worker local check available");
      auto calls = f.http.full;
      transfer::CancelGate second;
      transfer::Report reused;
      bool sawPublished=false;
      transfer::download(f.plan.snapshot(), f.resolved, f.http, second,
                         [&](const auto &p) {if(!p.published.empty()) sawPublished=true;}, reused, saved.files);
      expect(sawPublished,"receipt published before terminal");
      expect(reused.files.size() == 1 && f.http.full == calls,
             "verified receipt skips network without overwriting");
      std::ofstream(f.target(), std::ios::trunc) << "changed";
      expect(transfer::check_local(saved.files.front())==transfer::LocalState::changed,"worker local check modified");
      transfer::CancelGate third;
      transfer::Report bad;
      rejects([&] { transfer::download(f.plan.snapshot(), f.resolved, f.http,
                third, [](const auto &) {}, bad, saved.files); }, "modified receipt target rejected");
      expect(read(f.target()) == "changed" && f.http.full == calls,
             "modified original preserved");
      std::filesystem::remove(f.target());
      expect(transfer::check_local(saved.files.front())==transfer::LocalState::missing,"worker local check missing");
      transfer::CancelGate fourth;
      transfer::Report missing;
      transfer::download(f.plan.snapshot(), f.resolved, f.http, fourth,
                        [](const auto &) {}, missing, saved.files);
      expect(missing.files.size() == 1 && f.http.full == calls + 1,
             "missing original fetched again");
    }
    network_tests();
    {
      auto state =
          std::make_shared<std::pair<std::mutex, std::condition_variable>>();
      auto stopped = std::make_shared<std::atomic<bool>>();
      transfer::Service service([] {},
                                [state, stopped] {
                                  *stopped = true;
                                  state->second.notify_all();
                                });
      service.stop();
      std::unique_lock lock(state->first);
      expect(state->second.wait_for(lock, std::chrono::seconds(3),
                                    [&] { return stopped->load(); }),
             "worker asynchronous stop");
    }
    std::cout << "transfer_tests: " << checks
              << " checks passed; fixture artifacts="
              << net::utf8(root.wstring()) << "\n";
    return 0;
  } catch (const std::exception &e) {
    std::cerr << "transfer_tests failed: " << e.what() << " after " << checks
              << " checks\n";
    return 1;
  }
}
