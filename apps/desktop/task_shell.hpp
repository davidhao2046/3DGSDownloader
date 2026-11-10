#pragma once
// Native task shell. Plans remain owned by TaskListService entries; this file
// owns only navigation, transient creation drafts, and immutable view captures.
inline std::wstring task_bytes(std::uint64_t n) {
  wchar_t b[64];
  if (n >= 1024 * 1024)
    swprintf_s(b, L"%.1f MB", double(n) / (1024 * 1024));
  else if (n >= 1024)
    swprintf_s(b, L"%.1f KB", double(n) / 1024);
  else
    swprintf_s(b, L"%llu B", static_cast<unsigned long long>(n));
  return b;
}
inline std::wstring App::read_clipboard() const {
  std::wstring result;
  if (!OpenClipboard(hwnd))
    return result;
  if (auto h = GetClipboardData(CF_UNICODETEXT))
    if (auto p = (const wchar_t *)GlobalLock(h)) {
      size_t n = GlobalSize(h) / sizeof(wchar_t), len = 0;
      while (len < n && len < 32768 && p[len])
        ++len;
      if (len < n && len < 32768)
        result.assign(p, len);
      GlobalUnlock(h);
    }
  CloseClipboard();
  return result;
}
inline void App::show_list() {
  const bool wasCreate=screen==Screen::create;
  leave_editor();
  if(creationWorker && screen==Screen::create) creationWorker->cancel();
  screen = Screen::list;
  if(wasCreate) {focus=enabled(modalReturnFocus)?modalReturnFocus:taskui::NewTask;status=L"已返回任务列表；未创建新任务";}
  layout();
  publish();
}
inline void App::new_task(bool paste) {
  if (!commit_focus())
    return;
  modalReturnFocus=focus;
  leave_editor();
  newLink.clear();
  newDirectory = lastUsedBaseDirectory.empty()?lastDirectory:lastUsedBaseDirectory;
  auto clipboard = read_clipboard();
  gsdl::Source source;
  if (paste || gsdl::canonical_share_url(clipboard, source))
    newLink = clipboard;
  creationPlan.replaceLoaded(gsdl::make_empty_plan(),{});
  creationExpanded = false;
  creationFirstFile = 0;
  screen = Screen::create;
  status =
      L"一个链接创建一条任务；首次选择主模型和相机数据，以后沿用上次下载选择";
  for (auto &e : editors)
    e.cancel();
  layout();
  set_focus(Link);
  editors[Link].select_all();
  resolve_creation();
  publish();
}
inline void App::create_task(bool start) {
  if (!commit_focus())
    return;
  gsdl::Source source;
  auto valid = gsdl::canonical_share_url(newLink, source);
  if (!valid) {
    status = valid.error;
    set_focus(Link);
    publish();
    return;
  }
  valid = gsdl::validate_output_directory(newDirectory);
  if (!valid || newDirectory.empty()) {
    status = L"请选择有效的绝对保存路径";
    set_focus(Directory);
    publish();
    return;
  }
  auto output = creation_output();
  std::string id;
  auto draft = creationPlan.snapshot()->plan;
  if(draft.source.shareUrl != source.shareUrl || draft.resources.empty()) { resolve_creation(); status=L"正在解析，请等待资源就绪后开始"; publish(); return; }
  if(std::none_of(draft.resources.begin(),draft.resources.end(),[](const auto &r){return r.included;})) { status=L"请至少勾选一个文件"; publish(); return; }
  draft.outputDirectory = output.wstring();
  auto r = taskList.addPlan(std::move(draft),{},id);
  if(r && start) r = taskList.enqueue(id);
  if (!r) {
    if(!id.empty() && !taskList.locked(id)) taskList.remove(id);
    status = r.error;
    publish();
    return;
  }
  remember_directory(newDirectory);
  lastUsedBaseDirectory=newDirectory;
  gsdl::remember_selection_preferences(selectionPreferences,creationPlan.snapshot()->plan.resources);
  selectedTask = id;
  selection.clear();
  completedFiles.clear();

  leave_editor();
  for (auto &e : editors)
    e.cancel();
  screen = Screen::list;
  if(!start) {expandedTaskId=id;expansionAnchor.reset();}
  if (!start)
    taskList.resolve(id);
  status = start ? L"已添加到下载队列；原文件不会覆盖"
                 : L"正在解析资源，请勾选需要的原文件";
  layout();
  persist_workspace();
  publish();
}
inline void App::browse_directory(bool defaults) {
  if (!commit_focus())
    return;
  leave_editor();
  IFileOpenDialog *dialog{};
  if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr,
                              CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog)))) {
    status = L"无法打开文件夹选择器；可在保存位置中输入路径";
    publish();
    return;
  }
  DWORD options{};
  dialog->GetOptions(&options);
  dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM |
                     FOS_PATHMUSTEXIST | FOS_NOCHANGEDIR);
  dialog->SetTitle(defaults?L"设置默认下载目录（仅影响以后新建）":L"选择本次下载文件夹");
  dialog->SetOkButtonLabel(L"选择文件夹");
  std::wstring initial =
      defaults ? (lastUsedBaseDirectory.empty()?lastDirectory:lastUsedBaseDirectory) : screen == Screen::create ? newDirectory : field_value(Directory);
  IShellItem *folder{};
  if (!initial.empty() &&
      SUCCEEDED(SHCreateItemFromParsingName(initial.c_str(), nullptr,
                                            IID_PPV_ARGS(&folder)))) {
    dialog->SetFolder(folder);
    folder->Release();
  }
  if (SUCCEEDED(dialog->Show(hwnd))) {
    IShellItem *item{};
    if (SUCCEEDED(dialog->GetResult(&item))) {
      PWSTR value{};
      if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &value))) {
        if(defaults) { lastDirectory=value; lastUsedBaseDirectory=value; status=L"新任务保存位置已更新；之后跟随确认下载的位置"; }
        else if (screen == Screen::create)
          newDirectory = value;
        else if (current()) {
          auto r = plans().edit("outputDirectory", {}, value);
          status = r ? L"保存位置已更新" : r.error;
          if (r.changed)
            plan_edited();
        }
        remember_directory(value);
        CoTaskMemFree(value);
        persist_workspace();
      }
      item->Release();
    }
  }
  dialog->Release();
  // Do not carry the old editor buffer over a path chosen in the system dialog.
  editors[Directory].cancel();
  publish();
}
inline taskui::TaskListVisualModel App::list_model() const {
  taskui::TaskListVisualModel m;
  m.firstRow = firstTaskRow;
  m.filter = int(taskFilter);
  m.focus = focus;
  m.hover = hover;
  m.pressed = pressed;
  m.active = active;
  m.totalCount = taskList.entries().size();
  m.status = status;
  for (auto &e : taskList.entries()) {
    m.activeCount += matches(*e, gsdl::TaskFilter::Active);
    m.completedCount +=
        matches(*e, gsdl::TaskFilter::Completed);
    m.otherCount += matches(*e, gsdl::TaskFilter::Attention);
    if (!matches(*e, taskFilter))
      continue;
    if (e->id == selectedTask)
      m.selectedRow = int(m.rows.size());
    if (e->id == expandedTaskId)
      m.expandedRow = int(m.rows.size());
    auto p = e->plans.snapshot();
    taskui::TaskListVisualRow r;
    auto capture = p->plan.source.captureId;
    r.title = p->plan.internalFixture
                  ? L"内部样本"
                  : L"影石 3D 时光舱 · " +
                        std::wstring(capture.begin(), capture.end());
    r.fullTitle=r.title;
    if(!p->plan.internalFixture && capture.size()>12)
      r.title=L"影石时光舱 · "+std::wstring(capture.begin(),capture.begin()+8)+L"…"+std::wstring(capture.end()-4,capture.end());
    r.directory = p->plan.outputDirectory;
    r.revealEnabled=can_reveal(e.get());
    r.status = gsdl::task_state_label(e->state);
    r.running = matches(*e, gsdl::TaskFilter::Active);
    r.completed = e->state == gsdl::TaskState::Succeeded;
    const auto available=std::count_if(e->report.files.begin(),e->report.files.end(),[](const auto &f){return f.localState==gsdl::transfer::LocalState::available;});
    const bool unavailable=std::size_t(available)!=e->report.files.size();
    if(r.completed && unavailable) {r.completed=false;r.status=L"本地文件待恢复";}
    r.failed = e->state == gsdl::TaskState::Failed;
    if(unavailable && !r.running) r.progressText=std::to_wstring(available)+L" 个已核对 · 部分文件缺失或有变化，展开查看";
    else if (!e->error.empty())
      r.progressText = e->error;
    else if (r.completed) {
      uint64_t total{};
      for (auto &file : e->report.files)
        total += file.bytes;
      r.progressText = std::to_wstring(e->report.files.size()) + L" 个文件 · " +
                       task_bytes(total) + L" · 已校验";
      r.fraction = 1.;
    } else if (e->state == gsdl::TaskState::Downloading ||
               e->state == gsdl::TaskState::Cancelling) {
      r.progressText =
          L"核对/下载第 " + std::to_wstring(std::min(e->progress.completedFiles + 1, e->progress.fileCount)) +
          L"/" + std::to_wstring(e->progress.fileCount) + L" 个 · " + e->progress.resourceName + L" · " +
          e->progress.stage + L" · " + task_bytes(e->progress.bytes);
      if (e->progress.total && *e->progress.total) {
        r.progressText += L" / " + task_bytes(*e->progress.total);
        r.fraction = std::min(1., double(e->progress.bytes) /
                                      double(*e->progress.total));
      }
      r.progressText += L" · " + std::to_wstring(e->elapsedMs / 1000) + L" 秒";
    } else if (e->state == gsdl::TaskState::Ready)
      r.progressText =
          std::to_wstring(p->plan.resources.size()) + L" 个资源可选";
    else if (e->state == gsdl::TaskState::Queued)
      r.progressText = L"等待前一条任务完成";
    else if (e->state == gsdl::TaskState::Resolving)
      r.progressText = L"读取公开分享与原文件信息…";
    else
      r.progressText = L"选择任务后开始下载或编辑资源";
    m.rows.push_back(std::move(r));
  }
  auto on = [&](const char *key) {
    auto a = actions.query(key);
    return a && a->enabled;
  };
  m.expanded = m.expandedRow>=0;
  m.hiddenSelected = current() && m.selectedRow<0;
  if(const auto owner=expanded_entry();owner && m.expanded) {
    const auto p=owner->plans.snapshot();
    const bool locked=taskList.locked(owner->id);
    for (const auto &resource : p->plan.resources) {
      const auto found = std::find_if(owner->report.files.begin(),owner->report.files.end(),[&](const auto &receipt) {
        return receipt.resourceId == resource.id && receipt.path == fs::path(p->plan.outputDirectory)/resource.outputName;
      });
      const bool done = saved_resource(*owner,resource);
      std::wstring state = done ? L"已下载 · 校验通过" : found != owner->report.files.end() ?
        (found->localState==gsdl::transfer::LocalState::missing?L"本地缺失 · 可重新下载":L"文件有变化或无法核对 · 不会覆盖") : L"未下载";
      if(owner->progress.resourceName==resource.outputName && locked) state=owner->progress.stage;
      m.files.push_back({std::wstring(done?L"✓ ":resource.included?L"☑ ":L"☐ ")+resource.outputName+L"    ·    "+state, !locked && !done, done});
      auto &file=m.files.back();file.name=resource.outputName;
      file.size=resource.expectedBytes?task_bytes(*resource.expectedBytes):L"大小待确认";
      file.status=state;file.checked=resource.included;
    }
  }
  if(current()) {
    auto p=snapshot();
    const auto count=std::count_if(p->plan.resources.begin(),p->plan.resources.end(),[&](const auto &r){return r.included&&!saved_resource(r);});
    m.startLabel=L"下载所选 ("+std::to_wstring(count)+L")";
  }
  m.detailsEnabled = on("task.details");
  m.startEnabled = on("download.start");
  m.cancelEnabled = on("download.cancel");
  m.revealEnabled = on("result.reveal");
  const int folderKey=hover>=taskui::RevealBase && hover<taskui::RowBase?hover:focus;
  if(folderKey>=taskui::RevealBase && folderKey<taskui::RowBase)
    if(const auto target=task_at(folderKey-taskui::RevealBase);can_reveal(target))m.status=L"打开文件夹："+target->plans.snapshot()->plan.outputDirectory;
  if (current() && (current()->state == gsdl::TaskState::Failed ||
                    current()->state == gsdl::TaskState::Cancelled))
    m.startLabel = L"重试下载";
  return m;
}
inline taskui::TaskListLayout App::list_layout() const {
  return taskui::task_list_layout(width / dpi, height / dpi, list_model());
}
inline void App::select_task(int index, bool details) {
  if (!commit_focus())
    return;
  int i = 0;
  for (auto &e : taskList.entries())
    if (matches(*e, taskFilter)) {
      if (i++ != index)
        continue;
      selectedTask = e->id;
      selection.clear();
      workspace.firstResourceRow = 0;
      completedFiles.clear();
      for (auto &f : e->report.files)
        completedFiles[f.resourceId] = f;
      for (auto &editor : editors)
        editor.cancel();
      focus = taskui::RowBase + index;
      sync_ime();
      status =
          e->error.empty()
              ? std::wstring(gsdl::task_state_label(e->state))
              : e->error + L"；已完成文件保留。可重试，或在新建中选择其他目录。";
      if(e->state==gsdl::TaskState::Succeeded && !matches(*e,gsdl::TaskFilter::Completed)) status=L"本地文件缺失或有变化；展开查看并恢复";
      if (details) {
        toggle_expanded();
      }
      publish();
      return;
    }
}
inline bool App::select_expanded_owner() {
  const auto owner=expandedTaskId;
  const auto m=list_model();
  if(owner.empty() || m.expandedRow<0) return false;
  if(selectedTask!=owner) select_task(m.expandedRow);
  return selectedTask==owner;
}
inline void App::shell_accessibility(const taskui::TaskListLayout &layout) {
  if (!accessibility)
    return;
  POINT origin{};
  ClientToScreen(hwnd, &origin);
  std::vector<gsdl::ui::AccessibleNode> nodes;
  for (auto &item : layout.hits) {
    auto r = item.rect;
    const bool completedFile = item.key >= taskui::FileBase &&
        item.key < taskui::RevealBase && !item.enabled &&
        item.label.starts_with(L"✓");
    nodes.push_back(
        {item.key + 1,
         item.label,
         L"",
         {origin.x + LONG(r.left * dpi), origin.y + LONG(r.top * dpi),
          origin.x + LONG(r.right * dpi), origin.y + LONG(r.bottom * dpi)},
         focus == item.key && active,
           completedFile || (item.enabled && !input_locked()),
           completedFile ? gsdl::ui::AccessibleKind::text
                         : gsdl::ui::AccessibleKind::button});
  }
  nodes.push_back({ControlCount + 1,
                   status,
                   L"",
                   {},
                   false,
                   true,
                   gsdl::ui::AccessibleKind::text});
  accessibility->update(std::move(nodes));
}
inline void App::publish_shell() {
  auto f = std::make_shared<Frame>();
  f->width = width;
  f->height = height;
  f->dpi = dpi;
  f->serial = ++serial;
  f->heartbeat = heartbeat;
  f->maxGap = maxGap;
  f->focus = focus;
  f->closing = closing;
  f->plan = snapshot();
  if (current()) {
    f->revision = f->plan->revision;
    f->dirty = plans().dirty();
    f->undoCount = plans().undo_count();
    f->redoCount = plans().redo_count();
    f->link = f->plan->plan.source.shareUrl;
    f->output = f->plan->plan.outputDirectory;
  }
  auto m = list_model();
  auto l = taskui::render_task_list(*f, m);
  for (auto &item : l.hits)
    if (item.row) {
      auto &row = m.rows[item.key - taskui::RowBase];
      item.label +=
          L"，" + row.status + L"，" + row.progressText + L"，" + row.directory;
    }
  shell_accessibility(l);
  std::ostringstream o;
  o << "{\"taskCount\":" << taskList.entries().size() << ",\"selectedTask\":\""
    << selectedTask << "\",\"expandedTask\":\"" << expandedTaskId << "\",\"lastOpenedTask\":\"" << lastOpenedTask << "\",\"lastOpenedDirectory\":" << json(lastOpenedDirectory) << ",\"tasks\":[";
  bool comma = false;
  for (auto &e : taskList.entries()) {
    if (comma)
      o << ',';
    comma = true;
    o << "{\"id\":\"" << e->id << "\",\"state\":" << int(e->state)
      << ",\"revision\":" << e->plans.snapshot()->revision
      << ",\"bytes\":" << e->progress.bytes << '}';
  }
  o << "]}";
  f->networkState = o.str();
  std::wstring title = gsdl::product::windowTitle;
  if (title != titleCache) {
    SetWindowTextW(hwnd, title.c_str());
    titleCache = title;
  }
  render->publish(f);
  update_menu();
}
inline void App::initialize_workspace() {
  if (workspaceLoading || workspaceSaving || closing)
    return;
  PWSTR path{};
  if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Downloads, 0, nullptr, &path))) {
    lastDirectory = (fs::path(path) / L"3DGSDownloader").wstring();
    CoTaskMemFree(path);
    path = nullptr;
  }
  if (workspacePath.empty() && SUCCEEDED(SHGetKnownFolderPath(
                                   FOLDERID_LocalAppData, 0, nullptr, &path))) {
    workspacePath = fs::path(path) / L"3DGSDownloader" / L"tasks.json";
    CoTaskMemFree(path);
  }
  if (workspacePath.empty() || !workspaceIo) {
    workspaceFailed = true;
    status = L"无法取得任务列表保存位置或启动保存服务";
    return;
  }
  workspaceLoading = workspaceIo->submit(st::WorkspaceRead{1, workspacePath});
  if (!workspaceLoading) {
    // We have not validated the original file. Never let a subsequent save
    // replace it, even when the read was rejected before reaching the worker.
    const auto recoveryId = gsdl::make_empty_plan().planId;
    workspacePath =
        workspacePath.parent_path() /
        (L"tasks-recovery-" +
         std::wstring(recoveryId.begin(), recoveryId.end()) + L".json");
    workspaceFailed = true;
    status = L"任务列表读取未开始；原文件保留，新任务将使用独立恢复文件";
  }
}
inline void App::persist_workspace() {
  if (!workspaceIo || workspaceLoading || workspacePath.empty() || closing)
    return;
  // This is a fingerprint of immutable documents and durable task facts. Byte
  // progress and elapsed time deliberately do not schedule filesystem writes.
  std::ostringstream key;
  auto part = [&](const std::string &value) {
    key << value.size() << ':' << value;
  };
  part(utf8(lastDirectory));
  part(utf8(lastUsedBaseDirectory));
  key << ':' << selectionPreferences.remembered;
  for(const auto &[kind,included]:selectionPreferences.choices) {part(kind);key << ':' << included;}
  for(const auto &p:recentDirectories) part(utf8(p));
  part(utf8(workspacePath.wstring()));
  for (auto &e : taskList.entries()) {
    const auto p = e->plans.snapshot();
    part(e->id);
    key << '|' << p->sessionId << ':' << p->revision << ':' << int(e->state);
    part(utf8(e->plans.target()));
    part(utf8(e->error));
    part(utf8(e->report.receiptPath.wstring()));
    part(utf8(e->report.warning));
    for (const auto &file : e->report.files) {
      part(file.resourceId);
      part(utf8(file.path.wstring()));
      key << ':' << file.bytes;
      part(file.md5);
      part(file.sha256);
      part(file.expectedMd5);
    }
  }
  const auto stamp = key.str();
  if (stamp == workspaceStamp && !workspaceFailed)
    return;
  workspaceStamp = stamp;
  ++workspaceGeneration;
  if (workspaceSaving) {
    workspacePending = true;
    return;
  }
  workspacePending = false;
  workspaceSaving = workspaceIo->submit(
      st::WorkspaceSave{workspaceGeneration,
                        workspacePath,
                        {lastDirectory, taskList.exportSnapshot(),recentDirectories,lastUsedBaseDirectory,selectionPreferences}});
  if (workspaceSaving) {
    workspaceInFlightGeneration = workspaceGeneration;
    workspaceFailed = false;
  } else {
    workspaceFailed = true;
    status = L"任务列表保存队列不可用；内存中的任务保留，请重试关闭";
  }
}
inline void App::on_workspace() {
  if (!workspaceIo)
    return;
  for (auto &result : workspaceIo->take_results()) {
    if (result.operation == st::Operation::read) {
      if (!workspaceLoading || result.requestId != 1 ||
          result.path != workspacePath)
        continue;
      workspaceLoading = false;
      std::wstring restoreError;
      if (result.error) {
        restoreError = result.error->message;
      } else if (result.data) {
        auto r = taskList.restore(std::move(result.data->tasks));
        if (r) {
          if (!result.data->lastDirectory.empty())
            lastDirectory = result.data->lastDirectory;
          recentDirectories = result.data->recentDirectories;
          lastUsedBaseDirectory=result.data->lastUsedBaseDirectory;
          selectionPreferences=result.data->selectionPreferences;
          selectedTask.clear();
          expandedTaskId.clear();expansionAnchor.reset();
          selection.clear();
          completedFiles.clear();
          status = L"已恢复任务列表；未完成任务由你手动开始";
        } else
          restoreError = r.error;
      } else if (!result.missing) {
        restoreError = L"读取结果没有有效的任务列表";
      }
      if (!restoreError.empty()) {
        // Both decoder failures and application-level restore failures must
        // retain the unreadable original. UUID avoids clobbering another run's
        // recovery file; no filesystem checks or hashing run on the UI thread.
        const auto recoveryId = gsdl::make_empty_plan().planId;
        workspacePath =
            workspacePath.parent_path() /
            (L"tasks-recovery-" +
             std::wstring(recoveryId.begin(), recoveryId.end()) + L".json");
        workspaceStamp.clear();
        workspaceFailed = false;
        status = L"旧任务列表无法恢复（" + restoreError +
                 L"），原文件已保留；新任务保存到 " + workspacePath.wstring();
      }
      // A close requested during startup waits for this read, but does not
      // start a fixture/import that the user has already chosen to leave.
      if (deferred != Intent::close && !closing) {
        if (fixture) {
          std::string id;
          auto r = taskList.addPlan(gsdl::make_internal_fixture(), L"", id);
          if (r) {
            selectedTask = id;
            screen = Screen::list; expandedTaskId=id;expansionAnchor.reset();
            layout();
          } else
            status = r.error;
        }
        if (!startupPlan.empty())
          open_path(startupPlan);
      }
      persist_workspace();
    } else {
      // There is exactly one accepted save in flight. A stale notification may
      // not acknowledge a newer generation, clear its failure, or close the UI.
      if (result.operation != st::Operation::save || !workspaceSaving ||
          result.generation != workspaceInFlightGeneration ||
          result.path != workspacePath)
        continue;
      workspaceSaving = false;
      workspaceInFlightGeneration = 0;
      if (result.error) {
        workspaceFailed = true;
        workspacePending = false;
        deferred = Intent::none;
        status = L"任务列表保存失败：" + result.error->message +
                 L"；原文件与内存任务保留，可重试关闭";
      } else if (workspacePending || result.generation != workspaceGeneration) {
        workspacePending = false;
        workspaceSaving = workspaceIo->submit(
            st::WorkspaceSave{workspaceGeneration,
                              workspacePath,
                              {lastDirectory, taskList.exportSnapshot(),recentDirectories,lastUsedBaseDirectory,selectionPreferences}});
        if (workspaceSaving) {
          workspaceInFlightGeneration = workspaceGeneration;
          workspaceFailed = false;
        } else {
          workspaceFailed = true;
          deferred = Intent::none;
          status = L"任务列表保存未开始；内存任务保留，请重试关闭";
        }
      } else {
        workspaceFailed = false;
      }
    }
    // Finish an already requested close directly, rather than re-entering a
    // save/request-close cycle. Other workers must have completed their work.
    if (!workspaceLoading && !workspaceSaving && !workspacePending &&
        !workspaceFailed && deferred == Intent::close && !tasks.busy() &&
        !network_busy()) {
      deferred = Intent::none;
      closeAfterTransfer = false;
      shutdown();
    }
  }
  publish();
}

