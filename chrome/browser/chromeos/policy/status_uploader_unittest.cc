// Copyright (c) 2015 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "base/prefs/pref_registry_simple.h"
#include "base/prefs/testing_pref_service.h"
#include "base/test/test_simple_task_runner.h"
#include "chrome/browser/chromeos/policy/device_status_collector.h"
#include "chrome/browser/chromeos/policy/status_uploader.h"
#include "chrome/browser/chromeos/settings/cros_settings.h"
#include "chrome/browser/chromeos/settings/device_settings_service.h"
#include "chrome/common/pref_names.h"
#include "components/policy/core/common/cloud/cloud_policy_client.h"
#include "components/policy/core/common/cloud/mock_cloud_policy_client.h"
#include "components/policy/core/common/cloud/mock_device_management_service.h"
#include "content/public/test/test_utils.h"
#include "net/url_request/url_request_context_getter.h"
#include "testing/gmock/include/gmock/gmock.h"
#include "testing/gtest/include/gtest/gtest.h"

using ::testing::_;
using ::testing::Return;
using ::testing::SaveArg;
using ::testing::WithArgs;

namespace em = enterprise_management;

namespace {

class MockDeviceStatusCollector : public policy::DeviceStatusCollector {
 public:
  explicit MockDeviceStatusCollector(PrefService* local_state)
      : DeviceStatusCollector(
            local_state,
            nullptr,
            policy::DeviceStatusCollector::LocationUpdateRequester(),
            policy::DeviceStatusCollector::VolumeInfoFetcher()) {
  }

  MOCK_METHOD1(GetDeviceStatus, bool(em::DeviceStatusReportRequest*));
  MOCK_METHOD1(GetDeviceSessionStatus, bool(em::SessionStatusReportRequest*));
};

}  // namespace

namespace policy {
class StatusUploaderTest : public testing::Test {
 public:
  StatusUploaderTest()
      : task_runner_(new base::TestSimpleTaskRunner()) {
    DeviceStatusCollector::RegisterPrefs(prefs_.registry());
    prefs_.registry()->RegisterIntegerPref(
        prefs::kDeviceStatusUploadRate,
        StatusUploader::kDefaultUploadDelayMs);
  }

  void SetUp() override {
    client_.SetDMToken("dm_token");
    collector_.reset(new MockDeviceStatusCollector(&prefs_));
  }

  void TearDown() override {
    content::RunAllBlockingPoolTasksUntilIdle();
  }

  // Given a pending task to upload status, mocks out a server response.
  void RunPendingUploadTaskAndCheckNext(const StatusUploader& uploader) {
    EXPECT_FALSE(task_runner_->GetPendingTasks().empty());
    CloudPolicyClient::StatusCallback callback;
    EXPECT_CALL(client_, UploadDeviceStatus(_, _, _))
        .WillOnce(SaveArg<2>(&callback));
    task_runner_->RunPendingTasks();
    testing::Mock::VerifyAndClearExpectations(&device_management_service_);
    // Make sure no status upload is queued up yet (since an upload is in
    // progress).
    EXPECT_TRUE(task_runner_->GetPendingTasks().empty());

    // Now invoke the response.
    callback.Run(true);

    // Now that the previous request was satisfied, a task to do the next
    // upload should be queued.
    EXPECT_EQ(1U, task_runner_->GetPendingTasks().size());

    // The next refresh would have been scheduled when we invoked the callback.
    // So the next task should be scheduled sometime between |last_upload| +
    // kDefaultUploadDelayMs and |now| + kDefaultUploadDelayMs.
    base::Time now = base::Time::NowFromSystemTime();
    base::Time next_task = now + task_runner_->NextPendingTaskDelay();

    base::TimeDelta expected_delay = base::TimeDelta::FromMilliseconds(
        StatusUploader::kDefaultUploadDelayMs);
    EXPECT_LE(next_task, now + expected_delay);
    EXPECT_GE(next_task, uploader.last_upload() + expected_delay);
  }

  base::MessageLoop loop_;
  scoped_refptr<base::TestSimpleTaskRunner> task_runner_;
  chromeos::ScopedTestDeviceSettingsService test_device_settings_service_;
  chromeos::ScopedTestCrosSettings test_cros_settings_;
  scoped_ptr<MockDeviceStatusCollector> collector_;
  MockCloudPolicyClient client_;
  MockDeviceManagementService device_management_service_;
  TestingPrefServiceSimple prefs_;
};

TEST_F(StatusUploaderTest, BasicTest) {
  EXPECT_TRUE(task_runner_->GetPendingTasks().empty());
  StatusUploader uploader(
      &prefs_, &client_, collector_.Pass(), task_runner_);
  EXPECT_EQ(1U, task_runner_->GetPendingTasks().size());
  // On startup, first update should happen immediately.
  EXPECT_EQ(base::TimeDelta(), task_runner_->NextPendingTaskDelay());
}

TEST_F(StatusUploaderTest, ResetTimerAfterStatusCollection) {
  // Keep a pointer to the mock collector because the scoped_ptr gets cleared
  // when it is passed to CreateStatusUploader() below.
  MockDeviceStatusCollector* mock_collector = collector_.get();
  StatusUploader uploader(
      &prefs_, &client_, collector_.Pass(), task_runner_);
  EXPECT_CALL(*mock_collector, GetDeviceStatus(_)).WillRepeatedly(Return(true));
  EXPECT_CALL(*mock_collector, GetDeviceSessionStatus(_)).WillRepeatedly(
      Return(true));
  RunPendingUploadTaskAndCheckNext(uploader);

  // Handle this response also, and ensure new task is queued.
  RunPendingUploadTaskAndCheckNext(uploader);

  // Now that the previous request was satisfied, a task to do the next
  // upload should be queued again.
  EXPECT_EQ(1U, task_runner_->GetPendingTasks().size());
}

TEST_F(StatusUploaderTest, ResetTimerAfterFailedStatusCollection) {
  // Keep a pointer to the mock collector because the scoped_ptr gets cleared
  // when it is passed to CreateStatusUploader() below.
  MockDeviceStatusCollector* mock_collector = collector_.get();
  StatusUploader uploader(
      &prefs_, &client_, collector_.Pass(), task_runner_);
  EXPECT_CALL(*mock_collector, GetDeviceStatus(_)).WillOnce(Return(false));
  EXPECT_CALL(*mock_collector, GetDeviceSessionStatus(_)).WillOnce(
      Return(false));
  task_runner_->RunPendingTasks();

  // Make sure the next status upload is queued up.
  EXPECT_EQ(1U, task_runner_->GetPendingTasks().size());
}

}  // namespace policy
