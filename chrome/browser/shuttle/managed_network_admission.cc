// Copyright 2026 Silkweave
#include "chrome/browser/shuttle/managed_network_admission.h"

#include <utility>
#include <memory>
#include <vector>

#include "base/check.h"
#include "base/supports_user_data.h"
#include "components/os_crypt/async/common/encryptor.h"
#include "content/public/browser/browser_context.h"

namespace shuttle {
namespace {
const char kAdmissionKey = 0;
struct Admission : base::SupportsUserData::Data {
  enum class State { kPending, kReady, kFailed };
  State state = State::kPending;
  scoped_refptr<os_crypt_async::Encryptor> encryptor;
  std::vector<base::OnceClosure> waiters;
};
Admission* Get(content::BrowserContext* context) {
  return static_cast<Admission*>(context->GetUserData(&kAdmissionKey));
}
}  // namespace

void BeginManagedNetworkAdmission(content::BrowserContext* context) {
  CHECK(!Get(context));
  context->SetUserData(&kAdmissionKey, std::make_unique<Admission>());
}

void FinishManagedNetworkAdmission(
    content::BrowserContext* context, bool success,
    scoped_refptr<os_crypt_async::Encryptor> encryptor) {
  Admission* gate = Get(context);
  CHECK(gate);
  CHECK(gate->state == Admission::State::kPending);
  gate->state = success ? Admission::State::kReady : Admission::State::kFailed;
  if (encryptor) {
    gate->encryptor = std::move(encryptor);
  }
  auto waiters = std::move(gate->waiters);
  if (success) {
    for (auto& waiter : waiters) {
      std::move(waiter).Run();
    }
  }
}

void PauseManagedNetworkAdmission(content::BrowserContext* context) {
  Admission* gate = Get(context);
  CHECK(gate);
  CHECK(gate->state != Admission::State::kFailed);
  gate->state = Admission::State::kPending;
}

void AwaitManagedNetworkAdmission(content::BrowserContext* context,
                                  base::OnceClosure admitted) {
  Admission* gate = Get(context);
  if (!gate || gate->state == Admission::State::kReady) {
    std::move(admitted).Run();
  } else if (gate->state == Admission::State::kPending) {
    gate->waiters.push_back(std::move(admitted));
  }
}

bool ManagedNetworkAdmissionReady(content::BrowserContext* context) {
  Admission* gate = Get(context);
  return !gate || gate->state == Admission::State::kReady;
}

scoped_refptr<os_crypt_async::Encryptor> ManagedNetworkEncryptor(
    content::BrowserContext* context) {
  Admission* gate = Get(context);
  return gate ? gate->encryptor : nullptr;
}
}  // namespace shuttle
