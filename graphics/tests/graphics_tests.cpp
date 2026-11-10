#include <gsdl/graphics/skia_canvas.hpp>
#include <gsdl/graphics/win32_presenter.hpp>
#include <chrono>
#include <future>
#include <iostream>
#include <thread>
#include <atomic>

using namespace gsdl::graphics;
using namespace std::chrono_literals;
namespace {
void require(bool x, const char* why) { if (!x) throw std::runtime_error(why); }
std::atomic<unsigned> pings{};
std::atomic<unsigned> changes{};
LRESULT CALLBACK procedure(HWND w, UINT m, WPARAM a, LPARAM b) {
    if(m == WM_APP + 1) { ++pings; return 0; }
    if(m == WM_APP + 2) {
        SetWindowPos(w,nullptr,0,0,static_cast<int>(a),static_cast<int>(b),SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE);
        ++changes; return 0;
    }
    if(m == WM_APP + 3) {
        ShowWindow(w,a ? SW_SHOWMINNOACTIVE : SW_SHOWNOACTIVATE);
        ShowWindow(w,SW_HIDE);++changes;return 0;
    }
    return DefWindowProcW(w,m,a,b);
}
}
int wmain(int argc, wchar_t** argv) {
    if(argc != 2) return 2;
    WNDCLASSW wc{};wc.lpfnWndProc=procedure;wc.hInstance=GetModuleHandleW(nullptr);wc.lpszClassName=L"GsdlGraphicsVerification";
    RegisterClassW(&wc);
    HWND hwnd=CreateWindowW(wc.lpszClassName,L"Graphics verification",WS_POPUP,0,0,37,23,nullptr,nullptr,wc.hInstance,nullptr);
    if(!hwnd) return 3;
    std::promise<void> done; auto completed=done.get_future();
    std::exception_ptr error; const DWORD ui_thread=GetCurrentThreadId();
    std::thread worker([&]{
      try {
        require(GetCurrentThreadId()!=ui_thread,"render work ran on UI thread");
        SkiaCanvas canvas(std::filesystem::absolute(argv[1]));canvas.resize(17,19);canvas.clear({18,18,18});
        canvas.rect({0,0,8,19},{220,20,40});
        canvas.save();canvas.clip({8,0,17,19});canvas.rect({0,0,17,19},{30,70,180});canvas.restore();
        const auto pixels=canvas.pixels();
        require(pixels[0]==220 && pixels[1]==20 && pixels[2]==40 && pixels[3]==255,"Skia RGBA order mismatch");
        const auto i=16*4;require(pixels[i]==30 && pixels[i+2]==180,"Skia clip mismatch");
        bool rejected=false;try{canvas.restore();}catch(...){rejected=true;}require(rejected,"unbalanced restore accepted");
        Win32Presenter presenter(hwnd);
        Rgba8Frame frame{canvas.width(),canvas.height(),{pixels.begin(),pixels.end()}};
        // Explicitly model slow rendering. UI must keep pumping while this waits.
        PostMessageW(hwnd,WM_APP+1,0,0);std::this_thread::sleep_for(200ms);
        require(pings.load()>0,"UI did not pump during slow render");
        const auto present_ready=[&] {
            const auto deadline=std::chrono::steady_clock::now()+5s;
            PresentResult result;
            do {
                result=presenter.present(frame,true);
                if(result.status!=PresentStatus::retry) return result;
                std::this_thread::sleep_for(10ms);
            } while(std::chrono::steady_clock::now()<deadline);
            throw std::runtime_error("presentation retry deadline exceeded");
        };
        const auto mutate_window=[&](UINT message, WPARAM w, LPARAM l) {
            const auto before=changes.load();PostMessageW(hwnd,message,w,l);
            const auto deadline=std::chrono::steady_clock::now()+3s;
            while(changes.load()==before && std::chrono::steady_clock::now()<deadline) std::this_thread::sleep_for(1ms);
            require(changes.load()>before,"UI mutation not serviced");
        };
        auto result=present_ready();
        require(result.status==PresentStatus::presented && result.readback_verified,"Vulkan readback/presentation failed");
        require(result.width==37&&result.height==23,"initial extent mismatch");
        auto generation=result.swapchain_generation;
        for(const auto size:{std::pair{53,31},std::pair{29,17}}) {
            mutate_window(WM_APP+2,size.first,size.second);presenter.resize();result=present_ready();
            require(result.status==PresentStatus::presented && result.readback_verified,"resized readback failed");
            require(result.width==size.first&&result.height==size.second&&result.swapchain_generation>generation,"resize did not recreate correctly");
            generation=result.swapchain_generation;
        }
        mutate_window(WM_APP+3,1,0);result=presenter.present(frame,true);
        require(result.status==PresentStatus::minimized&&!result.readback_verified,"minimized window reported a successful frame");
        mutate_window(WM_APP+3,0,0);presenter.resize();result=present_ready();
        require(result.status==PresentStatus::presented&&result.readback_verified,"restored readback failed");
        const auto info=presenter.diagnostics();
        std::cout<<"device="<<info.device_name<<"\nloader="<<info.loader_path<<"\nformat="<<info.swapchain_format<<"\n";
        std::cout<<"render_thread="<<GetCurrentThreadId()<<"\nui_thread="<<ui_thread<<"\nreadback=true\nresize_minimize_restore=true\n";
      }catch(...){error=std::current_exception();}
      done.set_value();
    });
    while(completed.wait_for(0ms)!=std::future_status::ready){
      MSG m;while(PeekMessageW(&m,nullptr,0,0,PM_REMOVE)){TranslateMessage(&m);DispatchMessageW(&m);}
      MsgWaitForMultipleObjects(0,nullptr,FALSE,5,QS_ALLINPUT);
    }
    // Harness only: thread already finished all graphics destruction; app shutdown
    // uses an asynchronous completion/lifetime protocol instead.
    worker.join();DestroyWindow(hwnd);
    if(error){try{std::rethrow_exception(error);}catch(const std::exception& e){std::cerr<<e.what()<<'\n';}return 1;}
    return 0;
}
