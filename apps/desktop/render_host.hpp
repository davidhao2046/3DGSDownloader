#pragma once
#include "core/domain/plan.hpp"
#include "infra/storage/plan_storage.hpp"
#include "resources/theme/tokens.hpp"
#include "ui/accessibility.hpp"
#include "ui/text/text.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <dwmapi.h>
#include <filesystem>
#include <fstream>
#include <gsdl/graphics/skia_canvas.hpp>
#include <gsdl/graphics/win32_presenter.hpp>
#include <imm.h>
#include <memory>
#include <mutex>
#include <objbase.h>
#include <shellapi.h>
#include <sstream>
#include <windows.h>
#include <windowsx.h>

namespace g = gsdl::graphics;
namespace tx = gsdl::text;
namespace th = gsdl::theme;
namespace ly = gsdl::theme::layout;
namespace wm = gsdl::theme::windowMetrics;
namespace fs = std::filesystem;
constexpr UINT stopped_message = WM_APP + 1, error_message = WM_APP + 2,
               frame_message = WM_APP + 3;
constexpr UINT accessibility_message = WM_APP + 4;
std::string utf8(std::wstring_view s) {
  if (s.empty())
    return {};
  int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0,
                              nullptr, nullptr);
  std::string r(n, 0);
  WideCharToMultiByte(CP_UTF8, 0, s.data(), (int)s.size(), r.data(), n, nullptr,
                      nullptr);
  return r;
}
std::string json(std::wstring_view s) {
  std::string r = "\"";
  for (unsigned char c : utf8(s)) {
    if (c == '"' || c == '\\') {
      r += '\\';
      r += char(c);
    } else if (c < 32)
      r += ' ';
    else
      r += char(c);
  }
  return r + '"';
}
struct Box {
  g::Rect rect;
  g::Rgba color;
  float radius{};
  // Positive strokeWidth denotes a line from left/top to right/bottom.
  float strokeWidth{};
};
struct Text {
  std::wstring value;
  float x, y, size;
  g::Rgba color;
  std::optional<tx::Clip> clip;
  unsigned weight{400};
};
struct Frame {
  unsigned width{}, height{};
  float dpi = 1;
  uint64_t serial{}, revision{}, heartbeat{}, maxGap{}, commits{}, imeResults{};
  std::size_t undoCount{}, redoCount{};
  int selectedRow = -1;
  int focus = -1;
  bool composing{}, invalid{}, closing{};
  std::wstring link, output, draft;
  gsdl::Snapshot plan;
  bool dirty{};
  std::wstring planPath, ioState;
  std::string networkState{"null"};
  std::vector<Box> boxes;
  std::vector<Text> texts;
  std::size_t overlayBoxes=SIZE_MAX, overlayTexts=SIZE_MAX;
};
// Shared by the live renderer and deterministic component/page captures.
inline void render_frame(g::SkiaCanvas &canvas, tx::Rasterizer &text,
                         const Frame &frame) {
  canvas.resize(frame.width, frame.height);
  canvas.clear(th::window);
  const auto nb=std::min(frame.overlayBoxes,frame.boxes.size());
  const auto nt=std::min(frame.overlayTexts,frame.texts.size());
  auto boxes=[&](std::size_t begin,std::size_t end) {
    canvas.save();canvas.scale(frame.dpi);
    for(auto i=begin;i<end;++i) {
      const auto &b=frame.boxes[i];
      if(b.strokeWidth>0) canvas.line(b.rect.left,b.rect.top,b.rect.right,b.rect.bottom,b.color,b.strokeWidth);
      else if(b.radius>0) canvas.rounded_rect(b.rect,b.radius,b.radius,b.color);
      else canvas.rect(b.rect,b.color);
    }
    canvas.restore();
  };
  auto labels=[&](std::size_t begin,std::size_t end) {
    for(auto i=begin;i<end;++i) {
      const auto &t=frame.texts[i];
      text.draw(canvas.mutable_pixels(),canvas.width(),canvas.height(),t.value,t.x,t.y,t.size,
                {t.color.r,t.color.g,t.color.b,t.color.a},frame.dpi,t.clip,t.weight);
    }
  };
  boxes(0,nb);labels(0,nt);
  boxes(nb,frame.boxes.size());labels(nt,frame.texts.size());
}
struct RenderHost {
  HWND hwnd{};
  fs::path dll, evidence;
  unsigned delay{};
  std::mutex mutex;
  std::condition_variable cv;
  bool stop{};
  std::shared_ptr<const Frame> latest;
  std::deque<std::string> events;
  std::string error;
  DWORD uiThread{};
  void publish(std::shared_ptr<const Frame> f) {
    std::lock_guard lock(mutex);
    latest = std::move(f);
    cv.notify_one();
  }
  void event(std::string e) {
    std::lock_guard lock(mutex);
    if (events.size() < 4096)
      events.push_back(std::move(e));
  }
  void shutdown() {
    std::lock_guard lock(mutex);
    stop = true;
    cv.notify_one();
  }
  void record(const Frame &f, std::ofstream &log) noexcept {
    if (evidence.empty())
      return;
    // Evidence is best effort. Its encoding or filesystem failures must never
    // become rendering failures, including the last record during shutdown.
    try {
      std::string encodedPlan = "null";
      bool planEncodingFailed = false;
      try {
        if (f.plan)
          encodedPlan = gsdl::storage::encode_plan(f.plan->plan);
      } catch (...) {
        planEncodingFailed = true;
      }
      std::deque<std::string> drained;
      {
        std::lock_guard lock(mutex);
        drained.swap(events);
      }
      for (const auto &e : drained)
        log << e << '\n';
      log.flush();
      std::ofstream o(evidence / "state.json");
      o << "{\"ui_thread\":" << uiThread
        << ",\"render_thread\":" << GetCurrentThreadId()
        << ",\"serial\":" << f.serial << ",\"revision\":" << f.revision
        << ",\"heartbeat\":" << f.heartbeat
        << ",\"max_heartbeat_gap_ms\":" << f.maxGap << ",\"focus\":" << f.focus
        << ",\"commits\":" << f.commits << ",\"ime_results\":" << f.imeResults
        << ",\"undo_count\":" << f.undoCount
        << ",\"redo_count\":" << f.redoCount
        << ",\"selected_row\":" << f.selectedRow
        << ",\"composing\":" << (f.composing ? "true" : "false")
        << ",\"invalid\":" << (f.invalid ? "true" : "false")
        << ",\"closing\":" << (f.closing ? "true" : "false")
        << ",\"link\":" << json(f.link) << ",\"output\":" << json(f.output)
        << ",\"draft\":"
        << json(f.focus == 0 ? L"[link draft omitted]" : f.draft)
        << ",\"width\":" << f.width << ",\"height\":" << f.height
        << ",\"dpi\":" << f.dpi << ",\"dirty\":" << (f.dirty ? "true" : "false")
        << ",\"plan_path\":" << json(f.planPath)
        << ",\"io_state\":" << json(f.ioState)
        << ",\"network\":" << f.networkState
        << ",\"session_id\":" << (f.plan ? f.plan->sessionId : 0)
        << ",\"input_revision\":" << (f.plan ? f.plan->inputRevision : 0)
        << ",\"evidence_plan_encoding_failed\":"
        << (planEncodingFailed ? "true" : "false")
        << ",\"plan\":" << encodedPlan << "}\n";
    } catch (...) {
      OutputDebugStringW(
          L"3DGSDownloader: evidence record failed; rendering continues.\n");
    }
  }
  void run() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    std::ofstream log;
    std::shared_ptr<const Frame> current;
    try {
      if (!evidence.empty()) {
        try {
          fs::create_directories(evidence);
          log.open(evidence / "events.jsonl");
        } catch (...) {
          evidence.clear();
          OutputDebugStringW(
              L"3DGSDownloader: evidence setup failed; rendering continues.\n");
        }
      }
      {
        g::SkiaCanvas canvas(dll);
        g::Win32Presenter presenter(hwnd);
        tx::Rasterizer text;
        uint64_t last = 0;
        for (;;) {
          {
            std::unique_lock lock(mutex);
            cv.wait_for(lock, std::chrono::milliseconds(100), [&] {
              return stop || (latest && latest->serial != last);
            });
            if (stop) {
              current = latest;
              break;
            }
            current = latest;
          }
          if (!current || current->serial == last)
            continue;
          if (delay) {
            std::unique_lock lock(mutex);
            if (cv.wait_for(lock, std::chrono::milliseconds(delay),
                            [&] { return stop; })) {
              current = latest;
              break;
            }
            current = latest;
          }
          if (!current->width || !current->height) {
            last = current->serial;
            record(*current, log);
            continue;
          }
          {
            std::lock_guard lock(mutex);
            if (latest && (latest->width != current->width ||
                           latest->height != current->height ||
                           latest->dpi != current->dpi))
              continue;
          }
          render_frame(canvas, text, *current);
          {
            std::lock_guard lock(mutex);
            if (latest && (latest->width != current->width ||
                           latest->height != current->height ||
                           latest->dpi != current->dpi))
              continue;
          }
          g::Rgba8Frame pixels;
          pixels.width = canvas.width();
          pixels.height = canvas.height();
          pixels.rgba.assign(canvas.pixels().begin(), canvas.pixels().end());
          const auto result = presenter.present(pixels);
          if (result.status == g::PresentStatus::retry) {
            std::unique_lock lock(mutex);
            cv.wait_for(lock, std::chrono::milliseconds(16),
                        [&] { return stop; });
            continue;
          }
          last = current->serial;
          record(*current, log);
          if (result.status == g::PresentStatus::presented)
            PostMessageW(hwnd, frame_message, (WPARAM)last, 0);
        }
      } // All Vulkan fence waits and resource destruction happen here, never on
        // UI.
    } catch (const std::exception &e) {
      {
        std::lock_guard lock(mutex);
        error = e.what();
      }
      PostMessageW(hwnd, error_message, 0, 0);
      std::unique_lock lock(mutex);
      cv.wait(lock, [&] { return stop; });
      current = latest;
    }
    if (current)
      record(*current, log);
    log.close();
    CoUninitialize();
    PostMessageW(hwnd, stopped_message, 0, 0);
  }
};
