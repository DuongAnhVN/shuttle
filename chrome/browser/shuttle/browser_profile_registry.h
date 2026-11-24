// Copyright 2026 Silkweave
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_SHUTTLE_BROWSER_PROFILE_REGISTRY_H_
#define CHROME_BROWSER_SHUTTLE_BROWSER_PROFILE_REGISTRY_H_

#include <stdint.h>

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "base/files/file_path.h"
#include "base/functional/callback.h"
#include "base/memory/scoped_refptr.h"
#include "base/memory/weak_ptr.h"
#include "base/sequence_checker.h"
#include "base/time/time.h"
#include "base/types/expected.h"
#include "base/values.h"

namespace base {
class SequencedTaskRunner;
}

namespace shuttle {

// The persistent registry of isolated ("managed") Chromium Profiles of this
// browser: the core side of tools/shuttle/docs/PROFILES.md (contract V1,
// sections 1, 3 and 5). It is NOT the agent registry (profile_registry.h:
// agents/workspaces inside one Chromium Profile) and has no dependency on
// Profile, ProfileManager or content: it is plain data + a state machine +
// atomic persistence, so it can be unit tested alone.
//
// Not yet compiled (see PROFILES.md, "Implementation status").

// File name, in the root of the user data dir (never inside a Profile).
inline constexpr char kBrowserProfilesFileName[] = "ShuttleBrowserProfiles.json";
inline constexpr int kBrowserProfilesManifestVersion = 1;

// Contract section 5: stopped -> loading -> initializing -> ready ->
// draining -> stopped; failures -> failed; failed -> loading (retry) and
// failed -> stopped (cleanup); stopped -> deleted (tombstone).
enum class BrowserProfileState {
  kStopped,
  kLoading,
  kInitializing,
  kReady,
  kDraining,
  kFailed,
  kDeleted,
};

// "stopped", "loading", "initializing", "ready", "draining", "failed",
// "deleted" (the `lifecycle` values of ProfileRecord).
std::string_view BrowserProfileStateToString(BrowserProfileState state);
std::optional<BrowserProfileState> BrowserProfileStateFromString(
    std::string_view value);

// Contract section 3 error codes.
enum class BrowserProfileError {
  kNone,
  kForbidden,
  kUnsupported,
  kInvalidProfileKey,
  kProfileNotFound,
  kProfileNotReady,
  kProfileBusy,
  kProfileFailed,
  kPersonaInvalid,
  kProxyNotReady,
  kStorageError,
  kDrainTimeout,
  kRequestConflict,
};

// "forbidden", "unsupported", "invalid_profile_key", ... ("" for kNone).
std::string_view BrowserProfileErrorCode(BrowserProfileError error);
std::optional<BrowserProfileError> BrowserProfileErrorFromCode(
    std::string_view code);

// An error result. `message` is fixed text per code: it never contains
// secrets, paths, seeds or endpoints.
struct BrowserProfileFailure {
  BrowserProfileFailure();
  explicit BrowserProfileFailure(BrowserProfileError code);
  BrowserProfileFailure(BrowserProfileError code, std::string message);
  BrowserProfileFailure(const BrowserProfileFailure&);
  BrowserProfileFailure& operator=(const BrowserProfileFailure&);
  ~BrowserProfileFailure();

  // {code, message}: the `structuredContent.error` of an MCP error result.
  base::DictValue ToValue() const;

