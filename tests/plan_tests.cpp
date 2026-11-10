#include <core/application/plan_service.hpp>
#include <iostream>
#include <stdexcept>

namespace {
int checks{};
void check(bool value,const char* expression,int line){++checks;if(!value)throw std::runtime_error(std::string("line ")+std::to_string(line)+": "+expression);}
#define CHECK(...) check(static_cast<bool>((__VA_ARGS__)),#__VA_ARGS__,__LINE__)
const std::wstring link=L"https://app.insta360.com/3dspace/detail/GS3D000000000000000000000000000000000";
void validation(){
    auto fixture=gsdl::make_internal_fixture();CHECK(gsdl::validate_plan(fixture));CHECK(fixture.resources.size()==3);CHECK(fixture.internalFixture);CHECK(fixture.source.shareUrl.empty());
    CHECK(gsdl::validate_output_name(L"中文 空间.sog"));CHECK(gsdl::validate_output_name(L"日本語_😀.sog"));
    for(const auto* name:{L"",L" ",L"\u3000\u00a0",L"..",L"../x",L"a\\b.sog",L"C:test",L"trailing.",L"trailing ",L"CON.sog",L"cOm1.zip",L"LPT².json",L"NUL",L"a?b"})CHECK(!gsdl::validate_output_name(name));
    CHECK(!gsdl::validate_output_name(std::wstring(1,0xd800)));CHECK(!gsdl::validate_output_name(std::wstring(1,0xdc00)));
    CHECK(gsdl::validate_output_directory(L""));CHECK(gsdl::validate_output_directory(L"D:\\"));CHECK(gsdl::validate_output_directory(L"D:\\中文 文件\\结果"));CHECK(gsdl::validate_output_directory(L"\\\\server\\share\\结果"));CHECK(gsdl::validate_output_directory(L"C:/valid/path/"));
    for(const auto* directory:{L"relative",L"C:relative",L"\\rooted",L"C:\\a\\..\\b",L"C:\\a\\CON",L"C:\\a\\\\b",L"\\\\server",L"\\\\server\\",L"\\\\?\\C:\\x",L"C:\\a:stream",L"C:\\\u3000"})CHECK(!gsdl::validate_output_directory(directory));
    auto invalid=fixture;invalid.resources[1].outputName=invalid.resources[0].outputName;CHECK(!gsdl::validate_plan(invalid));
    invalid=fixture;invalid.resources[0].outputName=L"Case.sog";invalid.resources[1].outputName=L"case.SOG";CHECK(!gsdl::validate_plan(invalid));
    invalid.resources[0].outputName=L"Ä.sog";invalid.resources[1].outputName=L"ä.SOG";CHECK(!gsdl::validate_plan(invalid));
    invalid=fixture;invalid.resources[1].id=invalid.resources[0].id;CHECK(!gsdl::validate_plan(invalid));
    invalid=fixture;invalid.internalFixture=false;CHECK(!gsdl::validate_plan(invalid));
    invalid=fixture;invalid.schemaVersion=2;CHECK(!gsdl::validate_plan(invalid));
    invalid=fixture;invalid.planId="not-a-uuid";CHECK(!gsdl::validate_plan(invalid));
    invalid=fixture;invalid.modifiedUtc="2026-13-01T00:00:00Z";CHECK(!gsdl::validate_plan(invalid));
    invalid.modifiedUtc="2026-02-29T00:00:00Z";CHECK(!gsdl::validate_plan(invalid));invalid.modifiedUtc="2024-02-29T00:00:00Z";CHECK(gsdl::validate_plan(invalid));
    CHECK(!gsdl::validate_output_name(L"CON .sog"));
    gsdl::Source source;CHECK(gsdl::canonical_share_url(link+L"?token=sensitive#fragment",source));CHECK(source.shareUrl==link);CHECK(source.captureId=="GS3D000000000000000000000000000000000");
    CHECK(!gsdl::canonical_share_url(L"http://app.insta360.com/3dspace/detail/GS3Dx",source));CHECK(!gsdl::canonical_share_url(L"https://app.insta360.com.attacker.test/3dspace/detail/GS3Dx",source));
    auto real=gsdl::make_empty_plan();real.source=source;CHECK(gsdl::validate_plan(real));real.source.shareUrl+=L"?signature=bad";CHECK(!gsdl::validate_plan(real));
}
void history(){
    gsdl::PlanService service;CHECK(service.newPlan(true));const auto original=service.snapshot();CHECK(!service.dirty());CHECK(service.undo_count()==0);
    CHECK(service.edit("resource.outputName","model",L"第一次.sog").changed);const auto edited=service.snapshot();CHECK(edited->revision==1);CHECK(edited->inputRevision==1);CHECK(edited->sessionId==original->sessionId);CHECK(original->plan.resources[0].outputName==L"中文空间.sog");CHECK(service.undo_count()==1);CHECK(service.dirty());
    CHECK(service.undo());const auto undone=service.snapshot();CHECK(undone->revision==2);CHECK(undone->inputRevision==2);CHECK(undone->plan==original->plan);CHECK(!service.dirty());CHECK(service.redo_count()==1);
    auto no_op=service.edit("resource.outputName","model",L"中文空间.sog");CHECK(no_op&&!no_op.changed);CHECK(service.snapshot()==undone);CHECK(service.redo_count()==1);
    CHECK(!service.edit("resource.outputName","model",L"CON.sog"));CHECK(!service.edit("resource.outputName","missing",L"a.sog"));CHECK(!service.edit("resource.format","model",L"PLY"));CHECK(!service.edit("unknown","",L"x"));CHECK(!service.edit("outputDirectory","",L"relative"));CHECK(service.snapshot()==undone);CHECK(service.redo_count()==1);
    CHECK(service.redo());CHECK(service.snapshot()->plan==edited->plan);CHECK(service.snapshot()->revision==3);CHECK(service.dirty());
    const std::vector<std::string> ids{"cameras","voxel"};const auto count=service.undo_count();CHECK(service.setIncluded(ids,true));CHECK(service.undo_count()==count+1);CHECK(service.snapshot()->plan.resources[1].included&&service.snapshot()->plan.resources[2].included);
    CHECK(service.undo());const auto before=service.snapshot();const std::vector<std::string> badIds{"cameras","missing"};CHECK(!service.setIncluded(badIds,true));CHECK(service.snapshot()==before);CHECK(service.redo_count()==1);
    CHECK(service.setIncluded({},true));CHECK(service.snapshot()==before);CHECK(service.redo_count()==1);CHECK(service.redo());
    CHECK(service.edit("resource.included","model",L"false"));CHECK(!service.snapshot()->plan.resources[0].included);CHECK(!service.edit("resource.included","model",L"yes"));
    CHECK(service.newPlan());CHECK(service.snapshot()->sessionId!=original->sessionId);CHECK(service.undo_count()==0&&service.redo_count()==0);CHECK(!service.dirty());CHECK(service.snapshot()->plan.resources.empty());CHECK(service.target().empty());
}
void persistence_markers(){
    gsdl::PlanService service;service.newPlan(true);CHECK(service.edit("resource.outputName","model",L"已保存.sog"));const auto captured=service.captureSave(L"D:\\计划.gsdlplan");
    CHECK(service.edit("outputDirectory","",L"D:\\下载结果"));CHECK(service.acceptSaved(captured));CHECK(service.dirty());CHECK(service.target()==L"D:\\计划.gsdlplan");CHECK(service.undo());CHECK(!service.dirty());CHECK(service.snapshot()->plan==captured.snapshot->plan);CHECK(service.redo());CHECK(service.dirty());
    const auto old=service.captureSave(L"D:\\旧目标.gsdlplan");CHECK(service.edit("resource.outputName","model",L"最新.sog"));const auto latest=service.captureSave(L"D:\\新目标.gsdlplan");CHECK(service.acceptSaved(latest));CHECK(!service.dirty());CHECK(!service.acceptSaved(old));CHECK(!service.acceptSaved(latest));CHECK(service.target()==latest.target);CHECK(!service.dirty());
    CHECK(service.edit("resource.outputName","model",L"未保存.sog"));const auto failed=service.captureSave(L"D:\\不能写.gsdlplan");const auto before=service.snapshot();CHECK(service.rejectSave(failed));CHECK(service.snapshot()==before);CHECK(service.dirty());CHECK(service.target()==latest.target);CHECK(!service.acceptSaved(failed));CHECK(!service.rejectSave(failed));
    const auto pending=service.captureSave(L"D:\\另存.gsdlplan");auto forged=pending;forged.target=L"D:\\篡改.gsdlplan";CHECK(!service.acceptSaved(forged));forged=pending;forged.snapshot=std::make_shared<const gsdl::PlanSnapshot>(*pending.snapshot);CHECK(!service.acceptSaved(forged));CHECK(service.acceptSaved(pending));CHECK(!service.dirty());
    const auto session=pending.sessionId;const auto stillRunning=service.captureSave(L"D:\\跨会话.gsdlplan");auto loaded=service.snapshot()->plan;CHECK(service.replaceLoaded(loaded,L"D:\\重新打开.gsdlplan"));CHECK(service.snapshot()->sessionId!=session);CHECK(!service.acceptSaved(stillRunning));CHECK(service.target()==L"D:\\重新打开.gsdlplan");CHECK(!service.dirty());CHECK(service.undo_count()==0&&service.redo_count()==0);
    const auto valid=service.snapshot();loaded.schemaVersion=2;CHECK(!service.replaceLoaded(loaded,L"D:\\坏文件.gsdlplan"));CHECK(service.snapshot()==valid);
    bool threw=false;try{service.captureSave({});}catch(const std::invalid_argument&){threw=true;}CHECK(threw);
}
void source_replacement(){
    gsdl::PlanService service;auto plan=gsdl::make_empty_plan();CHECK(gsdl::canonical_share_url(link,plan.source));plan.source.adapterVersion="future-adapter";plan.resources=gsdl::make_internal_fixture().resources;CHECK(service.replaceLoaded(plan,L"D:\\来源.gsdlplan"));
    const auto previous=service.snapshot();CHECK(!service.edit("source.shareUrl","",link+L"?different=1").changed);CHECK(service.snapshot()==previous);CHECK(service.snapshot()->plan.resources.size()==3);
    CHECK(service.edit("source.shareUrl","",L"https://app.insta360.com/3dspace/detail/GS3Dnew?signature=removed"));CHECK(service.snapshot()->plan.resources.empty());CHECK(service.snapshot()->plan.source.shareUrl==L"https://app.insta360.com/3dspace/detail/GS3Dnew");CHECK(service.undo_count()==1);CHECK(service.undo());CHECK(service.snapshot()->plan==previous->plan);CHECK(!service.dirty());
    service.newPlan(true);const auto fixture=service.snapshot();CHECK(!service.edit("source.shareUrl","",link));CHECK(service.snapshot()==fixture);
    const auto& schema=service.schema();CHECK(schema.size()==5);CHECK(schema[2].parameterId=="resource.outputName");CHECK(!schema[4].editable&&!schema[4].undoable);CHECK(!schema[0].minimum&&!schema[0].storagePrecision);
}
void actions_and_tasks(){
    gsdl::ActionRegistry actions;bool enabled=false,checked=false;int invocations{};actions.add({"test",L"测试",L"workspace",L"没有选择",[&]{return enabled;},[&]{return checked;},[&]{++invocations;}});
    CHECK(!actions.query("missing"));CHECK(!actions.invoke("missing"));CHECK(!actions.query("test")->enabled);CHECK(actions.query("test")->disabledReason==L"没有选择");CHECK(!actions.invoke("test"));CHECK(invocations==0);enabled=true;checked=true;CHECK(actions.query("test")->checked);CHECK(actions.query("test")->disabledReason.empty());CHECK(actions.invoke("test"));CHECK(invocations==1);
    bool threw=false;try{actions.add({"test",L"",L"",L"",[]{return true;},[]{return false;},[]{}});}catch(const std::invalid_argument&){threw=true;}CHECK(threw);
    gsdl::TaskCoordinator tasks;CHECK(!tasks.busy());const auto first=tasks.begin(42,L"保存");CHECK(first&&tasks.busy());CHECK(!tasks.begin(42,L"打开"));CHECK(!tasks.running(*first+1));CHECK(tasks.running(*first));CHECK(!tasks.running(*first));CHECK(tasks.finish(*first,false,L"磁盘已满，原文件保持不变"));CHECK(tasks.snapshot().terminal());CHECK(!tasks.busy());CHECK(!tasks.finish(*first,true));CHECK(!tasks.running(*first));
    const auto second=tasks.begin(43,L"打开");CHECK(second&&*second!=*first);CHECK(!tasks.finish(*first,true));CHECK(tasks.snapshot().sessionId==43);CHECK(tasks.finish(*second,true));CHECK(tasks.snapshot().state==gsdl::IoTaskState::succeeded);CHECK(tasks.snapshot().error.empty());
    gsdl::WorkspaceStore workspace;workspace.firstResourceRow=100;gsdl::SelectionStore selection;selection.resourceId="model";selection.selectedIds={"model"};selection.clear();CHECK(selection.resourceId.empty()&&selection.selectedIds.empty());
}
}
int main(){try{validation();history();persistence_markers();source_replacement();actions_and_tasks();std::cout<<"plan_tests: "<<checks<<" checks passed\n";return 0;}catch(const std::exception& error){std::cerr<<"plan_tests failed: "<<error.what()<<'\n';return 1;}}
