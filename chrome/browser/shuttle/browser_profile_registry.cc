// Copyright 2026 Silkweave
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/shuttle/browser_profile_registry.h"

#include <limits>
#include <set>
#include <utility>

#include "base/check.h"
#include "base/files/file_util.h"
#include "base/files/important_file_writer.h"
#include "base/functional/bind.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/json/values_util.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "base/task/bind_post_task.h"
#include "base/task/sequenced_task_runner.h"
#include "base/uuid.h"
#include "crypto/hash.h"
#include "crypto/random.h"

namespace shuttle {

namespace {

constexpr size_t kMaxSlugLength = 64;
constexpr size_t kMaxDirectoryLength = 64;
constexpr char kKeyPrefix[] = "bp_";
constexpr size_t kKeyHexLength = 32;
constexpr char kReservedThrowawayPrefix[] = "tmp-";

// Manifest field names.
constexpr char kVersion[] = "version";
constexpr char kStartupProfileDir[] = "startupProfileDir";
constexpr char kProfiles[] = "profiles";
constexpr char kKey[] = "key";
constexpr char kDirectory[] = "directory";
constexpr char kWorkspaceAgent[] = "workspaceAgent";
constexpr char kLifecycleEpoch[] = "lifecycleEpoch";
constexpr char kState[] = "state";
constexpr char kLoaded[] = "loaded";
constexpr char kDeletionPending[] = "deletionPending";
constexpr char kRequestId[] = "requestId";
constexpr char kRequestFingerprint[] = "requestFingerprint";
constexpr char kPersona[] = "persona";
constexpr char kProxy[] = "proxy";
constexpr char kProvisioned[] = "provisioned";
constexpr char kPersonaReplacementPending[] = "personaReplacementPending";
constexpr char kPersonaSummary[] = "personaSummary";
constexpr char kLastError[] = "lastError";
constexpr char kCreated[] = "created";

std::string ToLowerAscii(std::string_view value) {
  return base::ToLowerASCII(value);
}

bool IsLowerHex(std::string_view value) {
  for (char c : value) {
    if (!base::IsAsciiDigit(c) && !(c >= 'a' && c <= 'f')) {
      return false;
    }
  }
  return true;
}

// Windows reserved device names, plus the Chromium directories that must
// never be a managed Profile directory.
bool IsReservedDirectoryName(std::string_view lower) {
  static constexpr std::string_view kReserved[] = {
      "default", "system profile", "guest profile", "con", "prn", "aux", "nul"};
  for (std::string_view reserved : kReserved) {
    if (lower == reserved) {
      return true;
    }
  }
  if (lower.size() == 4 &&
      (lower.substr(0, 3) == "com" || lower.substr(0, 3) == "lpt") &&
      base::IsAsciiDigit(lower[3])) {
    return true;
  }
  return false;
}

// A dictionary value stored as canonical JSON text ("" when absent).
std::string DictToJson(const base::DictValue* dict) {
  if (!dict) {
    return std::string();
  }
  return base::WriteJson(*dict).value_or(std::string());
}

std::optional<base::DictValue> JsonToDict(const std::string& json) {
  if (json.empty()) {
    return std::nullopt;
  }
  return base::JSONReader::ReadDict(json, base::JSON_PARSE_RFC);
}

base::Value DictJsonToValue(const std::string& json) {
  if (std::optional<base::DictValue> dict = JsonToDict(json)) {
    return base::Value(std::move(*dict));
  }
  return base::Value();
}

base::DictValue EntryToValue(const BrowserProfileEntry& entry) {
  base::DictValue dict;
  dict.Set(kKey, entry.key);
  dict.Set(kDirectory, entry.directory);
  dict.Set(kWorkspaceAgent, entry.workspace_agent);
  dict.Set(kLifecycleEpoch, static_cast<int>(entry.lifecycle_epoch));
  dict.Set(kState, std::string(BrowserProfileStateToString(entry.state)));
  dict.Set(kLoaded, entry.loaded);
  dict.Set(kDeletionPending, entry.deletion_pending);
  dict.Set(kRequestId, entry.request_id);
  dict.Set(kRequestFingerprint, entry.request_fingerprint);
  dict.Set(kPersona, DictJsonToValue(entry.persona_input_json));
  dict.Set(kProxy, DictJsonToValue(entry.proxy_pref_json));
  dict.Set(kProvisioned, entry.provisioned);
  dict.Set(kPersonaReplacementPending, entry.persona_replacement_pending);
  dict.Set(kPersonaSummary, DictJsonToValue(entry.persona_summary_json));
  dict.Set(kLastError, std::string(BrowserProfileErrorCode(entry.last_error)));
  dict.Set(kCreated, base::TimeToValue(entry.created));
  return dict;
}

// Parses one entry; nullopt rejects the whole manifest.
std::optional<BrowserProfileEntry> EntryFromValue(const base::Value& value) {
  const base::DictValue* dict = value.GetIfDict();
  if (!dict) {
    return std::nullopt;
  }
  BrowserProfileEntry entry;
  const std::string* key = dict->FindString(kKey);
  const std::string* directory = dict->FindString(kDirectory);
  const std::string* agent = dict->FindString(kWorkspaceAgent);
  const std::string* state = dict->FindString(kState);
  std::optional<int> epoch = dict->FindInt(kLifecycleEpoch);
  if (!key || !directory || !agent || !state || !epoch) {
    return std::nullopt;
  }
  if (!IsValidBrowserProfileKey(*key) || !IsSafeProfileDirectoryName(*directory) ||
      !IsValidWorkspaceAgentSlug(*agent) || *epoch < 1) {
    return std::nullopt;
  }
  std::optional<BrowserProfileState> parsed_state =
      BrowserProfileStateFromString(*state);
  if (!parsed_state) {
    return std::nullopt;
  }
  entry.key = *key;
  entry.directory = *directory;
  entry.workspace_agent = *agent;
  entry.lifecycle_epoch = *epoch;
  entry.state = *parsed_state;
  entry.loaded = dict->FindBool(kLoaded).value_or(false);
  entry.deletion_pending = dict->FindBool(kDeletionPending).value_or(false);
  entry.provisioned = dict->FindBool(kProvisioned).value_or(false);
  entry.persona_replacement_pending =
      dict->FindBool(kPersonaReplacementPending).value_or(false);
  if (const std::string* request_id = dict->FindString(kRequestId);
      request_id && !request_id->empty()) {
    base::Uuid uuid = base::Uuid::ParseCaseInsensitive(*request_id);
    if (!uuid.is_valid()) {
      return std::nullopt;
    }
    entry.request_id = uuid.AsLowercaseString();
  }
  if (const std::string* fingerprint = dict->FindString(kRequestFingerprint)) {
    entry.request_fingerprint = *fingerprint;
  }
  if (const base::Value* persona = dict->Find(kPersona)) {
    if (!persona->is_none()) {
      if (!persona->is_dict()) {
        return std::nullopt;
      }
      entry.persona_input_json = DictToJson(persona->GetIfDict());
    }
  }
  if (const base::Value* proxy = dict->Find(kProxy)) {
    if (!proxy->is_none()) {
      if (!proxy->is_dict()) {
        return std::nullopt;
      }
      entry.proxy_pref_json = DictToJson(proxy->GetIfDict());
    }
  }
  if (const base::Value* summary = dict->Find(kPersonaSummary)) {
    if (!summary->is_none()) {
      if (!summary->is_dict()) {
        return std::nullopt;
      }
      entry.persona_summary_json = DictToJson(summary->GetIfDict());
    }
  }
  if (const std::string* last_error = dict->FindString(kLastError);
      last_error && !last_error->empty()) {
    std::optional<BrowserProfileError> parsed =
        BrowserProfileErrorFromCode(*last_error);
    if (!parsed) {
      return std::nullopt;
    }
    entry.last_error = *parsed;
  }
  if (const base::Value* created = dict->Find(kCreated)) {
    entry.created = base::ValueToTime(*created).value_or(base::Time());
  }
  return entry;
}

void RunAsync(BrowserProfileRegistry::CommitCallback callback,
              base::expected<BrowserProfileEntry, BrowserProfileFailure> result) {
  base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
      FROM_HERE, base::BindOnce(std::move(callback), std::move(result)));
}

class FileManifestWriter : public BrowserProfileManifestWriter {
 public:
  FileManifestWriter(const base::FilePath& path,
                     scoped_refptr<base::SequencedTaskRunner> io_task_runner)
      : writer_(path, std::move(io_task_runner)) {}

