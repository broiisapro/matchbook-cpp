#pragma once

namespace lob::bench {

/// Raises the calling thread's QoS class to USER_INTERACTIVE. macOS has no core
/// pinning or isolation, so this plus an otherwise idle machine is the
/// strongest scheduling guarantee available; the report notes it.
/// Returns the pthread status code: 0 on success.
int set_user_interactive_qos();

}  // namespace lob::bench
