// Copyright 2013 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_PROFILES_CHROME_BROWSER_MAIN_EXTRA_PARTS_PROFILES_H_
#define CHROME_BROWSER_PROFILES_CHROME_BROWSER_MAIN_EXTRA_PARTS_PROFILES_H_

#include "chrome/browser/chrome_browser_main_extra_parts.h"

class ChromeBrowserMainParts;

void AddProfilesExtraParts(ChromeBrowserMainParts* main_parts);

class ChromeBrowserMainExtraPartsProfiles : public ChromeBrowserMainExtraParts {
 public:
  ChromeBrowserMainExtraPartsProfiles();
  ChromeBrowserMainExtraPartsProfiles(
      const ChromeBrowserMainExtraPartsProfiles&) = delete;
  ChromeBrowserMainExtraPartsProfiles& operator=(
      const ChromeBrowserMainExtraPartsProfiles&) = delete;
  ~ChromeBrowserMainExtraPartsProfiles() override;

  // Instantiates all chrome KeyedService factories, which is
  // especially important for services that should be created at profile
  // creation time as compared to lazily on first access.
  static void EnsureBrowserContextKeyedServiceFactoriesBuilt();

  // Overridden from ChromeBrowserMainExtraParts:
  void PreProfileInit() override;
  // Shuttle: captures the fixed startup Profile for the isolated-profile
  // router (a no-op unless --shuttle-isolated-profiles is set).
  void PostProfileInit(Profile* profile, bool is_initial_profile) override;
  // Shuttle: destroys the isolated-profile router while Profiles are alive.
  void PostMainMessageLoopRun() override;
};

#endif  // CHROME_BROWSER_PROFILES_CHROME_BROWSER_MAIN_EXTRA_PARTS_PROFILES_H_
