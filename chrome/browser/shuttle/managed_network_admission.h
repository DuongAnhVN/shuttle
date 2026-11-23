// Copyright 2026 Silkweave
#ifndef CHROME_BROWSER_SHUTTLE_MANAGED_NETWORK_ADMISSION_H_
#define CHROME_BROWSER_SHUTTLE_MANAGED_NETWORK_ADMISSION_H_

#include "base/functional/callback.h"
#include "base/memory/scoped_refptr.h"
#include "components/os_crypt/async/common/encryptor.h"

namespace content { class BrowserContext; }

namespace shuttle {
// Installed at the start of ProfileImpl construction, before delegate/prefs
// callbacks. Only NetworkContext creation waits; preference services may run.
void BeginManagedNetworkAdmission(content::BrowserContext* context);
// Reclose only network creation while a stopped, already-loaded identity is
// replaced/recovered. Existing services and the validated encryptor survive.
void PauseManagedNetworkAdmission(content::BrowserContext* context);
// Failure is terminal. Pending callbacks are destroyed, closing their Mojo
// receivers without constructing or configuring a NetworkContext.
void FinishManagedNetworkAdmission(
    content::BrowserContext* context, bool success,
    scoped_refptr<os_crypt_async::Encryptor> encryptor = nullptr);
void AwaitManagedNetworkAdmission(content::BrowserContext* context,
                                  base::OnceClosure admitted);
bool ManagedNetworkAdmissionReady(content::BrowserContext* context);
scoped_refptr<os_crypt_async::Encryptor> ManagedNetworkEncryptor(
    content::BrowserContext* context);
}  // namespace shuttle
#endif
