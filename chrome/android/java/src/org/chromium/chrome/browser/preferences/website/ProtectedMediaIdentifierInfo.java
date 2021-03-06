// Copyright 2015 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package org.chromium.chrome.browser.preferences.website;

/**
 * This class represents protected media identifier permission information for given requesting
 * frame origin and embedding frame origin.
 */
public class ProtectedMediaIdentifierInfo extends PermissionInfo {
    public ProtectedMediaIdentifierInfo(String origin, String embedder) {
        super(origin, embedder);
    }

    @Override
    protected int getNativePreferenceValue(String origin, String embedder) {
        return WebsitePreferenceBridge.nativeGetProtectedMediaIdentifierSettingForOrigin(
                origin, embedder);
    }

    @Override
    protected void setNativePreferenceValue(
            String origin, String embedder, int value) {
        WebsitePreferenceBridge.nativeSetProtectedMediaIdentifierSettingForOrigin(
                origin, embedder, value);
    }
}
