#pragma once
#include "ui_components.hpp"
#include "resources/theme/task_workspace_tokens.hpp"
#include <cmath>

// Immutable projections only: business state and history remain in PlanService.
namespace taskui {
namespace tl=gsdl::theme::taskList;
namespace tw=gsdl::theme::taskWorkspace;
enum Key {
  NewTask=2000,PasteLink,FilterAll,FilterActive,FilterCompleted,FilterOther,
  Details,Start,Cancel,Reveal,Settings,ViewAdded,
  ExpandBase=20000,FileBase=30000,RevealBase=40000,RowBase=100000
};
struct FileRow {
  std::wstring label;
  bool enabled{},completed{};
  std::wstring name,size,status;
  bool checked{};
};
struct TaskListVisualRow {
  std::wstring title,directory,status,progressText;
  std::optional<double> fraction;
  bool running{},failed{},completed{};
  std::wstring fullTitle;
  bool revealEnabled{};
};
struct TaskListVisualModel {
  std::vector<TaskListVisualRow> rows;
  std::size_t totalCount{},activeCount{},completedCount{},otherCount{},firstRow{};
  int selectedRow=-1,expandedRow=-1,filter{},focus=-1,hover=-1,pressed=-1;
  bool active=true,detailsEnabled{},startEnabled{},cancelEnabled{},revealEnabled{};
  bool expanded{},hiddenSelected{};
  std::vector<FileRow> files;
  std::wstring startLabel=L"开始下载",status;
};
struct TaskListHit {
  int key;g::Rect rect;std::wstring label;bool enabled=true,row{};
};
struct TaskListLayout {
  std::vector<TaskListHit> hits;g::Rect list{};std::size_t visibleRows{};
  int hit(float x,float y)const {
    // Controls inside a task have priority over its broad selection region.
    for(bool parent:{false,true}) for(const auto &h:hits)
      if(h.row==parent && x>=h.rect.left && x<h.rect.right && y>=h.rect.top && y<h.rect.bottom)
        return h.key;
    return -1;
  }
};
inline float operation_left(float right) {
  return right-tw::actionInset-tw::controlHeight-tw::actionGap-tw::actionWidth;
}
inline float status_left(float right) {
  return operation_left(right)-tw::statusGap-tw::statusWidth;
}
inline int row_for_key(const TaskListVisualModel &m,int key) {
  int row=-1;
  if(key>=RowBase)row=key-RowBase;
  else if(key>=RevealBase)row=key-RevealBase;
  else if(key>=ExpandBase&&key<FileBase)row=key-ExpandBase;
  else if(key==Start||key==Cancel||key==Reveal||key==Details)row=m.selectedRow;
  return row>=0&&row<int(m.rows.size())?row:-1;
}
inline TaskListLayout task_list_layout(float width,float height,const TaskListVisualModel &m) {
  TaskListLayout out;
  const float left=tl::sidebarWidth+tl::margin,right=width-tl::margin;
  const float footer=height-tl::footerHeight-tl::statusHeight;
  out.list={left,tl::headerHeight,right,footer-tl::buttonGap};
  auto add=[&](int key,g::Rect rect,std::wstring label,bool enabled=true,bool row=false) {
    out.hits.push_back({key,rect,std::move(label),enabled,row});
  };
  add(NewTask,{tl::sidebarInset,tw::headerTop,tl::sidebarWidth-tl::sidebarInset,
               tw::headerTop+tw::controlHeight},L"新建下载");
  add(PasteLink,{right-tl::statusWidth,tw::headerTop,right,tw::headerTop+tw::controlHeight},L"粘贴链接  Ctrl+V");
  const std::array<std::wstring,4> labels{L"全部任务",L"进行中",L"已完成",L"未完成"};
  for(int i=0;i<4;++i) {
    const float y=tl::filterTop+i*(tl::filterHeight+tl::filterGap);
    add(FilterAll+i,{tl::sidebarInset,y,tl::sidebarWidth-tl::sidebarInset,y+tl::filterHeight},labels[i]);
  }
  const float settingsBottom=height-tl::statusHeight-tl::sidebarInset;
  add(Settings,{tl::sidebarInset,settingsBottom-tw::controlHeight,
                tl::sidebarWidth-tl::sidebarInset,settingsBottom},L"目录设置");
  if(m.hiddenSelected) {
    out.list.bottom-=tw::footerActionHeight+tw::sectionGap;
    add(ViewAdded,{left,out.list.bottom+tw::sectionGap,left+tl::statusWidth,
                   out.list.bottom+tw::sectionGap+tw::footerActionHeight},L"查看已添加任务");
  }
  float y=out.list.top;
  for(std::size_t i=m.firstRow;i<m.rows.size();++i) {
    if(y+tl::rowHeight>out.list.bottom)break;
    const auto &row=m.rows[i];
    add(RowBase+int(i),{left,y,right,y+tl::rowHeight},row.fullTitle.empty()?row.title:row.fullTitle,true,true);
    const bool selected=m.selectedRow==int(i);
    const bool hovered=row_for_key(m,m.hover)==int(i);
    const bool focused=m.active&&row_for_key(m,m.focus)==int(i);
    const float actionTop=y+(tl::rowHeight-tw::controlHeight)/2;
    add(ExpandBase+int(i),{left+tw::arrowInset,y+(tl::rowHeight-tw::controlHeight)/2,
                          left+tw::arrowInset+tw::arrowWidth,y+(tl::rowHeight+tw::controlHeight)/2},
        m.expandedRow==int(i)&&m.expanded?L"收起文件":L"展开文件");
    if(selected) {
      const g::Rect primary{operation_left(right),actionTop,
                            operation_left(right)+tw::actionWidth,actionTop+tw::controlHeight};
      if(m.cancelEnabled)add(Cancel,primary,L"取消任务");
      else if(m.startEnabled)add(Start,primary,m.startLabel);
    }
    // The hit key carries this row's owner. Hover must never borrow the current
    // selection's action, and moving from the row onto its button keeps it shown.
    if(row.revealEnabled&&(selected||hovered||focused))
      add(RevealBase+int(i),{right-tw::actionInset-tw::controlHeight,actionTop,
                            right-tw::actionInset,actionTop+tw::controlHeight},
          L"打开文件夹："+(row.fullTitle.empty()?row.title:row.fullTitle));
    y+=tl::rowHeight+tl::rowGap;++out.visibleRows;
    // Selection alone never inserts rows. Explicit expansion is structural.
    if(m.expandedRow==int(i)&&m.expanded) {
      for(std::size_t n=0;n<m.files.size();++n) {
        if(y+tl::fileHeight>out.list.bottom)break;
        add(FileBase+int(n),{left+tl::rowInset,y,right,y+tl::fileHeight},m.files[n].label,m.files[n].enabled);
        y+=tl::fileHeight;
      }
      y+=tl::rowGap;
    }
  }
  return out;
}

inline TaskListLayout render_task_list(Frame &f,const TaskListVisualModel &m) {
  static thread_local tx::Rasterizer metrics;
  const float width=f.width/f.dpi,height=f.height/f.dpi;
  auto layout=task_list_layout(width,height,m);
  auto box=[&](g::Rect r,g::Rgba color,float radius=0.f){f.boxes.push_back({r,color,radius});};
  auto outline=[&](g::Rect r,g::Rgba color){
    const float line=1.f/f.dpi;
    box({r.left,r.top,r.right,r.top+line},color);box({r.left,r.bottom-line,r.right,r.bottom},color);
    box({r.left,r.top,r.left+line,r.bottom},color);box({r.right-line,r.top,r.right,r.bottom},color);
  };
  auto label=[&](g::Rect r,std::wstring_view text,float size,g::Rgba color,
                 ux::Align align=ux::Align::left,unsigned weight=400){
    ux::label(f,metrics,r,text,size,color,align,weight);
  };
  const float footer=height-tl::statusHeight;
  box({0,0,width,height},tl::contentSurface);
  box({0,0,tl::sidebarWidth,footer},tl::sidebarSurface);
  box({tl::sidebarWidth,0,tl::sidebarWidth+1.f/f.dpi,footer},th::border);
  label({layout.list.left,tw::headerTop,layout.list.right-tl::statusWidth,tw::headerTop+tw::headerHeight},
        L"下载任务",tl::titleSize,th::text,ux::Align::left,600);
  const auto titleMetrics=metrics.measure(L"下载任务",tl::titleSize,600);
  const float countX=layout.list.left+titleMetrics.width+tw::countGap;
  const auto countText=std::to_wstring(m.rows.size())+L" 项";
  const auto countMetrics=metrics.measure(countText,th::fontSmall);
  const float countTop=tw::headerTop+(tw::headerHeight-titleMetrics.height)/2+
                        titleMetrics.baseline-countMetrics.baseline;
  label({countX,countTop,layout.list.right-tl::statusWidth,countTop+countMetrics.height},
        countText,th::fontSmall,tl::secondaryText);
  box({0,footer,width,height},th::panel);
  box({tl::sidebarWidth,footer,width,footer+1.f/f.dpi},th::border);
  label({tl::sidebarInset,footer,width-tl::sidebarInset,height},
        m.status.empty()?L"粘贴影石 3D 时光舱公开链接，开始新的下载。":m.status,
        th::fontSmall,tl::secondaryText);
  // Parents first so nested controls always appear above the row surface.
  for(const auto &hit:layout.hits)if(hit.row) {
    const auto r=hit.rect;const auto &row=m.rows[hit.key-RowBase];
    const bool selected=m.selectedRow==hit.key-RowBase,hovered=row_for_key(m,m.hover)==hit.key-RowBase;
    box(r,selected?tl::rowSelected:hovered?th::form:tl::rowSurface);
    if(m.active&&m.focus==hit.key)outline(r,ux::c::focus);
    const float textLeft=r.left+tw::contentInset,textRight=status_left(r.right)-tw::statusGap;
    label({textLeft,r.top+tw::titleTop,textRight,r.top+tw::titleTop+tw::titleHeight},
          row.title,tl::rowTitleSize,th::text,ux::Align::left,600);
    label({textLeft,r.top+tw::metaTop,textRight,r.top+tw::metaTop+tw::metaHeight},
          row.progressText,th::fontSmall,row.failed?th::error:tl::secondaryText);
    const auto color=row.failed?th::error:row.running?ux::c::running:row.completed?tl::successText:tl::secondaryText;
    float stateX=status_left(r.right);
    if(row.completed) {
      ux::icon(f,{stateX,r.top+(tl::rowHeight-tw::iconSize)/2,stateX+tw::iconSize,
                   r.top+(tl::rowHeight+tw::iconSize)/2},ux::Icon::check,color);
      stateX+=tw::iconSize+tw::sectionGap;
    }
    label({stateX,r.top,operation_left(r.right)-tw::statusGap,r.bottom},row.status,th::body,color);
    if(row.running&&row.fraction&&std::isfinite(*row.fraction)) {
      const float py=r.bottom-tw::progressBottom;
      box({textLeft,py,textRight,py+tw::progressHeight},th::border);
      box({textLeft,py,textLeft+(textRight-textLeft)*float(std::clamp(*row.fraction,0.,1.)),
           py+tw::progressHeight},th::accent);
    }
  }
  for(const auto &hit:layout.hits) {
    if(hit.row)continue;
    const auto r=hit.rect;
    const bool focused=m.active&&m.focus==hit.key,hovered=hit.enabled&&m.hover==hit.key;
    ux::State state{hit.enabled,hovered,hovered&&m.pressed==hit.key,focused,false};
    if(hit.key>=ExpandBase&&hit.key<FileBase) {
      const bool expanded=m.expandedRow==hit.key-ExpandBase&&m.expanded;
      ux::button(f,metrics,r,L"",ux::ButtonRole::icon,state,
                 expanded?ux::Icon::chevron_down:ux::Icon::chevron_right);continue;
    }
    if(hit.key>=FileBase&&hit.key<RevealBase) {
      const auto &file=m.files[hit.key-FileBase];
      box(r,hovered?th::hover:th::panel);if(focused)outline(r,ux::c::focus);
      const float iconTop=r.top+(tl::fileHeight-tw::iconSize)/2;
      const g::Rect iconRect{r.left+tw::fileInset,iconTop,r.left+tw::fileInset+tw::iconSize,iconTop+tw::iconSize};
      const bool checked=file.checked||file.label.starts_with(L"☑");
      if(file.completed)ux::icon(f,iconRect,ux::Icon::check,tl::successText);
      else ux::checkbox(f,iconRect,checked,state);
      auto name=file.name;
      if(name.empty()) {
        name=file.label;
        if(!name.empty()&&(name[0]==L'☐'||name[0]==L'☑'||name[0]==L'✓'))name.erase(0,2);
        if(auto at=name.find(L" · ");at!=std::wstring::npos)name.resize(at);
      }
      const float statusX=r.right-tw::fileEndInset-tw::fileStatusWidth;
      const float sizeX=statusX-tw::fileColumnGap-tw::fileSizeWidth;
      label({r.left+tw::fileNameInset,r.top,sizeX-tw::fileColumnGap,r.bottom},name,th::body,th::text);
      label({sizeX,r.top,statusX-tw::fileColumnGap,r.bottom},file.size,th::fontSmall,tl::secondaryText,ux::Align::right);
      label({statusX,r.top,r.right-tw::fileEndInset,r.bottom},
            file.status.empty()?(file.completed?L"已下载 · 校验通过":L"未下载"):file.status,
            th::fontSmall,file.completed?tl::successText:tl::secondaryText);continue;
    }
    if(hit.key>=FilterAll&&hit.key<=FilterOther) {
      const bool selected=hit.key-FilterAll==m.filter;
      box(r,state.pressed?th::hover:selected?th::form:hovered?th::popup:tl::sidebarSurface,th::radius);
      if(selected)box({r.left,r.top+tw::sectionGap,r.left+tl::scrollWidth,r.bottom-tw::sectionGap},th::accent);
      if(focused)outline(r,ux::c::focus);
      label({r.left+tl::buttonTextInset,r.top,r.right-tw::countWidth-tl::buttonTextInset,r.bottom},hit.label,th::body,th::text);
      const std::array<std::size_t,4> counts{m.totalCount,m.activeCount,m.completedCount,m.otherCount};
      label({r.right-tw::countWidth-tl::buttonTextInset,r.top,r.right-tl::buttonTextInset,r.bottom},
            std::to_wstring(counts[hit.key-FilterAll]),th::fontSmall,tl::secondaryText,ux::Align::right);continue;
    }
    if(hit.key==Reveal||(hit.key>=RevealBase&&hit.key<RowBase))ux::button(f,metrics,r,L"",ux::ButtonRole::icon,state,ux::Icon::folder);
    else if(hit.key==Start)ux::button(f,metrics,r,hit.label,ux::ButtonRole::secondary,state);
    else if(hit.key==Cancel)ux::button(f,metrics,r,hit.label,ux::ButtonRole::quiet,state);
    else ux::button(f,metrics,r,hit.label,hit.key==NewTask?ux::ButtonRole::primary:ux::ButtonRole::quiet,state,
                    hit.key==NewTask?ux::Icon::plus:ux::Icon::none);
  }
  if(m.rows.empty()) {
    const float x=layout.list.left+tw::contentInset,y=layout.list.top+tl::emptyTop;
    label({x,y,x+tw::emptyWidth,y+tw::emptyHeight},m.totalCount?L"此分类下暂无任务":L"还没有下载任务",tl::rowTitleSize,th::text);
    label({x,y+tl::emptyLineGap,layout.list.right,y+tl::emptyLineGap+tw::emptyHeight},
          m.totalCount?L"选择“全部任务”查看其他下载。":L"点击“新建下载”，或按 Ctrl+V 粘贴公开分享链接。",th::body,tl::secondaryText);
  }
  if(m.rows.size()>layout.visibleRows&&layout.visibleRows) {
    const float track=layout.list.bottom-layout.list.top;
    const float thumb=std::max(tl::margin,track*float(layout.visibleRows)/m.rows.size());
    const auto maximum=m.rows.size()-layout.visibleRows;
    const float top=layout.list.top+(track-thumb)*float(std::min(m.firstRow,maximum))/maximum;
    box({width-tl::scrollInset-tl::scrollWidth,top,width-tl::scrollInset,top+thumb},th::border,th::radius);
  }
  return layout;
}
}
