// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

#include <vector>

#include "kudu/gutil/strings/substitute.h"
#include "kudu/integration-tests/cluster_verifier.h"
#include "kudu/integration-tests/external_mini_cluster-itest-base.h"
#include "kudu/integration-tests/test_workload.h"
#include "kudu/util/metrics.h"

using std::string;
using strings::Substitute;

DECLARE_string(block_manager);

METRIC_DECLARE_entity(server);
METRIC_DECLARE_counter(log_block_manager_containers);
METRIC_DECLARE_counter(log_block_manager_unavailable_containers);

namespace kudu {

namespace {
Status GetTsCounterValue(ExternalTabletServer* ets, MetricPrototype* metric, int64_t* value) {
  return ets->GetInt64Metric(
             &METRIC_ENTITY_server,
             "kudu.tabletserver",
             metric,
             "value",
             value);
}
} // namespace

class DiskReservationITest : public ExternalMiniClusterITestBase {
};

// Test that when we fill up a disk beyond its configured reservation limit, we
// use other disks for data blocks until all disks are full, at which time we
// crash. This functionality is only implemented in the log block manager.
TEST_F(DiskReservationITest, TestFillMultipleDisks) {
  if (FLAGS_block_manager != "log") {
    LOG(INFO) << "This platform does not use the log block manager by default. Skipping test.";
    return;
  }

  // Set up the tablet so that flushes are constantly occurring.
  vector<string> ts_flags;
  ts_flags.push_back("--flush_threshold_mb=0");
  ts_flags.push_back("--maintenance_manager_polling_interval_ms=100");
  ts_flags.push_back("--disable_core_dumps");
  ts_flags.push_back(Substitute("--fs_data_dirs=$0/a,$0/b",
                                GetTestDataDirectory()));
  NO_FATALS(StartCluster(ts_flags, {}, 1));

  TestWorkload workload(cluster_.get());
  workload.set_num_replicas(1);
  // Use a short timeout so that at the end of the test, when we expect a
  // crash, stopping the workload and joining the client threads is quick.
  workload.set_timeout_allowed(true);
  workload.set_write_timeout_millis(100);
  workload.Setup();
  workload.Start();

  // Wait until we have 2 active containers.
  while (true) {
    int64_t num_containers;
    ASSERT_OK(GetTsCounterValue(cluster_->tablet_server(0), &METRIC_log_block_manager_containers,
                                &num_containers));
    if (num_containers >= 2) break;
    SleepFor(MonoDelta::FromMilliseconds(10));
  }

  LOG(INFO) << "Two log block containers are active";

  // Simulate that /a has 0 bytes free but /b has 1GB free.
  ASSERT_OK(cluster_->SetFlag(cluster_->tablet_server(0),
                              "disk_reserved_prefixes_with_bytes_free_for_testing",
                              Substitute("$0/a:0,$0/b:$1",
                                         GetTestDataDirectory(),
                                         1L * 1024 * 1024 * 1024)));

  // Wait until we have 1 unusable container.
  while (true) {
    int64_t num_unavailable_containers;
    ASSERT_OK(GetTsCounterValue(cluster_->tablet_server(0),
                                &METRIC_log_block_manager_unavailable_containers,
                                &num_unavailable_containers));
    if (num_unavailable_containers >= 1) break;
    SleepFor(MonoDelta::FromMilliseconds(10));
  }

  LOG(INFO) << "Have 1 unavailable log block container";

  // Now simulate that all disks are full.
  ASSERT_OK(cluster_->SetFlag(cluster_->tablet_server(0),
                              "disk_reserved_prefixes_with_bytes_free_for_testing",
                              Substitute("$0/a:0,$0/b:0",
                                         GetTestDataDirectory())));

  // Wait for crash due to inability to flush or compact.
  ASSERT_OK(cluster_->tablet_server(0)->WaitForCrash(MonoDelta::FromSeconds(10)));
  workload.StopAndJoin();
}

// When the WAL disk goes beyond its configured reservation, attempts to write
// to the WAL should cause a fatal error.
TEST_F(DiskReservationITest, TestWalWriteToFullDiskAborts) {
  vector<string> ts_flags;
  ts_flags.push_back("--log_segment_size_mb=1"); // Encourage log rolling to speed up the test.
  ts_flags.push_back("--disable_core_dumps");
  NO_FATALS(StartCluster(ts_flags, {}, 1));

  TestWorkload workload(cluster_.get());
  workload.set_num_replicas(1);
  workload.set_timeout_allowed(true); // Allow timeouts because we expect the server to crash.
  workload.set_write_timeout_millis(100); // Keep test time low after crash.
  // Write lots of data to quickly fill up our 1mb log segment size.
  workload.set_num_write_threads(8);
  workload.set_write_batch_size(1024);
  workload.set_payload_bytes(128);
  workload.Setup();
  workload.Start();

  // Set the disk to "nearly full" which should eventually cause a crash at WAL
  // preallocation time.
  ASSERT_OK(cluster_->SetFlag(cluster_->tablet_server(0),
                              "fs_wal_dir_reserved_bytes", "10000000"));
  ASSERT_OK(cluster_->SetFlag(cluster_->tablet_server(0),
                              "disk_reserved_bytes_free_for_testing", "10000001"));

  ASSERT_OK(cluster_->tablet_server(0)->WaitForCrash(MonoDelta::FromSeconds(10)));
  workload.StopAndJoin();
}

} // namespace kudu
