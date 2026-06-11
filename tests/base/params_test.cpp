#include <bupp/base/params.h>
#include <liburing.h>

#include <cassert>

int main() {
  bupp::base::params params;

  assert(params.raw() != nullptr);
  assert(params.sq_entries() == 0);
  assert(params.cq_entries() == 0);
  assert(params.flags() == 0);
  assert(params.sq_thread_cpu() == 0);
  assert(params.sq_thread_idle() == 0);
  assert(params.features() == 0);
  assert(params.wq_fd() == 0);

  params.set_sq_entries(8);
  params.set_cq_entries(16);
  params.set_flags(IORING_SETUP_CLAMP);
  params.set_sq_thread_cpu(1);
  params.set_sq_thread_idle(10);
  params.set_features(IORING_FEAT_NODROP);
  params.set_wq_fd(2);

  assert(params.sq_entries() == 8);
  assert(params.cq_entries() == 16);
  assert(params.flags() == IORING_SETUP_CLAMP);
  assert(params.sq_thread_cpu() == 1);
  assert(params.sq_thread_idle() == 10);
  assert(params.features() == IORING_FEAT_NODROP);
  assert(params.wq_fd() == 2);

  params.reset();
  assert(params.sq_entries() == 0);
  assert(params.cq_entries() == 0);
  assert(params.flags() == 0);
  assert(params.features() == 0);

  return 0;
}
