#include "apps/desktop/task_list_visual.hpp"
#include <iostream>
void require(bool ok,const char *why) {if(!ok) throw std::runtime_error(why);}
const taskui::TaskListHit *find_hit(const taskui::TaskListLayout &layout,int key) {
  const auto it=std::find_if(layout.hits.begin(),layout.hits.end(),[&](const auto &hit){return hit.key==key;});
  return it==layout.hits.end()?nullptr:&*it;
}
void stable_content(const taskui::TaskListLayout &before,const taskui::TaskListLayout &after) {
  require(before.visibleRows==after.visibleRows,"hover changed visible task count");
  for(const auto &hit:before.hits) if(hit.row||(hit.key>=taskui::ExpandBase&&hit.key<taskui::RevealBase)) {
    const auto *next=find_hit(after,hit.key);
    require(next&&memcmp(&hit.rect,&next->rect,sizeof(g::Rect))==0,"hover moved a task or file");
  }
}
void hover_reveal_contracts(float width,float height,taskui::TaskListVisualModel m) {
  m.selectedRow=0;m.focus=m.hover=m.pressed=-1;
  const auto selectedA=taskui::task_list_layout(width,height,m);
  require(find_hit(selectedA,taskui::RevealBase)!=nullptr,"selected task lost its folder action");
  require(find_hit(selectedA,taskui::RevealBase+1)==nullptr,"idle unselected task exposes folder action");
  m.hover=taskui::RowBase+1;
  const auto hoveredB=taskui::task_list_layout(width,height,m);
  stable_content(selectedA,hoveredB);
  const auto *folderB=find_hit(hoveredB,taskui::RevealBase+1);
  require(folderB!=nullptr,"hovered completed task has no folder action");
  require(folderB->label.find(m.rows[1].title)!=std::wstring::npos,"folder action label borrows selected task");
  require(hoveredB.hit((folderB->rect.left+folderB->rect.right)/2,(folderB->rect.top+folderB->rect.bottom)/2)==taskui::RevealBase+1,
          "hovered folder hit points at selected task instead of target task");
  for(int key:{taskui::RevealBase+1,taskui::ExpandBase+1}) {
    m.hover=key;
    const auto child=taskui::task_list_layout(width,height,m);
    const auto *folder=find_hit(child,taskui::RevealBase+1);
    require(folder&&memcmp(&folder->rect,&folderB->rect,sizeof(g::Rect))==0,"folder disappeared or moved while entering row child");
    require(taskui::row_for_key(m,key)==1,"child hover lost its parent row");
    stable_content(hoveredB,child);
  }
  m.hover=-1;m.focus=taskui::RowBase+1;
  const auto focusedB=taskui::task_list_layout(width,height,m);
  require(find_hit(focusedB,taskui::RevealBase+1)!=nullptr,"keyboard-focused task has no folder action");
  m.focus=taskui::RevealBase+1;
  require(find_hit(taskui::task_list_layout(width,height,m),taskui::RevealBase+1)!=nullptr,"folder disappears when keyboard focus enters it");
  m.active=false;
  require(find_hit(taskui::task_list_layout(width,height,m),taskui::RevealBase+1)==nullptr,"inactive keyboard focus exposes unrelated action");
  m.active=true;m.focus=-1;m.hover=taskui::RowBase+2;m.selectedRow=2;
  const auto unavailable=taskui::task_list_layout(width,height,m);
  require(find_hit(unavailable,taskui::RevealBase+2)==nullptr,"unavailable folder action was exposed");
  stable_content(selectedA,unavailable);
}
void bitmap(const fs::path &path,const g::SkiaCanvas &canvas) {
  auto bytes=canvas.pixels();
  std::vector<unsigned char> bgra(bytes.begin(),bytes.end());
  for(std::size_t i=0;i<bgra.size();i+=4) std::swap(bgra[i],bgra[i+2]);
  BITMAPFILEHEADER file{};file.bfType=0x4d42;file.bfOffBits=sizeof(file)+sizeof(BITMAPINFOHEADER);file.bfSize=file.bfOffBits+DWORD(bgra.size());
  BITMAPINFOHEADER info{};info.biSize=sizeof(info);info.biWidth=LONG(canvas.width());info.biHeight=-LONG(canvas.height());info.biPlanes=1;info.biBitCount=32;info.biSizeImage=DWORD(bgra.size());
  std::ofstream out(path,std::ios::binary);out.write((char*)&file,sizeof(file));out.write((char*)&info,sizeof(info));out.write((char*)bgra.data(),bgra.size());
}
int wmain(int argc,wchar_t **argv) {
  if(argc!=3) return 2;
  try {
    fs::create_directories(argv[2]);
    tx::Rasterizer text;g::SkiaCanvas canvas(fs::absolute(argv[1]));
    taskui::TaskListVisualModel m;
    m.rows={{L"深圳 · 公开时光舱",L"D:\\下载\\深圳",L"下载中",L"第 2/3 个 · effect_1.mp4 · 正在下载 · 8.0 MB / 16.0 MB",.5,true,false,false},
            {L"展馆 · 公开时光舱",L"D:\\下载\\展馆",L"已完成",L"1 个文件 · 15.4 MB · 已校验",1.,false,false,true},
            {L"公园 · 公开时光舱",L"D:\\下载\\公园",L"失败",L"文件已存在；原文件保留，请更换输出位置",{},false,true,false}};
    m.selectedRow=0;m.totalCount=3;m.activeCount=1;m.completedCount=1;m.otherCount=1;
    m.expanded=true;m.expandedRow=0;m.detailsEnabled=true;m.cancelEnabled=true;m.revealEnabled=true;
    m.rows[0].revealEnabled=true; // A running task may already own completed files.
    m.rows[1].revealEnabled=true;
    m.files={{L"✓ model.sog · 已下载 · 校验通过",false,true,L"model.sog",L"15.4 MB",L"已下载 · 校验通过",true},
             {L"☑ effect_1.mp4 · 正在下载",false,false,L"effect_1.mp4",L"16.0 MB",L"正在下载",true},
             {L"☑ effect_2.mp4 · 未下载",false,false,L"effect_2.mp4",L"18.2 MB",L"未下载",true}};
    m.status=L"正在下载；原文件不会覆盖";
    for(auto scale:{1.f,1.5f,2.f}) for(auto size:{std::pair{1280,800},std::pair{960,640}}) {
      hover_reveal_contracts(float(size.first),float(size.second),m);
      Frame f;f.dpi=scale;f.width=unsigned(size.first*scale);f.height=unsigned(size.second*scale);
      auto layout=taskui::render_task_list(f,m);
      for(const auto &hit:layout.hits) {
        require(hit.rect.left>=0 && hit.rect.right<=size.first && hit.rect.bottom<=size.second,"hit outside client");
        require(layout.hit((hit.rect.left+hit.rect.right)/2,(hit.rect.top+hit.rect.bottom)/2)==hit.key,"ambiguous hit region");
      }
      auto stable=taskui::task_list_layout(float(size.first),float(size.second),m);m.hover=taskui::Cancel;m.focus=taskui::Cancel;m.pressed=taskui::Cancel;
      auto pressed=taskui::task_list_layout(float(size.first),float(size.second),m);
      require(stable.hits.size()==pressed.hits.size(),"state changed layout");
      for(std::size_t i=0;i<stable.hits.size();++i) require(memcmp(&stable.hits[i].rect,&pressed.hits[i].rect,sizeof(g::Rect))==0,"state moved control");
      m.hover=m.focus=m.pressed=-1;
      // A user's selection must not move any parent row. The former extra
      // toolbar below the selected row violated this even without expansion.
      auto collapsed=m;collapsed.expanded=false;collapsed.selectedRow=-1;
      const auto unselected=taskui::task_list_layout(float(size.first),float(size.second),collapsed);
      for(int selected=0;selected<int(m.rows.size());++selected) {
        collapsed.selectedRow=selected;
        const auto selectedLayout=taskui::task_list_layout(float(size.first),float(size.second),collapsed);
        for(const auto &row:unselected.hits) if(row.row) {
          auto found=std::find_if(selectedLayout.hits.begin(),selectedLayout.hits.end(),[&](const auto &h){return h.key==row.key;});
          require(found!=selectedLayout.hits.end() && memcmp(&found->rect,&row.rect,sizeof(g::Rect))==0,"selection moved a task row");
        }
      }
      // Once explicitly expanded, its owner and file geometry must survive
      // selection of another task. Selection is never disclosure state.
      auto independent=m;independent.selectedRow=-1;
      const auto expanded=taskui::task_list_layout(float(size.first),float(size.second),independent);
      for(int selected=0;selected<int(m.rows.size());++selected) {
        independent.selectedRow=selected;
        const auto next=taskui::task_list_layout(float(size.first),float(size.second),independent);
        require(next.visibleRows==expanded.visibleRows,"selection changed visible row count while expanded");
        for(const auto &before:expanded.hits) if(before.row || (before.key>=taskui::ExpandBase&&before.key<taskui::RevealBase)) {
          const auto after=std::find_if(next.hits.begin(),next.hits.end(),[&](const auto &h){return h.key==before.key;});
          require(after!=next.hits.end() && memcmp(&before.rect,&after->rect,sizeof(g::Rect))==0,
                  "selection moved an expanded task or file");
          require(before.label==after->label,"selection changed disclosure or file ownership");
        }
      }
      // Each visible row has an independently hittable disclosure control.
      for(const auto &hit:layout.hits) if(hit.key>=taskui::ExpandBase&&hit.key<taskui::FileBase)
        require(layout.hit((hit.rect.left+hit.rect.right)/2,(hit.rect.top+hit.rect.bottom)/2)==hit.key,"task swallowed disclosure input");
      // A completed task must not still have an active blue progress strip.
      for(const auto &hit:layout.hits) if(hit.row&&m.rows[hit.key-taskui::RowBase].completed)
        for(const auto &b:f.boxes) if(b.rect.top>=hit.rect.top&&b.rect.bottom<=hit.rect.bottom&&b.rect.right-b.rect.left>100)
          require(!(b.color.r==th::accent.r&&b.color.g==th::accent.g&&b.color.b==th::accent.b),"completed task has active progress");
      render_frame(canvas,text,f);
      bitmap(fs::path(argv[2])/(L"list-"+std::to_wstring(int(scale*96))+L"dpi-"+std::to_wstring(size.first)+L".bmp"),canvas);
    }
    std::cout<<"six deterministic native raster cases; selection geometry, nested hit testing, hover target ownership and completion rendering passed\n";
  } catch(const std::exception &e) {std::cerr<<e.what();return 1;}return 0;
}