  // BrowserProfileManifestWriter:
  void Write(std::string data,
             base::OnceCallback<void(bool success)> done) override {
    // The registry keeps one write in flight, so the single "next write"
    // callback slot of ImportantFileWriter is never clobbered. The callback
    // runs on the writer's task runner: hop back to this sequence.
    writer_.RegisterOnNextWriteCallbacks(
        base::OnceClosure(), base::BindPostTaskToCurrentDefault(std::move(done)));
    writer_.WriteNow(std::move(data));
  }

 private:
  base::ImportantFileWriter writer_;
};

}  // namespace

// --- Enums -------------------------------------------------------------------

std::string_view BrowserProfileStateToString(BrowserProfileState state) {
  switch (state) {
    case BrowserProfileState::kStopped:
      return "stopped";
    case BrowserProfileState::kLoading:
      return "loading";
    case BrowserProfileState::kInitializing:
      return "initializing";
    case BrowserProfileState::kReady:
      return "ready";
    case BrowserProfileState::kDraining:
      return "draining";
    case BrowserProfileState::kFailed:
      return "failed";
    case BrowserProfileState::kDeleted:
      return "deleted";
  }
  return "failed";
}

std::optional<BrowserProfileState> BrowserProfileStateFromString(
    std::string_view value) {
  static constexpr BrowserProfileState kAll[] = {
      BrowserProfileState::kStopped,      BrowserProfileState::kLoading,
      BrowserProfileState::kInitializing, BrowserProfileState::kReady,
      BrowserProfileState::kDraining,     BrowserProfileState::kFailed,
      BrowserProfileState::kDeleted};
  for (BrowserProfileState state : kAll) {
    if (BrowserProfileStateToString(state) == value) {
      return state;
    }
  }
  return std::nullopt;
}