  BrowserProfileError code = BrowserProfileError::kNone;
  std::string message;
};

// --- Validation helpers (pure) ----------------------------------------------

// ^bp_[0-9a-f]{32}$
bool IsValidBrowserProfileKey(std::string_view key);
// A new random key (128 bits from crypto::RandBytes). The caller checks
// uniqueness against the registry (Create() refuses a duplicate).
std::string GenerateBrowserProfileKey();
// ^[a-z0-9]+(-[a-z0-9]+)*$, 1-64 characters, and not the reserved throwaway
// prefix "tmp-".
bool IsValidWorkspaceAgentSlug(std::string_view slug);
// A Chromium-managed directory NAME relative to the user data dir (never a
// path): ASCII letters, digits, space, '_' and '-', 1-64 characters, no
// leading or trailing space, no '.', no separators, no drive or device
// syntax, not a reserved Chromium name. Used to validate what the registry
// stores and loads; the router additionally requires the form
// "Profile <number>" for the names it allocates.
bool IsSafeProfileDirectoryName(std::string_view name);
// Hex SHA-256 over the canonical form of the create arguments; used for
// requestId idempotency (same requestId + same fingerprint = replay,
// different fingerprint = request_conflict). Never reversible to the proxy
// password in practice, never returned by any API.
std::string ComputeCreateRequestFingerprint(std::string_view workspace_agent,
                                            std::string_view persona_json,
                                            std::string_view proxy_json);

// Whether `from` -> `to` is an edge of the state machine.
bool IsValidBrowserProfileTransition(BrowserProfileState from,
                                     BrowserProfileState to);
// Contract section 7: the persona of a managed Profile is replaced only while
// it is `stopped`. kNone for kStopped; otherwise the error a replacement gets:
// profile_busy (ready, loading, initializing, draining: an operation holds
// the Profile), profile_not_ready (failed: stop it first) and
// profile_not_found (deleted).
BrowserProfileError PersonaReplacementStateError(BrowserProfileState state);

// Edges that start a new lifecycle generation: the epoch is persisted and
// incremented BEFORE the work (load, drain, deletion). Idempotent repeats
// never reach a transition and so never increment.
bool TransitionBumpsLifecycleEpoch(BrowserProfileState from,
                                   BrowserProfileState to);

// --- Entry -------------------------------------------------------------------

struct BrowserProfileEntry {
  BrowserProfileEntry();
  BrowserProfileEntry(const BrowserProfileEntry&);
  BrowserProfileEntry& operator=(const BrowserProfileEntry&);
  ~BrowserProfileEntry();

  // Core-issued, immutable, never reused (tombstones keep their key).
  std::string key;
  // Chromium-managed directory name relative to the user data dir. Chosen by
  // the core (ProfileManager), never derived from the key, a name or a slug,
  // never accepted from a client.
  std::string directory;
  std::string workspace_agent;
  // Positive, persisted before every load/drain/deletion.
  int64_t lifecycle_epoch = 1;
  BrowserProfileState state = BrowserProfileState::kStopped;
  // Hint only (a stopped profile may stay loaded in RAM); the router reports
  // the runtime truth. Always false after a restart.
  bool loaded = false;
  // Tombstone only: physical removal of the directory is not confirmed yet.
  bool deletion_pending = false;
  // Creation idempotency.
  std::string request_id;
  std::string request_fingerprint;
  // Provisioning intent: the validated persona input (the `persona` argument
  // of create-browser-profile, no seed) and the proxy as the ENCRYPTED pref
  // value (ProxyToPrefValue; empty = none). Applied by managed_profile_
  // bootstrap on the first load; afterwards the Profile prefs are the source
  // of truth and these only document the intent.
  // While the Profile is stopped this is the source of truth of its persona:
  // ReplacePersonaIntent() replaces it (with a full persona input, preset
  // required) and sets `persona_replacement_pending` until the next load
  // (bootstrap, or the router for a Profile still in memory) has committed it
  // to the Profile prefs.
  std::string persona_input_json;
  std::string proxy_pref_json;
  // Set once the bootstrap committed the persona to the Profile prefs.
  bool provisioned = false;
  // A replacement persona (`persona_input_json`) is persisted but not yet
  // committed to the Profile prefs. Persisted atomically with the epoch bump.
  bool persona_replacement_pending = false;
  // personaSummary of the committed persona (no seed), "" until provisioned.
  std::string persona_summary_json;
  BrowserProfileError last_error = BrowserProfileError::kNone;
  base::Time created;
};

// --- Persistence --------------------------------------------------------------

// Writes the serialized manifest durably (atomically replacing the file) and
// reports success on the calling sequence. The default implementation uses
// base::ImportantFileWriter; tests use an in-memory one.
class BrowserProfileManifestWriter {
 public:
  virtual ~BrowserProfileManifestWriter() = default;
  virtual void Write(std::string data,
                     base::OnceCallback<void(bool success)> done) = 0;
};

std::unique_ptr<BrowserProfileManifestWriter> CreateFileManifestWriter(
    const base::FilePath& path,
    scoped_refptr<base::SequencedTaskRunner> io_task_runner);

// Result of parsing a manifest.
struct BrowserProfileManifest {
  BrowserProfileManifest();
  BrowserProfileManifest(BrowserProfileManifest&&);
  BrowserProfileManifest& operator=(BrowserProfileManifest&&);
  ~BrowserProfileManifest();

