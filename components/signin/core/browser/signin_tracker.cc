// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "components/signin/core/browser/signin_tracker.h"

#include "components/signin/core/browser/account_reconcilor.h"
#include "components/signin/core/browser/gaia_cookie_manager_service.h"
#include "components/signin/core/browser/profile_oauth2_token_service.h"
#include "components/signin/core/browser/signin_client.h"
#include "google_apis/gaia/gaia_constants.h"

SigninTracker::SigninTracker(ProfileOAuth2TokenService* token_service,
                             SigninManagerBase* signin_manager,
                             AccountReconcilor* account_reconcilor,
                             GaiaCookieManagerService* cookie_manager_service,
                             SigninClient* client,
                             Observer* observer)
    : token_service_(token_service),
      signin_manager_(signin_manager),
      account_reconcilor_(account_reconcilor),
      cookie_manager_service_(cookie_manager_service),
      client_(client),
      observer_(observer) {
  Initialize();
}

SigninTracker::~SigninTracker() {
  signin_manager_->RemoveObserver(this);
  token_service_->RemoveObserver(this);
  cookie_manager_service_->RemoveObserver(this);
}

void SigninTracker::Initialize() {
  DCHECK(observer_);
  signin_manager_->AddObserver(this);
  token_service_->AddObserver(this);
  cookie_manager_service_->AddObserver(this);
}

void SigninTracker::GoogleSigninFailed(const GoogleServiceAuthError& error) {
  observer_->SigninFailed(error);
}

void SigninTracker::OnRefreshTokenAvailable(const std::string& account_id) {
  if (account_id != signin_manager_->GetAuthenticatedAccountId())
    return;

  observer_->SigninSuccess();
}

void SigninTracker::OnRefreshTokenRevoked(const std::string& account_id) {
  NOTREACHED();
}

void SigninTracker::OnAddAccountToCookieCompleted(
    const std::string& account_id,
    const GoogleServiceAuthError& error) {
  observer_->MergeSessionComplete(error);
}
