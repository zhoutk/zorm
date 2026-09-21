#pragma once
// ---------------------------------------------------------------------------
// mcp/types.hpp - core protocol value types shared by every layer.
//
// Source of truth: specification 2026-07-28 (schema.md + basic/index.md).
// Conventions:
//  - optional members are empty strings / Null Json, never pointer owners
//  - every type has encodeX/decodeX pairs; decode returns std::optional or a
//    McpResult and NEVER throws
//  - building Json text values goes through Json::str(...) - see the zjson
//    sniffing convention note in project CLAUDE.md
// ---------------------------------------------------------------------------

#include <mcp/error.hpp>
#include <mcp/json.hpp>
#include <mcp/version.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <variant>

namespace mcp {

// ---- request id -------------------------------------------------------------
// "Requests MUST include a string or integer ID ... MUST NOT be null"
// (basic/index.md). Integer covers the full int64 range; fractional numbers
// are rejected by the decoder.

using RequestId = std::variant<int64_t, std::string>;

// DIRECT-member lookup: returns obj[key] when the key is an immediate
// member, Null otherwise. Never deep-searches - zjson's operator[] falls
// back to a deep search for missing keys, which misreads nested protocol
// data (e.g. params.id vs. a request id). Protocol layer must use this.
Json member(const Json& obj, std::string_view key);

// Decodes a wire id value. Accepts strings and integral numbers only.
std::optional<RequestId> decodeRequestId(const Json& v);

// Encodes a request id as a Json value (Number or String).
Json encodeRequestId(const RequestId& id);

// ---- result types -----------------------------------------------------------
// Result responses are polymorphic via `resultType` (basic/index.md).

inline constexpr std::string_view kResultTypeComplete = "complete";
inline constexpr std::string_view kResultTypeInputRequired = "input_required";
inline constexpr std::string_view kResultTypeKey = "resultType";

// The resultType of a result object; absent resultType (legacy servers) reads
// as "complete" per the spec's backward-compatibility rule.
std::string resultTypeOf(const Json& result);

// ---- _meta reserved keys ----------------------------------------------------
// (basic/index.md "Reserved keys" table.)

inline constexpr std::string_view kMetaProtocolVersion = "io.modelcontextprotocol/protocolVersion";
inline constexpr std::string_view kMetaClientInfo = "io.modelcontextprotocol/clientInfo";
inline constexpr std::string_view kMetaClientCapabilities = "io.modelcontextprotocol/clientCapabilities";
inline constexpr std::string_view kMetaServerInfo = "io.modelcontextprotocol/serverInfo";
inline constexpr std::string_view kMetaLogLevel = "io.modelcontextprotocol/logLevel";
inline constexpr std::string_view kMetaSubscriptionId = "io.modelcontextprotocol/subscriptionId";
inline constexpr std::string_view kMetaProgressToken = "progressToken";

// ---- Implementation ----------------------------------------------------------
// { name, version } required; title/description/websiteUrl optional;
// icons kept verbatim (Null when absent).

struct Implementation {
  std::string name;
  std::string title;
  std::string version;
  std::string description;
  std::string websiteUrl;
  Json icons = Json(nullptr);
};

Json encodeImplementation(const Implementation& impl);

// Requires name + version to be present strings; nullopt otherwise.
std::optional<Implementation> decodeImplementation(const Json& v);

// ---- per-request metadata ----------------------------------------------------
// Modern (2026-07-28) requests carry protocol metadata in params._meta
// (basic/index.md "Per-request protocol fields"): protocolVersion and
// clientCapabilities are REQUIRED on every request; clientInfo is optional.

struct RequestMeta {
  std::string protocolVersion;         // required (a wire version string)
  Json clientCapabilities;             // required (object; Null only in decode failure)
  std::optional<Implementation> clientInfo;  // optional
};

// Decodes params._meta. Fails with InvalidParams when _meta is missing, not
// an object, or any required field is missing/ill-typed - the caller maps
// that to the -32602 response the spec mandates.
McpResult<RequestMeta> decodeRequestMeta(const Json& params);

// Builds a params._meta object for outgoing client requests (M3).
Json encodeRequestMeta(std::string_view protocolVersion,
                       const Json& clientCapabilities,
                       const std::optional<Implementation>& clientInfo);

// ---- capability objects ------------------------------------------------------
// Capabilities evolve per spec revision, so the SDK keeps them Json-backed
// (structural typing, like the TS schema) with typed probes for the fields
// the runtime itself branches on. Full typed structs would freeze a field
// set the specification actively extends.

// True when the capabilities object advertises `key` (e.g. "tools").
bool capabilitiesInclude(const Json& capabilities, std::string_view key);

// True when capabilities advertise the extension `extensionId` under
// "extensions" (e.g. "io.modelcontextprotocol/ui").
bool capabilitiesIncludeExtension(const Json& capabilities, std::string_view extensionId);

}  // namespace mcp