inline creationui::Model App::creation_model() const {
  creationui::Model m;
  m.linkText=newLink;m.directoryText=newDirectory;m.expanded=creationExpanded;
  m.firstFile=creationFirstFile;m.recentEnabled=!recentDirectories.empty();
  m.busy=creationWorker && creationWorker->view().busy;
  m.submitEnabled=enabled(CreateDownload);m.renderFields=false;
  const auto p=creationPlan.snapshot();
  std::size_t selectedCount{};
  for(const auto &r:p->plan.resources) {
    m.files.push_back({r.outputName,r.expectedBytes?task_bytes(*r.expectedBytes):L"大小待确认",r.included});
    if(r.included) {
      const auto key=gsdl::resource_preference_key(r);
      const auto name=key=="primaryModel"?L"主模型":key=="cameraData"?L"相机数据":key=="voxelArchive"?L"体素包":key=="effectVideo1"?L"效果视频 1":key=="effectVideo2"?L"效果视频 2":r.displayName;
      if(selectedCount++)m.selectionSummary+=L"、";
      m.selectionSummary+=name;
    }
  }
  if(!p->plan.resources.empty())m.selectionSummary=selectedCount?m.selectionSummary+L" · "+std::to_wstring(selectedCount)+L" 个文件":L"未选择文件";
  const auto destination=creation_output();
  m.preview=newLink.empty()?L"每条任务自动建立独立子文件夹，原文件不会覆盖。":L"子文件夹："+destination.filename().wstring();
  m.hint=m.busy?L"正在解析公开链接…":m.files.empty()?L"粘贴公开分享链接，无需登录。":L"已找到 "+std::to_wstring(m.files.size())+L" 个原文件，可在下方选择下载内容。";
  if(!m.busy && !m.files.empty())m.hint=selectedCount?(selectionPreferences.remembered?L"已沿用上次下载选择，可在下方调整。":L"默认选择主模型和相机数据，可在下方调整。"):(selectionPreferences.remembered?L"当前没有选中文件；上次选择可能不在此分享中，请选择下载内容。":L"请至少选择一个文件后开始下载。");
  if(creationWorker && !m.busy && !creationWorker->view().error.empty()) {m.hint=creationWorker->view().error;m.error=true;}
  gsdl::Source parsed;
  if(!newLink.empty()) if(auto result=gsdl::canonical_share_url(newLink,parsed);!result) {m.hint=result.error;m.error=true;}
  if(text_focus() && editors[focus].invalid()) {m.hint=status;m.error=true;}
  if(auto directory=gsdl::validate_output_directory(newDirectory);!directory && !newDirectory.empty()) {m.preview=directory.error;}
  auto part=[](int id) {
    switch(id) {case Link:return int(creationui::link);case Directory:return int(creationui::directory);
      case BrowseDirectory:return int(creationui::browse);case RecentDirectory:return int(creationui::recent);
      case CreateDetails:return int(creationui::disclosure);case DismissCreate:return int(creationui::cancel);
      case CreateDownload:return int(creationui::submit);case CloseCreate:return int(creationui::close);default:break;}
    if(id>=taskui::FileBase && id<taskui::RevealBase) return creationui::fileBase+id-taskui::FileBase;
    return -1;
  };
  m.focus=active?part(focus):-1;m.hover=part(hover);m.pressed=part(pressed);
  return m;
}
inline void App::publish_create() {
  layout();
  auto f=std::make_shared<Frame>();
  f->width=width;f->height=height;f->dpi=dpi;f->serial=++serial;
  f->heartbeat=heartbeat;f->maxGap=maxGap;f->focus=focus;f->plan=snapshot();f->imeResults=imeResults;
  auto background=list_model();background.active=false;background.focus=background.hover=background.pressed=-1;
  background.status=L"";
  taskui::render_task_list(*f,background);
  f->overlayBoxes=f->boxes.size();f->overlayTexts=f->texts.size();
  f->boxes.push_back({{0,0,width/dpi,height/dpi},cr::modalMask});
  const auto model=creation_model();
  const auto creation=creationui::render(*f,metrics,model);
  const auto destination=creation_output();
  auto box=[&](g::Rect r,g::Rgba c){f->boxes.push_back({r,c,2});};
  auto clip=[&](g::Rect r){return ux::clip(*f,r);};
  auto text=[&](std::wstring value,float x,float y,g::Rgba color=th::text,float size=th::body,std::optional<tx::Clip> c={}){f->texts.push_back({std::move(value),x,y,size,color,c});};
  for (int id : {Link, Directory}) {
    auto r = bounds[id];
    bool editing = focus == id && editors[id].active();
    auto value = editing ? editors[id].display_text() : field_value(id);
    bool on = enabled(id);
    box(r, editing && editors[id].invalid() ? th::error
           : focus == id && active          ? th::accent
                                            : th::border);
    box({r.left + th::line, r.top + th::line, r.right - th::line,
         r.bottom - th::line},
        th::window);
    auto fieldClip = clip({r.left + ly::fieldClipHorizontalInset,
                           r.top + ly::fieldClipVerticalInset,
                           r.right - ly::fieldClipHorizontalInset,
                           r.bottom - ly::fieldClipVerticalInset});
    if (editing) {
      auto &e = editors[id];
      float cx = metrics.caret_x(value, th::body, e.display_caret()),
            avail = r.right - r.left - ly::fieldScrollReserve;
      scroll[id] =
          std::max(0.f, std::max(cx - avail, std::min(scroll[id], cx)));
      if (e.anchor() != e.caret() && !e.composing()) {
        float a = metrics.caret_x(value, th::body,
                                  std::min(e.anchor(), e.caret())),
              b = metrics.caret_x(value, th::body,
                                  std::max(e.anchor(), e.caret()));
        box({std::max(r.left + ly::fieldClipHorizontalInset,
                      r.left + ly::fieldTextLeftInset + a - scroll[id]),
             r.top + ly::selectionVerticalInset,
             std::min(r.right - ly::fieldClipHorizontalInset,
                      r.left + ly::fieldTextLeftInset + b - scroll[id]),
             r.bottom - ly::selectionVerticalInset},
            th::selection);
      }
      if (active)
        box({r.left + ly::fieldTextLeftInset + cx - scroll[id],
             r.top + ly::caretVerticalInset,
             r.left + ly::fieldTextLeftInset + cx - scroll[id] + th::line,
             r.bottom - ly::caretVerticalInset},
            th::text);
      if (e.composing()) {
        auto [a, b] = e.composition_range();
        float x = metrics.caret_x(value, th::body, a),
              end = metrics.caret_x(value, th::body, a + b);
        box({std::max(r.left + ly::fieldClipHorizontalInset,
                      r.left + ly::fieldTextLeftInset + x - scroll[id]),
             r.bottom - ly::compositionBottomInset,
             std::min(r.right - ly::fieldClipHorizontalInset,
                      r.left + ly::fieldTextLeftInset + end - scroll[id]),
             r.bottom - ly::compositionBottomInset + th::line},
            th::accent);
      }
      f->draft = e.draft();
      f->invalid = e.invalid();
      f->composing = e.composing();
    } else
      scroll[id] = 0;
    std::wstring placeholder = id == Link ? L"粘贴影石 3D 时光舱公开分享链接"
                               : id == Directory ? L"例如 D:\\我的时光舱"
                                                 : L"选择资源后编辑";
    text(value.empty() ? placeholder : value,
         r.left + ly::fieldTextLeftInset - scroll[id],
         r.top + (r.bottom-r.top-metrics.measure(value.empty()?placeholder:value,th::body).height)*.5f,
         !on ? th::disabled : value.empty() ? th::control::placeholder : th::text, th::body, fieldClip);
  }
  taskui::TaskListLayout access;
  for(int id:{Link,Directory,BrowseDirectory,CreateDownload,CreateDetails,DismissCreate,CloseCreate,RecentDirectory}) {
    std::wstring name=id==Link?L"公开分享链接":id==Directory?L"保存到":actions.query(action_id(id))->label;
    if(id==CloseCreate) name=L"关闭新建下载";
    if(id==BrowseDirectory) name=L"选择文件夹";
    if(id==CreateDetails) name=creationExpanded?L"收起下载内容":L"展开下载内容";
    if(id==CreateDownload) {const auto n=std::count_if(model.files.begin(),model.files.end(),[](auto &r){return r.checked;}); name=model.files.empty()?L"开始下载":L"开始下载 ("+std::to_wstring(n)+L")";}
    access.hits.push_back({id,bounds[id],name,enabled(id),false});
  }
  for(std::size_t i=creation.first;i<creation.first+creation.visible;++i) {
    const auto &r=model.files[i];
    access.hits.push_back({taskui::FileBase+int(i),creation.file(i),std::wstring(r.checked?L"已勾选 ":L"未勾选 ")+r.name,true,false});
  }
  if(accessibility) {
    POINT origin{};ClientToScreen(hwnd,&origin);
    std::vector<gsdl::ui::AccessibleNode> nodes;
    for(auto &item:access.hits) {
      auto r=item.rect;const int id=item.key;
      nodes.push_back({id+1,item.label,id<=OutputName?(editors[id].active()?editors[id].draft():field_value(id)):L"",
        {origin.x+LONG(r.left*dpi),origin.y+LONG(r.top*dpi),origin.x+LONG(r.right*dpi),origin.y+LONG(r.bottom*dpi)},
        focus==id&&active,item.enabled,id<=OutputName?gsdl::ui::AccessibleKind::edit:gsdl::ui::AccessibleKind::button});
    }
    nodes.push_back({ControlCount+1,model.hint,L"",{},false,true,gsdl::ui::AccessibleKind::text});
    nodes.push_back({ControlCount+2,L"实际下载目录："+destination.wstring(),L"",{},false,true,gsdl::ui::AccessibleKind::text});
    accessibility->update(std::move(nodes));
  }
  if(titleCache!=gsdl::product::windowTitle) {titleCache=gsdl::product::windowTitle;SetWindowTextW(hwnd,titleCache.c_str());}
  render->publish(f);ime_position();update_menu();
}

