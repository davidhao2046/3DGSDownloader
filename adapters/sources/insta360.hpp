#pragma once
#include "core/domain/plan.hpp"
#include "infra/net/http.hpp"
#include <memory>
namespace gsdl::sources {
inline constexpr const char *adapter_version = "insta360-next-v1";
struct RemoteResource {
  Resource descriptor;
  std::wstring signedUrl; // Private, process-only. Never serialize this object.
  std::string md5;
};
struct ResolvedSource {
  Snapshot request;
  std::vector<RemoteResource> resources;
};
std::vector<RemoteResource> parse_insta360(std::string_view html,
                                           std::string_view captureId);
ResolvedSource resolve(Snapshot, net::Http &, const net::Check &);
} // namespace gsdl::sources
