// Copyright 2019 Open Source Robotics Foundation, Inc.
// Copyright 2016-2018 Proyectos y Sistemas de Mantenimiento SL (eProsima).
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

#include <array>
#include <mutex>
#include <utility>
#include <set>
#include <string>

#include "rcutils/filesystem.h"
#include "rcutils/logging_macros.h"

#include "rmw/allocators.h"
#include "rmw/error_handling.h"
#include "rmw/impl/cpp/macros.hpp"
#include "rmw/rmw.h"

#include "fastrtps/config.h"
#include "fastrtps/Domain.h"
#include "fastrtps/participant/Participant.h"
#include "fastrtps/attributes/ParticipantAttributes.h"
#include "fastrtps/publisher/Publisher.h"
#include "fastrtps/attributes/PublisherAttributes.h"
#include "fastrtps/publisher/PublisherListener.h"
#include "fastrtps/subscriber/Subscriber.h"
#include "fastrtps/subscriber/SubscriberListener.h"
#include "fastrtps/subscriber/SampleInfo.h"
#include "fastrtps/attributes/SubscriberAttributes.h"

#include "fastrtps/rtps/RTPSDomain.h"

#include "fastrtps/rtps/reader/RTPSReader.h"
#include "fastrtps/rtps/reader/StatefulReader.h"
#include "fastrtps/rtps/reader/ReaderListener.h"
#include "fastrtps/rtps/builtin/discovery/endpoint/EDPSimple.h"

#include "rmw_dds_common/context.hpp"

#include "rmw_fastrtps_shared_cpp/custom_participant_info.hpp"
#include "rmw_fastrtps_shared_cpp/rmw_common.hpp"
#include "rmw_fastrtps_shared_cpp/rmw_context_impl.h"

using Domain = eprosima::fastrtps::Domain;
using Participant = eprosima::fastrtps::Participant;
using ParticipantAttributes = eprosima::fastrtps::ParticipantAttributes;
using StatefulReader = eprosima::fastrtps::rtps::StatefulReader;