  // The directory of the startup (legacy) Profile, recorded the first time.
  std::string startup_profile_dir;
  std::vector<BrowserProfileEntry> entries;
};

enum class BrowserProfileParseError {
  kNone,
  kMalformed,       // not JSON / wrong shape / invalid entry / duplicates
  kUnknownVersion,  // written by a newer core: read-only, never overwritten
};

struct BrowserProfileParseResult {
  BrowserProfileParseResult();
  BrowserProfileParseResult(BrowserProfileParseResult&&);
  BrowserProfileParseResult& operator=(BrowserProfileParseResult&&);
  ~BrowserProfileParseResult();

  BrowserProfileParseError error = BrowserProfileParseError::kNone;
  BrowserProfileManifest manifest;
};

// Strict parser: any invalid entry (key regex, unsafe or duplicate directory,
// invalid slug, unknown state, non-positive epoch, duplicate key or
// requestId) rejects the whole manifest. Nothing is dropped silently.
BrowserProfileParseResult ParseBrowserProfileManifest(std::string_view json);
// Inverse of the parser.
std::string SerializeBrowserProfileManifest(
    const BrowserProfileManifest& manifest);

// The registry. UI thread only (one sequence).
class BrowserProfileRegistry {
 public:
  using CommitCallback = base::OnceCallback<void(
      base::expected<BrowserProfileEntry, BrowserProfileFailure>)>;

  // `manifest` is a parsed manifest or an empty one (first run). The registry
  // recovers every non-deleted entry as `stopped` in memory (the persisted
  // state is rewritten by the next commit); lifecycle epochs are kept.
  BrowserProfileRegistry(BrowserProfileManifest manifest,
                         std::unique_ptr<BrowserProfileManifestWriter> writer);
  BrowserProfileRegistry(const BrowserProfileRegistry&) = delete;
  BrowserProfileRegistry& operator=(const BrowserProfileRegistry&) = delete;
  ~BrowserProfileRegistry();

  // Blocking: reads `<user_data_dir>/ShuttleBrowserProfiles.json`. A missing
  // file is an empty manifest; an unreadable or invalid one is an error and
  // the registry must not be created (the router then reports the isolated
  // capability disabled and never overwrites the file).
  static BrowserProfileParseResult ReadManifestFile(
      const base::FilePath& user_data_dir);

  // Lookups. Tombstones are returned too (state kDeleted).
  const BrowserProfileEntry* Find(std::string_view key) const;
  const BrowserProfileEntry* FindByDirectory(std::string_view directory) const;
  const BrowserProfileEntry* FindByRequestId(std::string_view request_id) const;
  std::vector<BrowserProfileEntry> List() const;

  const std::string& startup_profile_dir() const {
    return startup_profile_dir_;
  }
  // Records the startup Profile directory once (later calls do nothing).
  void RecordStartupProfileDir(const std::string& directory);
  void SetWriteStartedForTesting(base::OnceClosure hook) {
    write_started_for_testing_ = std::move(hook);
  }
  base::OnceClosure write_started_for_testing_;
  void FailNextWriteForTesting() { fail_next_write_for_testing_ = true; }
  bool fail_next_write_for_testing_ = false;

  // Creates an entry in `stopped`, epoch 1, then persists. Idempotent by
  // `request.request_id`: the same id and fingerprint return the existing
  // entry (nothing written, epoch unchanged); another fingerprint is
  // request_conflict. An invalid key, directory or slug, a duplicate key or a
  // duplicate directory (tombstones included, case-insensitively) are caller
  // bugs and fail with profile_failed (nothing written).
  struct CreateRequest {
    CreateRequest();
    CreateRequest(const CreateRequest&);
    ~CreateRequest();

    std::string request_id;
    std::string key;
    std::string directory;
    std::string workspace_agent;
    std::string persona_input_json;
    std::string proxy_pref_json;
    std::string request_fingerprint;
  };
  void Create(CreateRequest request, CommitCallback callback);

