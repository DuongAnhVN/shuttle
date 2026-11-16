// Copyright 2026 Silkweave
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_SHUTTLE_PROFILE_PERSONA_SERVICE_FACTORY_H_
#define CHROME_BROWSER_SHUTTLE_PROFILE_PERSONA_SERVICE_FACTORY_H_

#include <memory>

#include "base/no_destructor.h"
#include "chrome/browser/profiles/profile_keyed_service_factory.h"

class Profile;

namespace shuttle {

class ProfilePersonaService;

class ProfilePersonaServiceFactory : public ProfileKeyedServiceFactory {
 public:
  // The profile's persona service, created if needed; null for a null profile
  // and for profiles without one (off-the-record, system). Use it where the
  // service is set up (constructors, MCP tools).
  static ProfilePersonaService* GetForProfile(Profile* profile);
  // The profile's persona service if it exists, never creating it; null for a
  // null profile and for profiles without one. Use it in the runtime paths
  // (tab prefs, network context configuration, worker prefs), which may run
  // while the profile shuts down. The service is created together with the
  // profile (ServiceIsCreatedWithBrowserContext), so it exists during normal
  // operation.
  static ProfilePersonaService* GetForProfileIfExists(Profile* profile);
  static ProfilePersonaServiceFactory* GetInstance();

  ProfilePersonaServiceFactory(const ProfilePersonaServiceFactory&) = delete;
  ProfilePersonaServiceFactory& operator=(const ProfilePersonaServiceFactory&) =
      delete;

 private:
  friend base::NoDestructor<ProfilePersonaServiceFactory>;

  ProfilePersonaServiceFactory();
  ~ProfilePersonaServiceFactory() override;

  // BrowserContextKeyedServiceFactory:
  std::unique_ptr<KeyedService> BuildServiceInstanceForBrowserContext(
      content::BrowserContext* context) const override;
  // Registers prefs::kFingerprintPersona.
  void RegisterProfilePrefs(
      user_prefs::PrefRegistrySyncable* registry) override;
  bool ServiceIsCreatedWithBrowserContext() const override;
};

}  // namespace shuttle

#endif  // CHROME_BROWSER_SHUTTLE_PROFILE_PERSONA_SERVICE_FACTORY_H_
