// Copyright 2026 Silkweave
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/shuttle/profile_persona_service_factory.h"

#include <memory>

#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/shuttle/pref_names.h"
#include "chrome/browser/shuttle/profile_persona_service.h"
#include "components/pref_registry/pref_registry_syncable.h"

namespace shuttle {

// static
ProfilePersonaService* ProfilePersonaServiceFactory::GetForProfile(
    Profile* profile) {
  if (!profile) {
    return nullptr;
  }
  return static_cast<ProfilePersonaService*>(
      GetInstance()->GetServiceForBrowserContext(profile, /*create=*/true));
}

// static
ProfilePersonaService* ProfilePersonaServiceFactory::GetForProfileIfExists(
    Profile* profile) {
  if (!profile) {
    return nullptr;
  }
  return static_cast<ProfilePersonaService*>(
      GetInstance()->GetServiceForBrowserContext(profile, /*create=*/false));
}

// static
ProfilePersonaServiceFactory* ProfilePersonaServiceFactory::GetInstance() {
  static base::NoDestructor<ProfilePersonaServiceFactory> instance;
  return instance.get();
}

ProfilePersonaServiceFactory::ProfilePersonaServiceFactory()
    : ProfileKeyedServiceFactory(
          "ProfilePersonaService",
          // The persona belongs to the regular profile; an off-the-record
          // profile has none.
          ProfileSelections::Builder()
              .WithRegular(ProfileSelection::kOriginalOnly)
              .Build()) {}

ProfilePersonaServiceFactory::~ProfilePersonaServiceFactory() = default;

std::unique_ptr<KeyedService>
ProfilePersonaServiceFactory::BuildServiceInstanceForBrowserContext(
    content::BrowserContext* context) const {
  return std::make_unique<ProfilePersonaService>(
      Profile::FromBrowserContext(context));
}

void ProfilePersonaServiceFactory::RegisterProfilePrefs(
    user_prefs::PrefRegistrySyncable* registry) {
  // Profile-level, one persona for the whole profile; not synced.
  registry->RegisterDictionaryPref(prefs::kFingerprintPersona);
}

// Created with the profile: the network stack asks for it while it configures
// the profile's NetworkContexts, so it must not be created during shutdown.
bool ProfilePersonaServiceFactory::ServiceIsCreatedWithBrowserContext() const {
  return true;
}

}  // namespace shuttle
