#include "themed_menu.hpp"
#include "desktop_app.hpp"
LRESULT CALLBACK wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  auto a = (App *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
  if (msg == WM_NCCREATE) {
    a = (App *)((CREATESTRUCTW *)lp)->lpCreateParams;
    a->hwnd = hwnd;
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)a);
  }
  if (!a)
    return DefWindowProcW(hwnd, msg, wp, lp);
  try {
    // The last committed snapshot is frozen once asynchronous shutdown starts.
    // Continue pumping lifetime/paint messages, but do not accept more edits.
    if (a->closing &&
        ((msg >= WM_KEYFIRST && msg <= WM_KEYLAST) ||
         (msg >= WM_MOUSEFIRST && msg <= WM_MOUSELAST) || msg == WM_PASTE ||
         msg == WM_CUT || msg == WM_CLEAR || msg == WM_UNDO ||
         msg == WM_IME_STARTCOMPOSITION || msg == WM_IME_COMPOSITION ||
         msg == WM_IME_ENDCOMPOSITION || msg == WM_IME_CHAR))
      return 0;
    if (a->text_focus()) {
      if (msg == WM_IME_COMPOSITION && (lp & GCS_RESULTSTR)) {
        ++a->imeResults;
        a->trace("ime_result");
      }
      if (auto handled =
              tx::ime_message(hwnd, msg, wp, lp, a->editors[a->focus])) {
        if (msg == WM_IME_ENDCOMPOSITION && !a->active)
          a->commit_focus();
        a->trace("ime_message");
        a->publish();
        return *handled;
      }
    }
    switch (msg) {
    case WM_ERASEBKGND:
      return 1;
    case WM_PAINT: {
      PAINTSTRUCT ps{};
      BeginPaint(hwnd, &ps);
      EndPaint(hwnd, &ps);
      a->publish();
      return 0;
    }
    case WM_SIZE:
      a->layout();
      a->publish();
      return 0;
    case WM_DPICHANGED: {
      auto r = (RECT *)lp;
      SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left,
                   r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
      a->layout();
      a->trace("dpi_changed");
      a->publish();
      return 0;
    }
    case WM_GETMINMAXINFO: {
      auto m = (MINMAXINFO *)lp;
      auto dpi = GetDpiForWindow(hwnd);
      RECT r{0, 0, MulDiv(wm::minimumWidth, dpi, wm::baseDpi),
             MulDiv(wm::minimumHeight, dpi, wm::baseDpi)};
      AdjustWindowRectExForDpi(&r, WS_OVERLAPPEDWINDOW, FALSE, 0, dpi);
      m->ptMinTrackSize = {r.right - r.left, r.bottom - r.top};
      return 0;
    }
    case WM_TIMER: {
      auto now = GetTickCount64();
      a->maxGap = std::max(a->maxGap, now - a->lastTick);
      a->lastTick = now;
      ++a->heartbeat;
      if (a->heartbeat % 10 == 0)
        a->publish();
      return 0;
    }
    case WM_SETFOCUS:
      a->active = true;
      if (a->input_locked()) {
        a->publish();
        return 0;
      }
      if (a->text_focus() && !a->editors[a->focus].active())
        a->begin_editor();
      a->sync_ime();
      a->publish();
      return 0;
    case WM_KILLFOCUS:
      a->active = false;
      a->highSurrogate = 0;
      a->cancel_capture();
      a->commit_focus();
      a->trace("window_blur");
      a->publish();
      return 0;
    case WM_LBUTTONDOWN:
    case WM_LBUTTONDBLCLK:
      a->down(GET_X_LPARAM(lp) / a->dpi, GET_Y_LPARAM(lp) / a->dpi,
              msg == WM_LBUTTONDBLCLK);
      return 0;
    case WM_MOUSEMOVE: {
      TRACKMOUSEEVENT t{sizeof t, TME_LEAVE, hwnd, 0};
      TrackMouseEvent(&t);
      a->move(GET_X_LPARAM(lp) / a->dpi, GET_Y_LPARAM(lp) / a->dpi);
      return 0;
    }
    case WM_MOUSELEAVE:
      a->hover = -1;
      a->publish();
      return 0;
    case WM_LBUTTONUP:
      a->up(GET_X_LPARAM(lp) / a->dpi, GET_Y_LPARAM(lp) / a->dpi);
      return 0;
    case WM_MOUSEWHEEL:
      a->wheel(GET_WHEEL_DELTA_WPARAM(wp));
      return 0;
    case WM_INITMENUPOPUP:
      a->update_menu();
      return 0;
    case WM_MEASUREITEM:
      if (themed_menu::measure(reinterpret_cast<MEASUREITEMSTRUCT *>(lp)))
        return TRUE;
      break;
    case WM_DRAWITEM:
      if (themed_menu::draw(reinterpret_cast<DRAWITEMSTRUCT *>(lp)))
        return TRUE;
      break;
    case WM_COMMAND:
      if (LOWORD(wp) == 200)
        a->close();
      else if (LOWORD(wp) >= 100 && LOWORD(wp) < 100 + ControlCount)
        a->activate(LOWORD(wp) - 100);
      return 0;
    case WM_CONTEXTMENU: {
      if (a->input_locked() || a->screen == App::Screen::create)
        return 0;
      POINT pos{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
      const bool keyboard = pos.x == -1 && pos.y == -1;
      auto model = a->list_model();
      auto layout = a->list_layout();
      bool taskContext = false;
      if (!keyboard) {
        POINT client = pos;
        ScreenToClient(hwnd, &client);
        const float x = client.x / a->dpi, y = client.y / a->dpi;
        // Resolve the containing parent row even over its inline controls.
        for (const auto &hit : layout.hits) {
          if (hit.row && x >= hit.rect.left && x < hit.rect.right &&
              y >= hit.rect.top && y < hit.rect.bottom) {
            a->select_task(hit.key - taskui::RowBase);
            taskContext = true;
            break;
          }
        }
        const auto key = layout.hit(x, y);
        if (key >= taskui::FileBase && key < taskui::RevealBase) {
          taskContext = a->select_expanded_owner();
          if(taskContext) a->focus=key;
        }
      } else {
        // Keyboard menu belongs to the focused task, never the last mouse point.
        const auto focused = std::find_if(layout.hits.begin(), layout.hits.end(),
            [&](const auto &hit) { return hit.key == a->focus; });
        if(focused!=layout.hits.end()) {
          const int key=focused->key;
          if(key>=taskui::RevealBase && key<taskui::RowBase)
            a->select_task(key-taskui::RevealBase);
          else if(key>=taskui::FileBase && key<taskui::RevealBase)
            a->select_expanded_owner();
          else if(key>=taskui::ExpandBase && key<taskui::FileBase)
            a->select_task(key-taskui::ExpandBase);
          else if(focused->row)
            a->select_task(key-taskui::RowBase);
          a->focus=key;
          model=a->list_model();
        }
        taskContext = a->current() && focused != layout.hits.end() &&
            (focused->row || a->focus >= taskui::ExpandBase ||
             a->focus == taskui::Details || a->focus == taskui::Start ||
             a->focus == taskui::Cancel || a->focus == taskui::Reveal);
        auto anchor = focused != layout.hits.end() ? focused->rect : layout.list;
        if (taskContext) {
          const auto row = std::find_if(layout.hits.begin(), layout.hits.end(),
              [&](const auto &hit) { return hit.row && hit.key == taskui::RowBase + model.selectedRow; });
          if (row != layout.hits.end()) anchor = row->rect;
        }
        pos = {LONG(anchor.left * a->dpi), LONG(anchor.bottom * a->dpi)};
        ClientToScreen(hwnd, &pos);
      }
      themed_menu::Popup popup(hwnd);
      const auto add = [&](int id, const char *key, std::wstring label,
                           std::wstring shortcut = {}, bool omitUnavailable = false) {
        const auto action = a->actions.query(key);
        if (!action || (omitUnavailable && !action->enabled)) return;
        popup.add(100 + id, std::move(label), action->enabled, std::move(shortcut));
      };
      if (taskContext) {
        add(taskui::Details, "task.details", a->expandedTaskId==a->selectedTask ? L"收起文件" : L"展开文件");
        model = a->list_model();
        add(taskui::Start, "download.start", model.startLabel, {}, true);
        add(taskui::Cancel, "download.cancel", L"取消任务", {}, true);
        add(taskui::Reveal, "result.reveal", L"打开文件夹", {}, true);
        popup.separator();
        add(RemoveTask, "task.remove", L"移除任务记录");
      } else {
        add(NewPlan, "file.newPlan", L"新建下载", L"Ctrl+N");
        popup.separator();
        add(OpenPlan, "file.openPlan", L"导入下载计划…", L"Ctrl+O");
        add(SavePlan, "file.savePlan", L"导出所选任务计划…", L"Ctrl+S");
      }
      const auto command = popup.show(pos);
      if (command)
        a->activate(command - 100);
      return 0;
    }
    case WM_CAPTURECHANGED:
      a->pressed = -1;
      a->selecting = false;
      a->trace("capture_lost");
      a->publish();
      return 0;
    case WM_KEYDOWN:
      a->key(wp, lp);
      return 0;
    case WM_CHAR:
      if (!a->input_locked() && a->text_focus() && wp >= 32 &&
          !a->editors[a->focus].composing()) {
        if (!a->editors[a->focus].active())
          a->begin_editor();
        wchar_t c = (wchar_t)wp;
        if (c >= 0xD800 && c <= 0xDBFF) {
          a->highSurrogate = c;
          return 0;
        }
        if (c >= 0xDC00 && c <= 0xDFFF) {
          if (a->highSurrogate) {
            wchar_t pair[]{a->highSurrogate, c};
            a->editors[a->focus].insert({pair, 2});
          }
        } else
          a->editors[a->focus].insert({&c, 1});
        a->highSurrogate = 0;
        a->trace("text");
        a->publish();
      }
      return 0;
    case WM_PASTE:
      a->clipboard(false);
      return 0;
    case WM_CLOSE:
      a->close();
      return 0;
    case WM_GETOBJECT:
      if (a->accessibility)
        return a->accessibility->get_object(wp, lp);
      break;
    case accessibility_message: {
      std::unique_ptr<gsdl::ui::AccessibleRequest> req(
          (gsdl::ui::AccessibleRequest *)lp);
      if (!req || a->input_locked())
        return 0;
      --req->id;
      if (a->set_focus(req->id)) {
        SetFocus(hwnd);
        if (req->kind == gsdl::ui::AccessibleRequestKind::invoke)
          a->activate(req->id);
        else if (req->kind == gsdl::ui::AccessibleRequestKind::set_value &&
                 req->id >= 0 && req->id <= OutputName) {
          a->editors[req->id].select_all();
          if (req->value.empty())
            a->editors[req->id].erase();
          else
            a->editors[req->id].insert(req->value);
          a->publish();
        }
      }
      return 0;
    }
    case stopped_message:
      a->renderStopped = true;
      a->finish_shutdown();
      return 0;
    case io_stopped_message:
      a->ioStopped = true;
      a->finish_shutdown();
      return 0;
    case workspace_message:
      a->on_workspace();
      return 0;
    case workspace_stopped_message:
      a->workspaceStopped = true;
      a->finish_shutdown();
      return 0;
    case folder_message:
      a->on_folder_open();return 0;
    case folder_stopped_message:
      a->folderStopped=true;a->finish_shutdown();return 0;
    case io_message:
      a->on_io();
      return 0;
    case WM_APP + 11:
      a->on_creation(); return 0;
    case WM_APP + 12:
      a->creationStopped = true; a->finish_shutdown(); return 0;
    case transfer_message:
      a->on_transfer();
      return 0;
    case transfer_stopped_message:
      a->transferStopped = true;
      a->finish_shutdown();
      return 0;
    case error_message: {
      std::string err;
      {
        std::lock_guard lock(a->render->mutex);
        err = a->render->error;
      }
      MessageBoxA(hwnd, err.c_str(), "Rendering error", MB_OK | MB_ICONERROR);
      a->close();
      return 0;
    }
    case frame_message:
      return 0;
    case WM_DESTROY:
      PostQuitMessage(0);
      return 0;
    }
  } catch (const std::exception &e) {
    MessageBoxA(hwnd, e.what(), "3DGSDownloader", MB_OK | MB_ICONERROR);
  }
  return DefWindowProcW(hwnd, msg, wp, lp);
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show) {
  SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
  struct ComLifetime {
    ComLifetime() { CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED); }
    ~ComLifetime() { CoUninitialize(); }
  } com;
  try {
    App app;
    fs::path evidence;
    unsigned delay = 0;
    int argc{};
    auto argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    for (int i = 1; i < argc; ++i) {
      std::wstring arg = argv[i];
      if (arg == L"--test-fixture")
        app.fixture = true;
      else if (arg == L"--evidence-dir" && i + 1 < argc)
        evidence = fs::absolute(argv[++i]);
      else if (arg == L"--workspace" && i + 1 < argc)
        app.workspacePath = fs::absolute(argv[++i]);
      else if (arg == L"--plan" && i + 1 < argc)
        app.startupPlan = fs::absolute(argv[++i]);
      else if (arg == L"--render-delay-ms" && i + 1 < argc)
        delay = std::min(5000UL, std::stoul(argv[++i]));
      else if (arg == L"--io-delay-ms" && i + 1 < argc)
        app.ioDelay = std::min(10000UL, std::stoul(argv[++i]));
      else if (arg == L"--transfer-chunk-delay-ms" && i + 1 < argc)
        app.transferChunkDelay = std::min(2000UL, std::stoul(argv[++i]));
    }
    LocalFree(argv);
    if (app.workspacePath.empty()) {
      PWSTR local{};
      if (FAILED(
              SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &local)))
        throw std::runtime_error("Cannot locate workspace directory");
      app.workspacePath = fs::path(local) / L"3DGSDownloader" / L"tasks.json";
      CoTaskMemFree(local);
    }
    // One owner per task-list path, including while the asynchronous writer
    // drains.
    auto identity = app.workspacePath.lexically_normal().wstring();
    CharUpperBuffW(identity.data(), static_cast<DWORD>(identity.size()));
    uint64_t hash = 14695981039346656037ULL;
    for (wchar_t c : identity) {
      hash ^= uint16_t(c);
      hash *= 1099511628211ULL;
    }
    auto mutexName =
        L"Local\\3DGSDownloader.Workspace." + std::to_wstring(hash);
    struct InstanceHandle {
      HANDLE value{};
      ~InstanceHandle() {
        if (value)
          CloseHandle(value);
      }
    } instanceLock;
    instanceLock.value = CreateMutexW(nullptr, FALSE, mutexName.c_str());
    if (!instanceLock.value)
      throw std::runtime_error("Cannot lock task workspace");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
      MessageBoxW(nullptr, L"这个任务列表已在另一窗口打开，请使用已有窗口。",
                  L"3DGSDownloader", MB_OK | MB_ICONINFORMATION);
      return 0;
    }

    WNDCLASSEXW wc{sizeof wc};
    wc.style = CS_DBLCLKS;
    wc.lpfnWndProc = wndproc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = L"3DGSDownloader.Native";
    RegisterClassExW(&wc);
    RECT wr{0, 0, wm::defaultWidth, wm::defaultHeight};
    auto dpi = GetDpiForSystem();
    wr.right = MulDiv(wr.right, dpi, wm::baseDpi);
    wr.bottom = MulDiv(wr.bottom, dpi, wm::baseDpi);
    AdjustWindowRectExForDpi(&wr, WS_OVERLAPPEDWINDOW, FALSE, 0, dpi);
    auto hwnd = CreateWindowExW(
        0, wc.lpszClassName, gsdl::product::windowTitle, WS_OVERLAPPEDWINDOW,
        wm::initialScreenX, wm::initialScreenY, wr.right - wr.left,
        wr.bottom - wr.top, nullptr, nullptr, instance, &app);
    if (!hwnd)
      throw std::runtime_error("CreateWindow failed");
    BOOL dark = TRUE;
    DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark,
                          sizeof dark);
    app.ime = std::make_unique<tx::ImeBinding>(hwnd);
    app.create_menu();
    app.io = std::make_unique<st::IoWorker>(
        [hwnd] { PostMessageW(hwnd, io_message, 0, 0); },
        [hwnd] { PostMessageW(hwnd, io_stopped_message, 0, 0); },
        st::WorkerOptions{8, app.ioDelay});
    app.transfers = std::make_unique<gsdl::transfer::Service>(
        [hwnd] { PostMessageW(hwnd, transfer_message, 0, 0); },
        [hwnd] { PostMessageW(hwnd, transfer_stopped_message, 0, 0); },
        app.transferChunkDelay);
    app.taskList.bind(*app.transfers);
    app.workspaceIo = std::make_unique<st::WorkspaceWorker>(
        [hwnd] { PostMessageW(hwnd, workspace_message, 0, 0); },
        [hwnd] { PostMessageW(hwnd, workspace_stopped_message, 0, 0); });
    app.folderStopped=false;
    app.folderOpener=std::make_unique<gsdl::desktop::FolderOpener>(
        [hwnd]{PostMessageW(hwnd,folder_message,0,0);},
        [hwnd]{PostMessageW(hwnd,folder_stopped_message,0,0);});
    wchar_t exe[32768];
    GetModuleFileNameW(nullptr, exe, 32768);
    app.render = std::make_shared<RenderHost>();
    app.render->hwnd = hwnd;
    app.render->dll = fs::path(exe).parent_path() / L"libSkiaSharp.dll";
    app.render->evidence = evidence;
    app.render->delay = delay;
    app.render->uiThread = GetCurrentThreadId();
    auto owned = new std::shared_ptr<RenderHost>(app.render);
    auto thread = CreateThread(
        nullptr, 0,
        [](void *ptr) -> DWORD {
          std::unique_ptr<std::shared_ptr<RenderHost>> p(
              (std::shared_ptr<RenderHost> *)ptr);
          (*p)->run();
          return 0;
        },
        owned, 0, nullptr);
    if (!thread) {
      delete owned;
      throw std::runtime_error("Cannot start rendering thread");
    }
    CloseHandle(thread);
    app.accessibility = std::make_unique<gsdl::ui::AccessibilityHost>(
        hwnd, accessibility_message);
    app.layout();
    ShowWindow(hwnd, show);
    UpdateWindow(hwnd);
    SetTimer(hwnd, 1, 20, nullptr);
    app.lastTick = GetTickCount64();
    app.initialize_workspace();
    app.publish();
    MSG message;
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
      TranslateMessage(&message);
      DispatchMessageW(&message);
    }
    return 0;
  } catch (const std::exception &e) {
    MessageBoxA(nullptr, e.what(), "3DGSDownloader startup",
                MB_OK | MB_ICONERROR);
    return 1;
  }
}
