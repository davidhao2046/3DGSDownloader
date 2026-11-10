#include "plan_storage.hpp"
#include <windows.h>
#include <bcrypt.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <variant>

namespace gsdl::storage {
namespace {
struct Handle {
    HANDLE value{INVALID_HANDLE_VALUE};
    explicit Handle(HANDLE h=INVALID_HANDLE_VALUE):value(h){}
    ~Handle(){if(value!=INVALID_HANDLE_VALUE)CloseHandle(value);}
    Handle(const Handle&)=delete;
    Handle& operator=(const Handle&)=delete;
    bool valid()const{return value!=INVALID_HANDLE_VALUE;}
    bool close(){if(!valid())return true;const auto h=value;value=INVALID_HANDLE_VALUE;return CloseHandle(h)!=FALSE;}
};
std::wstring system_message(DWORD code) {
    wchar_t* buffer{};
    const DWORD n=FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER|FORMAT_MESSAGE_FROM_SYSTEM|FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr,code,0,reinterpret_cast<wchar_t*>(&buffer),0,nullptr);
    std::wstring text=n?std::wstring(buffer,n):L"Windows 错误 "+std::to_wstring(code);
    if(buffer)LocalFree(buffer);
    while(!text.empty() && (text.back()==L'\r'||text.back()==L'\n'))text.pop_back();
    return text;
}
[[noreturn]] void fail(std::string stage, std::wstring action, DWORD code=GetLastError(), std::filesystem::path recovery={}) {
    throw StorageError({std::move(action)+L"："+system_message(code)+L"。未保存编辑保留；可更换位置或重试。",std::move(stage),code,std::move(recovery)});
}
std::filesystem::path normalized(const std::filesystem::path& path) {
    const auto& text=path.native();
    if(text.empty() || text.find(L'\0')!=std::wstring::npos || text.size()>32000)
        throw StorageError({L"计划文件路径为空、含 NUL 或过长。","path",0,{}});
    std::error_code error;
    auto result=std::filesystem::absolute(path,error).lexically_normal();
    if(error || result.filename().empty() || result.filename()==L"." || result.filename()==L"..")
        throw StorageError({L"计划文件路径无效。","path",static_cast<std::uint32_t>(error.value()),{}});
    return result;
}
std::wstring native_path(const std::filesystem::path& path) {
    auto value=path.native();
    if(value.starts_with(L"\\\\?\\"))return value;
    if(value.starts_with(L"\\\\"))return L"\\\\?\\UNC\\"+value.substr(2);
    return L"\\\\?\\"+value;
}
std::wstring random_suffix() {
    std::array<unsigned char,16> bytes{};
    if(BCryptGenRandom(nullptr,bytes.data(),static_cast<ULONG>(bytes.size()),BCRYPT_USE_SYSTEM_PREFERRED_RNG)!=0)
        throw StorageError({L"无法创建安全的唯一临时文件名；未写入计划。","random",0,{}});
    constexpr wchar_t hex[]=L"0123456789abcdef";
    std::wstring out;
    for(auto b:bytes){out+=hex[b>>4];out+=hex[b&15];}
    return out;
}
struct OwnedTemporary {
    std::filesystem::path path;
    bool owned{};
    ~OwnedTemporary(){if(owned)DeleteFileW(native_path(path).c_str());}
};
void injected(SaveFault value, SaveFault requested) {
    if(value==requested) fail("test_injection",L"模拟写入失败",ERROR_DISK_FULL);
}
}

DownloadPlan read_plan(const std::filesystem::path& requested) {
    const auto path=normalized(requested);
    Handle file(CreateFileW(native_path(path).c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_FLAG_SEQUENTIAL_SCAN,nullptr));
    if(!file.valid())fail("open_read",L"无法打开计划文件");
    LARGE_INTEGER size{};
    if(!GetFileSizeEx(file.value,&size))fail("read_size",L"无法读取计划大小");
    if(size.QuadPart<0 || static_cast<unsigned long long>(size.QuadPart)>max_plan_bytes)
        throw StorageError({L"计划超过 8 MiB 读取上限；原文件保持不变。","read_size",0,{}});
    std::string bytes(static_cast<std::size_t>(size.QuadPart),'\0');
    std::size_t offset{};
    while(offset<bytes.size()) {
        DWORD read{};
        const auto count=static_cast<DWORD>(std::min<std::size_t>(bytes.size()-offset,65536));
        if(!ReadFile(file.value,bytes.data()+offset,count,&read,nullptr))fail("read",L"读取计划失败");
        if(read==0)throw StorageError({L"计划文件在读取时发生截断；当前计划保持不变。","read",0,{}});
        offset+=read;
    }
    char extra{};DWORD count{};
    if(!ReadFile(file.value,&extra,1,&count,nullptr))fail("read",L"检查计划文件尾部失败");
    if(count!=0)throw StorageError({L"计划文件在读取时发生变化；请重试。","read",0,{}});
    return decode_plan(bytes);
}

