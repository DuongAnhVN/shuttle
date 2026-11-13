// Copyright 2026 Silkweave
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_SHUTTLE_PROFILE_PROXY_SERVICE_FACTORY_H_
#define CHROME_BROWSER_SHUTTLE_PROFILE_PROXY_SERVICE_FACTORY_H_

#include <memory>

#include "base/memory/raw_ptr.h"
#include "base/no_destructor.h"
#include "chrome/browser/profiles/profile_keyed_service_factory.h"

class Profile;

namespace os_crypt_async {
class OSCryptAsync;
}  // namespace os_crypt_async

namespace shuttle {

class ProfileProxyService;

class ProfileProxyServiceFactory : public ProfileKeyedServiceFactory {
 public:
  // Null for profiles without agents (off-the-record, system).
  static ProfileProxyService* GetForProfile(Profile* profile);
  static ProfileProxyServiceFactory* GetInstance();

  ProfileProxyServiceFactory(const ProfileProxyServiceFactory&) = delete;
  ProfileProxyServiceFactory& operator=(const ProfileProxyServiceFactory&) =
      delete;

  // Set once by the browser to encrypt agents' passwords. A service built
  // before this call gets the encryptor on its next GetForProfile; until then
  // its password proxies read as kCorrupt (fail closed).
  void set_os_crypt_async(os_crypt_async::OSCryptAsync* os_crypt_async);

 private:
  friend base::NoDestructor<ProfileProxyServiceFactory>;

  ProfileProxyServiceFactory();
  ~ProfileProxyServiceFactory() override;

  // BrowserContextKeyedServiceFactory:
  std::unique_ptr<KeyedService> BuildServiceInstanceForBrowserContext(
      content::BrowserContext* context) const override;
  bool ServiceIsCreatedWithBrowserContext() const override;

  raw_ptr<os_crypt_async::OSCryptAsync> os_crypt_async_ = nullptr;
};

}  // namespace shuttle

#endif  // CHROME_BROWSER_SHUTTLE_PROFILE_PROXY_SERVICE_FACTORY_H_
