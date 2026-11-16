// Copyright 2020 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef THIRD_PARTY_BLINK_RENDERER_CORE_INSPECTOR_LOCALE_CONTROLLER_H_
#define THIRD_PARTY_BLINK_RENDERER_CORE_INSPECTOR_LOCALE_CONTROLLER_H_

#include <optional>

#include "base/i18n/language_tag.h"
#include "base/synchronization/lock.h"
#include "third_party/blink/renderer/platform/wtf/text/wtf_string.h"

namespace blink {

class LocaleController {
 public:
  static LocaleController& instance();

  LocaleController(const LocaleController&) = delete;
  LocaleController& operator=(const LocaleController&) = delete;

  String SetLocaleOverride(const String& locale, bool is_claiming_override);

  // Shuttle: sets (empty: removes) the base locale of this renderer process
  // that replaces the embedder's: the profile's fingerprint persona
  // (RendererPreferences::locale_override). Not exclusive: a DevTools override
  // (SetLocaleOverride) takes precedence while it is active, and resetting it
  // falls back to the persona locale instead of the embedder's. The ICU default
  // locale is PROCESS-global: this changes it for every page and worker of the
  // process, with no per-page isolation; keeping it inside one profile is the
  // embedder's job (see TimeZoneController::SetPersonaTimeZone). Returns an
  // error message for an invalid locale (nothing changes), else a null string.
  String SetPersonaLocale(const String& locale);

 private:
  LocaleController();
  ~LocaleController() = default;

  void UpdateLocale(const base::i18n::LanguageTag& locale);
  // The persona's locale if set, else the embedder's.
  const base::i18n::LanguageTag& BaseLocale() const
      EXCLUSIVE_LOCKS_REQUIRED(lock_);

  base::Lock lock_;

  base::i18n::LanguageTag embedder_locale_;
  String locale_override_ GUARDED_BY(lock_);
  String persona_locale_ GUARDED_BY(lock_);
  std::optional<base::i18n::LanguageTag> persona_locale_tag_ GUARDED_BY(lock_);
};

}  // namespace blink

#endif  // THIRD_PARTY_BLINK_RENDERER_CORE_INSPECTOR_LOCALE_CONTROLLER_H_
