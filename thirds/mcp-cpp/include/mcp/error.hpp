#pragma once
// ---------------------------------------------------------------------------
// mcp/error.hpp - MCP error model + result type.
//
// Error codes follow JSON-RPC 2.0 plus the MCP-reserved sub-range
// (-32020..-32099). Source of truth: specification 2026-07-28,
// basic/index.md "Error codes" (the three named MCP codes were verified
// against it; codes introduced by earlier drafts were renumbered there).
//
// Result type: `McpResult<T>` aliases `std::expected<T, McpError>` when the
// standard library provides it (C++23), otherwise a built-in value-semantic
// `Result<T>` with a matching surface (has_value/value/error) is used, so
// call sites are identical under both modes. Error returns are written with
// the `Ok(...)` / `Err(...)` factories, which also paper over the
// std::unexpected vs. constructor difference between the two modes:
//
//   McpResult<int> divide(int a, int b) {
//     if (b == 0) return Err(McpErrorCode::InvalidParams, "b must not be 0");
//     return Ok(a / b);
//   }
// ---------------------------------------------------------------------------

#include <mcp/json.hpp>

#include <cstdint>
#include <string>
#include <utility>
#include <variant>

#if defined(__cpp_lib_expected) && __cpp_lib_expected >= 202202L
#include <expected>
#define MCP_HAS_STD_EXPECTED 1
#endif

namespace mcp {

// ---- error codes ----------------------------------------------------------

enum class McpErrorCode : int32_t {
  // JSON-RPC 2.0 standard codes.
  ParseError = -32700,
  InvalidRequest = -32600,
  MethodNotFound = -32601,
  InvalidParams = -32602,
  InternalError = -32603,

  // MCP-reserved sub-range (2026-07-28). Note: resource-not-found is NOT a
  // dedicated code in this spec version - it maps to InvalidParams (-32602).
  HeaderMismatch = -32020,
  MissingRequiredClientCapability = -32021,
  UnsupportedProtocolVersion = -32022,
};

// Bounds of the MCP-reserved implementation error sub-range.
inline constexpr int32_t kMcpReservedCodeMin = -32099;
inline constexpr int32_t kMcpReservedCodeMax = -32020;

// True when `code` falls into the range reserved for the MCP specification.
constexpr bool isMcpReservedCode(int32_t code) noexcept {
  return code >= kMcpReservedCodeMin && code <= kMcpReservedCodeMax;
}

// ---- error object ----------------------------------------------------------
// `data` (JSON payload, e.g. requiredCapabilities for -32021) is added with
// the M1 message model once Json ownership semantics are settled; see
// PLAN.md Update 3.

struct McpError {
  McpErrorCode code;
  std::string message;
  Json data = Json(nullptr);  // structured payload (e.g. -32022 supported list)
};

// ---- result type -----------------------------------------------------------

#ifdef MCP_HAS_STD_EXPECTED

template <class T>
using McpResult = std::expected<T, McpError>;

namespace detail {

template <class T>
McpResult<T> makeOk(T&& v) {
  return std::forward<T>(v);
}

inline McpResult<void> makeOk() { return {}; }

template <class T>
McpResult<T> makeErr(McpError e) {
  return std::unexpected(std::move(e));
}

}  // namespace detail

#else  // C++17 fallback

// Value-semantic result with the same member surface as std::expected, so
// handler code is identical under both modes.
template <class T>
class [[nodiscard]] Result {
 public:
  Result(T v) : v_(std::in_place_index<0>, std::move(v)) {}
  Result(McpError e) : v_(std::in_place_index<1>, std::move(e)) {}

  bool has_value() const noexcept { return v_.index() == 0; }
  explicit operator bool() const noexcept { return has_value(); }

  T& value() & { return std::get<0>(v_); }
  const T& value() const& { return std::get<0>(v_); }
  T&& value() && { return std::get<0>(std::move(v_)); }

  T* operator->() { return &std::get<0>(v_); }
  const T* operator->() const { return &std::get<0>(v_); }
  T& operator*() & { return std::get<0>(v_); }
  const T& operator*() const& { return std::get<0>(v_); }
  T&& operator*() && { return std::get<0>(std::move(v_)); }

  McpError& error() & { return std::get<1>(v_); }
  const McpError& error() const& { return std::get<1>(v_); }
  McpError&& error() && { return std::get<1>(std::move(v_)); }

 private:
  std::variant<T, McpError> v_;
};

// void specialization: no value payload, only ok/error.
template <>
class [[nodiscard]] Result<void> {
 public:
  Result() = default;
  Result(McpError e) : v_(std::move(e)) {}

  bool has_value() const noexcept { return v_.index() == 0; }
  explicit operator bool() const noexcept { return has_value(); }

  void value() const noexcept {}

  McpError& error() & { return std::get<1>(v_); }
  const McpError& error() const& { return std::get<1>(v_); }
  McpError&& error() && { return std::get<1>(std::move(v_)); }

 private:
  std::variant<std::monostate, McpError> v_{};
};

template <class T>
using McpResult = Result<T>;

namespace detail {

template <class T>
McpResult<std::decay_t<T>> makeOk(T&& v) {
  return std::forward<T>(v);
}

inline McpResult<void> makeOk() { return {}; }

template <class T>
McpResult<T> makeErr(McpError e) {
  return McpResult<T>(std::move(e));
}

}  // namespace detail

#endif  // MCP_HAS_STD_EXPECTED

// ---- Ok / Err factories ----------------------------------------------------
// `Ok(v)` / `Ok()` build a success result. Error returns go through `Err`,
// whose carrier converts implicitly to whatever McpResult<T> the enclosing
// function returns - so no explicit template argument is needed:
//   return Err(McpErrorCode::InvalidParams, "bad input");

namespace detail {

// Carrier for an error value awaiting conversion to a concrete McpResult<T>.
struct ErrCarrier {
  McpError e;

  template <class T>
  operator McpResult<T>() const {
    return makeErr<T>(e);
  }
  template <class T>
  operator McpResult<T>() && {
    return makeErr<T>(std::move(e));
  }
};

}  // namespace detail

template <class T>
McpResult<std::decay_t<T>> Ok(T&& v) {
  return detail::makeOk(std::forward<T>(v));
}

inline McpResult<void> Ok() { return detail::makeOk(); }

inline detail::ErrCarrier Err(McpError e) { return detail::ErrCarrier{std::move(e)}; }

inline detail::ErrCarrier Err(McpErrorCode code, std::string message) {
  return detail::ErrCarrier{McpError{code, std::move(message)}};
}

}  // namespace mcp
