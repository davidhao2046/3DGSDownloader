#pragma once
#include "core/application/plan_service.hpp"
#include "core/application/download_preferences.hpp"
#include "core/application/task_list.hpp"
#include "core/application/transfer_service.hpp"
#include "infra/storage/task_workspace.hpp"
#include "render_host.hpp"
#include "task_list_visual.hpp"
#include "creation_visual.hpp"
#include "resources/product.hpp"
#include "folder_opener.hpp"
#include <commctrl.h>
#include <shlobj.h>
#include <shobjidl.h>

#pragma comment(                                                               \
    linker,                                                                    \
    "/manifestdependency:\"type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='amd64' publicKeyToken='6595b64144ccf1df' language='*'\"")
namespace cr = gsdl::theme::creation;
namespace pl = gsdl::theme::planLayout;
namespace st = gsdl::storage;
constexpr UINT workspace_message = WM_APP + 9,
               workspace_stopped_message = WM_APP + 10;
constexpr UINT io_message = WM_APP + 5, io_stopped_message = WM_APP + 6;
constexpr UINT transfer_message = WM_APP + 7,
               transfer_stopped_message = WM_APP + 8;
constexpr UINT folder_message=WM_APP+13,folder_stopped_message=WM_APP+14;
enum Control {
  Link,
  Directory,
  OutputName,
  Included,
  NewPlan,
  OpenPlan,
  SavePlan,
  SaveAs,
  Undo,
  Redo,
  ResourceList,
  ResolveSource,
  DownloadSelected,
  CancelTransfer,
  RevealResult,
  BrowseDirectory,
  BackToList,
  CreateDownload,
  CreateDetails,
  DismissCreate,
  RemoveTask,
  CloseCreate,
  RecentDirectory,
  ControlCount
};
constexpr int row_check_base = 10000;
enum class Intent { none, new_plan, open_plan, close };

struct App {
  HWND hwnd{};
  gsdl::TaskListService taskList;
  std::string selectedTask, expandedTaskId, ioTaskId, commandTaskId;
  enum class Screen { list, create } screen{Screen::list};
  std::wstring newLink, newDirectory, lastDirectory, lastUsedBaseDirectory;
  gsdl::SelectionPreferences selectionPreferences;
  std::unique_ptr<gsdl::desktop::FolderOpener> folderOpener;
  bool folderStopped{true};
  std::string lastOpenedTask;
  std::wstring lastOpenedDirectory;
  void on_folder_open() {
    if(!folderOpener)return;
    for(const auto &result:folderOpener->take_results()) {
      if(render)render->event("{\"event\":\"folder_open\",\"taskId\":\""+result.taskId+"\",\"path\":"+json(result.path.wstring())+",\"success\":"+(result.success()?"true":"false")+"}");
      const auto owner=taskList.find(result.taskId);
      if(!owner || fs::path(owner->plans.snapshot()->plan.outputDirectory)!=result.path)continue;
      if(result.success()) {lastOpenedTask=result.taskId;lastOpenedDirectory=result.path.wstring();status=L"已打开文件夹："+lastOpenedDirectory;}
      else status=result.error+L"；下载记录保留，请检查原保存位置："+result.path.wstring();
    }
    publish();
  }
  std::vector<std::wstring> recentDirectories;
  gsdl::TaskFilter taskFilter{gsdl::TaskFilter::All};
  std::size_t firstTaskRow{};
  int modalReturnFocus=taskui::NewTask;
  std::optional<std::size_t> expansionAnchor;
  bool creationStopped{true}, creationExpanded{};
  std::size_t creationFirstFile{};
  creationui::Model creation_model() const;
  creationui::Layout creation_layout() const {return creationui::layout(width/dpi,height/dpi,creation_model());}
  gsdl::PlanService creationPlan;
  std::unique_ptr<gsdl::transfer::Service> creationWorker;
  void resolve_creation();
  void on_creation();
  std::unique_ptr<st::WorkspaceWorker> workspaceIo;
  fs::path workspacePath;
  bool workspaceLoading{}, workspaceStopped{}, workspaceSaving{},
      workspacePending{}, workspaceFailed{};
  uint64_t workspaceGeneration{}, workspaceInFlightGeneration{};
  std::string workspaceStamp;
  gsdl::TaskEntry *current() { return taskList.find(selectedTask); }
  const gsdl::TaskEntry *current() const { return taskList.find(selectedTask); }
  const gsdl::TaskEntry *expanded_entry() const { return taskList.find(expandedTaskId); }
  const gsdl::TaskEntry *task_at(int index) const {
    if(index<0)return nullptr;
    for(const auto &entry:taskList.entries()) if(matches(*entry,taskFilter) && index--==0)return entry.get();
    return nullptr;
  }
  const gsdl::TaskEntry *reveal_target() const {return commandTaskId.empty()?current():taskList.find(commandTaskId);}
  bool can_reveal(const gsdl::TaskEntry *entry) const {
    if(!entry || input_locked() || screen==Screen::create)return false;
    const auto p=entry->plans.snapshot();
    return !p->plan.outputDirectory.empty() && std::any_of(p->plan.resources.begin(),p->plan.resources.end(),[&](const auto &r){return saved_resource(*entry,r);});
  }
  gsdl::PlanService &plans() { return current()->plans; }
  const gsdl::PlanService &plans() const { return current()->plans; }
  void new_task(bool paste = false);
  void create_task(bool start);
  void browse_directory(bool defaults=false);
  void directory_menu(bool settings);
  void remember_directory(std::wstring);
  fs::path creation_output() const;
  static bool matches(const gsdl::TaskEntry &e,gsdl::TaskFilter filter) {
    const bool missing=std::any_of(e.report.files.begin(),e.report.files.end(),[](const auto &f){return f.localState!=gsdl::transfer::LocalState::available;});
    if(missing && e.state==gsdl::TaskState::Succeeded) return filter==gsdl::TaskFilter::All || filter==gsdl::TaskFilter::Attention;
    return gsdl::task_matches(e.state,filter);
  }
  bool saved_resource(const gsdl::TaskEntry &owner,const gsdl::Resource &r) const {
    const auto p=owner.plans.snapshot();
    return std::any_of(owner.report.files.begin(),owner.report.files.end(),[&](const auto &f){return f.resourceId==r.id && f.path==fs::path(p->plan.outputDirectory)/r.outputName && f.localState==gsdl::transfer::LocalState::available;});
  }
  bool saved_resource(const gsdl::Resource &r) const {
    return current() && saved_resource(*current(),r);
  }
  void show_list();
  void toggle_expanded();
  void select_task(int index, bool details = false);
  bool select_expanded_owner();
  taskui::TaskListVisualModel list_model() const;
  taskui::TaskListLayout list_layout() const;
  void publish_shell();
  void publish_create();
  void persist_workspace();
  void on_workspace();
  void initialize_workspace();
  void shell_accessibility(const taskui::TaskListLayout &);
  std::wstring read_clipboard() const;