inline void App::resolve_creation() {
  if(screen != Screen::create || closing) return;
  if(newLink.empty()) { status=L"粘贴公开分享链接后解析"; return; }
  auto edited=creationPlan.edit("source.shareUrl",{},newLink);
  if(!edited) { status=edited.error; return; }
  if(!creationWorker) {
    creationStopped=false;
    const auto window=hwnd;
    creationWorker=std::make_unique<gsdl::transfer::Service>([window]{PostMessageW(window,WM_APP+11,0,0);},[window]{PostMessageW(window,WM_APP+12,0,0);});
  }
  if(creationWorker->view().busy) { creationWorker->cancel(); return; }
  if(!creationPlan.snapshot()->plan.resources.empty()) return;
  auto r=creationWorker->resolve(creationPlan.snapshot());
  status=r?L"正在解析公开链接…":r.error;
}
inline void App::on_creation() {
  if(!creationWorker) return;
  if(auto c=creationWorker->take_completion()) {
    if(screen==Screen::create && !closing) {
      auto p=creationPlan.snapshot();
      gsdl::Source visible;
      const auto visibleText=focus==Link && editors[Link].active()?editors[Link].draft():newLink;
      if(!gsdl::canonical_share_url(visibleText,visible) || visible.shareUrl!=c->request->plan.source.shareUrl) {publish();return;}
      if(c->request->sessionId==p->sessionId && c->request->revision==p->revision && c->resolved) {
        std::vector<gsdl::Resource> rows;
        for(const auto &r:c->resolved->resources) rows.push_back(r.descriptor);
        gsdl::apply_selection_preferences(rows,selectionPreferences);
        auto ok=creationPlan.acceptResolved(c->request,std::move(rows),gsdl::sources::adapter_version);
        status=ok?L"资源已就绪；可展开下载内容选择文件":ok.error;
      } else if(c->request->sessionId!=p->sessionId || c->request->revision!=p->revision) resolve_creation();
      else status=c->view.error;
    }
  }
  publish();
}

