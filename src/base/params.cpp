#include <bupp/base/params.h>

namespace bupp::base {

params::params() noexcept = default;

io_uring_params* params::raw() noexcept { return &params_; }

const io_uring_params* params::raw() const noexcept { return &params_; }

void params::reset() noexcept { params_ = {}; }

std::uint32_t params::sq_entries() const noexcept { return params_.sq_entries; }

void params::set_sq_entries(std::uint32_t sq_entries) noexcept {
  params_.sq_entries = sq_entries;
}

std::uint32_t params::cq_entries() const noexcept { return params_.cq_entries; }

void params::set_cq_entries(std::uint32_t cq_entries) noexcept {
  params_.cq_entries = cq_entries;
}

std::uint32_t params::flags() const noexcept { return params_.flags; }

void params::set_flags(std::uint32_t flags) noexcept { params_.flags = flags; }

std::uint32_t params::sq_thread_cpu() const noexcept {
  return params_.sq_thread_cpu;
}

void params::set_sq_thread_cpu(std::uint32_t sq_thread_cpu) noexcept {
  params_.sq_thread_cpu = sq_thread_cpu;
}

std::uint32_t params::sq_thread_idle() const noexcept {
  return params_.sq_thread_idle;
}

void params::set_sq_thread_idle(std::uint32_t sq_thread_idle) noexcept {
  params_.sq_thread_idle = sq_thread_idle;
}

std::uint32_t params::features() const noexcept { return params_.features; }

void params::set_features(std::uint32_t features) noexcept {
  params_.features = features;
}

std::uint32_t params::wq_fd() const noexcept { return params_.wq_fd; }

void params::set_wq_fd(std::uint32_t wq_fd) noexcept { params_.wq_fd = wq_fd; }

}  // namespace bupp::base
