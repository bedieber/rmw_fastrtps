#include "fastrtps/rtps/common/Guid.h"

#include "rmw/types.h"

#include "rmw_fastrtps_shared_cpp/create_rmw_gid.hpp"

rmw_gid_t
rmw_fastrtps_shared_cpp::create_rmw_gid(
  const char * identifier, const eprosima::fastrtps::rtps::GUID_t & guid)
{
  rmw_gid_t rmw_gid;
  rmw_gid.implementation_identifier = identifier;
  static_assert(
    sizeof(eprosima::fastrtps::rtps::GUID_t) <= RMW_GID_STORAGE_SIZE,
    "RMW_GID_STORAGE_SIZE insufficient to store the fastrtps GUID_t."
  );
  memset(rmw_gid.data, 0, RMW_GID_STORAGE_SIZE);
  memcpy(rmw_gid.data, &guid, sizeof(eprosima::fastrtps::rtps::GUID_t));
  return rmw_gid;
}