  // Checked state transition; the lifecycle epoch is incremented when
  // TransitionBumpsLifecycleEpoch(). The change is applied in memory at once
  // (so concurrent callers see `to` and can join) and `callback` runs once
  // it is durable. If the write fails the whole batch of pending changes is
  // rolled back to the last durable state and every pending callback gets
  // storage_error. An edge that is not in the state machine fails with
  // profile_busy (the caller raced another operation) without any change.
  // `to == kDeleted` also sets deletion_pending.
  void Transition(std::string_view key,
                  BrowserProfileState to,
                  BrowserProfileError last_error,
                  CommitCallback callback);

  // Sets the state in memory only (nothing written, no epoch change, no
  // callback). Used when the disk cannot be written, so a lifecycle that
  // could not be persisted still ends in `failed`/`stopped` instead of staying
  // `loading`/`initializing`/`draining` forever. The next successful commit
  // persists it. A state that is not an edge of the machine is ignored.
  void ForceStateInMemory(std::string_view key,
                          BrowserProfileState to,
                          BrowserProfileError last_error);
  // Replaces the persona intent of a `stopped` entry (contract section 7):
  // `persona_input_json` is the full validated persona input,
  // `persona_summary_json` its seedless summary. Persists the new intent, sets
  // persona_replacement_pending and increments the lifecycle epoch in ONE
  // durable write (a failed write rolls both back: storage_error). Any other
  // state fails with PersonaReplacementStateError() and changes nothing. Never
  // touches state, cookies or storage.
  void ReplacePersonaIntent(std::string_view key,
                            std::string persona_input_json,
                            std::string persona_summary_json,
                            CommitCallback callback);
  // The pending replacement has been committed to the Profile prefs: clears
  // persona_replacement_pending and records the committed summary, but ONLY
  // if the entry is still the generation that was applied: same lifecycle
  // epoch, same persona intent, still pending. Anything else (another
  // replacement, a load, a delete raced in) changes nothing and fails with
  // profile_busy; an unknown or deleted key with profile_not_found.
  void ReinstatePersonaPending(std::string_view key, int64_t expected_epoch,
                              std::string_view expected_intent,
                              CommitCallback callback);
  void MarkPersonaCommitted(std::string_view key,
                            int64_t expected_epoch,
                            std::string_view expected_persona_input_json,
                            std::string persona_summary_json,
                            CommitCallback callback);
  // Records the first-load result: persona summary and provisioned flag. Also
  // clears persona_replacement_pending (the persona is committed), only if
  // both the lifecycle epoch and persona intent still match the applied load.
  void MarkProvisioned(std::string_view key,
                       int64_t expected_epoch,
                       std::string_view expected_persona_input_json,
                       std::string persona_summary_json,
                       CommitCallback callback);
  // Tombstone whose directory is confirmed gone: clears deletion_pending.
  void ConfirmDeletion(std::string_view key, CommitCallback callback);
  // The `loaded` hint (not persisted until the next commit).
  void SetLoaded(std::string_view key, bool loaded);

  // True when a write is queued or in flight.
  bool HasPendingWrite() const { return write_in_flight_ || !waiters_.empty(); }

 private:
  struct Waiter {
    Waiter();
    Waiter(Waiter&&);
    Waiter& operator=(Waiter&&);
    ~Waiter();

    // The entry as it was when the change was applied.
    BrowserProfileEntry entry;
    CommitCallback callback;
  };

  BrowserProfileEntry* FindMutable(std::string_view key);
  // Queues `waiter` for the next durable write and starts one if none runs.
  void Commit(Waiter waiter);
  void StartWrite();
  void OnWritten(BrowserProfileManifest snapshot, bool success);
  BrowserProfileManifest Snapshot() const;

  SEQUENCE_CHECKER(sequence_checker_);
  std::unique_ptr<BrowserProfileManifestWriter> writer_;
  std::string startup_profile_dir_;
  // By key. Tombstones stay forever (keys are never reused).
  std::map<std::string, BrowserProfileEntry> entries_;
  // The last state known to be on disk (rollback target).
  BrowserProfileManifest durable_;
  bool write_in_flight_ = false;
  std::vector<Waiter> in_flight_waiters_;
  std::vector<Waiter> waiters_;
  base::WeakPtrFactory<BrowserProfileRegistry> weak_factory_{this};
};

}  // namespace shuttle

#endif  // CHROME_BROWSER_SHUTTLE_BROWSER_PROFILE_REGISTRY_H_
