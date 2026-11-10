#include "plan.hpp"
#include <Windows.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <ctime>
#include <cwctype>
#include <limits>
#include <random>
#include <set>
#include <stdexcept>

namespace gsdl {
namespace {
Result fail(std::wstring text){return {false,false,std::move(text)};}
std::atomic<std::uint64_t> sessions{1};
std::uint64_t next_session(){const auto id=sessions.fetch_add(1);if(!id||id==UINT64_MAX)throw std::overflow_error("Session identity exhausted");return id;}
std::string now(){const auto value=std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());std::tm time{};gmtime_s(&time,&value);char result[32]{};std::strftime(result,sizeof(result),"%Y-%m-%dT%H:%M:%SZ",&time);return result;}
std::string uuid(){std::random_device random;std::array<unsigned char,16> bytes{};for(auto& b:bytes)b=static_cast<unsigned char>(random());bytes[6]=(bytes[6]&15)|64;bytes[8]=(bytes[8]&63)|128;std::string result;for(std::size_t i=0;i<bytes.size();++i){if(i==4||i==6||i==8||i==10)result+='-';result+="0123456789abcdef"[bytes[i]>>4];result+="0123456789abcdef"[bytes[i]&15];}return result;}
bool ascii_id(std::string_view text,std::size_t max=128){if(text.empty()||text.size()>max)return false;return std::all_of(text.begin(),text.end(),[](unsigned char c){return (c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='-'||c=='_'||c=='.';});}
bool valid_uuid(std::string_view s){if(s.size()!=36)return false;for(std::size_t i=0;i<s.size();++i){if(i==8||i==13||i==18||i==23){if(s[i]!='-')return false;}else if(!((s[i]>='0'&&s[i]<='9')||(s[i]>='a'&&s[i]<='f')||(s[i]>='A'&&s[i]<='F')))return false;}return true;}
bool timestamp(std::string_view s){
    if(s.size()!=20)return false;
    for(std::size_t i=0;i<s.size();++i){
        const char expected=i==4||i==7?'-':i==10?'T':i==13||i==16?':':i==19?'Z':0;
        if(expected){if(s[i]!=expected)return false;}else if(s[i]<'0'||s[i]>'9')return false;
    }
    const auto number=[&](std::size_t start,std::size_t count){int result{};for(std::size_t i=start;i<start+count;++i)result=result*10+s[i]-'0';return result;};
    const int year=number(0,4);
    const std::chrono::year_month_day date{std::chrono::year{year},std::chrono::month{static_cast<unsigned>(number(5,2))},std::chrono::day{static_cast<unsigned>(number(8,2))}};
    return year>0&&date.ok()&&number(11,2)<24&&number(14,2)<60&&number(17,2)<60;
}
bool space(wchar_t c){return c==L' '||c==0x0085||c==0x00a0||c==0x1680||(c>=0x2000&&c<=0x200a)||c==0x2028||c==0x2029||c==0x202f||c==0x205f||c==0x3000;}
bool text(std::wstring_view s,std::size_t max,bool allow_empty=false){if((s.empty()&&!allow_empty)||s.size()>max)return false;bool visible=false;for(std::size_t i=0;i<s.size();++i){auto c=s[i];if(c<32||c==127)return false;if(c>=0xd800&&c<=0xdbff){if(++i>=s.size()||s[i]<0xdc00||s[i]>0xdfff)return false;}else if(c>=0xdc00&&c<=0xdfff)return false;if(!space(c))visible=true;}return (allow_empty&&s.empty())||visible;}
std::wstring folded(std::wstring_view s){std::wstring out(s);for(auto& c:out)c=std::towupper(c);return out;}
bool reserved(std::wstring_view name){auto stem=name.substr(0,name.find(L'.'));while(!stem.empty()&&stem.back()==L' ')stem.remove_suffix(1);const auto n=folded(stem);if(n==L"CON"||n==L"PRN"||n==L"AUX"||n==L"NUL"||n==L"CONIN$"||n==L"CONOUT$")return true;if(n.size()==4&&(n.starts_with(L"COM")||n.starts_with(L"LPT"))&&((n[3]>=L'1'&&n[3]<=L'9')||n[3]==0x00b9||n[3]==0x00b2||n[3]==0x00b3))return true;return false;}
struct WindowsNameLess { bool operator()(const std::wstring& a,const std::wstring& b)const{return CompareStringOrdinal(a.data(),static_cast<int>(a.size()),b.data(),static_cast<int>(b.size()),TRUE)==CSTR_LESS_THAN;} };
bool matches(const SaveCapture& a,const SaveCapture& b){return a.sessionId==b.sessionId&&a.saveId==b.saveId&&a.snapshot==b.snapshot&&a.target==b.target;}
}
Result validate_output_name(std::wstring_view name){
    if(!text(name,240))return fail(L"文件名不能为空、只含空格、超长或包含非法 Unicode/控制字符");
    if(name==L"."||name==L".."||name.find_first_of(L"\\/:*?\"<>|")!=name.npos||name.back()==L'.'||name.back()==L' ')return fail(L"文件名不能包含路径分隔符、保留字符或以点/空格结尾");
    if(reserved(name))return fail(L"此文件名是 Windows 保留名称，请更换名称");
    return {};
}
Result validate_output_directory(std::wstring_view path){
    if(path.empty())return {};
    if(!text(path,32700)||path.find_first_of(L"<>\"|?*")!=path.npos)return fail(L"目录包含非法字符、非法 Unicode 或过长");
    std::size_t start{};
    if(path.size()>=3&&((path[0]>=L'A'&&path[0]<=L'Z')||(path[0]>=L'a'&&path[0]<=L'z'))&&path[1]==L':'&&(path[2]==L'\\'||path[2]==L'/'))start=3;
    else if(path.starts_with(L"\\\\")){start=2;const auto slash=path.find_first_of(L"\\/",start);if(slash==path.npos||slash==start||slash+1==path.size())return fail(L"UNC 目录必须包含服务器和共享名称");}
    else return fail(L"请输入 Windows 绝对目录路径");
    for(std::size_t begin=start;begin<path.size();){auto end=path.find_first_of(L"\\/",begin);if(end==path.npos)end=path.size();const auto part=path.substr(begin,end-begin);if(part.empty())return fail(L"目录不能包含连续分隔符");if(auto r=validate_output_name(part);!r)return fail(L"目录段无效："+r.error);begin=end+1;}
    return {};
}
Result canonical_share_url(std::wstring_view input,Source& result){
    if(input.empty()){result=Source{};return {};}
    if(!text(input,4096))return fail(L"分享链接包含非法字符或过长");
    constexpr std::wstring_view prefix=L"https://app.insta360.com/3dspace/detail/";
    if(!input.starts_with(prefix))return fail(L"请输入影石 3D 时光舱 HTTPS 分享链接");
    const auto base=input.substr(0,input.find_first_of(L"?#"));const auto id=base.substr(prefix.size());
    if(id.size()<5||id.size()>128||!id.starts_with(L"GS3D")||!std::all_of(id.begin(),id.end(),[](wchar_t c){return (c>=L'a'&&c<=L'z')||(c>=L'A'&&c<=L'Z')||(c>=L'0'&&c<=L'9');}))return fail(L"分享链接的时光舱编号无效");
    result=Source{};result.shareUrl=base;result.captureId.reserve(id.size());for(const auto c:id)result.captureId.push_back(static_cast<char>(c));return {};
}
Result validate_plan(const DownloadPlan& plan){
    if(plan.schemaVersion!=1)return fail(L"不支持此计划版本，原文件保持不变");
    if(!valid_uuid(plan.planId))return fail(L"计划标识不是有效 UUID");
    if(!timestamp(plan.createdUtc)||!timestamp(plan.modifiedUtc))return fail(L"计划时间字段无效");
    if(!ascii_id(plan.source.adapterVersion))return fail(L"来源适配器版本无效");
    if(plan.internalFixture){if(plan.source.provider!="internal-fixture"||!plan.source.shareUrl.empty()||plan.source.captureId!="p1-fixture")return fail(L"内部样本来源标记不一致");}
    else {if(plan.source.provider!="insta360")return fail(L"此计划来源尚不支持");Source canonical;if(auto r=canonical_share_url(plan.source.shareUrl,canonical);!r)return r;if(canonical.shareUrl!=plan.source.shareUrl||canonical.captureId!=plan.source.captureId)return fail(L"持久化来源必须是规范分享链接，不得含查询签名");if(plan.source.shareUrl.empty()&&!plan.resources.empty())return fail(L"真实资源必须关联有效的公开分享来源");}
    if(auto r=validate_output_directory(plan.outputDirectory);!r)return r;
    if(plan.resources.size()>4096)return fail(L"资源数量超过计划上限");
    std::set<std::string> ids;std::set<std::wstring,WindowsNameLess> names;
    for(const auto& resource:plan.resources){
        if(!ascii_id(resource.id)||!ids.insert(resource.id).second)return fail(L"资源标识无效或重复");
        if(!text(resource.displayName,1024)||!text(resource.format,64))return fail(L"资源名称或格式无效");
        if(auto r=validate_output_name(resource.outputName);!r)return r;
        if(!names.insert(resource.outputName).second)return fail(L"资源输出文件名重复，请为每个文件指定不同名称");
    }
    return {};
}
DownloadPlan make_empty_plan(){DownloadPlan plan;plan.planId=uuid();plan.createdUtc=plan.modifiedUtc=now();return plan;}
DownloadPlan make_internal_fixture(){auto plan=make_empty_plan();plan.internalFixture=true;plan.source={"internal-fixture","p1-fixture",L"","fixture-v1"};plan.resources={{"model",L"中文空间.sog",L"SOG",L"中文空间.sog",true,16112434},{"cameras",L"相机路径.json",L"JSON",L"相机路径.json",false,807963},{"voxel",L"体素资源.zip",L"ZIP",L"体素资源.zip",false,std::nullopt}};return plan;}
DocumentStore::DocumentStore(){reset(make_empty_plan(),{},true);}
bool DocumentStore::dirty()const{return !saved_||saved_->plan!=current_->plan;}
Snapshot DocumentStore::nextSnapshot(DownloadPlan plan)const{if(current_->revision==UINT64_MAX||current_->inputRevision==UINT64_MAX)throw std::overflow_error("Plan revision exhausted");return std::make_shared<const PlanSnapshot>(PlanSnapshot{std::move(plan),current_->sessionId,current_->revision+1,current_->inputRevision+1});}
void DocumentStore::reset(DownloadPlan plan,std::wstring target,bool clean){const auto session=next_session();auto next=std::make_shared<const PlanSnapshot>(PlanSnapshot{std::move(plan),session,0,0});current_=std::move(next);saved_=clean?current_:Snapshot{};target_=std::move(target);past_.clear();future_.clear();pendingSaves_.clear();acceptedSave_=0;}
Result DocumentStore::apply(DownloadPlan next){if(auto r=validate_plan(next);!r)return r;if(next.planId!=current_->plan.planId||next.createdUtc!=current_->plan.createdUtc||next.internalFixture!=current_->plan.internalFixture)return fail(L"编辑不能更换计划身份或样本来源");if(next==current_->plan)return {};next.modifiedUtc=now();auto published=nextSnapshot(std::move(next));past_.push_back(current_);current_=std::move(published);if(past_.size()>128)past_.erase(past_.begin());future_.clear();return {true,true,{}};}
Result DocumentStore::undo(){if(past_.empty())return fail(L"没有可撤销操作");auto published=nextSnapshot(past_.back()->plan);future_.push_back(current_);current_=std::move(published);past_.pop_back();return {true,true,{}};}
Result DocumentStore::redo(){if(future_.empty())return fail(L"没有可重做操作");auto published=nextSnapshot(future_.back()->plan);past_.push_back(current_);current_=std::move(published);future_.pop_back();return {true,true,{}};}
Result DocumentStore::replaceLoaded(DownloadPlan plan,std::wstring target){if(auto r=validate_plan(plan);!r)return r;reset(std::move(plan),std::move(target),true);return {true,true,{}};}
Result DocumentStore::newPlan(bool fixture){reset(fixture?make_internal_fixture():make_empty_plan(),{},true);return {true,true,{}};}
SaveCapture DocumentStore::captureSave(std::wstring target){if(target.empty())throw std::invalid_argument("Save requires a target path");if(nextSave_==UINT64_MAX)throw std::overflow_error("Save identity exhausted");SaveCapture capture{current_->sessionId,nextSave_++,current_,std::move(target)};pendingSaves_.push_back(capture);if(pendingSaves_.size()>128)pendingSaves_.erase(pendingSaves_.begin());return capture;}
Result DocumentStore::acceptSaved(const SaveCapture& capture){
    if(!capture.snapshot||capture.sessionId!=current_->sessionId||capture.snapshot->sessionId!=capture.sessionId)return fail(L"保存结果属于旧会话，当前计划不受影响");
    if(capture.saveId<=acceptedSave_)return fail(L"已忽略过期或重复的保存完成结果");
    const auto found=std::find_if(pendingSaves_.begin(),pendingSaves_.end(),[&](const SaveCapture& expected){return matches(expected,capture);});
    if(found==pendingSaves_.end())return fail(L"保存结果与已捕获的请求不匹配");
    auto target=capture.target; // Allocate before changing either clean marker or target.
    saved_=capture.snapshot;target_=std::move(target);acceptedSave_=capture.saveId;
    std::erase_if(pendingSaves_,[&](const auto& c){return c.saveId<=acceptedSave_;});return {true,false,{}};
}
Result DocumentStore::rejectSave(const SaveCapture& capture){
    const auto found=std::find_if(pendingSaves_.begin(),pendingSaves_.end(),[&](const SaveCapture& expected){return matches(expected,capture);});
    if(found==pendingSaves_.end())return fail(L"失败保存结果已失效，当前计划不受影响");
    pendingSaves_.erase(found);return {};
}
std::optional<std::uint64_t> TaskCoordinator::begin(std::uint64_t session,std::wstring operation){if(busy())return {};if(next_==UINT64_MAX)throw std::overflow_error("IO task identity exhausted");value_={next_++,session,IoTaskState::queued,std::move(operation),{}};return value_.taskId;}
bool TaskCoordinator::running(std::uint64_t id){if(value_.taskId!=id||value_.state!=IoTaskState::queued)return false;value_.state=IoTaskState::running;return true;}
bool TaskCoordinator::finish(std::uint64_t id,bool success,std::wstring error){if(value_.taskId!=id||!busy())return false;value_.state=success?IoTaskState::succeeded:IoTaskState::failed;value_.error=std::move(error);return true;}
}
