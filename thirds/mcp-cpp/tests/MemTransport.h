#pragma once
// Test helper: in-memory ITransport fed from a vector of lines.

#include <mcp/mcp.hpp>

#include <string>
#include <utility>
#include <vector>

class MemTransport final : public mcp::ITransport {
 public:
  explicit MemTransport(std::vector<std::string> input) : input_(std::move(input)) {}

  mcp::McpResult<std::optional<std::string>> read() override {
    if (pos_ >= input_.size()) return mcp::Ok(std::optional<std::string>(std::nullopt));
    return mcp::Ok(std::optional<std::string>(input_[pos_++]));
  }

  mcp::McpResult<void> write(std::string_view message) override {
    output_.emplace_back(message);
    return mcp::Ok();
  }

  mcp::McpResult<void> flush() override { return mcp::Ok(); }

  std::vector<std::string> output_;

 private:
  std::vector<std::string> input_;
  size_t pos_ = 0;
};