  gsdl::WorkspaceStore workspace;
  gsdl::SelectionStore selection;
  gsdl::TaskCoordinator tasks;
  gsdl::ActionRegistry actions;
  std::array<tx::Editor, 3> editors;
  tx::Rasterizer metrics;
  std::shared_ptr<RenderHost> render;
  std::unique_ptr<st::IoWorker> io;
  std::unique_ptr<gsdl::transfer::Service> transfers;
  std::shared_ptr<const gsdl::sources::ResolvedSource> resolved;
  gsdl::transfer::View transferView;
  std::map<std::string, gsdl::transfer::FileReceipt> completedFiles;
  fs::path lastResult;
  std::string lastNetworkReport{"null"};
  bool transferStopped{}, closeAfterTransfer{};
  std::unique_ptr<gsdl::ui::AccessibilityHost> accessibility;
  std::unique_ptr<tx::ImeBinding> ime;
  std::array<g::Rect, ControlCount> bounds{};
  std::array<float, 3> scroll{};
  int focus = -1, pressed = -1, hover = -1;
  bool selecting{}, active = true, closing{}, renderStopped{}, ioStopped{},
                    fixture{}, opening{};
  wchar_t highSurrogate{};
  std::string editResourceId, commandResourceId;
  Intent deferred = Intent::none;
  std::optional<fs::path> deferredOpen;
  fs::path startupPlan;
  unsigned ioDelay{}, transferChunkDelay{}, width = wm::defaultWidth,
                                            height = wm::defaultHeight;
  float dpi = 1;
  uint64_t serial{}, heartbeat{}, lastTick = GetTickCount64(), maxGap{},
                                  commits{}, imeResults{};
  std::wstring status = L"粘贴公开链接或新建下载";
  std::wstring titleCache;
  HMENU menu{};
  bool text_focus() const { return focus >= Link && focus <= OutputName; }
  bool network_busy() const { return taskList.busy(); }
  bool downloading() const {
    return current() && taskList.locked(selectedTask);
  }
  bool source_ready() const {
    return current() && !snapshot()->plan.resources.empty();
  }
  bool input_locked() const {
    return closing || closeAfterTransfer || deferred != Intent::none ||
           opening || workspaceLoading;
  }
  gsdl::Snapshot snapshot() const {
    if (current())
      return plans().snapshot();
    static const gsdl::Snapshot empty = gsdl::PlanService{}.snapshot();
    return empty;
  }
  const gsdl::Resource *selected(const gsdl::DownloadPlan &p) const {
    auto it = std::find_if(
        p.resources.begin(), p.resources.end(),
        [&](const auto &r) { return r.id == selection.resourceId; });
    return it == p.resources.end() ? nullptr : &*it;
  }
  int selected_row(const gsdl::DownloadPlan &p) const {
    for (std::size_t i = 0; i < p.resources.size(); ++i)
      if (p.resources[i].id == selection.resourceId)
        return static_cast<int>(i);
    return -1;
  }
  std::wstring field_value(int field) const {
    if (screen == Screen::create)
      return field == Link ? newLink : newDirectory;
    auto s = snapshot();
    if (field == Link)
      return s->plan.source.shareUrl;
    if (field == Directory)
      return s->plan.outputDirectory;
    if (auto r = selected(s->plan))
      return r->outputName;
    return {};
  }
  std::string_view parameter_id(int field) const {
    return field == Link        ? "source.shareUrl"
           : field == Directory ? "outputDirectory"
                                : "resource.outputName";
  }
  std::string_view action_id(int id) const {
    switch (id) {
    case taskui::Settings: return "settings.directory";
    case taskui::ViewAdded: return "task.viewAdded";
    case RecentDirectory: return "directory.recent";
    case RemoveTask:
      return "task.remove";
    case BrowseDirectory:
      return "directory.browse";
    case BackToList:
      return "task.list";
    case CreateDownload:
      return "task.createStart";
    case CreateDetails:
      return "task.createDetails";
    case CloseCreate:
    case DismissCreate:
      return "task.dismiss";
    case taskui::NewTask:
      return "file.newPlan";
    case taskui::PasteLink:
      return "task.paste";
    case taskui::Details:
      return "task.details";
    case taskui::Start:
      return "download.start";
    case taskui::Cancel:
      return "download.cancel";
    case taskui::Reveal:
      return "result.reveal";
    case taskui::FilterAll:
      return "task.all";
    case taskui::FilterActive:
      return "task.active";
    case taskui::FilterCompleted:
      return "task.completed";
    case taskui::FilterOther:
      return "task.other";
    case NewPlan:
      return "file.newPlan";
    case OpenPlan:
      return "file.openPlan";
    case SavePlan:
      return "file.savePlan";
    case SaveAs:
      return "file.savePlanAs";
    case Undo:
      return "edit.undo";
    case Redo:
      return "edit.redo";
    case Included:
      return "plan.setIncluded";
    case ResolveSource:
      return "source.resolve";
    case DownloadSelected:
      return "download.start";
    case CancelTransfer:
      return "download.cancel";
    case RevealResult:
      return "result.reveal";
    default:
      return "";
    }
  }
  App() {
    auto add = [&](
                   std::string id, std::wstring label, std::wstring reason,
                   std::function<bool()> enabled, std::function<void()> invoke,
                   std::function<bool()> checked = [] { return false; }) {
      actions.add({std::move(id), std::move(label), L"document",
                   std::move(reason), std::move(enabled), std::move(checked),
                   std::move(invoke)});
    };
    add(
        "file.newPlan", L"新建下载", L"请先完成当前输入",
        [&] { return !input_locked(); }, [&] { new_task(); });
    add(
        "file.openPlan", L"打开", L"请先等待当前文件操作完成",
        [&] { return !input_locked() && !tasks.busy(); },
        [&] { request_intent(Intent::open_plan); });
    add(
        "file.savePlan", L"保存", L"正在保存或打开计划",
        [&] {
          return current() && screen != Screen::create && !input_locked() &&
                 !tasks.busy();
        },
        [&] { save(false); });
    add(
        "file.savePlanAs", L"另存", L"正在保存或打开计划",
        [&] {
          return current() && screen != Screen::create && !input_locked() &&
                 !tasks.busy();
        },
        [&] { save(true); });
    add(
        "edit.undo", L"撤销", L"没有可撤销的参数编辑",
        [&] {
          return current() && screen != Screen::create && !input_locked() &&
                 !downloading() && plans().undo_count() > 0;
        },
        [&] { history(false); });
    add(
        "edit.redo", L"重做", L"没有可重做的参数编辑",
        [&] {
          return current() && screen != Screen::create && !input_locked() &&
                 !downloading() && plans().redo_count() > 0;
        },
        [&] { history(true); });
    add(
        "plan.setIncluded", L"参与下载", L"请先选择资源",
        [&] {
          auto s = snapshot();
          return screen != Screen::create && !input_locked() &&
                 !downloading() &&
                 (!commandResourceId.empty() || selected(s->plan));
        },
        [&] { toggle_included(); },
        [&] {
          auto s = snapshot();
          auto r = selected(s->plan);
          return r && r->included;
        });
    add(
        "source.resolve", L"解析", L"请先填写公开链接并等待当前任务结束",
        [&] {
          return current() && screen != Screen::create && !input_locked() &&
                 !downloading() && !snapshot()->plan.internalFixture;
        },
        [&] { start_resolve(); });
    add(
        "download.start", L"下载所选",
        L"请重新解析资源、设置保存目录并勾选文件",
        [&] {
          return current() && screen != Screen::create && !input_locked() &&
                 !downloading() && !snapshot()->plan.source.shareUrl.empty() &&
                 !snapshot()->plan.outputDirectory.empty() &&
                 (snapshot()->plan.resources.empty() || std::any_of(snapshot()->plan.resources.begin(), snapshot()->plan.resources.end(), [&](const auto &r) { return r.included && !saved_resource(r); }));
        },
        [&] { start_download(); });
    add(
        "download.cancel", L"取消任务", L"没有可取消的任务",
        [&] {
          return current() && screen != Screen::create && !closing &&
                 downloading() &&
                 current()->state != gsdl::TaskState::Cancelling;
        },
        [&] {
          taskList.cancel(selectedTask);
          persist_workspace();
          status = L"已请求取消；等待当前网络或文件操作结束";
          trace("cancel_requested");
          publish();
        });
    add(
        "result.reveal", L"打开结果目录", L"本次运行没有已完成文件",
        [&] {return folderOpener && can_reveal(reveal_target());},
        [&] {
          const auto target=reveal_target();
          if(!target)return;
          const auto path=target->plans.snapshot()->plan.outputDirectory;
          status=folderOpener->open(target->id,path)?L"正在打开文件夹："+path:L"打开文件夹请求未提交；请稍后重试";
          publish();
        });
    add(
        "task.remove", L"移除任务记录", L"请先等待任务停止",
        [&] {
          return current() && !input_locked() && !downloading() &&
                 !tasks.busy();
        },
        [&] {
          if (MessageBoxW(
                  hwnd, L"只从列表移除这条任务记录，已经下载的文件会保留。",
                  L"移除任务记录", MB_OKCANCEL | MB_ICONQUESTION) != IDOK)
            return;
          auto r = taskList.remove(selectedTask);
          if (r) {
            if(expandedTaskId==selectedTask) {expandedTaskId.clear();expansionAnchor.reset();}
            selectedTask.clear();
            selection.clear();
            firstTaskRow = 0;
            show_list();
            persist_workspace();
            status = L"已移除任务记录，下载文件保留";
          } else
            status = r.error;
          publish();
        });
    add("settings.directory",L"目录设置",L"",[&]{return !input_locked();},[&]{directory_menu(true);});
    add("directory.recent",L"最近",L"暂无最近目录",[&]{return !recentDirectories.empty();},[&]{directory_menu(false);});
    add("task.viewAdded",L"查看任务",L"",[&]{return current()!=nullptr;},[&]{taskFilter=gsdl::TaskFilter::All; firstTaskRow=0; publish();});
    add(
        "directory.browse", L"浏览…", L"请等待任务停止",
        [&] {
          return !input_locked() &&
                 (screen == Screen::create || (current() && !downloading()));
        },
        [&] { browse_directory(); });
    add(
        "task.list", L"返回列表", L"请处理当前草稿",
        [&] { return !input_locked(); },
        [&] {
          if (commit_focus())
            show_list();
        });
    add(
        "task.createStart", L"开始下载", L"请等待资源解析并至少勾选一个文件", [&] {
          auto p=creationPlan.snapshot();
          return !input_locked() && (!creationWorker || !creationWorker->view().busy) && std::any_of(p->plan.resources.begin(),p->plan.resources.end(),[](const auto &r){return r.included;});
        },
        [&] { create_task(true); });
    add(
        "task.createDetails", L"选择资源…", L"",
        [&] { return !input_locked(); }, [&] { creationExpanded = !creationExpanded; if(!creationWorker || !creationWorker->view().busy) resolve_creation(); layout(); publish(); });
    add(
        "task.dismiss", L"取消", L"", [&] { return !input_locked(); },
        [&] {
          for (auto &e : editors)
            e.cancel();
          show_list();
        });
    add(
        "task.paste", L"粘贴链接", L"", [&] { return !input_locked(); },
        [&] { new_task(true); });
    add(
        "task.details", L"展开 / 收起文件", L"请先选择任务",
        [&] { return current() && !input_locked(); },
        [&] {
          toggle_expanded();
        });
    auto filterAction = [&](std::string id, std::wstring name,
                            gsdl::TaskFilter filter) {
      add(
          id, name, L"", [&] { return !input_locked(); },
          [this, filter] {
            taskFilter = filter;
            firstTaskRow = 0;
            expansionAnchor.reset();
            selectedTask.clear();
            leave_editor();
            focus = taskui::FilterAll + int(filter);
            status = L"选择任务后展开文件；任务位置可通过打开文件夹查看";
            publish();
          });
    };
    filterAction("task.all", L"全部任务", gsdl::TaskFilter::All);
    filterAction("task.active", L"进行中", gsdl::TaskFilter::Active);
    filterAction("task.completed", L"已完成", gsdl::TaskFilter::Completed);
    filterAction("task.other", L"未完成", gsdl::TaskFilter::Attention);
  }
  void start_resolve() {
    if (!commit_focus() || !current())
      return;
    leave_editor();
    auto r = taskList.resolve(selectedTask);
    status = r ? L"已加入解析队列" : r.error;
    persist_workspace();
    publish();
  }
  void start_download() {
    if (!commit_focus() || !current())
      return;
    leave_editor();
    auto r = taskList.enqueue(selectedTask);
    status = r ? L"已加入下载队列" : r.error;
    persist_workspace();
    publish();
  }
  void on_transfer() {
    auto changed = taskList.poll();
    transferView = transfers->view();
    if (current()) {
      completedFiles.clear();
      for (auto &f : current()->report.files)
        completedFiles[f.resourceId] = f;
      if (screen != Screen::create && changed) {
        status = current()->error.empty()
                     ? std::wstring(gsdl::task_state_label(current()->state))
                     : current()->error;
        if (current()->state == gsdl::TaskState::Succeeded)
          status += L"；原文件已校验并保存";
      }
      sanitize_selection();
    }
    if (changed)
      persist_workspace();
    if (closeAfterTransfer && !network_busy()) {
      closeAfterTransfer = false;
      request_intent(Intent::close);
    }
    publish();
  }
  void create_menu() {
    menu = CreateMenu();
    auto file = CreatePopupMenu(), edit = CreatePopupMenu();
    AppendMenuW(file, MF_STRING, 100 + NewPlan, L"新建计划\tCtrl+N");
    AppendMenuW(file, MF_STRING, 100 + OpenPlan, L"打开计划…\tCtrl+O");
    AppendMenuW(file, MF_STRING, 100 + SavePlan, L"保存计划\tCtrl+S");
    AppendMenuW(file, MF_STRING, 100 + SaveAs, L"计划另存为…\tCtrl+Shift+S");
    AppendMenuW(file, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(file, MF_STRING, 200, L"退出\tAlt+F4");
    AppendMenuW(edit, MF_STRING, 100 + Undo, L"撤销参数编辑\tCtrl+Z");
    AppendMenuW(edit, MF_STRING, 100 + Redo, L"重做参数编辑\tCtrl+Y");
    auto sourceMenu = CreatePopupMenu();
    for (auto id :
         {ResolveSource, DownloadSelected, CancelTransfer, RevealResult})
      AppendMenuW(sourceMenu, MF_STRING, 100 + id,
                  actions.query(action_id(id))->label.c_str());
    AppendMenuW(menu, MF_POPUP, (UINT_PTR)file, L"文件");
    AppendMenuW(menu, MF_POPUP, (UINT_PTR)edit, L"编辑");
    AppendMenuW(menu, MF_POPUP, (UINT_PTR)sourceMenu, L"资源");
  }
  void update_menu() {
    if (!menu)
      return;
    for (int i = NewPlan; i <= Redo; ++i)
      EnableMenuItem(menu, 100 + i,
                     MF_BYCOMMAND | (enabled(i) ? MF_ENABLED : MF_GRAYED));
    for (auto id :
         {ResolveSource, DownloadSelected, CancelTransfer, RevealResult})
      EnableMenuItem(menu, 100 + id,
                     MF_BYCOMMAND | (enabled(id) ? MF_ENABLED : MF_GRAYED));
  }
  void layout() {
    RECT r{};
    GetClientRect(hwnd, &r);
    width = r.right;
    height = r.bottom;
    dpi = GetDpiForWindow(hwnd) / float(wm::baseDpi);
    workspace.dpi = dpi;
    bounds.fill({});
    if (screen == Screen::create) {
      const auto l=creation_layout();
      const std::array<int,creationui::partCount> ids{Link,Directory,BrowseDirectory,RecentDirectory,CreateDetails,DismissCreate,CreateDownload,CloseCreate};
      for(std::size_t i=0;i<ids.size();++i) bounds[ids[i]]=l.parts[i];
      return;
    }
  }
  std::size_t visible_rows() const {
    return (std::size_t)std::max(
        0.f, (bounds[ResourceList].bottom - pl::firstRow) / th::row);
  }
  void clamp_scroll() {
    auto n = snapshot()->plan.resources.size(), count = visible_rows();
    workspace.firstResourceRow =
        std::min(workspace.firstResourceRow, n > count ? n - count : 0);
  }
  bool inside(g::Rect r, float x, float y) const {
    return x >= r.left && x < r.right && y >= r.top && y < r.bottom;
  }
  int row_at(float x, float y) const {
    if (!inside(bounds[ResourceList], x, y) || y < pl::firstRow)
      return -1;
    auto row = workspace.firstResourceRow +
               (std::size_t)((y - pl::firstRow) / th::row);
    return row < snapshot()->plan.resources.size() ? static_cast<int>(row) : -1;
  }
  int hit(float x, float y) const {
    if(screen == Screen::create && creationExpanded) {
      const auto l=creation_layout();
      for(std::size_t i=l.first;i<l.first+l.visible;++i)
        if(inside(l.file(i),x,y)) return taskui::FileBase+int(i);
    }
    if (screen == Screen::list) {
      return list_layout().hit(x,y);
    }
    for (int i = 0; i < ControlCount; ++i)
      if (inside(bounds[i], x, y))
        return i;
    return -1;
  }
  bool enabled(int id) const {
    if (input_locked())
      return false;
    if(screen==Screen::list && id>=taskui::ExpandBase && id<taskui::FileBase) return true;
    if(screen==Screen::list && id>=taskui::RevealBase && id<taskui::RowBase) return can_reveal(task_at(id-taskui::RevealBase));
    if(screen == Screen::create && id >= taskui::FileBase && id < taskui::RevealBase) return true;
    if (screen == Screen::list && id >= taskui::FileBase && id < taskui::RevealBase) {
      const auto n=std::size_t(id-taskui::FileBase);
      const auto owner=expanded_entry();
      if(!owner || taskList.locked(owner->id)) return false;
      const auto p=owner->plans.snapshot();
      return n<p->plan.resources.size() && !saved_resource(*owner,p->plan.resources[n]);
    }
    if (screen == Screen::list && id >= taskui::RowBase)
      return !input_locked();
    if (id < 0)
      return false;
    if (screen == Screen::list &&
        (id == Link || id == Directory || id == OutputName || id == Included ||
         id == ResourceList))
      return false;
    if (screen == Screen::create && id != Link && id != Directory &&
        id != BrowseDirectory && id != CreateDownload && id != CreateDetails &&
        id != DismissCreate && id != CloseCreate && id != RecentDirectory)
      return false;
    if (screen == Screen::create && id >= 0 && id <= OutputName)
      return id == Link || id == Directory;
    if (input_locked())
      return false;
    if (downloading() &&
        (id == Link || id == Directory || id == OutputName || id == Included))
      return false;
    if (id == Link && snapshot()->plan.internalFixture)
      return false;
    if (id == OutputName || id == Included) {
      auto s = snapshot();
      if (!selected(s->plan))
        return false;
    }
    auto key = action_id(id);
    if (!key.empty()) {
      auto state = actions.query(key);
      return state && state->enabled;
    }
    return id >= 0 && id < ControlCount;
  }
  void trace(const char *event) {
    if (!render)
      return;
    if (!current())
      return;
    auto s = snapshot();
    std::ostringstream o;
    o << "{\"event\":\"" << event << "\",\"tick\":" << GetTickCount64()
      << ",\"session_id\":" << s->sessionId << ",\"revision\":" << s->revision
      << ",\"input_revision\":" << s->inputRevision
      << ",\"dirty\":" << (plans().dirty() ? "true" : "false")
      << ",\"focus\":" << focus << ",\"undo_count\":" << plans().undo_count()
      << ",\"redo_count\":" << plans().redo_count()
      << ",\"pressed\":" << pressed << "}";
    render->event(o.str());
  }
  void sync_ime() {
    if (ime)
      ime->set_text_focus(text_focus(),
                          text_focus() && editors[focus].composing());
  }
  void begin_editor() {
    if (text_focus()) {
      editors[focus].begin(field_value(focus));
      if (focus == OutputName)
        editResourceId = selection.resourceId;
      scroll[focus] = 0;
    }
  }
  bool commit_focus() {
    if (!text_focus() || !editors[focus].active())
      return true;
    auto &e = editors[focus];
    if (e.composing())
      return false;
    gsdl::Result result;
    bool ok = e.commit([&](auto value) {
      if (screen == Screen::create) {
        if(focus==Link) {gsdl::Source parsed;result=gsdl::canonical_share_url(value,parsed);}
        else result=gsdl::validate_output_directory(value);
        if(!result) return false;
        (focus == Link ? newLink : newDirectory) = value;
        result = {true, true, {}};
        return true;
      }
      result = plans().edit(
          parameter_id(focus),
          focus == OutputName ? editResourceId : std::string{}, value);
      return result.success;
    });
    if (!ok) {
      status = result.error + L"；草稿已保留，可修正或按 Esc 放弃";
      trace("invalid_retained");
      return false;
    }
    if (result.changed) {
      ++commits;
      status = screen == Screen::create ? L"点击开始下载，或先选择需要的资源"
                                        : L"参数已更新，任务列表会自动保存";
      sanitize_selection();
      trace("commit");
      if (screen != Screen::create)
        plan_edited();
      else if(focus == Link) resolve_creation();
    }
    return true;
  }
  void leave_editor() {
    focus = -1;
    highSurrogate = 0;
    sync_ime();
  }
  bool set_focus(int next) {
    if (next >= 0 && !enabled(next))
      return false;
    if (next == focus && text_focus() && editors[next].active())
      return true;
    if (!commit_focus())
      return false;
    if (next >= 0 && !enabled(next)) {
      leave_editor();
      publish();
      return false;
    }
    focus = next;
    highSurrogate = 0;
    begin_editor();
    sync_ime();
    trace("focus");
    publish();
    return true;
  }
  void sanitize_selection() {
    auto s = snapshot();
    if (!selected(s->plan))
      selection.clear();
    clamp_scroll();
  }
  void select_row(int row) {
    if (!commit_focus())
      return;
    if (row < 0 || row >= (int)snapshot()->plan.resources.size())
      return;
    selection.resourceId = snapshot()->plan.resources[row].id;
    selection.selectedIds = {selection.resourceId};
    focus = ResourceList;
    sync_ime();
    trace("select");
    publish();
  }
  void plan_edited() {
    // Plan edits do not rewrite the last execution result or file facts.
    persist_workspace();
  }
  void history(bool redo) {
    if (!commit_focus())
      return;
    leave_editor();
    auto result = redo ? plans().redo() : plans().undo();
    if (result) {
      sanitize_selection();
      status = redo ? L"已重做参数编辑" : L"已撤销参数编辑";
      trace(redo ? "redo" : "undo");
      plan_edited();
    } else
      status = result.error;
    publish();
  }
  void toggle_included() {
    if (!commit_focus())
      return;
    auto s = snapshot();
    std::string id =
        commandResourceId.empty() ? selection.resourceId : commandResourceId;
    auto it = std::find_if(s->plan.resources.begin(), s->plan.resources.end(),
                           [&](const auto &r) { return r.id == id; });
    if (it == s->plan.resources.end())
      return;
    std::array<std::string, 1> ids{id};
    auto result = plans().setIncluded(ids, !it->included);
    if (result.changed) {
      ++commits;
      status = L"下载勾选已更新，任务列表会自动保存";
      trace("included");
      plan_edited();
    } else if (!result)
      status = result.error;
    publish();
  }
  void activate(int id) {
    if (input_locked())
      return;
    if(screen==Screen::list && id>=taskui::RevealBase && id<taskui::RowBase) {
      if(const auto entry=task_at(id-taskui::RevealBase);can_reveal(entry)) {
        commandTaskId=entry->id;
        actions.invoke("result.reveal");
        commandTaskId.clear();
      }
      return;
    }
    if(screen==Screen::list && id>=taskui::ExpandBase && id<taskui::FileBase) {
      select_task(id-taskui::ExpandBase,true);return;
    }
    if (screen == Screen::list && id >= taskui::RowBase) {
      select_task(id - taskui::RowBase, true);
      return;
    }
    if (screen == Screen::list && id >= taskui::FileBase && id < taskui::RevealBase) {
      // File controls remain attached to the expanded task even when a different
      // task row is selected. Route the existing action through that owner.
      if(!enabled(id) || !select_expanded_owner()) return;
      const auto n = std::size_t(id-taskui::FileBase);
      auto s = snapshot();
      if(n < s->plan.resources.size() && !downloading() && !saved_resource(s->plan.resources[n])) {
        focus=id;
        commandResourceId = s->plan.resources[n].id;
        actions.invoke("plan.setIncluded");
        commandResourceId.clear();
      }
      return;
    }
    if(screen == Screen::create && id >= taskui::FileBase && id < taskui::RevealBase) {
      const auto n=std::size_t(id-taskui::FileBase);
      auto p=creationPlan.snapshot();
      if(n<p->plan.resources.size()) creationPlan.edit("resource.included",p->plan.resources[n].id,p->plan.resources[n].included?L"false":L"true");
      publish(); return;
    }
    auto key = action_id(id);
    if (!key.empty())
      actions.invoke(key);
    publish();
  }
  std::optional<fs::path> choose_path(bool saving) {
    IFileDialog *dialog = nullptr;
    HRESULT hr =
        saving ? CoCreateInstance(CLSID_FileSaveDialog, nullptr,
                                  CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog))
               : CoCreateInstance(CLSID_FileOpenDialog, nullptr,
                                  CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog));
    if (FAILED(hr)) {
      status = L"无法打开系统文件选择器；当前计划未改变";
      return {};
    }
    COMDLG_FILTERSPEC types[] = {{L"3DGSDownloader 计划", L"*.3dgsdl.json"},
                                 {L"JSON 文件", L"*.json"}};
    dialog->SetFileTypes(2, types);
    dialog->SetDefaultExtension(L"3dgsdl.json");
    DWORD options{};
    dialog->GetOptions(&options);
    dialog->SetOptions(options | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST |
                       (saving ? FOS_OVERWRITEPROMPT : FOS_FILEMUSTEXIST));
    dialog->SetTitle(saving ? L"保存下载计划" : L"打开下载计划");
    if (saving)
      dialog->SetFileName(plans().target().empty()
                              ? L"下载计划.3dgsdl.json"
                              : fs::path(plans().target()).filename().c_str());
    std::optional<fs::path> path;
    if (SUCCEEDED(dialog->Show(hwnd))) {
      IShellItem *item = nullptr;
      if (SUCCEEDED(dialog->GetResult(&item))) {
        PWSTR name = nullptr;
        if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &name))) {
          path = fs::path(name);
          CoTaskMemFree(name);
        }
        item->Release();
      }
    }
    dialog->Release();
    return path;
  }
  int dirty_prompt() {
    TASKDIALOG_BUTTON buttons[] = {
        {100, L"保存计划"}, {101, L"不保存计划"}, {IDCANCEL, L"返回编辑"}};
    TASKDIALOGCONFIG cfg{};
    cfg.cbSize = sizeof cfg;
    cfg.hwndParent = hwnd;
    cfg.dwFlags =
        TDF_ALLOW_DIALOG_CANCELLATION | TDF_POSITION_RELATIVE_TO_WINDOW;
    cfg.pszWindowTitle = L"3DGSDownloader";
    cfg.pszMainInstruction = L"当前下载计划有未保存的修改";
    cfg.pszContent =
        L"保存计划可保留输出目录、文件名和下载勾选。已有资源文件不会被删除。";
    cfg.cButtons = 3;
    cfg.pButtons = buttons;
    cfg.nDefaultButton = 100;
    int button = IDCANCEL;
    if (FAILED(TaskDialogIndirect(&cfg, &button, nullptr, nullptr)))
      return IDCANCEL;
    return button;
  }
  void request_intent(Intent intent) {
    if (closing)
      return;
    if (intent == Intent::new_plan) {
      new_task();
      return;
    }
    if (intent == Intent::open_plan) {
      if (!commit_focus())
        return;
      leave_editor();
      if (auto path = choose_path(false))
        open_path(*path);
      return;
    }
    if (intent != Intent::close)
      return;
    if (workspaceLoading) {
      deferred = Intent::close;
      status = L"正在读取任务列表，完成后关闭…";
      publish();
      return;
    }
    if (screen == Screen::create) {
      for (auto &e : editors)
        e.cancel();
      show_list();
    }
    if (!commit_focus()) {
      publish();
      return;
    }
    leave_editor();
    if (network_busy()) {
      if (MessageBoxW(
              hwnd,
              L"关闭会取消正在执行及排队的任务。已完成文件和任务列表会保留。",
              L"关闭下载器", MB_OKCANCEL | MB_ICONQUESTION) != IDOK)
        return;
      closeAfterTransfer = true;
      taskList.stopScheduling();
      persist_workspace();
      status = L"正在停止任务并保存列表…";
      publish();
      return;
    }
    if (tasks.busy()) {
      deferred = Intent::close;
      publish();
      return;
    }
    persist_workspace();
    if (workspaceSaving || workspacePending) {
      deferred = Intent::close;
      status = L"正在保存任务列表…";
      publish();
      return;
    }
    if (workspaceFailed) {
      status = L"任务列表保存失败，请检查保存位置后重试关闭";
      publish();
      return;
    }
    shutdown();
  }
  bool save(bool as) {
    if (!current() || closing || tasks.busy() || !commit_focus())
      return false;
    leave_editor();
    std::optional<fs::path> path;
    if (as || plans().target().empty())
      path = choose_path(true);
    else
      path = fs::path(plans().target());
    if (!path) {
      status = L"已取消保存，当前编辑保留";
      return false;
    }
    ioTaskId = selectedTask;
    auto capture = plans().captureSave(path->wstring());
    auto task = tasks.begin(capture.sessionId, L"保存计划");
    if (!task) {
      plans().rejectSave(capture);
      return false;
    }
    if (!io->submit(st::SaveRequest{capture, *task,
                                    st::PublishMode::replace_or_create})) {
      plans().rejectSave(capture);
      tasks.finish(*task, false, L"文件队列不可用");
      status = L"保存未开始，当前编辑已保留";
      return false;
    }
    tasks.running(*task);
    status = L"正在保存计划快照…";
    trace("save_requested");
    publish();
    return true;
  }
  void open_path(const fs::path &path) {
    if (tasks.busy() || closing)
      return;
    ioTaskId = selectedTask;
    auto s = snapshot();
    auto task = tasks.begin(s->sessionId, L"打开计划");
    if (!task)
      return;
    if (!io->submit(st::ReadRequest{s->sessionId, *task, s->revision, path})) {
      tasks.finish(*task, false, L"文件队列不可用");
      status = L"打开未开始，当前计划保持";
      return;
    }
    opening = true;
    tasks.running(*task);
    status = L"正在打开计划；完成前保留当前内容";
    trace("open_requested");
    publish();
  }
  void on_io() {
    for (auto &result : io->take_results()) {
      const auto pending = tasks.snapshot();
      if (result.requestId != pending.taskId ||
          result.sessionId != pending.sessionId) {
        trace("stale_io");
        continue;
      }
      opening = false;
      bool ok = result.success();
      std::wstring error = result.error ? result.error->message : L"";
      if (result.operation == st::Operation::read && ok) {
        std::string id;
        auto r = taskList.addPlan(std::move(*result.plan),
                                  result.path.wstring(), id);
        ok = r.success;
        error = r.error;
        if (ok) {
          selectedTask = id;
          selection.clear();
          leave_editor();
          screen = Screen::list; expandedTaskId=id;expansionAnchor.reset();
          layout();
          status = L"已添加计划；开始下载时重新解析链接";
        }
      } else if (result.operation == st::Operation::save) {
        if (auto entry = taskList.find(ioTaskId)) {
          auto r = ok ? entry->plans.acceptSaved(result.capture)
                      : entry->plans.rejectSave(result.capture);
          if (ok) {
            ok = r.success;
            error = r.error;
            status = L"计划快照已保存";
          }
        }
      }
      tasks.finish(result.requestId, ok, error);
      if (!ok) {
        deferred = Intent::none;
        status = error + L"；原内容已保留，可重试";
      }
      persist_workspace();
      if (deferred == Intent::close && ok) {
        deferred = Intent::none;
        request_intent(Intent::close);
      }
    }
    publish();
  }
  void shutdown() {
    if (closing)
      return;
    closing = true;
    cancel_capture();
    KillTimer(hwnd, 1);
    trace("close_requested");
    publish();
    io->request_stop();
    taskList.stopScheduling();
    transfers->stop();
    if (creationWorker) creationWorker->stop();
    if (workspaceIo)
      workspaceIo->request_stop();
    else
      workspaceStopped = true;
    if(folderOpener)folderOpener->request_stop();
    render->shutdown();
  }
  void close(bool failed = false) {
    if (failed) {
      deferred = Intent::none;
      shutdown();
    } else
      request_intent(Intent::close);
  }
  void finish_shutdown() {
    if (!closing || !ioStopped || !renderStopped || !transferStopped ||
        !workspaceStopped || !creationStopped || !folderStopped)
      return;
    if (accessibility)
      accessibility->disconnect();
    ime.reset();
    if (menu) {
      DestroyMenu(menu);
      menu = nullptr;
    }
    DestroyWindow(hwnd);
  }
  void ime_position() {
    if (text_focus()) {
      auto &e = editors[focus];
      auto x = metrics.caret_x(e.display_text(), th::body, e.display_caret());
      tx::position_ime(
          hwnd,
          (int)((bounds[focus].left + ly::fieldTextLeftInset + x -
                 scroll[focus]) *
                dpi),
          (int)((bounds[focus].top + ly::caretVerticalInset) * dpi),
          (int)(ly::imeLineHeight * dpi));
    }
  }
  void publish();
  void cancel_capture() {
    pressed = -1;
    selecting = false;
    if (GetCapture() == hwnd)
      ReleaseCapture();
    trace("capture_cancel");
    publish();
  }
  void down(float x, float y, bool twice) {
    if (input_locked())
      return;
    SetFocus(hwnd);
    int id = hit(x, y);
    if (id < 0) {
      set_focus(-1);
      return;
    }
    if (!enabled(id))
      return;
    // Dismissal must remain available with an invalid draft. Do not discard it
    // on pointer-down: a release outside the button leaves the draft untouched.
    if(screen==Screen::create && (id==DismissCreate || id==CloseCreate)) {
      hover=id;pressed=id;SetCapture(hwnd);publish();return;
    }
    if (!set_focus(id)) {
      publish();
      return;
    }
    hover = id;
    if (text_focus()) {
      auto &e = editors[id];
      if (twice)
        e.select_all();
      else
        e.set_caret(metrics.hit_test(e.draft(), th::body,
                                     x - bounds[id].left -
                                         ly::fieldTextLeftInset + scroll[id]),
                    GetKeyState(VK_SHIFT) < 0);
      selecting = true;
      SetCapture(hwnd);
    } else if (screen == Screen::list && id >= taskui::RowBase) {
      select_task(id - taskui::RowBase, twice);
    } else if (id == ResourceList) {
      int row = row_at(x, y);
      if (row >= 0) {
        if (x < pl::nameLeft) {
          pressed = row_check_base + row;
          SetCapture(hwnd);
        } else
          select_row(row);
      }
    } else {
      pressed = id;
      SetCapture(hwnd);
    }
    trace("pointer_down");
    publish();
  }
  void move(float x, float y) {
    hover = hit(x, y);
    if (selecting && text_focus())
      editors[focus].set_caret(
          metrics.hit_test(editors[focus].draft(), th::body,
                           x - bounds[focus].left - ly::fieldTextLeftInset +
                               scroll[focus]),
          true);
    publish();
  }
  void up(float x, float y) {
    int id = pressed;
    pressed = -1;
    selecting = false;
    if (GetCapture() == hwnd)
      ReleaseCapture();
    if (input_locked())
      return;
    if (id >= Included && id != ResourceList && hit(x, y) == id && enabled(id)) activate(id);
    trace("pointer_up");
    publish();
  }
  void wheel(int delta) {
    if (input_locked())
      return;
    cancel_capture();
    if(screen==Screen::create) {
      const auto l=creation_layout();const auto n=creationPlan.snapshot()->plan.resources.size();
      creationFirstFile=delta>0?(creationFirstFile>0?creationFirstFile-1:0):std::min(creationFirstFile+1,n>l.visible?n-l.visible:0);
      publish();return;
    }
    if (screen == Screen::list) {
      auto m = list_model();
      auto count = list_layout().visibleRows;
      firstTaskRow =
          delta > 0
              ? (firstTaskRow > 3 ? firstTaskRow - 3 : 0)
              : std::min(firstTaskRow + 3,
                         m.rows.size() > count ? m.rows.size() - count : 0);
      publish();
      return;
    }
    auto n = snapshot()->plan.resources.size();
    if (delta > 0)
      workspace.firstResourceRow =
          workspace.firstResourceRow > 3 ? workspace.firstResourceRow - 3 : 0;
    else
      workspace.firstResourceRow = std::min(n, workspace.firstResourceRow + 3);
    clamp_scroll();
    publish();
  }
  void clipboard(bool copy, bool cut = false) {
    if (input_locked())
      return;
    if (!text_focus()) {
      if (!copy)
        new_task(true);
      return;
    }
    auto &e = editors[focus];
    if (!e.active())
      begin_editor();
    if (!OpenClipboard(hwnd))
      return;
    if (copy) {
      auto s = e.selection();
      if (!s.empty()) {
        auto mem = GlobalAlloc(GMEM_MOVEABLE, (s.size() + 1) * sizeof(wchar_t));
        if (mem) {
          auto p = GlobalLock(mem);
          if (p) {
            memcpy(p, s.c_str(), (s.size() + 1) * sizeof(wchar_t));
            GlobalUnlock(mem);
            EmptyClipboard();
            if (SetClipboardData(CF_UNICODETEXT, mem)) {
              if (cut)
                e.erase();
            } else
              GlobalFree(mem);
          } else
            GlobalFree(mem);
        }
      }
    } else if (auto handle = GetClipboardData(CF_UNICODETEXT)) {
      if (auto p = (wchar_t *)GlobalLock(handle)) {
        size_t n = GlobalSize(handle) / sizeof(wchar_t), len = 0;
        while (len < n && len < 32768 && p[len])
          ++len;
        if (len < n && len < 32768)
          e.insert(std::wstring_view(p, len));
        GlobalUnlock(handle);
      }
    }
    CloseClipboard();
    publish();
  }
  void key(WPARAM k, LPARAM lp) {
    if (input_locked())
      return;
    bool ctrl = GetKeyState(VK_CONTROL) < 0, shift = GetKeyState(VK_SHIFT) < 0;
    trace("key");
    if (text_focus() && editors[focus].composing())
      return;
    if (ctrl && (k == 'N' || k == 'O' || k == 'S')) {
      if (!(lp & (1LL << 30)))
        activate(k == 'N'   ? NewPlan
                 : k == 'O' ? OpenPlan
                 : shift    ? SaveAs
                            : SavePlan);
      return;
    }
    if (k == VK_TAB) {
      if (!(lp & (1LL << 30))) {
        std::vector<int> order;
        if (screen == Screen::list) {
          for (auto &item : list_layout().hits)
            if (item.enabled)
              order.push_back(item.key);
        } else if (screen == Screen::create) {
          order = {Link, Directory, RecentDirectory, BrowseDirectory, CreateDetails};
          if(creationExpanded) {
            const auto visible=creation_layout();
            for(std::size_t i=visible.first;i<visible.first+visible.visible;++i) order.push_back(taskui::FileBase+int(i));
          }
          order.insert(order.end(),{DismissCreate,CreateDownload,CloseCreate});
        }
        else
          order = {BackToList,
                   Link,
                   ResolveSource,
                   Directory,
                   BrowseDirectory,
                   ResourceList,
                   OutputName,
                   Included,
                   DownloadSelected,
                   CancelTransfer,
                   RevealResult,
                   Undo,
                   Redo,
                   SavePlan};
        auto it = std::find(order.begin(), order.end(), focus);
        int index = it == order.end() ? (shift ? 0 : int(order.size()) - 1)
                                      : int(it - order.begin());
        for (size_t i = 0; i < order.size(); ++i) {
          index =
              (index + (shift ? int(order.size()) - 1 : 1)) % int(order.size());
          if (enabled(order[index]))
            break;
        }
        set_focus(order[index]);
      }
      publish();
      return;
    }
    if (text_focus()) {
      auto &e = editors[focus];
      if (!e.active())
        begin_editor();
      if (ctrl && k == 'A')
        e.select_all();
      else if (ctrl && k == 'C')
        clipboard(true);
      else if (ctrl && k == 'X')
        clipboard(true, true);
      else if (ctrl && k == 'V')
        clipboard(false);
      else if (ctrl && k == 'Z') {
        if (shift)
          e.redo();
        else
          e.undo();
      } else if (ctrl && k == 'Y')
        e.redo();
      else if (k == VK_LEFT || k == VK_RIGHT)
        e.move(k == VK_LEFT ? -1 : 1, shift);
      else if (k == VK_HOME || k == VK_END)
        e.home(k == VK_END, shift);
      else if (k == VK_BACK || k == VK_DELETE)
        e.erase(k == VK_BACK);
      else if (k == VK_ESCAPE) {
        e.cancel();
        leave_editor();
        status = L"已放弃草稿";
        trace("draft_cancel");
      } else if (k == VK_RETURN) {
        const bool resolveLink = focus == Link;
        if (commit_focus()) {
          leave_editor();
          if (screen != Screen::create && resolveLink)
            actions.invoke("source.resolve");
        }
      } else if (ctrl && k == 'L')
        set_focus(Link);
      publish();
      return;
    }
    if (screen == Screen::list && ctrl && k == 'V') {
      actions.invoke("task.paste");
      return;
    }
    if (k == VK_ESCAPE && screen != Screen::list) {
      show_list();
      return;
    }
    if (screen == Screen::list && focus >= taskui::RowBase &&
        (k == VK_UP || k == VK_DOWN)) {
      auto m = list_model();
      if (!m.rows.empty())
        select_task(
            std::clamp(focus - taskui::RowBase + (k == VK_DOWN ? 1 : -1), 0,
                       int(m.rows.size() - 1)));
      return;
    }
    if (ctrl && k == 'L') {
      if (screen == Screen::list)
        new_task();
      else
        set_focus(Link);
    } else if (ctrl && k == 'Z')
      actions.invoke(shift ? "edit.redo" : "edit.undo");
    else if (ctrl && k == 'Y')
      actions.invoke("edit.redo");
    else if (focus == ResourceList) {
      auto s = snapshot();
      int row = selected_row(s->plan);
      if (k == VK_UP || k == VK_DOWN) {
        int next = std::clamp(row + (k == VK_DOWN ? 1 : -1), 0,
                              std::max(0, (int)s->plan.resources.size() - 1));
        select_row(next);
        if (next < (int)workspace.firstResourceRow)
          workspace.firstResourceRow = next;
        else if (next >= (int)(workspace.firstResourceRow + visible_rows()))
          workspace.firstResourceRow =
              std::max(0, next - (int)visible_rows() + 1);
      } else if (k == VK_SPACE && !(lp & (1LL << 30)))
        actions.invoke("plan.setIncluded");
      else if (k == VK_F2 && enabled(OutputName))
        set_focus(OutputName);
    } else if ((k == VK_RETURN || k == VK_SPACE) && focus >= Included &&
               focus != ResourceList && !(lp & (1LL << 30)))
      activate(focus);
    publish();
  }
};

inline void App::publish() {
  if (!render)
    return;
  if (screen == Screen::list) {
    publish_shell();
    return;
  }
  if (screen == Screen::create) {
    publish_create();
    return;
  }
}

#include "task_shell.hpp"
