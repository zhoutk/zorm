#pragma once
// ---------------------------------------------------------------------------
// mcp/schema.hpp - JSON Schema 2020-12 validator (protocol self-defense).
//
// Scope (PLAN.md §4.1): the strong-semantics keywords MCP schemas actually
// use, plus local `$ref` (JSON Pointer into the same document, typically
// #/$defs/...). Remote refs are never dereferenced (basic/index.md
// "Implementations MUST NOT automatically dereference network URIs").
//
// Resource bounds (spec recommendation against DoS-by-schema): recursion
// depth and total keyword-check counts are capped; exceeding either fails
// validation rather than hanging.
//
// Supported: type, enum, const, required, properties, additionalProperties,
// patternProperties, items (schema or tuple), min/maxItems, uniqueItems,
// minLength/maxLength/pattern, minimum/maximum/exclusive*/multipleOf,
// minProperties/maxProperties, allOf/anyOf/oneOf/not, $ref, boolean schemas.
// ---------------------------------------------------------------------------

#include <mcp/error.hpp>
#include <mcp/json.hpp>

#include <string>

namespace mcp {

struct SchemaLimits {
  int maxDepth = 64;      // nested schema depth
  int maxChecks = 10000;  // total keyword checks per validation
};

// Validates `instance` against `schema` (root defaults to schema itself for
// $ref resolution). Ok on valid; Err(InvalidParams) with a descriptive
// message (instance path + violated keyword) on invalid or on a malformed /
// over-budget schema - the spec says schemas that cannot be validated MUST
// be rejected, never treated as permissive.
McpResult<void> validateSchema(const Json& instance, const Json& schema,
                               const Json& root, const SchemaLimits& limits);

// Convenience overload: root = schema, default limits.
McpResult<void> validateSchema(const Json& instance, const Json& schema);

}  // namespace mcp
