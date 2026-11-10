#pragma once
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace gsdl {
struct Result {
    bool success{true}, changed{};
    std::wstring error;
    explicit operator bool() const noexcept { return success; }
};
struct Source {
    std::string provider{"insta360"}, captureId;
    std::wstring shareUrl;
    std::string adapterVersion{"p1-schema-1"};
    bool operator==(const Source&) const = default;
};
struct Resource {
    std::string id;
    std::wstring displayName, format, outputName;
    bool included{};
    std::optional<std::uint64_t> expectedBytes;
    bool operator==(const Resource&) const = default;
};
struct DownloadPlan {
    std::uint32_t schemaVersion{1};
    std::string planId;
    Source source;
    std::wstring outputDirectory;
    std::vector<Resource> resources;
    std::string createdUtc, modifiedUtc;
    bool internalFixture{};
    bool operator==(const DownloadPlan&) const = default;
};
struct PlanSnapshot {
    DownloadPlan plan;
    std::uint64_t sessionId{}, revision{}, inputRevision{};
};
using Snapshot = std::shared_ptr<const PlanSnapshot>;
struct SaveCapture {
    std::uint64_t sessionId{}, saveId{};
    Snapshot snapshot;
    std::wstring target;
};

Result validate_plan(const DownloadPlan&);
Result validate_output_directory(std::wstring_view);
Result validate_output_name(std::wstring_view);
Result canonical_share_url(std::wstring_view input, Source& result);
DownloadPlan make_empty_plan();
DownloadPlan make_internal_fixture();

// Single application/coordinator-thread owner. Workers retain immutable
// Snapshot/SaveCapture values; they never call this mutable object directly.
class DocumentStore {
public:
    DocumentStore();
    Snapshot snapshot() const { return current_; }
    bool dirty() const;
    std::size_t undo_count() const { return past_.size(); }
    std::size_t redo_count() const { return future_.size(); }
    const std::wstring& target() const { return target_; }
    Result apply(DownloadPlan);
    Result undo();
    Result redo();
    Result replaceLoaded(DownloadPlan, std::wstring target);
    Result newPlan(bool internal_fixture=false);
    SaveCapture captureSave(std::wstring target);
    Result acceptSaved(const SaveCapture&);
    Result rejectSave(const SaveCapture&);
private:
    Snapshot current_;
    Snapshot saved_;
    std::wstring target_;
    std::vector<Snapshot> past_, future_;
    std::uint64_t nextSave_{1}, acceptedSave_{};
    std::vector<SaveCapture> pendingSaves_;
    Snapshot nextSnapshot(DownloadPlan) const;
    void reset(DownloadPlan,std::wstring,bool clean);
};

struct WorkspaceStore { float dpi{1}; float inspectorWidth{320}; std::size_t firstResourceRow{}; };
struct SelectionStore {
    std::string resourceId;
    std::vector<std::string> selectedIds;
    void clear(){resourceId.clear();selectedIds.clear();}
};
enum class IoTaskState { idle, queued, running, succeeded, failed };
struct IoTaskSnapshot {
    std::uint64_t taskId{}, sessionId{};
    IoTaskState state{IoTaskState::idle};
    std::wstring operation, error;
    bool terminal()const{return state==IoTaskState::succeeded||state==IoTaskState::failed;}
};
class TaskCoordinator {
public:
    std::optional<std::uint64_t> begin(std::uint64_t session,std::wstring operation);
    bool running(std::uint64_t id);
    bool finish(std::uint64_t id,bool success,std::wstring error={});
    IoTaskSnapshot snapshot()const{return value_;}
    bool busy()const{return value_.state==IoTaskState::queued||value_.state==IoTaskState::running;}
private:
    std::uint64_t next_{1};
    IoTaskSnapshot value_;
};
}
