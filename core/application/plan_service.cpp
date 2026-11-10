#include "plan_service.hpp"
#include <algorithm>
#include <stdexcept>

namespace gsdl {
Result PlanService::acceptResolved(Snapshot request,std::vector<Resource> resources,std::string adapterVersion){
    auto current=snapshot();
    if(!request||request->sessionId!=current->sessionId||request->revision!=current->revision||request->inputRevision!=current->inputRevision||request->plan.planId!=current->plan.planId||request->plan.source!=current->plan.source)
        return {false,false,L"解析期间计划已改变，已忽略旧结果；请重新解析"};
    if(current->plan.internalFixture||current->plan.source.shareUrl.empty()||resources.empty())return {false,false,L"解析结果没有有效来源或资源"};
    auto next=current->plan;
    for(auto& r:resources){auto old=std::find_if(next.resources.begin(),next.resources.end(),[&](const auto& value){return value.id==r.id;});if(old!=next.resources.end()){r.outputName=old->outputName;r.included=old->included;}}
    next.resources=std::move(resources);next.source.adapterVersion=std::move(adapterVersion);return document_.apply(std::move(next));
}
const std::vector<ParameterSchema>& PlanService::schema()const{
    static const std::vector<ParameterSchema> schemas={
        {"source.shareUrl",ParameterType::text,L"分享链接",L"",L"来源",L"",{},{},{},{},{},true,true,L"提交时","text"},
        {"outputDirectory",ParameterType::path,L"保存目录",L"",L"输出",L"",{},{},{},{},{},true,true,L"提交时","path"},
        {"resource.outputName",ParameterType::text,L"输出文件名",L"",L"输出",L"",{},{},{},{},{},true,true,L"提交时","text"},
        {"resource.included",ParameterType::boolean,L"下载此资源",L"",L"输出",L"false",{},{},{},{},{},true,true,L"提交时","checkbox"},
        {"resource.format",ParameterType::text,L"原格式",L"",L"来源",L"",{},{},{},{},{},false,false,L"只读","text"}
    };return schemas;
}
Result PlanService::edit(std::string_view id,std::string_view resourceId,std::wstring_view value){
    auto next=snapshot()->plan;
    if(id=="source.shareUrl"){
        if(next.internalFixture)return {false,false,L"内部样本不能改成真实分享来源，请新建空计划"};
        Source source;if(auto r=canonical_share_url(value,source);!r)return r;
        if(source.shareUrl==next.source.shareUrl&&source.captureId==next.source.captureId)return {};
        next.source=std::move(source);next.resources.clear();
    }else if(id=="outputDirectory"){next.outputDirectory=value;}
    else if(id=="resource.outputName"){
        const auto found=std::find_if(next.resources.begin(),next.resources.end(),[&](const auto& r){return r.id==resourceId;});
        if(found==next.resources.end())return {false,false,L"所选资源不存在，请重新选择"};found->outputName=value;
    }else if(id=="resource.included"){
        if(value!=L"true"&&value!=L"false")return {false,false,L"勾选值必须是 true 或 false"};
        const std::string selected(resourceId);return setIncluded(std::span<const std::string>(&selected,1),value==L"true");
    }else return {false,false,L"该参数不存在或只读"};
    return document_.apply(std::move(next));
}
Result PlanService::setIncluded(std::span<const std::string> ids,bool included){
    if(ids.empty())return {};auto next=snapshot()->plan;
    for(const auto& id:ids){const auto found=std::find_if(next.resources.begin(),next.resources.end(),[&](const auto& r){return r.id==id;});if(found==next.resources.end())return {false,false,L"批量选择包含不存在的资源，未修改任何勾选"};found->included=included;}
    return document_.apply(std::move(next));
}
void ActionRegistry::add(Action action){if(action.actionId.empty()||!action.enabled||!action.checked||!action.invoke)throw std::invalid_argument("Action registration incomplete");const auto id=action.actionId;if(!actions_.emplace(id,std::move(action)).second)throw std::invalid_argument("Duplicate action id");}
std::optional<ActionState> ActionRegistry::query(std::string_view id)const{const auto found=actions_.find(id);if(found==actions_.end())return {};const auto& a=found->second;const auto enabled=a.enabled();return ActionState{a.actionId,a.label,a.scope,enabled?L"":a.disabledReason,enabled,a.checked()};}
bool ActionRegistry::invoke(std::string_view id){const auto found=actions_.find(id);if(found==actions_.end()||!found->second.enabled())return false;auto callback=found->second.invoke;callback();return true;}
}
