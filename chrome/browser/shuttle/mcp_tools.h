// Copyright 2026 Silkweave
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_SHUTTLE_MCP_TOOLS_H_
#define CHROME_BROWSER_SHUTTLE_MCP_TOOLS_H_

#include <optional>
#include <string>
#include <string_view>

#include "base/containers/span.h"
#include "base/functional/callback_forward.h"
#include "base/memory/raw_ptr.h"
#include "base/values.h"
#include "chrome/browser/shuttle/browser_profile_registry.h"

namespace shuttle {

class ShuttleService;

// Result of an MCP tool call (`CallToolResult`).
struct ToolResult {
  ToolResult();
  ToolResult(ToolResult&&);
  ToolResult& operator=(ToolResult&&);
  ~ToolResult();

  // Text block with `value` as JSON, plus `structuredContent` for dicts.
  static ToolResult Json(base::Value value);
  static ToolResult Error(std::string message);
  // A contract error (PROFILES.md section 3): isError:true and
  // structuredContent.error = {code, message}; the message is the failure's
  // fixed text (never a secret, path, seed or endpoint).
  static ToolResult ProfileFailure(const BrowserProfileFailure& failure);
  static ToolResult ProfileFailure(BrowserProfileError code);
  static ToolResult Image(std::string base64_data, std::string mime_type);

  base::DictValue ToValue() &&;

  base::ListValue content;
  std::optional<base::DictValue> structured_content;
  bool is_error = false;
};

using ToolCallback = base::OnceCallback<void(ToolResult)>;

struct ToolContext {
  raw_ptr<ShuttleService> service;
  // Host header of the MCP request ("127.0.0.1:9222"), for CDP URLs.
  std::string host;
};

using ToolFunction = void (*)(const ToolContext& context,
                              const base::DictValue& args,
                              ToolCallback callback);

struct Tool {
  std::string_view name;
  std::string_view description;
  // JSON Schema of the arguments.
  std::string_view input_schema;
  ToolFunction run;
};

base::span<const Tool> GetTools();

// Browser-level tools (PROFILES.md section 3): not bound to a Profile, so
// they take no ToolContext and no `_meta` selector. They are routed through
// BrowserProfileRouter by ShuttleService::HandleHttpMcpRequest.
using BrowserToolFunction = void (*)(const base::DictValue& args,
                                     ToolCallback callback);

struct BrowserTool {
  std::string_view name;
  std::string_view description;
  // JSON Schema of the arguments; every one has additionalProperties:false and
  // the run function enforces it.
  std::string_view input_schema;
  // Admin-only tools are hidden from non-admin tools/list and refused to
  // non-admin callers (`forbidden`).
  bool admin_only;
  BrowserToolFunction run;
};

base::span<const BrowserTool> GetBrowserTools();
// null if `name` is not a browser-level tool.
const BrowserTool* FindBrowserTool(std::string_view name);

}  // namespace shuttle

#endif  // CHROME_BROWSER_SHUTTLE_MCP_TOOLS_H_