std::string_view BrowserProfileErrorCode(BrowserProfileError error) {
  switch (error) {
    case BrowserProfileError::kNone:
      return "";
    case BrowserProfileError::kForbidden:
      return "forbidden";
    case BrowserProfileError::kUnsupported:
      return "unsupported";
    case BrowserProfileError::kInvalidProfileKey:
      return "invalid_profile_key";
    case BrowserProfileError::kProfileNotFound:
      return "profile_not_found";
    case BrowserProfileError::kProfileNotReady:
      return "profile_not_ready";
    case BrowserProfileError::kProfileBusy:
      return "profile_busy";
    case BrowserProfileError::kProfileFailed:
      return "profile_failed";
    case BrowserProfileError::kPersonaInvalid:
      return "persona_invalid";
    case BrowserProfileError::kProxyNotReady:
      return "proxy_not_ready";
    case BrowserProfileError::kStorageError:
      return "storage_error";
    case BrowserProfileError::kDrainTimeout:
      return "drain_timeout";
    case BrowserProfileError::kRequestConflict:
      return "request_conflict";
  }
  return "profile_failed";
}

std::optional<BrowserProfileError> BrowserProfileErrorFromCode(
    std::string_view code) {
  static constexpr BrowserProfileError kAll[] = {
      BrowserProfileError::kForbidden,
      BrowserProfileError::kUnsupported,
      BrowserProfileError::kInvalidProfileKey,
      BrowserProfileError::kProfileNotFound,
      BrowserProfileError::kProfileNotReady,
      BrowserProfileError::kProfileBusy,
      BrowserProfileError::kProfileFailed,
      BrowserProfileError::kPersonaInvalid,
      BrowserProfileError::kProxyNotReady,
      BrowserProfileError::kStorageError,
      BrowserProfileError::kDrainTimeout,
      BrowserProfileError::kRequestConflict};
  for (BrowserProfileError error : kAll) {
    if (BrowserProfileErrorCode(error) == code) {
      return error;
    }
  }
  return std::nullopt;
}

namespace {

// Fixed text per code: never secrets, paths, seeds or endpoints.
const char* MessageForError(BrowserProfileError error) {
  switch (error) {
    case BrowserProfileError::kNone:
      return "";
    case BrowserProfileError::kForbidden:
      return "Not permitted.";
    case BrowserProfileError::kUnsupported:
      return "Isolated profiles are not available.";
    case BrowserProfileError::kInvalidProfileKey:
      return "Invalid profile key.";
    case BrowserProfileError::kProfileNotFound:
      return "Profile not found.";
    case BrowserProfileError::kProfileNotReady:
      return "Profile is not ready.";
    case BrowserProfileError::kProfileBusy:
      return "Profile is busy.";
    case BrowserProfileError::kProfileFailed:
      return "Profile failed.";
    case BrowserProfileError::kPersonaInvalid:
      return "Persona is invalid.";
    case BrowserProfileError::kProxyNotReady:
      return "Proxy is not ready.";
    case BrowserProfileError::kStorageError:
      return "Storage error.";
    case BrowserProfileError::kDrainTimeout:
      return "Drain timed out; the profile is still draining.";
    case BrowserProfileError::kRequestConflict:
      return "requestId was used with different arguments.";
  }
  return "";
}

}  // namespace

BrowserProfileFailure::BrowserProfileFailure() = default;
BrowserProfileFailure::BrowserProfileFailure(BrowserProfileError code)
    : code(code), message(MessageForError(code)) {}
BrowserProfileFailure::BrowserProfileFailure(BrowserProfileError code,
                                             std::string message)
    : code(code), message(std::move(message)) {}
BrowserProfileFailure::BrowserProfileFailure(const BrowserProfileFailure&) =
    default;