namespace rmw_fastrtps_shared_cpp
{
rmw_node_t *
__rmw_create_node(
  rmw_context_t * context,
  const char * identifier,
  const char * name,
  const char * namespace_)
{
  if (!name) {
    RMW_SET_ERROR_MSG("name is null");
    return nullptr;
  }

  if (!namespace_) {
    RMW_SET_ERROR_MSG("namespace_ is null");
    return nullptr;
  }

  rmw_node_t * node_handle = nullptr;
  rmw_dds_common::msg::ParticipantCustomInfo participant_info;
  auto common_context = static_cast<rmw_dds_common::Context *>(context->impl->common);
  rmw_dds_common::NodeCache & node_cache = common_context->node_cache;

  node_handle = rmw_node_allocate();
  if (!node_handle) {
    RMW_SET_ERROR_MSG("failed to allocate rmw_node_t");
    goto fail;
  }
  node_handle->implementation_identifier = identifier;
  node_handle->data = nullptr;

  node_handle->name =
    static_cast<const char *>(rmw_allocate(sizeof(char) * strlen(name) + 1));
  if (!node_handle->name) {
    RMW_SET_ERROR_MSG("failed to allocate memory");
    node_handle->namespace_ = nullptr;  // to avoid free on uninitialized memory
    goto fail;
  }
  memcpy(const_cast<char *>(node_handle->name), name, strlen(name) + 1);

  node_handle->namespace_ =
    static_cast<const char *>(rmw_allocate(sizeof(char) * strlen(namespace_) + 1));
  if (!node_handle->namespace_) {
    RMW_SET_ERROR_MSG("failed to allocate memory");
    goto fail;
  }
  memcpy(const_cast<char *>(node_handle->namespace_), namespace_, strlen(namespace_) + 1);
  node_handle->context = context;

  {
    // Though node_cache methods are thread safe, both cache update and publishing have to also
    // be atomic.
    // If not, the following race condition is possible:
    // node1-update-get-message / node2-update-get-message / node2-publish / node1-publish
    // In that case, the last message published is not accurate.
    std::lock_guard<std::mutex> guard(common_context->node_update_mutex);
    if (RMW_RET_OK != node_cache.add_node_name(
      common_context->gid,
      name,
      namespace_))
    {
      goto fail;
    }
    if (RMW_RET_OK != node_cache.get_participant_state_message(
      common_context->gid,
      participant_info))
    {
      goto fail;
    }
    if (RMW_RET_OK != __rmw_publish(
      identifier,
      common_context->pub,
      static_cast<void *>(&participant_info),
      nullptr))
    {
      goto fail;
    }
  }
  return node_handle;
fail:
  if (node_handle) {
    rmw_free(const_cast<char *>(node_handle->namespace_));
    node_handle->namespace_ = nullptr;
    rmw_free(const_cast<char *>(node_handle->name));
    node_handle->name = nullptr;
  }
  rmw_node_free(node_handle);
  return nullptr;
}

rmw_ret_t
__rmw_destroy_node(
  const char * identifier,
  rmw_node_t * node)
{
  rmw_ret_t result_ret = RMW_RET_OK;
  if (!node) {
    RMW_SET_ERROR_MSG("node handle is null");
    return RMW_RET_ERROR;
  }

  if (node->implementation_identifier != identifier) {
    RMW_SET_ERROR_MSG("node handle not from this implementation");
    return RMW_RET_ERROR;
  }

  auto common_context = static_cast<rmw_dds_common::Context *>(node->context->impl->common);
  rmw_dds_common::NodeCache & node_cache = common_context->node_cache;
  rmw_context_t * context = node->context;
  result_ret = node_cache.delete_node_name(
    common_context->gid,
    node->name,
    node->namespace_);
  if (RMW_RET_OK != result_ret) {
    return result_ret;
  }
  rmw_dds_common::msg::ParticipantCustomInfo participant_info_msg;
  result_ret = node_cache.get_participant_state_message(
    common_context->gid,
    participant_info_msg);
  if (RMW_RET_OK != result_ret) {
    return result_ret;
  }
  rmw_free(const_cast<char *>(node->name));
  node->name = nullptr;
  rmw_free(const_cast<char *>(node->namespace_));
  node->namespace_ = nullptr;
  rmw_node_free(node);

  return __rmw_publish(
    identifier,
    common_context->pub,
    static_cast<void *>(&participant_info_msg),
    nullptr);
}

rmw_ret_t
__rmw_node_assert_liveliness(
  const char * identifier,
  const rmw_node_t * node)
{
  (void)identifier;
  (void)node;
  // RMW_CHECK_ARGUMENT_FOR_NULL(node, RMW_RET_INVALID_ARGUMENT);
  // RMW_CHECK_TYPE_IDENTIFIERS_MATCH(
  //   node,
  //   node->implementation_identifier,
  //   identifier,
  //   return RMW_RET_INCORRECT_RMW_IMPLEMENTATION);

  // auto node_info = static_cast<CustomParticipantInfo *>(node->data);
  // if (nullptr == node_info) {
  //   RMW_SET_ERROR_MSG("node info handle is null");
  //   return RMW_RET_ERROR;
  // }
  // if (nullptr == node_info->participant) {
  //   RMW_SET_ERROR_MSG("node internal participant is invalid");
  //   return RMW_RET_ERROR;
  // }

  // node_info->participant->assert_liveliness();
  return RMW_RET_OK;
}

const rmw_guard_condition_t *
__rmw_node_get_graph_guard_condition(const rmw_node_t * node)
{
  auto impl = static_cast<CustomParticipantInfo *>(node->context->impl->participant_info);
  if (!impl) {
    RMW_SET_ERROR_MSG("node impl is null");
    return nullptr;
  }
  return impl->graph_guard_condition;
}
}  // namespace rmw_fastrtps_shared_cpp
