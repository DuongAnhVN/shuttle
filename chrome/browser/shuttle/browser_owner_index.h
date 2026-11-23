// Copyright 2026 Silkweave
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_SHUTTLE_BROWSER_OWNER_INDEX_H_
#define CHROME_BROWSER_SHUTTLE_BROWSER_OWNER_INDEX_H_

#include <stdint.h>

#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "base/sequence_checker.h"

namespace shuttle {

// Browser-wide ownership indexes of tools/shuttle/docs/PROFILES.md section 4.
// IDs are locators, never authority: every lookup returns the owner tuple and
// the caller compares it with the authenticated scope before acting. Plain
// data, no Profile/content dependency. UI thread only.
//
// Not yet compiled (see PROFILES.md, "Implementation status").

// Who owns a session or target. `profile_key` is "" for the startup/legacy
// Profile (sessions of the pre-isolation world); isolated Profiles always
// carry their coreProfileKey.
struct OwnerTuple {
  std::string profile_key;
  std::string workspace_agent;
  int64_t lifecycle_epoch = 0;
  std::string browser_epoch;

  bool operator==(const OwnerTuple&) const = default;
};

// Outcome of checking a caller's scope against a located owner. Maps to the
// CDP connection decision (kUnknown -> HTTP 404, kForbidden -> 403).
enum class OwnerCheck {
  kOk,
  kUnknown,    // no such session/target
  kForbidden,  // exists, but another owner, a stale epoch or another browser
};

// session id -> owner. Session ids are browser-wide UUIDv4, collision-checked
// here (not per Profile).
class SessionOwnerIndex {
 public:
  SessionOwnerIndex();
  SessionOwnerIndex(const SessionOwnerIndex&) = delete;
  SessionOwnerIndex& operator=(const SessionOwnerIndex&) = delete;
  ~SessionOwnerIndex();

  // Generates an unused UUIDv4 (lowercase, hyphenated), registers it for
  // `owner` and returns it. Never returns an id that exists or was removed
  // earlier (removed ids are retired until the browser exits).
  std::string RegisterNew(const OwnerTuple& owner);
  // Registers a caller-chosen id (tests, legacy ids). False if `id` is
  // already registered or retired.
  bool Register(const std::string& id, const OwnerTuple& owner);
  std::optional<OwnerTuple> Lookup(std::string_view id) const;
  // Compares the located owner with `expected` field by field (profile key,
  // agent, lifecycle epoch, browser epoch); every mismatch is kForbidden.
  OwnerCheck Check(std::string_view id, const OwnerTuple& expected) const;
  bool Remove(std::string_view id);
  // Revokes every session of the Profile (stop/delete); returns their ids so
  // the caller can close the connections that used them.
  std::vector<std::string> RemoveAllForProfile(std::string_view profile_key);
  std::vector<std::string> SessionsForProfile(
      std::string_view profile_key) const;
  size_t size() const { return owners_.size(); }

 private:
  SEQUENCE_CHECKER(sequence_checker_);
  std::map<std::string, OwnerTuple, std::less<>> owners_;
  // Removed ids stay unusable until the browser exits.
  std::set<std::string, std::less<>> retired_;
};

// target id -> owner + owning session and partition. A stub for the CDP lane
// (B2): the router owns an instance and clears it on stop/delete; the CDP
// filters register and look targets up.
struct TargetOwner {
  OwnerTuple owner;
  // The session that leased the target ("" for none).
  std::string session_id;
  // The storage partition (workspace agent) the target runs in.
  std::string partition;

  bool operator==(const TargetOwner&) const = default;
};

class TargetOwnerIndex {
 public:
  TargetOwnerIndex();
  TargetOwnerIndex(const TargetOwnerIndex&) = delete;
  TargetOwnerIndex& operator=(const TargetOwnerIndex&) = delete;
  ~TargetOwnerIndex();

  // Replaces any previous owner of `target_id`.
  void Register(const std::string& target_id, const TargetOwner& owner);
  std::optional<TargetOwner> Lookup(std::string_view target_id) const;
  OwnerCheck Check(std::string_view target_id,
                   const OwnerTuple& expected_owner) const;
  bool Remove(std::string_view target_id);
  std::vector<std::string> RemoveAllForSession(std::string_view session_id);
  std::vector<std::string> RemoveAllForProfile(std::string_view profile_key);
  size_t size() const { return targets_.size(); }

 private:
  SEQUENCE_CHECKER(sequence_checker_);
  std::map<std::string, TargetOwner, std::less<>> targets_;
};

}  // namespace shuttle

#endif  // CHROME_BROWSER_SHUTTLE_BROWSER_OWNER_INDEX_H_