BrowserProfileFailure& BrowserProfileFailure::operator=(
    const BrowserProfileFailure&) = default;
BrowserProfileFailure::~BrowserProfileFailure() = default;

base::DictValue BrowserProfileFailure::ToValue() const {
  return base::DictValue()
      .Set("code", std::string(BrowserProfileErrorCode(code)))
      .Set("message", message);
}

// --- Validation ----------------------------------------------------------------

bool IsValidBrowserProfileKey(std::string_view key) {
  constexpr size_t kPrefixLength = sizeof(kKeyPrefix) - 1;
  return key.size() == kPrefixLength + kKeyHexLength &&
         key.starts_with(kKeyPrefix) && IsLowerHex(key.substr(kPrefixLength));
}

std::string GenerateBrowserProfileKey() {
  return std::string(kKeyPrefix) +
         base::HexEncodeLower(crypto::RandBytesAsVector(kKeyHexLength / 2));
}

bool IsValidWorkspaceAgentSlug(std::string_view slug) {
  if (slug.empty() || slug.size() > kMaxSlugLength || slug.front() == '-' ||
      slug.back() == '-' || slug.starts_with(kReservedThrowawayPrefix)) {
    return false;
  }
  char previous = 0;
  for (char c : slug) {
    bool ok = base::IsAsciiLower(c) || base::IsAsciiDigit(c) || c == '-';
    if (!ok || (c == '-' && previous == '-')) {
      return false;
    }
    previous = c;
  }
  return true;
}

bool IsSafeProfileDirectoryName(std::string_view name) {
  if (name.empty() || name.size() > kMaxDirectoryLength ||
      name.front() == ' ' || name.back() == ' ') {
    return false;
  }
  for (char c : name) {
    bool ok = base::IsAsciiAlpha(c) || base::IsAsciiDigit(c) || c == ' ' ||
              c == '_' || c == '-';
    if (!ok) {
      return false;
    }
  }
  return !IsReservedDirectoryName(ToLowerAscii(name));
}

std::string ComputeCreateRequestFingerprint(std::string_view workspace_agent,
                                            std::string_view persona_json,
                                            std::string_view proxy_json) {
  std::string canonical = "shuttle-create-v1\n";
  canonical.append(workspace_agent);
  canonical.push_back('\n');
  canonical.append(persona_json);
  canonical.push_back('\n');
  canonical.append(proxy_json);
  return base::HexEncodeLower(crypto::hash::Sha256(canonical));
}

bool IsValidBrowserProfileTransition(BrowserProfileState from,
                                     BrowserProfileState to) {
  using S = BrowserProfileState;
  switch (from) {
    case S::kStopped:
      return to == S::kLoading || to == S::kDeleted;
    case S::kLoading:
      return to == S::kInitializing || to == S::kFailed;
    case S::kInitializing:
      return to == S::kReady || to == S::kFailed;
    case S::kReady:
      return to == S::kDraining || to == S::kFailed;
    case S::kDraining:
      return to == S::kStopped || to == S::kFailed;
    case S::kFailed:
      return to == S::kLoading || to == S::kStopped;
    case S::kDeleted:
      return false;
  }
  return false;
}

BrowserProfileError PersonaReplacementStateError(BrowserProfileState state) {
  switch (state) {
    case BrowserProfileState::kStopped:
      return BrowserProfileError::kNone;
    case BrowserProfileState::kReady:
    case BrowserProfileState::kLoading:
    case BrowserProfileState::kInitializing:
    case BrowserProfileState::kDraining:
      return BrowserProfileError::kProfileBusy;
    case BrowserProfileState::kFailed:
      return BrowserProfileError::kProfileNotReady;
    case BrowserProfileState::kDeleted:
      return BrowserProfileError::kProfileNotFound;
  }
  return BrowserProfileError::kProfileFailed;
}

bool TransitionBumpsLifecycleEpoch(BrowserProfileState from,
                                   BrowserProfileState to) {
  using S = BrowserProfileState;
  return IsValidBrowserProfileTransition(from, to) &&
         (to == S::kLoading || to == S::kDraining || to == S::kDeleted);
}

// --- Entry and manifest ---------------------------------------------------------

BrowserProfileEntry::BrowserProfileEntry() = default;
BrowserProfileEntry::BrowserProfileEntry(const BrowserProfileEntry&) = default;
BrowserProfileEntry& BrowserProfileEntry::operator=(
    const BrowserProfileEntry&) = default;
BrowserProfileEntry::~BrowserProfileEntry() = default;

BrowserProfileManifest::BrowserProfileManifest() = default;
BrowserProfileManifest::BrowserProfileManifest(BrowserProfileManifest&&) =
    default;
BrowserProfileManifest& BrowserProfileManifest::operator=(
    BrowserProfileManifest&&) = default;
