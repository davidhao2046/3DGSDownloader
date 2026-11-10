#include "apps/desktop/task_list_visual.hpp"
#include "apps/desktop/creation_visual.hpp"
#include <iostream>
#include <stdexcept>

namespace {
int checks{};
void require(bool ok,const char *message) {++checks;if(!ok)throw std::runtime_error(message);}
bool inside(g::Rect inner,g::Rect outer) {
  return inner.left>=outer.left && inner.top>=outer.top && inner.right<=outer.right && inner.bottom<=outer.bottom;
}
bool same(g::Rect a,g::Rect b) {
  return a.left==b.left&&a.top==b.top&&a.right==b.right&&a.bottom==b.bottom;
}
void bitmap(const fs::path &path,const g::SkiaCanvas &canvas) {
  auto pixels=canvas.pixels();std::vector<unsigned char> bgra(pixels.begin(),pixels.end());
  for(std::size_t i=0;i<bgra.size();i+=4) std::swap(bgra[i],bgra[i+2]);
  BITMAPFILEHEADER file{};file.bfType=0x4d42;file.bfOffBits=sizeof(file)+sizeof(BITMAPINFOHEADER);file.bfSize=file.bfOffBits+DWORD(bgra.size());
  BITMAPINFOHEADER info{};info.biSize=sizeof(info);info.biWidth=LONG(canvas.width());info.biHeight=-LONG(canvas.height());info.biPlanes=1;info.biBitCount=32;info.biSizeImage=DWORD(bgra.size());
  std::ofstream out(path,std::ios::binary);out.write((char*)&file,sizeof(file));out.write((char*)&info,sizeof(info));out.write((char*)bgra.data(),std::streamsize(bgra.size()));
  require(out.good(),"unable to write page board");
}
creationui::Model model(std::wstring_view state) {
  creationui::Model m;
  m.directoryText=L"D:\\下载\\3D时光舱";
  m.preview=L"子文件夹：时光舱_GS3D000000000000000000000000000000000";
  m.linkText=L"https://app.insta360.com/3dspace/detail/GS3D000000000000000000000000000000000";
  m.hint=L"已找到 5 个原文件，请选择下载内容。";
  m.files={{L"1_3DGS.sog",L"15.4 MB",true},{L"2_cameras.json",L"789 KB",false},
           {L"3_3DGS.voxel.zip",L"1.4 MB",false},{L"4_effect_1.mp4",L"12.1 MB",false},
           {L"5_effect_2.mp4",L"17.3 MB",false}};
  m.submitEnabled=true;m.recentEnabled=true;
  if(state==L"expanded") {m.expanded=true;m.files[3].checked=true;m.files[4].checked=true;}
  if(state==L"error") {
    m.files.clear();m.error=true;m.submitEnabled=false;m.focus=creationui::link;
    m.linkText=L"https://example.invalid/分享链接";
    m.hint=L"请输入影石 3D 时光舱公开链接；已输入的内容仍保留，可继续修改。";
    m.preview=L"解析后生成独立子文件夹";
  }
  if(state==L"zero-selection") {
    m.expanded=true;m.files[0].checked=false;m.submitEnabled=false;
    m.hint=L"请至少选择一个文件。";m.hover=creationui::submit;
  }
  return m;
}
taskui::TaskListVisualModel background() {
  taskui::TaskListVisualModel m;
  m.rows={{L"影石时光舱 · 0001…001",L"D:\\下载\\时光舱",L"已完成",L"1 个文件 · 15.4 MB · 已校验",{},false,false,true},
          {L"影石时光舱 · b728…e19",L"D:\\下载\\时光舱2",L"下载中",L"2/3 个文件 · effect_1.mp4 · 8.0 MB / 16.0 MB",.5,true,false,false},
          {L"影石时光舱 · c418…1ab",L"D:\\下载\\时光舱3",L"失败",L"网络连接中断；已完成文件保留，可重试",{},false,true,false}};
  m.totalCount=3;m.activeCount=1;m.completedCount=1;m.otherCount=1;m.active=false;
  m.status=L"任务列表已自动保存";
  return m;
}
void check_layout(float width,float height,const creationui::Model &m,const creationui::Layout &l) {
  require(inside(l.panel,{0,0,width,height}),"panel outside client");
  for(const auto &r:l.parts) require(inside(r,l.panel),"control outside panel");
  require(inside(l.hint,l.panel)&&inside(l.preview,l.panel),"hint outside panel");
  require(l.files.bottom<=l.footer.top,"resource area overlaps footer");
  for(std::size_t i=l.first;i<l.first+l.visible;++i)
    require(inside(l.file(i),l.files)&&l.file(i).bottom<=l.footer.top,"resource row overlaps footer");
  const auto &d=l.parts[creationui::directory],&r=l.parts[creationui::recent],&b=l.parts[creationui::browse];
  require(d.top==r.top&&d.top==b.top&&d.bottom==r.bottom&&d.bottom==b.bottom,"directory controls not aligned");
  require(d.right<r.left&&r.right<b.left,"directory control hit regions overlap");
  auto changed=m;changed.hover=creationui::submit;changed.pressed=creationui::submit;changed.focus=creationui::submit;changed.submitEnabled=!m.submitEnabled;
  auto other=creationui::layout(width,height,changed);
  require(same(l.panel,other.panel),"button state changes panel size");
  for(std::size_t i=0;i<l.parts.size();++i) require(same(l.parts[i],other.parts[i]),"button state moves field or action");
}
}
int wmain(int argc,wchar_t **argv) {
  if(argc!=3) return 2;
  try {
    tx::Rasterizer text;g::SkiaCanvas canvas(fs::absolute(argv[1]));fs::create_directories(argv[2]);
    for(float dpi:{1.f,1.5f,2.f}) for(auto size:{std::pair{1280,800},std::pair{960,640}})
      for(std::wstring_view state:{L"collapsed",L"expanded",L"error",L"zero-selection"}) {
        Frame f;f.width=unsigned(size.first*dpi);f.height=unsigned(size.second*dpi);f.dpi=dpi;
        taskui::render_task_list(f,background());
        f.overlayBoxes=f.boxes.size();f.overlayTexts=f.texts.size();
        f.boxes.push_back({{0,0,float(size.first),float(size.second)},gsdl::theme::creation::modalMask});
        auto m=model(state);auto l=creationui::render(f,text,m);
        check_layout(float(size.first),float(size.second),m,l);
        render_frame(canvas,text,f);
        bitmap(fs::path(argv[2])/(L"create-"+std::wstring(state)+L"-"+std::to_wstring(int(dpi*96))+L"dpi-"+std::to_wstring(size.first)+L".bmp"),canvas);
      }
    std::cout<<"page_visual_tests: "<<checks<<" checks passed; 24 native page captures\n";
  }catch(const std::exception &e){std::cerr<<e.what()<<'\n';return 1;}
  return 0;
}
