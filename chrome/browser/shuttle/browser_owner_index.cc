// Copyright 2026 Silkweave
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/shuttle/browser_owner_index.h"

#include <utility>

#include "base/check.h"
#include "base/uuid.h"

namespace shuttle {

SessionOwnerIndex::SessionOwnerIndex() = default;
SessionOwnerIndex::~SessionOwnerIndex() = default;

std::string SessionOwnerIndex::RegisterNew(const OwnerTuple& owner) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  std::string id;
  do {
    id = base::Uuid::GenerateRandomV4().AsLowercaseString();
  } while (owners_.contains(id) || retired_.contains(id));
  owners_.emplace(id, owner);
  return id;
}

bool SessionOwnerIndex::Register(const std::string& id,
                                 const OwnerTuple& owner) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (id.empty() || owners_.contains(id) || retired_.contains(id)) {
    return false;
  }
  owners_.emplace(id, owner);
  return true;
}

std::optional<OwnerTuple> SessionOwnerIndex::Lookup(std::string_view id) const {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  auto it = owners_.find(id);
  if (it == owners_.end()) {
    return std::nullopt;
  }
  return it->second;
}

OwnerCheck SessionOwnerIndex::Check(std::string_view id,
                                    const OwnerTuple& expected) const {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  auto it = owners_.find(id);
  if (it == owners_.end()) {
    return OwnerCheck::kUnknown;
  }
  return it->second == expected ? OwnerCheck::kOk : OwnerCheck::kForbidden;
}

bool SessionOwnerIndex::Remove(std::string_view id) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  auto it = owners_.find(id);
  if (it == owners_.end()) {
    return false;
  }
  retired_.insert(it->first);
  owners_.erase(it);
  return true;
}

std::vector<std::string> SessionOwnerIndex::RemoveAllForProfile(
    std::string_view profile_key) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  std::vector<std::string> removed;
  for (auto it = owners_.begin(); it != owners_.end();) {
    if (it->second.profile_key == profile_key) {
      removed.push_back(it->first);
      retired_.insert(it->first);
      it = owners_.erase(it);
    } else {
      ++it;
    }
  }
  return removed;
}

std::vector<std::string> SessionOwnerIndex::SessionsForProfile(
    std::string_view profile_key) const {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  std::vector<std::string> ids;
  for (const auto& [id, owner] : owners_) {
    if (owner.profile_key == profile_key) {
      ids.push_back(id);
    }
  }
  return ids;
}

TargetOwnerIndex::TargetOwnerIndex() = default;
TargetOwnerIndex::~TargetOwnerIndex() = default;

void TargetOwnerIndex::Register(const std::string& target_id,
                                const TargetOwner& owner) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  targets_[target_id] = owner;
}

std::optional<TargetOwner> TargetOwnerIndex::Lookup(
    std::string_view target_id) const {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  auto it = targets_.find(target_id);
  if (it == targets_.end()) {
    return std::nullopt;
  }
  return it->second;
}

OwnerCheck TargetOwnerIndex::Check(std::string_view target_id,
                                   const OwnerTuple& expected_owner) const {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  auto it = targets_.find(target_id);
  if (it == targets_.end()) {
    return OwnerCheck::kUnknown;
  }
  return it->second.owner == expected_owner ? OwnerCheck::kOk
                                            : OwnerCheck::kForbidden;
}

bool TargetOwnerIndex::Remove(std::string_view target_id) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  auto it = targets_.find(target_id);
  if (it == targets_.end()) {
    return false;
  }
  targets_.erase(it);
  return true;
}

std::vector<std::string> TargetOwnerIndex::RemoveAllForSession(
    std::string_view session_id) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  std::vector<std::string> removed;
  for (auto it = targets_.begin(); it != targets_.end();) {
    if (it->second.session_id == session_id) {
      removed.push_back(it->first);
      it = targets_.erase(it);
    } else {
      ++it;
    }
  }
  return removed;
}

std::vector<std::string> TargetOwnerIndex::RemoveAllForProfile(
    std::string_view profile_key) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  std::vector<std::string> removed;
  for (auto it = targets_.begin(); it != targets_.end();) {
    if (it->second.owner.profile_key == profile_key) {
      removed.push_back(it->first);
      it = targets_.erase(it);
    } else {
      ++it;
    }
  }
  return removed;
}

}  // namespace shuttle
