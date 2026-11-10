#include <infra/storage/plan_storage.hpp>
#include <windows.h>
#include <chrono>
#include <condition_variable>
#include <fstream>
#include <iostream>
#include <mutex>
#include <thread>

using namespace gsdl;
using namespace gsdl::storage;
namespace {
void require(bool value,const char* message){if(!value)throw std::runtime_error(message);}
template<class F> void rejects(F action,const char* message) {
    try{action();}catch(const StorageError&){return;}
    throw std::runtime_error(message);
}
std::string bytes(const std::filesystem::path& path){std::ifstream in(path,std::ios::binary);return {std::istreambuf_iterator<char>(in),{}};}
void put(const std::filesystem::path& path,std::string_view data){std::ofstream out(path,std::ios::binary);out.write(data.data(),static_cast<std::streamsize>(data.size()));require(out.good(),"test fixture write failed");}
std::string changed(std::string value,std::string_view find,std::string_view replacement){const auto p=value.find(find);require(p!=value.npos,"test replacement absent");value.replace(p,find.size(),replacement);return value;}
struct Fixture {
    std::filesystem::path directory;
    Fixture(){wchar_t root[MAX_PATH]{};require(GetTempPathW(MAX_PATH,root)!=0,"temp path failed");directory=std::filesystem::path(root)/(L"gsdl-storage-tests-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64()));require(std::filesystem::create_directory(directory),"test dir collision");}
    ~Fixture(){std::error_code ignored;std::filesystem::remove_all(directory,ignored);}
};
void codec_tests() {
    auto plan=make_internal_fixture();
    plan.outputDirectory=L"D:\\中文路径\\资料";
    plan.resources[0].expectedBytes=UINT64_MAX;
    const auto encoded=encode_plan(plan);
    require(decode_plan(encoded)==plan,"Chinese/uint64/null roundtrip changed data");
    require(encoded.find("session")==encoded.npos&&encoded.find("revision")==encoded.npos&&encoded.find("history")==encoded.npos,"runtime state leaked into plan");
    rejects([&]{decode_plan(changed(encoded,"\"schemaVersion\":1","\"schemaVersion\":2"));},"future version accepted");
    rejects([&]{decode_plan(changed(encoded,"\"schemaVersion\":1","\"schemaVersion\":1,\"schemaVersion\":1"));},"duplicate root key accepted");
    rejects([&]{decode_plan(changed(encoded,"\"provider\":","\"provider\":\"internal-fixture\",\"pro\\u0076ider\":"));},"escaped duplicate key accepted");
    rejects([&]{decode_plan(changed(encoded,"\"schemaVersion\":1","\"schemaVersion\":1,\"signedUrl\":\"https://bad/?sign=secret\""));},"unknown root field accepted");
    rejects([&]{decode_plan(changed(encoded,"\"included\":true","\"included\":\"true\""));},"string boolean accepted");
    rejects([&]{decode_plan(changed(encoded,"18446744073709551615","18446744073709551616"));},"overflow integer accepted");
    rejects([&]{decode_plan(changed(encoded,"18446744073709551615","1.0"));},"floating byte size accepted");
    rejects([&]{decode_plan(changed(encoded,"18446744073709551615","-1"));},"negative byte size accepted");
    rejects([&]{decode_plan(changed(encoded,"\"format\":\"SOG\"","\"format\":\"\\ud800\""));},"unpaired escaped surrogate accepted");
    rejects([&]{decode_plan(changed(encoded,"\"format\":\"SOG\"","\"format\":\"\\u0000\""));},"NUL accepted");
    auto badUtf8=encoded;badUtf8.insert(2,"\xc0\xaf");
    rejects([&]{decode_plan(badUtf8);},"malformed UTF8 accepted");
    rejects([&]{decode_plan(encoded+"false");},"trailing JSON accepted");
    rejects([&]{decode_plan(std::string(max_plan_bytes+1,' '));},"oversize input accepted");
    rejects([&]{decode_plan(std::string(18,'[')+"0"+std::string(18,']'));},"deep input accepted");
    auto unknown=changed(encoded,"\"outputName\":","\"unexpected\":null,\"outputName\":");
    rejects([&]{decode_plan(unknown);},"unknown nested field accepted");
    plan.resources[0].outputName=L"..\\escape.sog";
    rejects([&]{encode_plan(plan);},"invalid domain output name encoded");
    auto empty=make_empty_plan();
    empty.source.shareUrl=L"https://app.insta360.com/3dspace/detail/GS3D123?sign=secret";empty.source.captureId="GS3D123";
    rejects([&]{encode_plan(empty);},"signed public link persisted");
}
void publication_tests(const std::filesystem::path& directory) {
    const auto target=directory/L"中文计划.3dgsdl.json";
    auto first=make_internal_fixture();first.outputDirectory=L"D:\\旧目录";
    save_plan_atomic(target,first);
    require(read_plan(target)==first,"initial save/read failed");
    const auto original=bytes(target);
    auto next=first;next.outputDirectory=L"D:\\新目录";
    rejects([&]{save_plan_atomic(target,next);},"default save overwrote target");
    require(bytes(target)==original,"no-overwrite failure altered original");
    for(auto fault:{SaveFault::before_write,SaveFault::after_write,SaveFault::before_publish}) {
        rejects([&]{save_plan_atomic(target,next,{PublishMode::replace_or_create,fault});},"failure injection succeeded");
        require(bytes(target)==original,"injected failure changed original");
    }
    const auto foreign=directory/L".3dgsdl-foreign.tmp";put(foreign,"not ours");
    {
        HANDLE locked=CreateFileW(target.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr);
        require(locked!=INVALID_HANDLE_VALUE,"lock setup failed");
        try{rejects([&]{save_plan_atomic(target,next,{PublishMode::replace_or_create});},"replacement bypassed deny-delete handle");}
        catch(...){CloseHandle(locked);throw;}
        CloseHandle(locked);
    }
    require(bytes(target)==original,"locked replacement damaged original");
    require(SetFileAttributesW(target.c_str(),FILE_ATTRIBUTE_READONLY)!=FALSE,"set readonly failed");
    try{rejects([&]{save_plan_atomic(target,next,{PublishMode::replace_or_create});},"readonly replacement unexpectedly succeeded");}
    catch(...){SetFileAttributesW(target.c_str(),FILE_ATTRIBUTE_NORMAL);throw;}
    SetFileAttributesW(target.c_str(),FILE_ATTRIBUTE_NORMAL);
    require(bytes(target)==original,"readonly failure damaged original");
    rejects([&]{save_plan_atomic(directory/L"missing"/L"plan.3dgsdl.json",next);},"missing directory succeeded");
    require(bytes(foreign)=="not ours","foreign temp was removed");
    for(const auto& file:std::filesystem::directory_iterator(directory))require(file.path()==target||file.path()==foreign,"owned temporary leaked on expected failure");
    save_plan_atomic(target,next,{PublishMode::replace_or_create});
    require(read_plan(target)==next,"authorized atomic replacement failed");
    put(directory/L"future.3dgsdl.json",changed(encode_plan(next),"\"schemaVersion\":1","\"schemaVersion\":999"));
    rejects([&]{read_plan(directory/L"future.3dgsdl.json");},"future file opened");
}
void async_tests(const std::filesystem::path& directory) {
    std::mutex mutex;std::condition_variable condition;bool stopped{};unsigned notifications{};
    IoWorker worker([&]{std::lock_guard lock(mutex);++notifications;condition.notify_all();},[&]{std::lock_guard lock(mutex);stopped=true;condition.notify_all();},{8,120});
    DocumentStore document;require(bool(document.newPlan(true)),"fixture plan failed");
    auto plan=document.snapshot()->plan;plan.outputDirectory=L"D:\\S";require(bool(document.apply(plan)),"edit S failed");
    const auto path=directory/L"async.3dgsdl.json";
    const auto s=document.captureSave(path.native());
    require(worker.submit(SaveRequest{s,100,PublishMode::create_new}),"save S rejected");
    plan.outputDirectory=L"D:\\T";require(bool(document.apply(plan)),"edit T failed");
    {
        std::unique_lock lock(mutex);require(condition.wait_for(lock,std::chrono::seconds(5),[&]{return notifications>=1;}),"save S timeout");
    }
    auto results=worker.take_results();
    require(results.size()==1&&results[0].success()&&results[0].requestId==100,"save S result incorrect");
    require(results[0].capture.snapshot==s.snapshot&&read_plan(path)==s.snapshot->plan,"saved mutable latest instead of snapshot S");
    require(bool(document.acceptSaved(results[0].capture))&&document.dirty(),"S completion cleared T dirty");
    const auto t=document.captureSave(path.native());
    require(worker.submit(SaveRequest{t,101,PublishMode::replace_or_create}),"save T rejected");
    plan.outputDirectory=L"D:\\U";require(bool(document.apply(plan)),"edit U failed");
    const auto u=document.captureSave(path.native());
    require(worker.submit(SaveRequest{u,102,PublishMode::replace_or_create}),"save U rejected");
    const auto before=document.snapshot();
    require(worker.submit(ReadRequest{before->sessionId,103,before->revision,path}),"read queued rejected");
    worker.request_stop();
    require(!worker.submit(ReadRequest{}),"stopped worker accepted request");
    {
        std::unique_lock lock(mutex);require(condition.wait_for(lock,std::chrono::seconds(5),[&]{return stopped;}),"worker stop timeout");
    }
    results=worker.take_results();
    require(worker.stopped()&&results.size()==3,"drain lost accepted work");
    require(results[0].requestId==101&&results[1].requestId==102&&results[2].requestId==103,"same-target FIFO order changed");
    for(const auto& result:results)require(result.success(),"queued IO failed");
    require(read_plan(path)==u.snapshot->plan&&*results[2].plan==u.snapshot->plan,"old save overwrote latest");
    require(results[2].sessionId==before->sessionId&&results[2].capturedRevision==before->revision,"read stamp lost");
    require(bool(document.acceptSaved(results[0].capture))&&document.dirty(),"T completion cleared U dirty");
    require(bool(document.acceptSaved(results[1].capture))&&!document.dirty(),"U completion failed clean");

    bool detachedStopped{};
    {
        IoWorker detached({},[&]{std::lock_guard lock(mutex);detachedStopped=true;condition.notify_all();},{1,1000});
        require(detached.submit(ReadRequest{7,500,9,path}),"detached read rejected");
        require(!detached.submit(ReadRequest{7,501,9,path}),"bounded queue accepted extra");
        // Destructor must not wait for I/O and worker state outlives wrapper.
    }
    {std::unique_lock lock(mutex);require(condition.wait_for(lock,std::chrono::seconds(5),[&]{return detachedStopped;}),"detached state failed stop");}
}
}
int main() {
    try{Fixture fixture;codec_tests();publication_tests(fixture.directory);async_tests(fixture.directory);std::cout<<"strict_utf8_json=passed atomic_failure_preservation=passed fifo_snapshot_dirty=passed async_stop=passed\n";return 0;}
    catch(const StorageError& error){std::wcerr<<error.detail().message<<L"\n";return 1;}
    catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
