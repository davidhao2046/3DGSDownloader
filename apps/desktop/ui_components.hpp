#pragma once
#include "render_host.hpp"
#include <cmath>

// Small immutable visual primitives. No state, actions, editing or history live
// here: the owning application supplies a projection and handles user intent.
namespace ux {
namespace c = gsdl::theme::control;
enum class Align { left, center, right };
enum class ButtonRole { primary, secondary, quiet, icon, disclosure };
enum class Icon { none, close, chevron_right, chevron_down, folder, plus, check,
                  download, stop, more, history };
struct State {
  bool enabled{true}, hovered{}, pressed{}, focused{}, checked{};
};
struct Colors { g::Rgba fill, foreground, border; };
inline Colors colors(ButtonRole role, State s) {
  const g::Rgba transparent{0,0,0,0};
  const bool framed=role==ButtonRole::primary || role==ButtonRole::secondary;
  if(!s.enabled) return {framed?c::disabledFill:transparent,c::disabledText,
                         framed?c::edge:transparent};
  if(role==ButtonRole::primary)
    return {s.pressed&&s.hovered?c::primaryPressed:s.hovered?c::primaryHover:c::primary,
            c::onPrimary,transparent};
  auto fill=s.pressed&&s.hovered?c::secondaryPressed:s.hovered?c::secondaryHover:
            s.checked?c::selected:framed?c::secondary:transparent;
  return {fill,th::text,framed?c::edge:transparent};
}
inline tx::Clip clip(const Frame &f, g::Rect r) {
  return {int(std::floor(r.left*f.dpi)),int(std::floor(r.top*f.dpi)),
          int(std::ceil(r.right*f.dpi)),int(std::ceil(r.bottom*f.dpi))};
}
inline void surface(Frame &f,g::Rect r,g::Rgba fill,g::Rgba edge={0,0,0,0},float radius=c::radius) {
  if(edge.a && !fill.a) {
    // A focus outline over a transparent/quiet button must not turn the whole
    // button into a bright solid rectangle. Keep its interior unchanged.
    f.boxes.push_back({{r.left,r.top,r.right,r.top+th::line},edge});
    f.boxes.push_back({{r.left,r.bottom-th::line,r.right,r.bottom},edge});
    f.boxes.push_back({{r.left,r.top+th::line,r.left+th::line,r.bottom-th::line},edge});
    f.boxes.push_back({{r.right-th::line,r.top+th::line,r.right,r.bottom-th::line},edge});
    return;
  }
  if(edge.a) {
    f.boxes.push_back({r,edge,radius});
    r={r.left+th::line,r.top+th::line,r.right-th::line,r.bottom-th::line};
    radius=std::max(0.f,radius-th::line);
  }
  if(fill.a) f.boxes.push_back({r,fill,radius});
}
inline std::wstring ellipsize(tx::Rasterizer &metrics,std::wstring_view value,
                              float size,float width,unsigned weight=400) {
  if(width<=0) return {};
  if(metrics.measure(value,size,weight).width<=width) return std::wstring(value);
  constexpr std::wstring_view dots=L"…";
  const float reserve=metrics.measure(dots,size,weight).width;
  if(reserve>width) return {};
  const auto n=metrics.fit_prefix(value,size,width-reserve,weight);
  return std::wstring(value.substr(0,n))+std::wstring(dots);
}
inline void label(Frame &f,tx::Rasterizer &metrics,g::Rect r,std::wstring_view value,
                  float size,g::Rgba color,Align align=Align::left,
                  unsigned weight=400,bool ellipsis=true) {
  if(r.right<=r.left || r.bottom<=r.top) return;
  auto visible=ellipsis?ellipsize(metrics,value,size,r.right-r.left,weight):std::wstring(value);
  const auto m=metrics.measure(visible,size,weight);
  const float x=align==Align::center?(r.left+r.right-m.width)*.5f:
                align==Align::right?r.right-m.width:r.left;
  // Use measured line height, including fallback CJK metrics. The same run is
  // drawn with the exact same size/weight so its baseline stays stable.
  const float y=r.top+(r.bottom-r.top-m.height)*.5f;
  f.texts.push_back({std::move(visible),x,y,size,color,clip(f,r),weight});
}
inline void icon(Frame &f,g::Rect r,Icon kind,g::Rgba color) {
  if(kind==Icon::none) return;
  const float size=c::iconSize;
  const float x=(r.left+r.right-size)*.5f,y=(r.top+r.bottom-size)*.5f;
  auto line=[&](float a,float b,float d,float e) {
    f.boxes.push_back({{x+a,y+b,x+d,y+e},color,0,c::iconStroke});
  };
  switch(kind) {
  case Icon::close: line(3,3,13,13);line(13,3,3,13);break;
  case Icon::chevron_right:line(6,4,10,8);line(10,8,6,12);break;
  case Icon::chevron_down:line(4,6,8,10);line(8,10,12,6);break;
  case Icon::plus:line(8,3,8,13);line(3,8,13,8);break;
  case Icon::check:line(3,8,6.5f,11.5f);line(6.5f,11.5f,13,4.5f);break;
  case Icon::folder:
    line(2,5,2,13);line(2,13,14,13);line(14,13,14,6);line(14,6,7,6);
    line(7,6,5,3);line(5,3,2,3);line(2,3,2,5);line(2,5,6,5);break;
  case Icon::download:
    line(8,2,8,10);line(4,7,8,11);line(8,11,12,7);
    line(3,11,3,14);line(3,14,13,14);line(13,14,13,11);break;
  case Icon::stop:
    line(4,4,12,4);line(12,4,12,12);line(12,12,4,12);line(4,12,4,4);break;
  case Icon::more:
    for(float dot:{3.f,8.f,13.f}) f.boxes.push_back({{x+dot-1,y+7,x+dot+1,y+9},color,1});break;
  case Icon::history:
    line(3,4,6,2);line(6,2,11,3);line(11,3,14,7);line(14,7,12,12);
    line(12,12,7,14);line(7,14,3,11);line(3,4,3,8);line(3,8,7,8);
    line(8,5,8,8);line(8,8,11,9);break;
  default:break;
  }
}
inline void button(Frame &f,tx::Rasterizer &metrics,g::Rect r,std::wstring_view value,
                   ButtonRole role,State state={},Icon symbol=Icon::none) {
  auto palette=colors(role,state);
  if(state.focused&&state.enabled) {
    surface(f,r,palette.fill,c::focus);
  } else surface(f,r,palette.fill,palette.border);
  if(role==ButtonRole::icon) {icon(f,r,symbol,palette.foreground);return;}
  auto content=g::Rect{r.left+c::inset,r.top,r.right-c::inset,r.bottom};
  if(symbol!=Icon::none) {
    if(role!=ButtonRole::disclosure) {
      const float available=std::max(0.f,content.right-content.left-c::iconSize-c::gap);
      const auto visible=ellipsize(metrics,value,c::bodySize,available,
          role==ButtonRole::primary?c::emphasisWeight:c::normalWeight);
      const float labelWidth=metrics.measure(visible,c::bodySize,
          role==ButtonRole::primary?c::emphasisWeight:c::normalWeight).width;
      content.left=(r.left+r.right-labelWidth-c::iconSize-c::gap)*.5f;
    }
    auto ir=g::Rect{content.left,r.top,content.left+c::iconSize,r.bottom};
    icon(f,ir,symbol,palette.foreground);content.left+=c::iconSize+c::gap;
  }
  const auto align=(role==ButtonRole::disclosure || symbol!=Icon::none)?Align::left:Align::center;
  label(f,metrics,content,value,c::bodySize,palette.foreground,align,
        role==ButtonRole::primary?c::emphasisWeight:c::normalWeight);
}
inline void checkbox(Frame &f,g::Rect r,bool checked,State state={}) {
  const auto color=state.enabled?th::text:c::disabledText;
  const float x=r.left+(r.right-r.left-c::iconSize)*.5f;
  const float y=r.top+(r.bottom-r.top-c::iconSize)*.5f;
  surface(f,{x,y,x+c::iconSize,y+c::iconSize},!state.enabled?c::disabledFill:checked?c::primary:th::window,
          state.focused&&state.enabled?c::focus:c::edge,2);
  if(checked) icon(f,{x,y,x+c::iconSize,y+c::iconSize},Icon::check,color);
}
} // namespace ux
