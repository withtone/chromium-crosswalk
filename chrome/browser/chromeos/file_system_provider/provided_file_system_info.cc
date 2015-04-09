// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/chromeos/file_system_provider/provided_file_system_info.h"
#include "base/logging.h"

namespace chromeos {
namespace file_system_provider {

MountOptions::MountOptions()
    : writable(false),
      source(SOURCE_UNKNOWN),
      supports_notify_tag(false),
      opened_files_limit(0) {
}

MountOptions::MountOptions(const std::string& file_system_id,
                           const std::string& display_name)
    : file_system_id(file_system_id),
      display_name(display_name),
      writable(false),
      source(SOURCE_UNKNOWN),
      supports_notify_tag(false),
      opened_files_limit(0) {
}

ProvidedFileSystemInfo::ProvidedFileSystemInfo()
    : writable_(false), source_(SOURCE_UNKNOWN), supports_notify_tag_(false) {
}

ProvidedFileSystemInfo::ProvidedFileSystemInfo(
    const std::string& extension_id,
    const MountOptions& mount_options,
    const base::FilePath& mount_path)
    : extension_id_(extension_id),
      file_system_id_(mount_options.file_system_id),
      display_name_(mount_options.display_name),
      writable_(mount_options.writable),
      source_(mount_options.source),
      supports_notify_tag_(mount_options.supports_notify_tag),
      opened_files_limit_(mount_options.opened_files_limit),
      mount_path_(mount_path) {
  DCHECK_LE(0, mount_options.opened_files_limit);
}

ProvidedFileSystemInfo::~ProvidedFileSystemInfo() {}

}  // namespace file_system_provider
}  // namespace chromeos