BrowserProfileManifest::~BrowserProfileManifest() = default;

BrowserProfileParseResult::BrowserProfileParseResult() = default;
BrowserProfileParseResult::BrowserProfileParseResult(
    BrowserProfileParseResult&&) = default;
BrowserProfileParseResult& BrowserProfileParseResult::operator=(
    BrowserProfileParseResult&&) = default;
BrowserProfileParseResult::~BrowserProfileParseResult() = default;

BrowserProfileParseResult ParseBrowserProfileManifest(std::string_view json) {
  BrowserProfileParseResult result;
  std::optional<base::DictValue> root =
      base::JSONReader::ReadDict(json, base::JSON_PARSE_RFC);
  if (!root) {
    result.error = BrowserProfileParseError::kMalformed;
    return result;
  }
  std::optional<int> version = root->FindInt(kVersion);
  if (!version) {
    result.error = BrowserProfileParseError::kMalformed;
    return result;
  }
  if (*version != kBrowserProfilesManifestVersion) {
    // Written by another core version: never interpreted, never overwritten.
    result.error = BrowserProfileParseError::kUnknownVersion;
    return result;
  }
  if (const std::string* startup = root->FindString(kStartupProfileDir)) {
    if (!startup->empty() && !IsSafeProfileDirectoryName(*startup) &&
        *startup != "Default") {
      result.error = BrowserProfileParseError::kMalformed;
      return result;
    }
    result.manifest.startup_profile_dir = *startup;
  }
  const base::ListValue* profiles = root->FindList(kProfiles);
  if (!profiles) {
    result.error = BrowserProfileParseError::kMalformed;
    return result;
  }
  std::set<std::string> keys;
  std::set<std::string> directories;
  std::set<std::string> request_ids;
  for (const base::Value& value : *profiles) {
    std::optional<BrowserProfileEntry> entry = EntryFromValue(value);
    if (!entry || !keys.insert(entry->key).second ||
        !directories.insert(ToLowerAscii(entry->directory)).second ||
        (!entry->request_id.empty() &&
         !request_ids.insert(entry->request_id).second)) {
      result.manifest = BrowserProfileManifest();
      result.error = BrowserProfileParseError::kMalformed;
      return result;
    }
    result.manifest.entries.push_back(std::move(*entry));
  }
  return result;
}

std::string SerializeBrowserProfileManifest(
    const BrowserProfileManifest& manifest) {
  base::ListValue profiles;
  for (const BrowserProfileEntry& entry : manifest.entries) {
    profiles.Append(EntryToValue(entry));
  }
  base::DictValue root;
  root.Set(kVersion, kBrowserProfilesManifestVersion);
  root.Set(kStartupProfileDir, manifest.startup_profile_dir);
  root.Set(kProfiles, std::move(profiles));
  return base::WriteJsonWithOptions(root, base::JSONWriter::OPTIONS_PRETTY_PRINT)
      .value_or(std::string());
}

std::unique_ptr<BrowserProfileManifestWriter> CreateFileManifestWriter(
    const base::FilePath& path,
    scoped_refptr<base::SequencedTaskRunner> io_task_runner) {
  return std::make_unique<FileManifestWriter>(path, std::move(io_task_runner));
}

// --- Registry -----------------------------------------------------------------------

BrowserProfileRegistry::CreateRequest::CreateRequest() = default;
BrowserProfileRegistry::CreateRequest::CreateRequest(const CreateRequest&) =
    default;
BrowserProfileRegistry::CreateRequest::~CreateRequest() = default;

BrowserProfileRegistry::Waiter::Waiter() = default;
BrowserProfileRegistry::Waiter::Waiter(Waiter&&) = default;
BrowserProfileRegistry::Waiter& BrowserProfileRegistry::Waiter::operator=(
    Waiter&&) = default;
BrowserProfileRegistry::Waiter::~Waiter() = default;

BrowserProfileRegistry::BrowserProfileRegistry(
    BrowserProfileManifest manifest,
    std::unique_ptr<BrowserProfileManifestWriter> writer)
    : writer_(std::move(writer)),
      startup_profile_dir_(std::move(manifest.startup_profile_dir)) {
  CHECK(writer_);
  for (BrowserProfileEntry& entry : manifest.entries) {
    if (entry.state != BrowserProfileState::kDeleted) {
      // Browser restart: nothing is loaded, whatever the manifest says. The
      // lifecycle epoch is kept; the next load increments it.
      entry.state = BrowserProfileState::kStopped;
      entry.loaded = false;
      entry.last_error = BrowserProfileError::kNone;
    }
    entries_[entry.key] = std::move(entry);
  }
  durable_ = Snapshot();
}

