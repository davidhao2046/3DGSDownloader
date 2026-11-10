#include "apps/desktop/ui_components.hpp"
#include <iostream>
#include <stdexcept>

namespace {
int checks{};
void require(bool value,const char *message) {
  ++checks;
  if(!value) throw std::runtime_error(message);
}
bool same(g::Rgba a,g::Rgba b) {
  return a.r==b.r && a.g==b.g && a.b==b.b && a.a==b.a;
}
double luminance(g::Rgba color) {
  auto channel=[](unsigned n) {double x=n/255.;return x<=.04045?x/12.92:std::pow((x+.055)/1.055,2.4);};
  return .2126*channel(color.r)+.7152*channel(color.g)+.0722*channel(color.b);
}
double contrast(g::Rgba a,g::Rgba b) {
  const auto x=luminance(a),y=luminance(b);
  return (std::max(x,y)+.05)/(std::min(x,y)+.05);
}
void bitmap(const fs::path &path,const g::SkiaCanvas &canvas) {
  auto pixels=canvas.pixels();
  std::vector<unsigned char> bgra(pixels.begin(),pixels.end());
  for(std::size_t i=0;i<bgra.size();i+=4) std::swap(bgra[i],bgra[i+2]);
  BITMAPFILEHEADER file{};file.bfType=0x4d42;file.bfOffBits=sizeof(file)+sizeof(BITMAPINFOHEADER);file.bfSize=file.bfOffBits+DWORD(bgra.size());
  BITMAPINFOHEADER info{};info.biSize=sizeof(info);info.biWidth=LONG(canvas.width());info.biHeight=-LONG(canvas.height());info.biPlanes=1;info.biBitCount=32;info.biSizeImage=DWORD(bgra.size());
  std::ofstream out(path,std::ios::binary);out.write((char*)&file,sizeof(file));out.write((char*)&info,sizeof(info));out.write((char*)bgra.data(),std::streamsize(bgra.size()));
  require(out.good(),"unable to write component board");
}
void contracts(tx::Rasterizer &metrics) {
  for(auto role:{ux::ButtonRole::primary,ux::ButtonRole::secondary,ux::ButtonRole::quiet,
                 ux::ButtonRole::icon,ux::ButtonRole::disclosure}) {
    for(int bits=0;bits<16;++bits) {
      ux::State state{true,bool(bits&1),bool(bits&2),bool(bits&4),bool(bits&8)};
      auto p=ux::colors(role,state);
      require(contrast(p.foreground,p.fill.a?p.fill:th::popup)>=4.5,"enabled text contrast below 4.5");
      state.enabled=false;
      auto disabled=ux::colors(role,state);
      auto resting=ux::colors(role,{false});
      require(same(disabled.fill,resting.fill)&&same(disabled.foreground,resting.foreground),"disabled reacts to hover/pressed");
    }
  }
  const g::Rect r{10,10,170,42};
  Frame normal;ux::button(normal,metrics,r,L"开始下载",ux::ButtonRole::primary);
  for(auto state:{ux::State{true,true},ux::State{true,true,true},ux::State{true,false,false,true},ux::State{false,true,true}}) {
    Frame changed;ux::button(changed,metrics,r,L"开始下载",ux::ButtonRole::primary,state);
    require(changed.texts.size()==1,"button has unexpected labels");
    require(changed.texts[0].x==normal.texts[0].x&&changed.texts[0].y==normal.texts[0].y,"state shifts label position");
  }
  // Checked combinations use the same geometry as a momentary control. The
  // palette contract above already covers all 16 hover/press/focus/check bits.
  Frame toggleNormal;ux::button(toggleNormal,metrics,r,L"下载内容",ux::ButtonRole::secondary);
  for(auto state:{ux::State{true,false,false,false,true},ux::State{true,true,false,false,true},
                  ux::State{true,false,false,true,true},ux::State{true,true,true,true,true},
                  ux::State{false,true,true,true,true}}) {
    Frame checked;ux::button(checked,metrics,r,L"下载内容",ux::ButtonRole::secondary,state);
    require(checked.texts.size()==1,"checked control has unexpected labels");
    require(checked.texts[0].x==toggleNormal.texts[0].x&&checked.texts[0].y==toggleNormal.texts[0].y,
            "checked combination shifts label position");
  }
  const auto &text=normal.texts[0];
  const auto m=metrics.measure(text.value,text.size,text.weight);
  require(std::abs(text.x+m.width*.5f-90.f)<.01f,"button not horizontally centered");
  require(std::abs(text.y+m.height*.5f-26.f)<.01f,"button not vertically centered");
  constexpr std::wstring_view longName=L"中文下载 e\u0301 \U0001F4C1 GS3D_1234567890.json";
  for(float width=1;width<240;width+=3) {
    const auto value=ux::ellipsize(metrics,longName,13,width);
    require(metrics.measure(value,13).width<=width+.01f,"ellipsis exceeds bounds");
    for(std::size_t i=0;i<value.size();++i) {
      if(value[i]>=0xd800&&value[i]<=0xdbff) {
        require(i+1<value.size()&&value[i+1]>=0xdc00&&value[i+1]<=0xdfff,"split UTF-16 pair");++i;
      } else require(!(value[i]>=0xdc00&&value[i]<=0xdfff),"orphan UTF-16 trail");
    }
  }
}
Frame board(tx::Rasterizer &metrics,float scale) {
  Frame f;f.width=unsigned(1280*scale);f.height=unsigned(800*scale);f.dpi=scale;
  ux::surface(f,{24,24,1256,776},th::popup,ux::c::edge,4);
  ux::label(f,metrics,{48,40,1232,72},L"3DGSDownloader · 原生组件状态板",ux::c::titleSize,th::text,ux::Align::left,ux::c::emphasisWeight);
  ux::label(f,metrics,{48,74,1232,100},L"同一组件规范用于真实窗口；固定命中区、字形度量与组合状态",13,ux::c::muted);
  const std::array<std::wstring_view,6> names{L"正常",L"悬停",L"按下",L"键盘焦点",L"禁用",L"禁用 + 悬停"};
  const std::array<ux::State,6> states{ux::State{},ux::State{true,true},ux::State{true,true,true},
      ux::State{true,false,false,true},ux::State{false},ux::State{false,true,true}};
  const std::array<ux::ButtonRole,5> roles{ux::ButtonRole::primary,ux::ButtonRole::secondary,
      ux::ButtonRole::quiet,ux::ButtonRole::icon,ux::ButtonRole::disclosure};
  const std::array<std::wstring_view,5> roleNames{L"主要操作",L"次要操作",L"轻量操作",L"图标操作",L"内容展开"};
  const std::array<std::wstring_view,5> labels{L"开始下载",L"选择文件夹",L"复制链接",L"",L"下载内容"};
  for(std::size_t col=0;col<names.size();++col) {
    const float x=200+float(col)*172;
    ux::label(f,metrics,{x,112,x+148,140},names[col],12,ux::c::muted,ux::Align::center);
    for(std::size_t row=0;row<roles.size();++row) {
      const float y=154+float(row)*60;
      auto r=g::Rect{x,y,x+148,y+32};
      if(row==3) r={x+58,y,x+90,y+32};
      ux::button(f,metrics,r,labels[row],roles[row],states[col],
          row==3?ux::Icon::close:row==4?ux::Icon::chevron_down:ux::Icon::none);
    }
  }
  for(std::size_t row=0;row<roles.size();++row) {
    const float y=154+float(row)*60;
    ux::label(f,metrics,{48,y,184,y+32},roleNames[row],13,th::text);
  }
  f.boxes.push_back({{48,450,1232,451},ux::c::edge});
  ux::label(f,metrics,{48,470,184,502},L"输入与目录",13,th::text);
  ux::surface(f,{200,470,800,502},th::window,ux::c::focus);
  ux::label(f,metrics,{212,470,788,502},L"D:\\下载\\影石时光舱",13,th::text);
  ux::button(f,metrics,{808,470,904,502},L"最近目录",ux::ButtonRole::secondary);
  ux::button(f,metrics,{912,470,1060,502},L"选择文件夹",ux::ButtonRole::secondary,{},ux::Icon::folder);
  ux::label(f,metrics,{200,510,1100,534},L"实际保存到：D:\\下载\\影石时光舱\\GS3D…000",12,ux::c::muted);
  ux::label(f,metrics,{48,550,184,582},L"选择与状态",13,th::text);
  ux::checkbox(f,{200,550,232,582},true);
  ux::label(f,metrics,{240,550,430,582},L"主模型 · 15.4 MB",13,th::text);
  ux::checkbox(f,{440,550,472,582},false);
  ux::label(f,metrics,{480,550,650,582},L"效果视频",13,th::text);
  ux::icon(f,{680,550,712,582},ux::Icon::check,ux::c::success);
  ux::label(f,metrics,{720,550,900,582},L"已下载 · 校验通过",13,ux::c::success);
  ux::checkbox(f,{200,590,232,622},true,ux::State{true,true,false,false,true});
  ux::label(f,metrics,{240,590,430,622},L"已选 + 悬停",13,th::text);
  ux::checkbox(f,{440,590,472,622},true,ux::State{true,false,false,true,true});
  ux::label(f,metrics,{480,590,650,622},L"已选 + 焦点",13,th::text);
  ux::checkbox(f,{680,590,712,622},true,ux::State{false,true,true,true,true});
  ux::label(f,metrics,{720,590,1040,622},L"已选 + 禁用 + 悬停",13,ux::c::disabledText);
  ux::label(f,metrics,{48,650,184,682},L"统一图标",13,th::text);
  const std::array icons{ux::Icon::close,ux::Icon::chevron_right,ux::Icon::chevron_down,
      ux::Icon::folder,ux::Icon::plus,ux::Icon::check,ux::Icon::download,ux::Icon::stop,ux::Icon::more,ux::Icon::history};
  for(std::size_t i=0;i<icons.size();++i) ux::button(f,metrics,{200+float(i)*64,650,232+float(i)*64,682},L"",ux::ButtonRole::icon,{},icons[i]);
  ux::label(f,metrics,{48,698,1232,722},L"文字：标题 18 / 600 · 正文 13 / 400 · 辅助 12 / 400 · 命中高度 32 DIP · 图标视框 16 DIP",13,ux::c::muted);
  ux::label(f,metrics,{48,734,1232,758},L"主按钮三态文字对比 ≥ 4.5:1；禁用不响应悬停/按下；原生文字塑形与省略使用同一度量",13,ux::c::muted);
  return f;
}
}
int wmain(int argc,wchar_t **argv) {
  if(argc!=3) return 2;
  try {
    tx::Rasterizer text;g::SkiaCanvas canvas(fs::absolute(argv[1]));
    contracts(text);fs::create_directories(argv[2]);
    for(float scale:{1.f,1.5f,2.f}) {
      auto frame=board(text,scale);render_frame(canvas,text,frame);
      bitmap(fs::path(argv[2])/(L"components-"+std::to_wstring(int(96*scale))+L"dpi.bmp"),canvas);
    }
    std::cout<<"component_visual_tests: "<<checks<<" checks passed; 3 native component boards\n";
  } catch(const std::exception &e) {std::cerr<<e.what()<<'\n';return 1;}
  return 0;
}
