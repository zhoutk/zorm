// json_util.cpp - see json_util.hpp.

#include "json_util.hpp"

#include <algorithm>
#include <cctype>

namespace zorm_mcp {

mcp::Json textResult(const std::string& text) {
  mcp::Json result;
  mcp::Json content = mcp::Json("[]");
  mcp::Json item;
  item.add("type", mcp::Json::str("text"));
  item.add("text", mcp::Json::str(text));
  content.push_back(std::move(item));
  result.add("content", std::move(content));
  return result;
}

mcp::Json toolResult(const mcp::Json& structured) {
  mcp::Json result = textResult(structured.toString());
  if (structured.isObject()) {
    for (const auto& k : structured.getAllKeys().toVector()) {
      const std::string kk = k.toString();
      result.add(kk, structured[kk]);
    }
  }
  return result;
}

mcp::Json rowVal(const mcp::Json& row, std::initializer_list<const char*> keys) {
  if (!row.isObject()) return mcp::Json(nullptr);
  for (const char* k : keys) {
    if (ZJSON::hasChild(row, k)) return row[k];
  }
  return mcp::Json(nullptr);
}

std::string rowStr(const mcp::Json& row, std::initializer_list<const char*> keys) {
  const mcp::Json v = rowVal(row, keys);
  return (v.isNull() || v.isError()) ? std::string() : v.toString();
}

bool validIdentifier(const std::string& name) {
  static const auto alphaNum = [](unsigned char c) {
    return std::isalnum(c) || c == '_';
  };
  if (name.empty()) return false;
  if (!alphaNum(static_cast<unsigned char>(name[0])) ||
      std::isdigit(static_cast<unsigned char>(name[0]))) {
    return false;
  }
  return std::all_of(name.begin() + 1, name.end(),
                     [](unsigned char c) { return alphaNum(c); });
}

}  // namespace zorm_mcp
