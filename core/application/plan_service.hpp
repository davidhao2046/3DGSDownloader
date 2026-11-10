#pragma once
#include <core/domain/plan.hpp>
#include <functional>
#include <map>
#include <span>

namespace gsdl {
enum class ParameterType { text, path, boolean };
struct ParameterSchema {
    std::string parameterId;
    ParameterType type;
    std::wstring label, unit, group, defaultValue;
    std::optional<double> minimum,maximum,step;
    std::optional<unsigned> storagePrecision,displayPrecision;
    bool editable{true},undoable{true};
    std::wstring effectiveWhen{L"提交时"};
    std::string editorId;
};
class PlanService {
public:
    Snapshot snapshot()const{return document_.snapshot();}
    bool dirty()const{return document_.dirty();}
    std::size_t undo_count()const{return document_.undo_count();}
    std::size_t redo_count()const{return document_.redo_count();}
    const std::wstring& target()const{return document_.target();}
    const std::vector<ParameterSchema>& schema()const;
    Result edit(std::string_view parameterId,std::string_view resourceId,std::wstring_view text);
    Result setIncluded(std::span<const std::string> resourceIds,bool included);
    Result acceptResolved(Snapshot request, std::vector<Resource> resources, std::string adapterVersion);
    Result undo(){return document_.undo();}
    Result redo(){return document_.redo();}
    Result newPlan(bool fixture=false){return document_.newPlan(fixture);}
    Result replaceLoaded(DownloadPlan plan,std::wstring path){return document_.replaceLoaded(std::move(plan),std::move(path));}
    SaveCapture captureSave(std::wstring path){return document_.captureSave(std::move(path));}
    Result acceptSaved(const SaveCapture& capture){return document_.acceptSaved(capture);}
    Result rejectSave(const SaveCapture& capture){return document_.rejectSave(capture);}
private:
    DocumentStore document_;
};
struct Action {
    std::string actionId;
    std::wstring label,scope,disabledReason;
    std::function<bool()> enabled;
    std::function<bool()> checked;
    std::function<void()> invoke;
};
struct ActionState {std::string actionId;std::wstring label,scope,disabledReason;bool enabled{},checked{};};
class ActionRegistry {
public:
    void add(Action);
    std::optional<ActionState> query(std::string_view id)const;
    bool invoke(std::string_view id);
private:
    std::map<std::string,Action,std::less<>> actions_;
};
}
