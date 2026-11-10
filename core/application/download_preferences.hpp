#pragma once
#include "core/domain/plan.hpp"
#include <algorithm>
#include <array>
#include <map>

namespace gsdl {
// New-download defaults belong to the workspace, not to a plan's edit history.
// Keys describe a source resource's purpose, never an individual share/file ID.
inline constexpr std::array<std::string_view,5> selection_preference_keys{
    "primaryModel", "cameraData", "voxelArchive", "effectVideo1", "effectVideo2"};
struct SelectionPreferences {
    bool remembered{};
    std::map<std::string,bool,std::less<>> choices;
    bool operator==(const SelectionPreferences&) const = default;
};
inline bool valid_selection_preference_key(std::string_view key) {
    return std::find(selection_preference_keys.begin(),selection_preference_keys.end(),key)!=selection_preference_keys.end();
}
inline bool recommended_selection(std::string_view key) {
    return key=="primaryModel" || key=="cameraData";
}
inline std::string_view resource_preference_key(const Resource &resource) {
    // displayName is the immutable basename received from the source adapter.
    // outputName is user editable and must not influence cross-task defaults.
    auto name=resource.displayName, format=resource.format;
    const auto lower=[](wchar_t c){return c>=L'A' && c<=L'Z'?wchar_t(c+32):c;};
    std::transform(name.begin(),name.end(),name.begin(),lower);
    std::transform(format.begin(),format.end(),format.begin(),lower);
    // The public source prefixes names with an export ordinal. Ignore it, but
    // only recognize the observed semantic basename and matching media format.
    std::size_t prefix{};
    while(prefix<name.size() && name[prefix]>=L'0' && name[prefix]<=L'9')++prefix;
    if(prefix && prefix<name.size() && name[prefix]==L'_')name.erase(0,prefix+1);
    if(format==L"sog" && (name==L"3dgs.sog" || name==L"model.sog"))return "primaryModel";
    if(format==L"json" && name==L"cameras.json")return "cameraData";
    if(format==L"zip" && name==L"3dgs.voxel.zip")return "voxelArchive";
    if(format==L"mp4" && name==L"effect_1.mp4")return "effectVideo1";
    if(format==L"mp4" && name==L"effect_2.mp4")return "effectVideo2";
    return {};
}
inline void apply_selection_preferences(std::vector<Resource> &resources,const SelectionPreferences &preferences) {
    for(auto &resource:resources) {
        const auto key=resource_preference_key(resource);
        const auto choice=preferences.choices.find(key);
        resource.included=!key.empty() &&
            (preferences.remembered && choice!=preferences.choices.end()?choice->second:recommended_selection(key));
    }
}
inline void remember_selection_preferences(SelectionPreferences &preferences,const std::vector<Resource> &resources) {
    if(!preferences.remembered) {
        preferences.choices.clear();
        for(const auto key:selection_preference_keys)preferences.choices.emplace(key,recommended_selection(key));
    }
    // Missing categories do not erase previous choices. If a source returns
    // several files for one category, only remember it checked when all are
    // checked; a partial selection must never silently expand future downloads.
    std::map<std::string,bool,std::less<>> visible;
    for(const auto &resource:resources) {
        const auto key=resource_preference_key(resource);
        if(key.empty())continue;
        const auto [it,inserted]=visible.emplace(key,resource.included);
        if(!inserted)it->second=it->second && resource.included;
    }
    for(const auto &[key,included]:visible)preferences.choices[key]=included;
    preferences.remembered=true;
}
} // namespace gsdl
