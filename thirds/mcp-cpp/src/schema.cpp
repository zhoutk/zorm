#include <mcp/schema.hpp>

#include <cmath>
#include <regex>
#include <set>
#include <string>

namespace mcp {
namespace {

// zjson operator[] falls back to a DEEP search when the direct member is
// missing - fatal for schema keyword extraction (direct(schema, "type") on an
// anyOf-only object would find a nested "type"). Use direct members only.
Json direct(const Json& obj, const char* key) {
  const Json* p = obj.directMemberPtr(std::string_view(key));
  return p ? *p : Json(nullptr);  // Null doubles as "absent"
}

// Maps a Json node to the 2020-12 primitive type names.
std::string typeName(const Json& v) {
  if (v.isNull()) return "null";
  if (v.isObject()) return "object";
  if (v.isArray()) return "array";
  if (v.isString()) return "string";
  if (v.isNumber()) {
    // "integer" is a distinct type in 2020-12; report the finest match.
    if (static_cast<double>(v.toInt64()) == v.toDouble()) return "integer";
    return "number";
  }
  return "boolean";
}

bool typeMatches(const Json& v, const std::string& expected) {
  const std::string actual = typeName(v);
  if (actual == expected) return true;
  // An integer is also a valid number.
  return expected == "number" && actual == "integer";
}

struct Validator {
  const Json& root;
  const SchemaLimits limits;
  int checks = 0;
  std::string error;

  bool fail(const std::string& path, const std::string& message) {
    if (error.empty()) error = path.empty() ? message : path + ": " + message;
    return false;
  }

  bool budget() {
    if (++checks > limits.maxChecks) {
      if (error.empty()) error = "schema validation exceeded resource bounds";
      return false;
    }
    return true;
  }

  // Resolves a local $ref ("#/x/y" JSON Pointer form). Remote/anchor forms
  // are rejected (never dereferenced, never silently permissive).
  bool resolveRef(const Json& refNode, Json& out) {
    if (!refNode.isString()) return fail("", "$ref must be a string");
    const std::string ref(refNode.toString());
    if (ref.empty() || ref[0] != '#') {
      return fail("", "non-local $ref '" + ref + "' is not supported and not allowed");
    }
    const Json* node = &root;
    size_t pos = 1;
    if (pos < ref.size() && ref[pos] == '/') ++pos;
    while (pos <= ref.size()) {
      const size_t next = ref.find('/', pos);
      std::string token = ref.substr(pos, next == std::string::npos ? std::string::npos : next - pos);
      // JSON Pointer escapes.
      std::string unescaped;
      for (size_t i = 0; i < token.size(); ++i) {
        if (token[i] == '~' && i + 1 < token.size()) {
          if (token[i + 1] == '0') unescaped.push_back('~');
          else if (token[i + 1] == '1') unescaped.push_back('/');
          ++i;
        } else {
          unescaped.push_back(token[i]);
        }
      }
      if (!unescaped.empty()) {
        if (node->isArray()) {
          // array index
          for (const char ch : unescaped) {
            if (ch < '0' || ch > '9') return fail("", "invalid $ref array index '" + unescaped + "'");
          }
          const int idx = std::stoi(unescaped);
          Json child = (*node)[idx];
          if (child.isError()) return fail("", "$ref index out of range: " + unescaped);
          out = child;
          node = &out;
        } else {
          const Json child = (*node)[unescaped];
          if (child.isError()) return fail("", "unresolvable $ref token: " + unescaped);
          out = child;
          node = &out;
        }
      }
      if (next == std::string::npos) break;
      pos = next + 1;
    }
    return true;
  }

  bool numberOk(const Json& value, const std::string& path, const Json& schema, bool& ok) {
    ok = true;
    const double v = value.toDouble();

    const Json minimum = direct(schema, "minimum");
    if (minimum.isNumber() && !(v >= minimum.toDouble())) {
      ok = fail(path, "value " + std::to_string(v) + " is below minimum");
    }
    const Json maximum = direct(schema, "maximum");
    if (maximum.isNumber() && !(v <= maximum.toDouble())) {
      ok = fail(path, "value " + std::to_string(v) + " exceeds maximum");
    }
    const Json exMin = direct(schema, "exclusiveMinimum");
    if (exMin.isNumber() && !(v > exMin.toDouble())) {
      ok = fail(path, "value " + std::to_string(v) + " is not above exclusiveMinimum");
    }
    const Json exMax = direct(schema, "exclusiveMaximum");
    if (exMax.isNumber() && !(v < exMax.toDouble())) {
      ok = fail(path, "value " + std::to_string(v) + " is not below exclusiveMaximum");
    }
    const Json multipleOf = direct(schema, "multipleOf");
    if (multipleOf.isNumber()) {
      const double m = multipleOf.toDouble();
      if (m > 0) {
        const double q = v / m;
        if (std::fabs(q - std::round(q)) > 1e-9) {
          ok = fail(path, "value " + std::to_string(v) + " is not a multiple of " +
                              std::to_string(m));
        }
      }
    }
    return ok;
  }