inline void App::remember_directory(std::wstring path) {
  std::erase(recentDirectories,path);
  recentDirectories.insert(recentDirectories.begin(),std::move(path));
  if(recentDirectories.size()>8) recentDirectories.resize(8);
}
inline fs::path App::creation_output() const {
  gsdl::Source source;
  if(!gsdl::canonical_share_url(newLink,source)) return fs::path(newDirectory);
  const auto base=fs::path(newDirectory)/(L"时光舱_"+std::wstring(source.captureId.begin(),source.captureId.end()));
  auto output=base;
  unsigned suffix=1;
  while(std::any_of(taskList.entries().begin(),taskList.entries().end(),[&](const auto &e){return _wcsicmp(e->plans.snapshot()->plan.outputDirectory.c_str(),output.c_str())==0;})) output=base.wstring()+L"_"+std::to_wstring(++suffix);
  return output;
}
inline void App::directory_menu(bool settings) {
  if(!commit_focus()) return;
  themed_menu::Popup popup(hwnd);
  if(settings) {
    popup.add(0,L"新任务保存到："+(lastUsedBaseDirectory.empty()?lastDirectory:lastUsedBaseDirectory),false);
    popup.separator();
    popup.add(1,L"更改保存位置…");
    popup.add(2,L"清空最近目录",!recentDirectories.empty());
  } else for(std::size_t i=0;i<recentDirectories.size();++i) popup.add(100+UINT(i),recentDirectories[i]);
  g::Rect anchor=bounds[RecentDirectory];
  if(settings) for(const auto &hit:list_layout().hits) if(hit.key==taskui::Settings) anchor=hit.rect;
  POINT p{LONG(anchor.left*dpi),LONG(anchor.bottom*dpi)};ClientToScreen(hwnd,&p);
  auto command=popup.show(p);
  if(settings && command==1) browse_directory(true);
  else if(settings && command==2) {recentDirectories.clear();status=L"最近目录已清空；默认目录及现有任务不变";persist_workspace();}
  else if(!settings && command>=100 && command-100<recentDirectories.size()) {newDirectory=recentDirectories[command-100];editors[Directory].cancel();}
  publish();
}

inline void App::toggle_expanded() {
  if(!current()) return;
  const bool collapse=expandedTaskId==selectedTask;
  if(!collapse) {
    expansionAnchor=firstTaskRow;
    expandedTaskId=selectedTask;
    const auto m=list_model();
    if(m.selectedRow>=0) {
      const auto l=list_layout();
      const auto needed=gsdl::theme::taskList::rowHeight+2*gsdl::theme::taskList::rowGap+float(m.files.size())*gsdl::theme::taskList::fileHeight;
      const auto before=std::size_t(std::max(0.f,std::floor((l.list.bottom-l.list.top-needed)/(gsdl::theme::taskList::rowHeight+gsdl::theme::taskList::rowGap))));
      if(std::size_t(m.selectedRow)>firstTaskRow+before) firstTaskRow=std::size_t(m.selectedRow)-before;
    }
  } else {
    expandedTaskId.clear();
    if(expansionAnchor) firstTaskRow=*expansionAnchor;
    expansionAnchor.reset();
  }
  screen=Screen::list;layout();publish();
}
