// Copyright 2015 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROMECAST_BROWSER_CAST_PERMISSION_MANAGER_H_
#define CHROMECAST_BROWSER_CAST_PERMISSION_MANAGER_H_

#include "base/callback_forward.h"
#include "base/macros.h"
#include "content/public/browser/permission_manager.h"

namespace chromecast {
namespace shell {

class CastPermissionManager : public content::PermissionManager {
 public:
  CastPermissionManager();
  ~CastPermissionManager() override;

  // content::PermissionManager implementation:
  void RequestPermission(
      content::PermissionType permission,
      content::WebContents* web_contents,
      int request_id,
      const GURL& requesting_origin,
      bool user_gesture,
      const base::Callback<void(content::PermissionStatus)>& callback) override;
  void CancelPermissionRequest(content::PermissionType permission,
                               content::WebContents* web_contents,
                               int request_id,
                               const GURL& requesting_origin) override;
  void ResetPermission(content::PermissionType permission,
                       const GURL& requesting_origin,
                       const GURL& embedding_origin) override;
  content::PermissionStatus GetPermissionStatus(
      content::PermissionType permission,
      const GURL& requesting_origin,
      const GURL& embedding_origin) override;
  void RegisterPermissionUsage(content::PermissionType permission,
                               const GURL& requesting_origin,
                               const GURL& embedding_origin) override;

 private:
  DISALLOW_COPY_AND_ASSIGN(CastPermissionManager);
};

}  // namespace shell
}  // namespace chromecast

#endif // CHROMECAST_BROWSER_CAST_PERMISSION_MANAGER_H_
