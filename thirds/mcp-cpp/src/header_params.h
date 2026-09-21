// x-mcp-header annotation walk (private to the library).
// Shared by the HTTP client (mirroring values into headers) and the HTTP
// server (validating that mirrored headers match the body).
//
// Spec constraints (basic/transports/streamable-http.md "Schema Extension"):
// only parameters statically reachable from the schema root via a chain of
// `properties` keys qualify; a chain must not pass through items, composition
// keywords, conditionals or $ref. The walk below simply never descends
// through anything else, which yields exactly the reachable set.
#pragma once

#include <mcp/json.hpp>
#include <mcp/types.hpp>  // mcp::member (direct-member lookup)

#include <string>
#include <utility>
#include <vector>

namespace mcp::detail {

// Renders one annotated argument value as its wire string form.
inline std::string headerParamText(const Json& value) {
  if (value.isString()) return std::string(value.toString());
  if (value.isNumber()) return std::to_string(value.toInt64());
  if (value.isFalse()) return "false";
  if (value.isTrue()) return "true";
  return "";  // unsupported (object/array/number-typed per spec) -> omit
}

// Collects (headerName, renderedValue) pairs for every annotated property
// whose value is present (and non-null) in `args`. Header names are the raw
// `x-mcp-header` values; encoding to `Mcp-Param-{name}` wire form happens at
// the call sites.
inline void collectHeaderParams(const Json& schema, const Json& args,
                                const std::vector<std::string>& path,
                                std::vector<std::pair<std::string, std::string>>& out) {
  if (!schema.isObject() || !args.isObject()) return;
  const Json properties = mcp::member(schema, "properties");
  if (!properties.isObject()) return;
  for (const auto& key : properties.getAllKeys().toVector()) {
    const std::string keyStr(key.toString());
    const Json prop = properties[keyStr];
    const Json annotation = mcp::member(prop, "x-mcp-header");
    if (annotation.isString() && !annotation.toString().empty()) {
      // Read the value at this exact path in the arguments.
      Json node = args;
      for (const auto& seg : path) node = node[seg];
      const Json value = node[keyStr];
      if (value.isError() || value.isNull()) continue;  // absent -> omit header
      std::string text = headerParamText(value);
      if (!text.empty()) out.emplace_back(std::string(annotation.toString()), std::move(text));
    }
    // Descend through properties chains only.
    std::vector<std::string> nextPath = path;
    nextPath.push_back(keyStr);
    collectHeaderParams(prop, args[keyStr], nextPath, out);
  }
}

}  // namespace mcp::detail