  bool stringOk(const Json& value, const std::string& path, const Json& schema, bool& ok) {
    ok = true;
    const std::string text(value.toString());

    const Json minLength = direct(schema, "minLength");
    if (minLength.isNumber() &&
        static_cast<int64_t>(text.size()) < minLength.toInt64()) {
      ok = fail(path, "string shorter than minLength " + minLength.toString());
    }
    const Json maxLength = direct(schema, "maxLength");
    if (maxLength.isNumber() &&
        static_cast<int64_t>(text.size()) > maxLength.toInt64()) {
      ok = fail(path, "string longer than maxLength " + maxLength.toString());
    }
    const Json pattern = direct(schema, "pattern");
    if (pattern.isString()) {
      try {
        if (!std::regex_search(text, std::regex(std::string(pattern.toString())))) {
          ok = fail(path, "string does not match pattern " + std::string(pattern.toString()));
        }
      } catch (const std::regex_error&) {
        ok = fail(path, "schema contains an invalid regex pattern");
      }
    }
    return ok;
  }

  bool check(const Json& value, const Json& schema, const std::string& path, int depth) {
    fprintf(stderr, "[chk] value=%s schema=%s depth=%d\n",
            value.toString().c_str(), schema.toString().c_str(), depth);
    if (!budget()) return false;

    // Boolean schemas: true = anything, false = nothing.
    if (schema.isTrue()) return true;
    if (schema.isFalse()) return fail(path, "schema is false: no value is allowed");

    if (!schema.isObject()) return fail(path, "schema must be an object or boolean");
    if (depth > limits.maxDepth) return fail(path, "schema nesting exceeds resource bounds");

    // $ref: validate against the target; sibling keywords are ignored
    // (2020-12 behavior).
    const Json ref = direct(schema, "$ref");
    if (ref.isString()) {
      Json target;
      if (!resolveRef(ref, target)) return false;
      return check(value, target, path, depth + 1);
    }

    // type
    const Json type = direct(schema, "type");
    fprintf(stderr, "[chk] typeMember=%s\n", type.isError() ? "<err>" : type.toString().c_str());
    if (type.isString()) {
      if (!typeMatches(value, std::string(type.toString()))) {
        return fail(path, "expected type " + std::string(type.toString()) + ", got " +
                              typeName(value));
      }
    } else if (type.isArray()) {
      bool any = false;
      for (const auto& t : type.toVector()) {
        if (t.isString() && typeMatches(value, std::string(t.toString()))) any = true;
      }
      if (!any) return fail(path, "type does not match any allowed type");
    }

    // enum / const
    const Json enumValues = direct(schema, "enum");
    if (enumValues.isArray()) {
      bool found = false;
      for (const auto& candidate : enumValues.toVector()) {
        if (candidate == value) found = true;
      }
      if (!found) return fail(path, "value not in enum");
    }
    const Json constValue = direct(schema, "const");
    if (constValue.isObject() || constValue.isArray() || constValue.isString() ||
        constValue.isNumber() || constValue.isTrue() || constValue.isFalse() ||
        constValue.isNull()) {
      if (schema.contains("const") && !(constValue == value)) {
        return fail(path, "value does not equal const");
      }
    }

    // per-type keyword groups
    if (value.isNumber()) {
      bool ok = true;
      if (!numberOk(value, path, schema, ok)) return false;
    } else if (value.isString()) {
      bool ok = true;
      if (!stringOk(value, path, schema, ok)) return false;
    } else if (value.isArray()) {
      const Json minItems = direct(schema, "minItems");
      if (minItems.isNumber() && value.size() < minItems.toInt64()) {
        return fail(path, "array has fewer than minItems " + minItems.toString());
      }
      const Json maxItems = direct(schema, "maxItems");
      if (maxItems.isNumber() && value.size() > maxItems.toInt64()) {
        return fail(path, "array has more than maxItems " + maxItems.toString());
      }
      const Json uniqueItems = direct(schema, "uniqueItems");
      if (uniqueItems.isTrue()) {
        const int n = value.size();
        for (int i = 0; i < n; ++i) {
          for (int j = i + 1; j < n; ++j) {
            if (!budget()) return false;
            if (value[i] == value[j]) return fail(path, "array items are not unique");
          }
        }
      }
      const Json items = direct(schema, "items");
      if (items.isObject() || items.isTrue() || items.isFalse()) {
        int idx = 0;
        for (const auto& item : value.toVector()) {
          const std::string itemPath = path + "/" + std::to_string(idx);
          if (!check(item, items, itemPath, depth + 1)) return false;
          ++idx;
        }
      } else if (items.isArray()) {
        // tuple form
        int idx = 0;
        for (const auto& item : items.toVector()) {
          if (idx >= value.size()) break;
          const std::string itemPath = path + "/" + std::to_string(idx);
          if (!check(value[idx], item, itemPath, depth + 1)) return false;
          ++idx;
        }
      }
    } else if (value.isObject()) {
      const Json minProperties = direct(schema, "minProperties");
      if (minProperties.isNumber() &&
          value.size() < minProperties.toInt64()) {
        return fail(path, "object has fewer than minProperties");
      }
      const Json maxProperties = direct(schema, "maxProperties");
      if (maxProperties.isNumber() &&
          value.size() > maxProperties.toInt64()) {
        return fail(path, "object has more than maxProperties");
      }

      // required
      const Json required = direct(schema, "required");
      if (required.isArray()) {
        for (const auto& item : required.toVector()) {
          if (!value.contains(std::string(item.toString()))) {
            return fail(path, "missing required property '" + std::string(item.toString()) + "'");
          }
        }
      }

      // properties + patternProperties + additionalProperties
      const Json properties = direct(schema, "properties");
      const Json patternProperties = direct(schema, "patternProperties");
      const Json additional = direct(schema, "additionalProperties");
      std::set<std::string> matched;
      if (properties.isObject()) {
        for (const auto& key : value.getAllKeys().toVector()) {
          const std::string keyStr(key.toString());
          const Json prop = direct(properties, keyStr.c_str());
          if (!prop.isError()) {
            matched.insert(keyStr);
            if (!check(value[keyStr], prop, path + "/" + keyStr, depth + 1)) return false;
          }
        }
      }
      if (patternProperties.isObject()) {
        for (const auto& patKey : patternProperties.getAllKeys().toVector()) {
          std::regex re;
          try {
            re.assign(std::string(patKey.toString()));
          } catch (const std::regex_error&) {
            return fail(path, "invalid patternProperties regex");
          }
          for (const auto& key : value.getAllKeys().toVector()) {
            const std::string keyStr(key.toString());
            if (std::regex_search(keyStr, re)) {
              matched.insert(keyStr);
              if (!check(value[keyStr], direct(patternProperties, patKey.toString().c_str()),
                         path + "/" + keyStr, depth + 1)) {
                return false;
              }
            }
          }
        }
      }
      if (additional.isFalse()) {
        for (const auto& key : value.getAllKeys().toVector()) {
          if (matched.count(std::string(key.toString())) == 0) {
            return fail(path, "additional property '" + std::string(key.toString()) +
                                  "' is not allowed");
          }
        }
      } else if (additional.isObject()) {
        for (const auto& key : value.getAllKeys().toVector()) {
          const std::string keyStr(key.toString());
          if (matched.count(keyStr) == 0) {
            if (!check(value[keyStr], additional, path + "/" + keyStr, depth + 1)) return false;
          }
        }
      }
    }

    // composition
    const Json allOf = direct(schema, "allOf");
    if (allOf.isArray()) {
      for (const auto& sub : allOf.toVector()) {
        if (!check(value, sub, path, depth + 1)) return false;
      }
    }
    const Json anyOf = direct(schema, "anyOf");
    if (anyOf.isArray()) {
      bool any = false;
      for (const auto& sub : anyOf.toVector()) {
        Validator probe{root, limits, 0, ""};
        bool pr = probe.check(value, sub, path, depth + 1);
        fprintf(stderr, "[dbg] sub=%s pr=%d pErr=[%s] oErr=[%s]\n",
                sub.toString().c_str(), (int)pr, probe.error.c_str(), error.c_str());
        if (pr) any = true;
      }
      if (!any) return fail(path, "value matches none of anyOf");
    }
    const Json oneOf = direct(schema, "oneOf");
    if (oneOf.isArray()) {
      int matches = 0;
      for (const auto& sub : oneOf.toVector()) {
        Validator probe{root, limits, 0, ""};
        if (probe.check(value, sub, path, depth + 1)) matches++;
      }
      if (matches != 1) return fail(path, "value matches " + std::to_string(matches) +
                                              " of oneOf (expected exactly 1)");
    }
    const Json notSchema = direct(schema, "not");
    if (notSchema.isObject() || notSchema.isTrue() || notSchema.isFalse()) {
      Validator probe{root, limits, 0, ""};
      if (probe.check(value, notSchema, path, depth + 1)) {
        return fail(path, "value matches the 'not' schema");
      }
    }
    return true;
  }
};

}  // namespace

McpResult<void> validateSchema(const Json& instance, const Json& schema, const Json& root,
                               const SchemaLimits& limits) {
  Validator v{root, limits, 0, ""};
  if (v.check(instance, schema, "", 0)) return Ok();
  return Err(McpErrorCode::InvalidParams, v.error.empty() ? "schema validation failed" : v.error);
}

McpResult<void> validateSchema(const Json& instance, const Json& schema) {
  return validateSchema(instance, schema, schema, SchemaLimits{});
}

}  // namespace mcp
