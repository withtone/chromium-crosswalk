// Copyright (c) 2015 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "base/bind.h"
#include "base/test/launcher/unit_test_launcher.h"
#include "base/test/test_suite.h"

int main(int argc, char** argv) {
  base::TestSuite test_suite(argc, argv);

  const auto& run_test_suite =
      base::Bind(&base::TestSuite::Run, base::Unretained(&test_suite));
  return base::LaunchUnitTests(argc, argv, run_test_suite);
}
