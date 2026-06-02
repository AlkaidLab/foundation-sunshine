#pragma once

extern "C" {
#include <moonlight-common-c/src/Session.h>
}

#include <alkaidlab/gamestream_host_adapter/gamestream_host_session_adapter.h>

#include <cstdint>

namespace stream::alkaidlab_session_bridge {

  bool update_from_li_session(AlkSessionAdapterContext &context, const LI_SESSION &session);
  bool project_to_li_session(const AlkSessionAdapterContext &context, LI_SESSION &session);
  bool update_cursor_plane(AlkSessionAdapterContext &context, const LI_SESSION_CURSOR_PLANE &cursor_plane);
  bool update_lease(AlkSessionAdapterContext &context,
                    const LI_SESSION_LEASE &lease,
                    std::uint32_t status);

}  // namespace stream::alkaidlab_session_bridge
