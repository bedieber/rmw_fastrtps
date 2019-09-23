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

#ifndef RMW_FASTRTPS_SHARED_CPP__CUSTOM_PARTICIPANT_INFO_HPP_
#define RMW_FASTRTPS_SHARED_CPP__CUSTOM_PARTICIPANT_INFO_HPP_

#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "fastrtps/rtps/common/InstanceHandle.h"
#include "fastrtps/attributes/ParticipantAttributes.h"
#include "fastrtps/participant/Participant.h"
#include "fastrtps/participant/ParticipantListener.h"

#include "rcpputils/thread_safety_annotations.hpp"
#include "rcutils/logging_macros.h"

#include "rmw/impl/cpp/key_value.hpp"
#include "rmw/rmw.h"

#include "rmw_dds_common/context.hpp"
#include "rmw_dds_common/node_cache.hpp"
#include "rmw_dds_common/topic_cache.hpp"

#include "rmw_fastrtps_shared_cpp/create_rmw_gid.hpp"
#include "rmw_fastrtps_shared_cpp/rmw_common.hpp"

using rmw_dds_common::operator<<;

class ParticipantListener;

typedef struct CustomParticipantInfo
{
  eprosima::fastrtps::Participant * participant;
  ::ParticipantListener * listener;
  rmw_guard_condition_t * graph_guard_condition;

  // Flag to establish if the QoS of the participant,
  // its publishers and its subscribers are going
  // to be configured only from an XML file or if
  // their settings are going to be overwritten by code
  // with the default configuration.
  bool leave_middleware_default_qos;
} CustomParticipantInfo;

class ParticipantListener : public eprosima::fastrtps::ParticipantListener
{
public:
  using TopicCache = rmw_dds_common::TopicCache;
  using NodeCache = rmw_dds_common::NodeCache;

  explicit ParticipantListener(
    rmw_guard_condition_t * graph_guard_condition,
    rmw_dds_common::Context * context)
  : context(context),
    graph_guard_condition_(graph_guard_condition)
  {}

  void onParticipantDiscovery(
    eprosima::fastrtps::Participant *,
    eprosima::fastrtps::rtps::ParticipantDiscoveryInfo && info) override
  {
    if (
      info.status != eprosima::fastrtps::rtps::ParticipantDiscoveryInfo::DISCOVERED_PARTICIPANT &&
      info.status != eprosima::fastrtps::rtps::ParticipantDiscoveryInfo::REMOVED_PARTICIPANT &&
      info.status != eprosima::fastrtps::rtps::ParticipantDiscoveryInfo::DROPPED_PARTICIPANT)
    {
      return;
    }
    if (eprosima::fastrtps::rtps::ParticipantDiscoveryInfo::DISCOVERED_PARTICIPANT == info.status) {
      context->node_cache.add_gid(rmw_fastrtps_shared_cpp::create_rmw_gid(
        graph_guard_condition_->implementation_identifier, info.info.m_guid));
    } else {
      context->node_cache.delete_node_names(rmw_fastrtps_shared_cpp::create_rmw_gid(
        graph_guard_condition_->implementation_identifier, info.info.m_guid));
    }
  }

  void onSubscriberDiscovery(
    eprosima::fastrtps::Participant *,
    eprosima::fastrtps::rtps::ReaderDiscoveryInfo && info) override
  {
    if (eprosima::fastrtps::rtps::ReaderDiscoveryInfo::CHANGED_QOS_READER != info.status) {
      bool is_alive =
        eprosima::fastrtps::rtps::ReaderDiscoveryInfo::DISCOVERED_READER == info.status;
      process_discovery_info(info.info, is_alive, true);
    }
  }

  void onPublisherDiscovery(
    eprosima::fastrtps::Participant *,
    eprosima::fastrtps::rtps::WriterDiscoveryInfo && info) override
  {
    if (eprosima::fastrtps::rtps::WriterDiscoveryInfo::CHANGED_QOS_WRITER != info.status) {
      bool is_alive =
        eprosima::fastrtps::rtps::WriterDiscoveryInfo::DISCOVERED_WRITER == info.status;
      process_discovery_info(info.info, is_alive, false);
    }
  }

  template<class T>
  void
  process_discovery_info(T & proxyData, bool is_alive, bool is_reader)
  {
    auto & topic_cache =
      is_reader ? context->reader_topic_cache : context->writer_topic_cache;

    bool trigger;
    const auto & group_data = proxyData.m_qos.m_groupData.getValue();
    const auto & map = rmw::impl::cpp::parse_key_value(group_data);
    const auto & name_found = map.find("name");
    const auto & ns_found = map.find("namespace");
    if (name_found == map.end() || ns_found == map.end()) {
      RCUTILS_LOG_DEBUG_NAMED("rmw_fastrtps_cpp", "Unexpected group data");
      return;
    }
    std::string name(name_found->second.begin(), name_found->second.end());
    std::string ns(ns_found->second.begin(), ns_found->second.end());
    {
      if (is_alive) {
        trigger = topic_cache.add_topic(
          rmw_fastrtps_shared_cpp::create_rmw_gid(
            graph_guard_condition_->implementation_identifier,
            iHandle2GUID(proxyData.RTPSParticipantKey())),
          ns,
          name,
          proxyData.topicName().to_string(),
          proxyData.typeName().to_string());
      } else {
        trigger = topic_cache.remove_topic(
          rmw_fastrtps_shared_cpp::create_rmw_gid(
            graph_guard_condition_->implementation_identifier,
            iHandle2GUID(proxyData.RTPSParticipantKey())),
          ns,
          name,
          proxyData.topicName().to_string(),
          proxyData.typeName().to_string());
      }
    }
    if (trigger) {
      rmw_fastrtps_shared_cpp::__rmw_trigger_guard_condition(
        graph_guard_condition_->implementation_identifier,
        graph_guard_condition_);
    }
  }

  using guid_map_t = std::map<eprosima::fastrtps::rtps::GUID_t, std::string>;
  mutable std::mutex names_mutex_;
  guid_map_t discovered_names RCPPUTILS_TSA_GUARDED_BY(names_mutex_);
  guid_map_t discovered_namespaces RCPPUTILS_TSA_GUARDED_BY(names_mutex_);
  rmw_dds_common::Context * context;
  rmw_guard_condition_t * graph_guard_condition_;
};

#endif  // RMW_FASTRTPS_SHARED_CPP__CUSTOM_PARTICIPANT_INFO_HPP_
