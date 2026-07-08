#include <bupp/base/linux/probe.h>
#include <bupp/base/linux/ring.h>

#include <cassert>
#include <cerrno>
#include <utility>

namespace {

bool is_unsupported_ring_error(int result) {
  return result == -ENOSYS || result == -EPERM || result == -EACCES;
}

}  // namespace

int main() {
  bupp::base::probe empty_probe;
  assert(empty_probe.raw() == nullptr);
  assert(!empty_probe.is_open());
  assert(empty_probe.opcode_supported(IORING_OP_NOP) == 0);

  bupp::base::probe global_probe;
  static_cast<void>(global_probe.get_probe());
  if (global_probe.is_open()) {
    assert(global_probe.raw() != nullptr);
    static_cast<void>(global_probe.opcode_supported(IORING_OP_NOP));
  }
  global_probe.free_probe();
  assert(!global_probe.is_open());

  bupp::base::ring ring;
  const int init_result = ring.queue_init(8);
  if (init_result < 0) {
    assert(is_unsupported_ring_error(init_result));
    return 0;
  }

  bupp::base::probe ring_probe;
  static_cast<void>(ring_probe.get_probe_ring(ring));
  if (ring_probe.is_open()) {
    assert(ring_probe.raw() != nullptr);
    static_cast<void>(ring_probe.opcode_supported(IORING_OP_NOP));
  }

  bupp::base::probe moved_probe(std::move(ring_probe));
  assert(!ring_probe.is_open());
  moved_probe.free_probe();

  return 0;
}
