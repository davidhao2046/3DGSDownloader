#pragma once
#include "ui_components.hpp"
#include "resources/theme/creation_geometry.hpp"

namespace creationui {
namespace c=gsdl::theme::creation;
enum Part { link, directory, browse, recent, disclosure, cancel, submit, close, partCount };
inline constexpr int fileBase=100;
struct File {std::wstring name,size;bool checked{};};
struct Model {
  std::wstring linkText,directoryText,preview,hint,selectionSummary;
  bool expanded{},busy{},error{},submitEnabled{},recentEnabled{},renderFields{true};
  int focus=-1,hover=-1,pressed=-1;
  std::size_t firstFile{};
  std::vector<File> files;
};
struct Layout {
  g::Rect panel,header,footer,hint,preview,files;
  std::array<g::Rect,partCount> parts{};
  std::size_t first{},visible{};
  g::Rect file(std::size_t i) const {const float y=files.top+float(i-first)*c::fileHeight;return {files.left,y,files.right,y+c::fileHeight};}
};
inline Layout layout(float width,float height,const Model &m) {
  Layout l;
  const auto count=m.expanded?m.files.size():0;
  l.visible=std::min(count,std::size_t(std::max(0.f,std::floor((height-c::minimumTop*2-c::collapsedHeight)/c::fileHeight))));
  l.first=std::min(m.firstFile,count-l.visible);
  const float w=c::width,h=c::collapsedHeight+float(l.visible)*c::fileHeight;
  const float x=(width-w)/2,y=std::max(c::minimumTop,(height-h)/2);
  l.panel={x,y,x+w,y+h};l.header={x,y,x+w,y+c::headerHeight};
  l.footer={x,y+h-c::footerHeight,x+w,y+h};
  l.parts[link]={x+c::inset,y+c::linkTop,x+c::right,y+c::linkTop+c::controlHeight};
  l.hint={x+c::inset,y+c::hintTop,x+c::right,y+c::hintTop+c::hintHeight};
  l.parts[directory]={x+c::inset,y+c::directoryTop,x+c::directoryRight,y+c::directoryTop+c::controlHeight};
  l.parts[recent]={x+c::recentLeft,y+c::directoryTop,x+c::recentRight,y+c::directoryTop+c::controlHeight};
  l.parts[browse]={x+c::browseLeft,y+c::directoryTop,x+c::right,y+c::directoryTop+c::controlHeight};
  l.preview={x+c::inset,y+c::previewTop,x+c::right,y+c::previewTop+c::previewHeight};
  l.parts[disclosure]={x+c::inset,y+c::disclosureTop,x+c::right,y+c::disclosureTop+c::controlHeight};
  l.files={x+c::inset,y+c::filesTop,x+c::right,y+c::filesTop+float(l.visible)*c::fileHeight};
  l.parts[cancel]={x+c::cancelLeft,l.footer.top+c::actionInset,x+c::cancelRight,l.footer.top+c::actionInset+c::controlHeight};
  l.parts[submit]={x+c::submitLeft,l.footer.top+c::actionInset,x+c::right,l.footer.top+c::actionInset+c::controlHeight};
  l.parts[close]={x+c::closeLeft,y+c::titleTop,x+c::right,y+c::titleTop+c::controlHeight};
  return l;
}
inline Layout render(Frame &f,tx::Rasterizer &metrics,const Model &m) {
  const auto l=layout(f.width/f.dpi,f.height/f.dpi,m);
  const auto x=l.panel.left,y=l.panel.top;
  f.boxes.push_back({{l.panel.left-c::borderWidth,l.panel.top-c::borderWidth,l.panel.right+c::borderWidth,l.panel.bottom+c::borderWidth},c::panelBorder,c::panelBorderRadius});
  f.boxes.push_back({l.panel,c::panelFill,c::panelRadius});
  f.boxes.push_back({{x+c::inset,l.header.bottom,x+c::right,l.header.bottom+c::borderWidth},c::separator});
  f.boxes.push_back({{x+c::inset,l.footer.top,x+c::right,l.footer.top+c::borderWidth},c::separator});
  ux::label(f,metrics,{x+c::inset,y+c::titleTop,x+c::titleRight,y+c::titleTop+c::titleHeight},L"新建下载",c::titleSize,c::foreground,ux::Align::left,c::titleWeight);
  ux::label(f,metrics,{x+c::inset,y+c::linkLabelTop,x+c::right,y+c::linkLabelTop+c::labelHeight},L"公开分享链接",c::bodySize,c::foreground);
  ux::label(f,metrics,{x+c::inset,y+c::directoryLabelTop,x+c::right,y+c::directoryLabelTop+c::labelHeight},L"保存到",c::bodySize,c::foreground);
  ux::label(f,metrics,l.hint,m.hint,c::hintSize,m.error?c::errorForeground:c::secondaryForeground);
  ux::label(f,metrics,l.preview,m.preview,c::hintSize,c::secondaryForeground);
  auto state=[&](int key,bool enabled=true){return ux::State{enabled,m.hover==key,m.pressed==key&&m.hover==key,m.focus==key};};
  ux::button(f,metrics,l.parts[close],L"",ux::ButtonRole::icon,state(close),ux::Icon::close);
  ux::button(f,metrics,l.parts[recent],L"最近",ux::ButtonRole::secondary,state(recent,m.recentEnabled));
  ux::button(f,metrics,l.parts[browse],L"选择文件夹",ux::ButtonRole::secondary,state(browse));
  ux::button(f,metrics,l.parts[cancel],L"取消",ux::ButtonRole::quiet,state(cancel));
  const auto selected=std::count_if(m.files.begin(),m.files.end(),[](auto &file){return file.checked;});
  const auto submitText=m.busy?L"正在解析…":m.files.empty()?L"开始下载":L"开始下载 ("+std::to_wstring(selected)+L")";
  ux::button(f,metrics,l.parts[submit],submitText,ux::ButtonRole::primary,state(submit,m.submitEnabled));
  const auto content=m.files.empty()?L"下载内容 · 默认主模型和相机数据":m.selectionSummary.empty()?L"下载内容 · 已选 "+std::to_wstring(selected)+L" / "+std::to_wstring(m.files.size()):L"下载内容 · "+m.selectionSummary;
  ux::button(f,metrics,l.parts[disclosure],content,ux::ButtonRole::disclosure,state(disclosure),m.expanded?ux::Icon::chevron_down:ux::Icon::chevron_right);
  for(std::size_t i=l.first;i<l.first+l.visible;++i) {
    const auto r=l.file(i);const auto &file=m.files[i];
    if(m.focus==fileBase+int(i)||m.hover==fileBase+int(i)) f.boxes.push_back({r,c::rowHover,c::fileHoverRadius});
    const g::Rect check{r.left+c::checkboxInset,r.top+c::checkboxTop,r.left+c::checkboxInset+c::checkboxSize,r.top+c::checkboxTop+c::checkboxSize};
    f.boxes.push_back({check,c::checkboxBorder,c::checkboxRadius});
    f.boxes.push_back({{check.left+c::borderWidth,check.top+c::borderWidth,check.right-c::borderWidth,check.bottom-c::borderWidth},c::checkboxFill,c::checkboxInnerRadius});
    if(file.checked) ux::icon(f,check,ux::Icon::check,c::checkboxMark);
    ux::label(f,metrics,{r.left+c::fileNameInset,r.top,r.right-c::fileNameRightReserve,r.bottom},file.name,c::bodySize,c::foreground);
    ux::label(f,metrics,{r.right-c::fileSizeRightReserve,r.top,r.right-c::fileSizeInset,r.bottom},file.size,c::hintSize,c::secondaryForeground,ux::Align::right);
  }
  if(m.renderFields) for(auto key:{link,directory}) {
    const auto r=l.parts[key];
    f.boxes.push_back({r,m.focus==key?c::fieldFocus:c::fieldBorder,c::fieldRadius});
    f.boxes.push_back({{r.left+c::borderWidth,r.top+c::borderWidth,r.right-c::borderWidth,r.bottom-c::borderWidth},c::fieldFill,c::fieldInnerRadius});
    const auto value=key==link?m.linkText:m.directoryText;
    ux::label(f,metrics,{r.left+c::fieldTextInset,r.top,r.right-c::fieldTextInset,r.bottom},value.empty()?L"粘贴影石 3D 时光舱公开分享链接":value,c::bodySize,value.empty()?c::placeholderForeground:c::foreground);
  }
  return l;
}
} // namespace creationui
