// Copyright 2026 Silkweave
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/shuttle/profile_proxy_service_factory.h"

#include <memory>
#include "base/check.h"

#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/shuttle/profile_proxy_service.h"
#include "chrome/browser/shuttle/managed_network_admission.h"
#include "components/os_crypt/async/common/encryptor.h"

namespace shuttle {

ProfileProxyService* ProfileProxyServiceFactory::GetForProfileIfExists(
    Profile* profile) {
  return profile ? static_cast<ProfileProxyService*>(
      GetInstance()->GetServiceForBrowserContext(profile, false)) : nullptr;
}

// static
ProfileProxyService* ProfileProxyServiceFactory::GetForProfile(
    Profile* profile) {
  ProfileProxyServiceFactory* factory = GetInstance();
  ProfileProxyService* service = static_cast<ProfileProxyService*>(
      factory->GetServiceForBrowserContext(profile, /*create=*/true));
  // Cheap after the first call. Heals a service built before set_os_crypt_async.
  if (service) {
    service->InitEncryption(factory->os_crypt_async_.get());
  }
  return service;
}

// static
ProfileProxyServiceFactory* ProfileProxyServiceFactory::GetInstance() {
  static base::NoDestructor<ProfileProxyServiceFactory> instance;
  return instance.get();
}

ProfileProxyServiceFactory::ProfileProxyServiceFactory()
    : ProfileKeyedServiceFactory(
          "ProfileProxyService",
          // Agents live in the regular profile only.
          ProfileSelections::Builder()
              .WithRegular(ProfileSelection::kOriginalOnly)
              .Build()) {}

ProfileProxyServiceFactory::~ProfileProxyServiceFactory() = default;

void ProfileProxyServiceFactory::set_os_crypt_async(
    os_crypt_async::OSCryptAsync* os_crypt_async) {
  os_crypt_async_ = os_crypt_async;
}

std::unique_ptr<KeyedService>
ProfileProxyServiceFactory::BuildServiceInstanceForBrowserContext(
    content::BrowserContext* context) const {
  CHECK(ManagedNetworkAdmissionReady(context));
  auto service = std::make_unique<ProfileProxyService>(
      Profile::FromBrowserContext(context), os_crypt_async_.get());
  service->AdoptBootstrapEncryptor(ManagedNetworkEncryptor(context));
  return service;
}

// Created with the profile: the network stack asks for it while it configures
// agent partitions, so it must not be created during shutdown.
bool ProfileProxyServiceFactory::ServiceIsCreatedWithBrowserContext() const {
  return true;
}

}  // namespace shuttle
