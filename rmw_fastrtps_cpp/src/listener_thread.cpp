// Copyright 2019 Open Source Robotics Foundation, Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <atomic>
#include <cassert>
#include <cstring>
#include <thread>

#include "rcutils/logging_macros.h"

#include "rmw/allocators.h"
#include "rmw/error_handling.h"
#include "rmw/init.h"
#include "rmw/ret_types.h"
#include "rmw/rmw.h"
#include "rmw/impl/cpp/macros.hpp"

#include "rmw_dds_common/context.hpp"
#include "rmw_dds_common/gid_utils.hpp"
#include "rmw_dds_common/node_cache.hpp"
#include "rmw_dds_common/msg/participant_custom_info.hpp"

#include "rmw_fastrtps_shared_cpp/rmw_context_impl.h"

#include "listener_thread.hpp"

using rmw_dds_common::operator<<;

static
void
node_listener(rmw_context_t * context);

rmw_ret_t
rmw_fastrtps_cpp::run_listener_thread(rmw_context_t * context)
{
  auto common_context = static_cast<rmw_dds_common::Context *>(context->impl->common);
  try {
    common_context->thread_is_running.store(true);
    common_context->listener_thread = std::thread(node_listener, context);
  } catch (...) {
    RMW_SET_ERROR_MSG("failed to create node listener thread");
    return RMW_RET_ERROR;
  }
  return RMW_RET_OK;
}

rmw_ret_t
rmw_fastrtps_cpp::join_listener_thread(rmw_context_t * context)
{
  auto common_context = static_cast<rmw_dds_common::Context *>(context->impl->common);
  try {
    common_context->thread_is_running.exchange(false);
    common_context->listener_thread.join();
  } catch (...) {
    RMW_SET_ERROR_MSG("failed to join node listener thread");
    return RMW_RET_ERROR;
  }
  return RMW_RET_ERROR;
}

static const char log_tag[] = "rmw_dds_common node listener";

static
void
terminate(const char * error_message) {
  RCUTILS_LOG_ERROR_NAMED(log_tag, "%s, terminating ...", error_message);
  std::terminate();
}

void
node_listener(rmw_context_t * context) {
  assert(nullptr != context);
  assert(nullptr != context->impl);
  assert(nullptr != context->impl->common);
  auto common_context = static_cast<rmw_dds_common::Context *>(context->impl->common);
  while (common_context->thread_is_running.load()) {
    assert(nullptr != common_context->sub);
    assert(nullptr != common_context->sub->data);
    void * subscriptions_buffer[] = {common_context->sub->data};
    rmw_subscriptions_t subscriptions;
    subscriptions.subscriber_count = 1;
    subscriptions.subscribers = subscriptions_buffer;
    rmw_wait_set_t * wait_set = rmw_create_wait_set(context, 2); // number of conditions of a subscription is 2
    if (nullptr == wait_set) {
      terminate("failed to create wait set");
    }
    if (RMW_RET_OK != rmw_wait(
      &subscriptions,
      nullptr,
      nullptr,
      nullptr,
      nullptr,
      wait_set,
      nullptr))
    {
      terminate("rmw_wait failed");
    }
    if (subscriptions_buffer[0]) {
      rmw_dds_common::msg::ParticipantCustomInfo msg;
      bool taken;
      if (RMW_RET_OK != rmw_take(
        common_context->sub,
        static_cast<void *>(&msg),
        &taken,
        nullptr))
      {
        terminate("rmw_take failed");
      }
      if (taken) {
        // TODO(ivanpauno): Should the program be terminated if taken is false?
        rmw_gid_t gid;
        rmw_dds_common::convert_msg_to_gid(&msg.id, &gid);
        if (std::strcmp(
          reinterpret_cast<char *>(common_context->gid.data),
          reinterpret_cast<char *>(gid.data)) == 0)
        {
          // ignore local messages
          continue;
        }
        common_context->node_cache.update_node_names(gid, msg.nodes_info);
        if (rcutils_logging_logger_is_enabled_for("rmw_dds_common",
          RCUTILS_LOG_SEVERITY_DEBUG))
        {
          std::ostringstream ss;
          ss << common_context->node_cache;
          RCUTILS_LOG_DEBUG_NAMED("rmw_fastrtps_cpp", "%s", ss.str().c_str());
        }
      }
    }
    if (RMW_RET_OK != rmw_destroy_wait_set(wait_set)) {
      terminate("failed to destroy wait set");
    }
  }
}
