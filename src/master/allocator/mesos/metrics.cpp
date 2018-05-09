// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "master/allocator/mesos/metrics.hpp"

#include <string>

#include <mesos/quota/quota.hpp>

#include <process/metrics/pull_gauge.hpp>
#include <process/metrics/push_gauge.hpp>
#include <process/metrics/metrics.hpp>

#include <stout/hashmap.hpp>

#include "common/protobuf_utils.hpp"

#include "master/allocator/mesos/hierarchical.hpp"

#include "master/metrics.hpp"

using std::string;

using process::metrics::PullGauge;
using process::metrics::PushGauge;

namespace mesos {
namespace internal {
namespace master {
namespace allocator {
namespace internal {

Metrics::Metrics(const HierarchicalAllocatorProcess& _allocator)
  : allocator(_allocator.self()),
    event_queue_dispatches(
        "allocator/mesos/event_queue_dispatches",
        process::defer(
            allocator, &HierarchicalAllocatorProcess::_event_queue_dispatches)),
    event_queue_dispatches_(
        "allocator/event_queue_dispatches",
        process::defer(
            allocator, &HierarchicalAllocatorProcess::_event_queue_dispatches)),
    allocation_runs("allocator/mesos/allocation_runs"),
    allocation_run("allocator/mesos/allocation_run", Hours(1)),
    allocation_run_latency("allocator/mesos/allocation_run_latency", Hours(1))
{
  process::metrics::add(event_queue_dispatches);
  process::metrics::add(event_queue_dispatches_);
  process::metrics::add(allocation_runs);
  process::metrics::add(allocation_run);
  process::metrics::add(allocation_run_latency);

  // Create and install gauges for the total and allocated
  // amount of standard scalar resources.
  //
  // TODO(bbannier) Add support for more than just scalar resources.
  // TODO(bbannier) Simplify this once MESOS-3214 is fixed.
  // TODO(dhamon): Set these up dynamically when adding a slave based on the
  // resources the slave exposes.
  string resources[] = {"cpus", "mem", "disk"};

  foreach (const string& resource, resources) {
    PullGauge total(
        "allocator/mesos/resources/" + resource + "/total",
        defer(allocator,
              &HierarchicalAllocatorProcess::_resources_total,
              resource));

    PullGauge offered_or_allocated(
        "allocator/mesos/resources/" + resource + "/offered_or_allocated",
        defer(allocator,
              &HierarchicalAllocatorProcess::_resources_offered_or_allocated,
              resource));

    resources_total.push_back(total);
    resources_offered_or_allocated.push_back(offered_or_allocated);

    process::metrics::add(total);
    process::metrics::add(offered_or_allocated);
  }
}


Metrics::~Metrics()
{
  process::metrics::remove(event_queue_dispatches);
  process::metrics::remove(event_queue_dispatches_);
  process::metrics::remove(allocation_runs);
  process::metrics::remove(allocation_run);
  process::metrics::remove(allocation_run_latency);

  foreach (const PullGauge& gauge, resources_total) {
    process::metrics::remove(gauge);
  }

  foreach (const PullGauge& gauge, resources_offered_or_allocated) {
    process::metrics::remove(gauge);
  }

  foreachkey (const string& role, quota_allocated) {
    foreachvalue (const PullGauge& gauge, quota_allocated[role]) {
      process::metrics::remove(gauge);
    }
  }

  foreachkey (const string& role, quota_guarantee) {
    foreachvalue (const PullGauge& gauge, quota_guarantee[role]) {
      process::metrics::remove(gauge);
    }
  }

  foreachvalue (const PullGauge& gauge, offer_filters_active) {
    process::metrics::remove(gauge);
  }
}


void Metrics::setQuota(const string& role, const Quota& quota)
{
  CHECK(!quota_allocated.contains(role));

  hashmap<string, PullGauge> allocated;
  hashmap<string, PullGauge> guarantees;

  foreach (const Resource& resource, quota.info.guarantee()) {
    CHECK_EQ(Value::SCALAR, resource.type());
    double value = resource.scalar().value();

    PullGauge guarantee = PullGauge(
        "allocator/mesos/quota"
        "/roles/" + role +
        "/resources/" + resource.name() +
        "/guarantee",
        process::defer([value]() { return value; }));

    PullGauge offered_or_allocated(
        "allocator/mesos/quota"
        "/roles/" + role +
        "/resources/" + resource.name() +
        "/offered_or_allocated",
        defer(allocator,
              &HierarchicalAllocatorProcess::_quota_allocated,
              role,
              resource.name()));

    guarantees.put(resource.name(), guarantee);
    allocated.put(resource.name(), offered_or_allocated);

    process::metrics::add(guarantee);
    process::metrics::add(offered_or_allocated);
  }

  quota_allocated[role] = allocated;
  quota_guarantee[role] = guarantees;
}


void Metrics::removeQuota(const string& role)
{
  CHECK(quota_allocated.contains(role));

  foreachvalue (const PullGauge& gauge, quota_allocated[role]) {
    process::metrics::remove(gauge);
  }

  quota_allocated.erase(role);
}


void Metrics::addRole(const string& role)
{
  CHECK(!offer_filters_active.contains(role));

  PullGauge gauge(
      "allocator/mesos/offer_filters/roles/" + role + "/active",
      defer(allocator,
            &HierarchicalAllocatorProcess::_offer_filters_active,
            role));

  offer_filters_active.put(role, gauge);

  process::metrics::add(gauge);
}


void Metrics::removeRole(const string& role)
{
  Option<PullGauge> gauge = offer_filters_active.get(role);

  CHECK_SOME(gauge);

  offer_filters_active.erase(role);

  process::metrics::remove(gauge.get());
}


FrameworkMetrics::FrameworkMetrics(const FrameworkInfo& _frameworkInfo)
  : frameworkInfo(_frameworkInfo),
    resources_filtered(
        getFrameworkMetricPrefix(frameworkInfo) +
          "allocation/resources_filtered"),
    resources_filtered_decline(
        getFrameworkMetricPrefix(frameworkInfo) +
          "allocation/resources_filtered/decline"),
    resources_filtered_gpu(
        getFrameworkMetricPrefix(frameworkInfo) +
          "allocation/resources_filtered/gpu_resources"),
    resources_filtered_region_aware(
        getFrameworkMetricPrefix(frameworkInfo) +
          "allocation/resources_filtered/region_aware"),
    resources_filtered_reservation_refinement(
        getFrameworkMetricPrefix(frameworkInfo) +
          "allocation/resources_filtered/reservation_refinement"),
    resources_filtered_revocable(
        getFrameworkMetricPrefix(frameworkInfo) +
          "allocation/resources_filtered/revocable_resources")
{
  process::metrics::add(resources_filtered);
  process::metrics::add(resources_filtered_decline);
  process::metrics::add(resources_filtered_gpu);
  process::metrics::add(resources_filtered_region_aware);
  process::metrics::add(resources_filtered_reservation_refinement);
  process::metrics::add(resources_filtered_revocable);

  // Add all roles non-suppressed by default.
  foreach (
      const string& role,
      protobuf::framework::getRoles(frameworkInfo)) {
    reviveRole(role);
  }
}


FrameworkMetrics::~FrameworkMetrics()
{
  process::metrics::remove(resources_filtered);
  process::metrics::remove(resources_filtered_decline);
  process::metrics::remove(resources_filtered_gpu);
  process::metrics::remove(resources_filtered_region_aware);
  process::metrics::remove(resources_filtered_reservation_refinement);
  process::metrics::remove(resources_filtered_revocable);

  foreachvalue (const DrfPositions& positions, roleDrfPositions) {
    process::metrics::remove(positions.min);
    process::metrics::remove(positions.max);
  }

  foreachvalue (const PushGauge& gauge, suppressed) {
    process::metrics::remove(gauge);
  }
}


FrameworkMetrics::DrfPositions::DrfPositions(const string& prefix)
  : min(prefix + "min"),
    max(prefix + "max") {}


void FrameworkMetrics::setDrfPositions(
    const std::string& role,
    const std::pair<size_t, size_t>& minMax)
{
  if (!roleDrfPositions.contains(role)) {
    roleDrfPositions.emplace(
        role,
        DrfPositions(
            getFrameworkMetricPrefix(frameworkInfo) + "allocation/roles/" +
              normalizeMetricKey(role) + "/latest_position/"));

    process::metrics::add(roleDrfPositions.at(role).min);
    process::metrics::add(roleDrfPositions.at(role).max);
  }

  roleDrfPositions.at(role).min = minMax.first;
  roleDrfPositions.at(role).max = minMax.second;
}


void FrameworkMetrics::reviveRole(const string& role)
{
  if (!suppressed.contains(role)) {
    suppressed.emplace(
        role,
        PushGauge(
            getFrameworkMetricPrefix(frameworkInfo) + "roles/" +
            normalizeMetricKey(role) + "/suppressed"));
    process::metrics::add(suppressed.at(role));
  }

  suppressed.at(role) = 0.;
}


void FrameworkMetrics::suppressRole(const string& role)
{
  if (!suppressed.contains(role)) {
    suppressed.emplace(
        role,
        PushGauge(
            getFrameworkMetricPrefix(frameworkInfo) + "roles/" +
            normalizeMetricKey(role) + "/suppressed"));
    process::metrics::add(suppressed.at(role));
  }

  suppressed.at(role) = 1.;
}


void FrameworkMetrics::removeSuppressedRole(const string& role)
{
  CHECK(suppressed.contains(role));
  process::metrics::remove(suppressed.at(role));
  suppressed.erase(role);
}

} // namespace internal {
} // namespace allocator {
} // namespace master {
} // namespace internal {
} // namespace mesos {