BrowserProfileRegistry::~BrowserProfileRegistry() = default;

// static
BrowserProfileParseResult BrowserProfileRegistry::ReadManifestFile(
    const base::FilePath& user_data_dir) {
  base::FilePath path = user_data_dir.AppendASCII(kBrowserProfilesFileName);
  if (!base::PathExists(path)) {
    return BrowserProfileParseResult();
  }
  std::string data;
  if (!base::ReadFileToString(path, &data)) {
    BrowserProfileParseResult result;
    result.error = BrowserProfileParseError::kMalformed;
    return result;
  }
  return ParseBrowserProfileManifest(data);
}

BrowserProfileEntry* BrowserProfileRegistry::FindMutable(std::string_view key) {
  auto it = entries_.find(std::string(key));
  return it == entries_.end() ? nullptr : &it->second;
}

const BrowserProfileEntry* BrowserProfileRegistry::Find(
    std::string_view key) const {
  auto it = entries_.find(std::string(key));
  return it == entries_.end() ? nullptr : &it->second;
}

const BrowserProfileEntry* BrowserProfileRegistry::FindByDirectory(
    std::string_view directory) const {
  std::string lower = ToLowerAscii(directory);
  for (const auto& [key, entry] : entries_) {
    if (ToLowerAscii(entry.directory) == lower) {
      return &entry;
    }
  }
  return nullptr;
}

const BrowserProfileEntry* BrowserProfileRegistry::FindByRequestId(
    std::string_view request_id) const {
  if (request_id.empty()) {
    return nullptr;
  }
  for (const auto& [key, entry] : entries_) {
    if (entry.request_id == request_id) {
      return &entry;
    }
  }
  return nullptr;
}

std::vector<BrowserProfileEntry> BrowserProfileRegistry::List() const {
  std::vector<BrowserProfileEntry> result;
  for (const auto& [key, entry] : entries_) {
    result.push_back(entry);
  }
  return result;
}

void BrowserProfileRegistry::RecordStartupProfileDir(
    const std::string& directory) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (!startup_profile_dir_.empty() || directory.empty()) {
    return;
  }
  startup_profile_dir_ = directory;
  // Persisted with the next commit; it is only a restart hint.
}

void BrowserProfileRegistry::Create(CreateRequest request,
                                    CommitCallback callback) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  base::Uuid uuid = base::Uuid::ParseCaseInsensitive(request.request_id);
  if (!uuid.is_valid()) {
    RunAsync(std::move(callback),
             base::unexpected(BrowserProfileFailure(
                 BrowserProfileError::kProfileFailed, "requestId is invalid.")));
    return;
  }
  request.request_id = uuid.AsLowercaseString();

  if (const BrowserProfileEntry* existing =
          FindByRequestId(request.request_id)) {
    if (existing->request_fingerprint != request.request_fingerprint) {
      RunAsync(std::move(callback),
               base::unexpected(
                   BrowserProfileFailure(BrowserProfileError::kRequestConflict)));
      return;
    }
    // Replay: no new entry, no epoch change. Wait for any pending write so
    // the caller never sees an entry that is not durable.
    if (HasPendingWrite()) {
      Waiter waiter;
      waiter.entry = *existing;
      waiter.callback = std::move(callback);
      Commit(std::move(waiter));
    } else {
      RunAsync(std::move(callback), *existing);
    }
    return;
  }

  if (!IsValidBrowserProfileKey(request.key) ||
      !IsSafeProfileDirectoryName(request.directory) ||
      !IsValidWorkspaceAgentSlug(request.workspace_agent) ||
      Find(request.key) || FindByDirectory(request.directory)) {
    RunAsync(std::move(callback),
             base::unexpected(
                 BrowserProfileFailure(BrowserProfileError::kProfileFailed)));
    return;
  }

  BrowserProfileEntry entry;
  entry.key = std::move(request.key);
  entry.directory = std::move(request.directory);
  entry.workspace_agent = std::move(request.workspace_agent);
  entry.lifecycle_epoch = 1;
  entry.state = BrowserProfileState::kStopped;
  entry.request_id = std::move(request.request_id);
  entry.request_fingerprint = std::move(request.request_fingerprint);
  entry.persona_input_json = std::move(request.persona_input_json);
  entry.proxy_pref_json = std::move(request.proxy_pref_json);
  entry.created = base::Time::Now();
  entries_[entry.key] = entry;

  Waiter waiter;
  waiter.entry = std::move(entry);
  waiter.callback = std::move(callback);
  Commit(std::move(waiter));
}

