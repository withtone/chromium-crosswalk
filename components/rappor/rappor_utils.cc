// Copyright 2015 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "components/rappor/rappor_utils.h"

#include "components/rappor/rappor_service.h"
#include "net/base/net_util.h"
#include "net/base/registry_controlled_domains/registry_controlled_domain.h"
#include "url/gurl.h"

namespace rappor {

std::string GetDomainAndRegistrySampleFromGURL(const GURL& gurl) {
  if (gurl.SchemeIsHTTPOrHTTPS()) {
    if (net::IsLocalhost(gurl.host()))
      return "localhost";
    if (gurl.HostIsIPAddress())
      return "ip_address";
    return net::registry_controlled_domains::GetDomainAndRegistry(
        gurl, net::registry_controlled_domains::INCLUDE_PRIVATE_REGISTRIES);
  }
  if (gurl.SchemeIsFile())
    return gurl.scheme() + "://";
  return gurl.scheme() + "://" + gurl.host();
}

void SampleDomainAndRegistryFromGURL(RapporService* rappor_service,
                                     const std::string& metric,
                                     const GURL& gurl) {
  if (!rappor_service)
    return;
  rappor_service->RecordSample(
      metric,
      rappor::ETLD_PLUS_ONE_RAPPOR_TYPE,
      GetDomainAndRegistrySampleFromGURL(gurl));
}

}  // namespace rappor
