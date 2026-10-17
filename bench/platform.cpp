#include "bench/platform.hpp"

#include <pthread/qos.h>

namespace lob::bench {

int set_user_interactive_qos() {
  return pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
}

}  // namespace lob::bench