void BrowserProfileRegistry::Transition(std::string_view key,
                                        BrowserProfileState to,
                                        BrowserProfileError last_error,
                                        CommitCallback callback) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  BrowserProfileEntry* entry = FindMutable(key);
  if (!entry) {
    RunAsync(std::move(callback),
             base::unexpected(
                 BrowserProfileFailure(BrowserProfileError::kProfileNotFound)));
    return;
  }
  if (!IsValidBrowserProfileTransition(entry->state, to)) {
    RunAsync(std::move(callback),
             base::unexpected(
                 BrowserProfileFailure(BrowserProfileError::kProfileBusy)));
    return;
  }
  if (TransitionBumpsLifecycleEpoch(entry->state, to)) {
    if (entry->lifecycle_epoch >= std::numeric_limits<int>::max()) {
      RunAsync(std::move(callback),
               base::unexpected(
                   BrowserProfileFailure(BrowserProfileError::kStorageError)));
      return;
    }
    ++entry->lifecycle_epoch;
  }
  entry->state = to;
  entry->last_error = last_error;
  if (to == BrowserProfileState::kDeleted) {
    entry->deletion_pending = true;
    entry->loaded = false;
  }
  Waiter waiter;
  waiter.entry = *entry;
  waiter.callback = std::move(callback);
  Commit(std::move(waiter));
}

void BrowserProfileRegistry::ReplacePersonaIntent(
    std::string_view key,
    std::string persona_input_json,
    std::string persona_summary_json,
    CommitCallback callback) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  BrowserProfileEntry* entry = FindMutable(key);
  const BrowserProfileError state_error =
      entry ? PersonaReplacementStateError(entry->state)
            : BrowserProfileError::kProfileNotFound;
  if (state_error != BrowserProfileError::kNone) {
    RunAsync(std::move(callback),
             base::unexpected(BrowserProfileFailure(state_error)));
    return;
  }
  if (persona_input_json.empty() ||
      entry->lifecycle_epoch >= std::numeric_limits<int>::max()) {
    RunAsync(std::move(callback),
             base::unexpected(BrowserProfileFailure(
                 persona_input_json.empty() ? BrowserProfileError::kPersonaInvalid
                                            : BrowserProfileError::kStorageError)));
    return;
  }
  // Intent, pending flag and epoch change in memory together and are made
  // durable by one write; a failed write rolls the whole batch back.
  entry->persona_input_json = std::move(persona_input_json);
  entry->persona_summary_json = std::move(persona_summary_json);
  entry->persona_replacement_pending = true;
  entry->last_error = BrowserProfileError::kNone;
  ++entry->lifecycle_epoch;
  Waiter waiter;
  waiter.entry = *entry;
  waiter.callback = std::move(callback);
  Commit(std::move(waiter));
}

void BrowserProfileRegistry::ReinstatePersonaPending(
    std::string_view key, int64_t expected_epoch,
    std::string_view expected_intent, CommitCallback callback) {
  auto* entry = FindMutable(key);
  if (!entry || entry->lifecycle_epoch != expected_epoch ||
      entry->persona_input_json != expected_intent) {
    RunAsync(std::move(callback), base::unexpected(
        BrowserProfileFailure(BrowserProfileError::kProfileBusy)));
    return;
  }
  entry->persona_replacement_pending = true;
  Waiter waiter;
  waiter.entry = *entry;
  waiter.callback = std::move(callback);
  Commit(std::move(waiter));
}

void BrowserProfileRegistry::MarkPersonaCommitted(
    std::string_view key,
    int64_t expected_epoch,
    std::string_view expected_persona_input_json,
    std::string persona_summary_json,
    CommitCallback callback) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  BrowserProfileEntry* entry = FindMutable(key);
  if (!entry || entry->state == BrowserProfileState::kDeleted) {
    RunAsync(std::move(callback),
             base::unexpected(
                 BrowserProfileFailure(BrowserProfileError::kProfileNotFound)));
    return;
  }
  if (!entry->persona_replacement_pending ||
      entry->lifecycle_epoch != expected_epoch ||
      entry->persona_input_json != expected_persona_input_json) {
    RunAsync(std::move(callback),
             base::unexpected(
                 BrowserProfileFailure(BrowserProfileError::kProfileBusy)));
    return;
  }
  entry->persona_replacement_pending = false;
  entry->persona_summary_json = std::move(persona_summary_json);
  Waiter waiter;
  waiter.entry = *entry;
  waiter.callback = std::move(callback);
  Commit(std::move(waiter));
}

void BrowserProfileRegistry::ForceStateInMemory(std::string_view key,
                                                BrowserProfileState to,
                                                BrowserProfileError last_error) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  BrowserProfileEntry* entry = FindMutable(key);
  if (!entry || !IsValidBrowserProfileTransition(entry->state, to) ||
      TransitionBumpsLifecycleEpoch(entry->state, to)) {
    return;
  }
  entry->state = to;
  entry->last_error = last_error;
}