void save_plan_atomic(const std::filesystem::path& requested, const DownloadPlan& plan, SaveOptions options) {
    // Serialization and validation precede all filesystem mutations.
    const auto bytes=encode_plan(plan);
    const auto path=normalized(requested);
    const auto target=native_path(path);
    const DWORD attributes=GetFileAttributesW(target.c_str());
    const DWORD attributeError=attributes==INVALID_FILE_ATTRIBUTES?GetLastError():ERROR_SUCCESS;
    const bool exists=attributes!=INVALID_FILE_ATTRIBUTES;
    if(exists && (attributes&FILE_ATTRIBUTE_DIRECTORY))fail("target",L"计划目标是目录",ERROR_DIRECTORY);
    if(!exists && attributeError!=ERROR_FILE_NOT_FOUND && attributeError!=ERROR_PATH_NOT_FOUND)
        fail("target",L"无法检查计划目标",attributeError);
    if(exists && options.mode==PublishMode::create_new)fail("target_exists",L"计划文件已存在；请另选文件名或明确覆盖",ERROR_FILE_EXISTS);
    OwnedTemporary temporary;
    Handle file;
    for(int attempt=0;attempt<8;++attempt) {
        temporary.path=path.parent_path()/(L".3dgsdl-"+random_suffix()+L".tmp");
        file.value=CreateFileW(native_path(temporary.path).c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
        if(file.valid()){temporary.owned=true;break;}
        if(GetLastError()!=ERROR_FILE_EXISTS && GetLastError()!=ERROR_ALREADY_EXISTS)fail("create_temp",L"无法在计划同目录创建临时文件");
    }
    if(!file.valid())fail("create_temp",L"无法取得唯一临时文件名",ERROR_FILE_EXISTS);
    injected(SaveFault::before_write,options.fault);
    std::size_t offset{};
    while(offset<bytes.size()) {
        DWORD written{};
        const auto count=static_cast<DWORD>(std::min<std::size_t>(bytes.size()-offset,65536));
        if(!WriteFile(file.value,bytes.data()+offset,count,&written,nullptr))fail("write_temp",L"写入临时计划失败");
        if(written==0)fail("write_temp",L"写入临时计划未取得进展",ERROR_WRITE_FAULT);
        offset+=written;
    }
    injected(SaveFault::after_write,options.fault);
    if(!FlushFileBuffers(file.value))fail("flush_temp",L"刷新临时计划失败");
    if(!file.close())fail("close_temp",L"关闭临时计划失败");
    injected(SaveFault::before_publish,options.fault);
    if(exists) {
        // A backup is necessary: documented ReplaceFile 1177 can move the old
        // name even when replacement fails. Keep it until successful publish.
        const auto backup=path.parent_path()/(L".3dgsdl-"+random_suffix()+L".rollback");
        const auto backupNative=native_path(backup);
        if(GetFileAttributesW(backupNative.c_str())!=INVALID_FILE_ATTRIBUTES)
            fail("backup_collision",L"恢复文件名已存在；未替换计划",ERROR_FILE_EXISTS);
        if(!ReplaceFileW(target.c_str(),native_path(temporary.path).c_str(),backupNative.c_str(),0,nullptr,nullptr)) {
            const DWORD error=GetLastError();
            const bool backupExists=GetFileAttributesW(backupNative.c_str())!=INVALID_FILE_ATTRIBUTES;
            const bool targetExists=GetFileAttributesW(target.c_str())!=INVALID_FILE_ATTRIBUTES;
            if(backupExists && !targetExists) {
                // Never overwrite a file another process created during failure.
                if(!MoveFileExW(backupNative.c_str(),target.c_str(),MOVEFILE_WRITE_THROUGH))
                    fail("rollback",L"替换失败且自动恢复未完成；原计划保留在恢复文件，请先恢复该文件",GetLastError(),backup);
            } else if(backupExists) {
                // Ambiguous external modification: preserve both for recovery.
                fail("replace",L"替换失败；原计划恢复副本已保留，请检查目标和恢复文件",error,backup);
            }
            fail("replace",L"替换计划失败；原计划文件保持",error);
        }
        temporary.owned=false;
        // Publication has succeeded; an undeletable backup must not turn this
        // into a false save failure. The old copy is safe to retain.
        DeleteFileW(backupNative.c_str());
    } else {
        // No REPLACE_EXISTING: if another writer wins, retain that file.
        if(!MoveFileExW(native_path(temporary.path).c_str(),target.c_str(),MOVEFILE_WRITE_THROUGH))
            fail("publish_new",L"发布新计划失败；已有目标不会被覆盖");
        temporary.owned=false;
    }
}

struct IoWorker::State {
    using Request=std::variant<ReadRequest,SaveRequest>;
    std::mutex mutex;
    std::condition_variable changed;
    std::deque<Request> requests;
    std::vector<IoResult> results;
    Notify onResult,onStopped;
    WorkerOptions options;
    bool stopping{},done{};
    std::size_t outstanding{};
    static void notify(const Notify& callback) noexcept {try{if(callback)callback();}catch(...){}}
    void run() noexcept {
        for(;;) {
            Request request;
            {
                std::unique_lock lock(mutex);
                changed.wait(lock,[&]{return stopping||!requests.empty();});
                if(requests.empty() && stopping)break;
                request=std::move(requests.front());requests.pop_front();
                if(options.testIoDelayMs)changed.wait_for(lock,std::chrono::milliseconds(options.testIoDelayMs),[&]{return stopping;});
            }
            IoResult result;
            try {
                if(auto* read=std::get_if<ReadRequest>(&request)) {
                    result.operation=Operation::read;result.sessionId=read->sessionId;result.requestId=read->requestId;
                    result.capturedRevision=read->capturedRevision;result.path=read->path;
                    result.plan=read_plan(read->path);
                } else {
                    const auto& save=std::get<SaveRequest>(request);
                    result.operation=Operation::save;result.sessionId=save.capture.sessionId;result.requestId=save.requestId;
                    result.capture=save.capture;result.path=save.capture.target;
                    if(!save.capture.snapshot || save.capture.snapshot->sessionId!=save.capture.sessionId)
                        throw StorageError({L"保存快照身份无效；未写入文件。","snapshot",0,{}});
                    result.capturedRevision=save.capture.snapshot->revision;
                    save_plan_atomic(result.path,save.capture.snapshot->plan,{save.mode,save.testFault});
                }
            } catch(const StorageError& error) { result.error=error.detail(); }
              catch(const std::exception&) { result.error=Error{L"存储操作发生异常；未保存编辑保留，请检查路径后重试。","exception",0,{}}; }
              catch(...) { result.error=Error{L"存储操作发生未知异常；未保存编辑保留。","exception",0,{}}; }
            {
                std::lock_guard lock(mutex);results.push_back(std::move(result));
            }
            notify(onResult);
        }
        {std::lock_guard lock(mutex);done=true;}
        notify(onStopped);
    }
};
IoWorker::IoWorker(Notify result,Notify stopped,WorkerOptions options):state_(std::make_shared<State>()) {
    if(options.maxPending==0 || options.maxPending>1024)throw std::invalid_argument("invalid storage queue limit");
    state_->onResult=std::move(result);state_->onStopped=std::move(stopped);state_->options=options;
    std::thread([state=state_]{state->run();}).detach();
}
IoWorker::~IoWorker(){request_stop();}
bool IoWorker::submit(ReadRequest request) {
    {std::lock_guard lock(state_->mutex);if(state_->stopping||state_->outstanding>=state_->options.maxPending)return false;
    state_->requests.emplace_back(std::move(request));++state_->outstanding;}
    state_->changed.notify_one();return true;
}
bool IoWorker::submit(SaveRequest request) {
    {std::lock_guard lock(state_->mutex);if(state_->stopping||state_->outstanding>=state_->options.maxPending)return false;
    state_->requests.emplace_back(std::move(request));++state_->outstanding;}
    state_->changed.notify_one();return true;
}
std::vector<IoResult> IoWorker::take_results() {
    std::lock_guard lock(state_->mutex);std::vector<IoResult> result;result.swap(state_->results);
    state_->outstanding-=result.size();return result;
}
void IoWorker::request_stop() noexcept {
    {std::lock_guard lock(state_->mutex);state_->stopping=true;}
    state_->changed.notify_one();
}
bool IoWorker::stopped() const noexcept {std::lock_guard lock(state_->mutex);return state_->done;}
} // namespace gsdl::storage