void BrowserProfileRegistry::MarkProvisioned(std::string_view key,
                                             int64_t expected_epoch,
                                             std::string_view expected_persona_input_json,
                                             std::string persona_summary_json,
                                             CommitCallback callback) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  BrowserProfileEntry* entry = FindMutable(key);
  if (!entry || entry->state == BrowserProfileState::kDeleted) {
    RunAsync(std::move(callback),
             base::unexpected(
                 BrowserProfileFailure(BrowserProfileError::kProfileNotFound)));
    return;
  }
  if (entry->lifecycle_epoch != expected_epoch ||
      entry->persona_input_json != expected_persona_input_json) {
    RunAsync(std::move(callback),
             base::unexpected(
                 BrowserProfileFailure(BrowserProfileError::kProfileBusy)));
    return;
  }
  entry->provisioned = true;
  entry->persona_replacement_pending = false;
  entry->persona_summary_json = std::move(persona_summary_json);
  Waiter waiter;
  waiter.entry = *entry;
  waiter.callback = std::move(callback);
  Commit(std::move(waiter));
}

void BrowserProfileRegistry::ConfirmDeletion(std::string_view key,
                                             CommitCallback callback) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  BrowserProfileEntry* entry = FindMutable(key);
  if (!entry || entry->state != BrowserProfileState::kDeleted) {
    RunAsync(std::move(callback),
             base::unexpected(
                 BrowserProfileFailure(BrowserProfileError::kProfileNotFound)));
    return;
  }
  entry->deletion_pending = false;
  Waiter waiter;
  waiter.entry = *entry;
  waiter.callback = std::move(callback);
  Commit(std::move(waiter));
}

void BrowserProfileRegistry::SetLoaded(std::string_view key, bool loaded) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (BrowserProfileEntry* entry = FindMutable(key)) {
    entry->loaded = loaded;
  }
}

BrowserProfileManifest BrowserProfileRegistry::Snapshot() const {
  BrowserProfileManifest manifest;
  manifest.startup_profile_dir = startup_profile_dir_;
  for (const auto& [key, entry] : entries_) {
    manifest.entries.push_back(entry);
  }
  return manifest;
}

void BrowserProfileRegistry::Commit(Waiter waiter) {
  waiters_.push_back(std::move(waiter));
  if (!write_in_flight_) {
    StartWrite();
  }
}

void BrowserProfileRegistry::StartWrite() {
  CHECK(!write_in_flight_);
  write_in_flight_ = true;
  in_flight_waiters_ = std::move(waiters_);
  waiters_.clear();
  BrowserProfileManifest snapshot = Snapshot();
  if (fail_next_write_for_testing_) {
    fail_next_write_for_testing_ = false;
    base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
        FROM_HERE, base::BindOnce(&BrowserProfileRegistry::OnWritten,
                                  weak_factory_.GetWeakPtr(),
                                  std::move(snapshot), false));
    return;
  }
  std::string data = SerializeBrowserProfileManifest(snapshot);
  if (data.empty()) {
    // Cannot serialize: treat as a failed write.
    base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
        FROM_HERE, base::BindOnce(&BrowserProfileRegistry::OnWritten,
                                  weak_factory_.GetWeakPtr(),
                                  std::move(snapshot), false));
    return;
  }
  writer_->Write(std::move(data),
                 base::BindOnce(&BrowserProfileRegistry::OnWritten,
                                 weak_factory_.GetWeakPtr(), std::move(snapshot)));
  if (write_started_for_testing_)
    std::move(write_started_for_testing_).Run();
}

void BrowserProfileRegistry::OnWritten(BrowserProfileManifest snapshot,
                                       bool success) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  write_in_flight_ = false;
  std::vector<Waiter> done = std::move(in_flight_waiters_);
  in_flight_waiters_.clear();
  if (success) {
    durable_ = std::move(snapshot);
  } else {
    // Roll back every change that is not durable (this batch and the queued
    // ones) so memory never runs ahead of disk; all their callers fail.
    entries_.clear();
    startup_profile_dir_ = durable_.startup_profile_dir;
    for (const BrowserProfileEntry& entry : durable_.entries) {
      entries_[entry.key] = entry;
    }
    for (Waiter& waiter : waiters_) {
      done.push_back(std::move(waiter));
    }
    waiters_.clear();
  }
  if (!waiters_.empty()) {
    StartWrite();
  }
  for (Waiter& waiter : done) {
    if (success) {
      std::move(waiter.callback).Run(std::move(waiter.entry));
    } else {
      std::move(waiter.callback)
          .Run(base::unexpected(
              BrowserProfileFailure(BrowserProfileError::kStorageError)));
    }
  }
}

}  // namespace shuttle
