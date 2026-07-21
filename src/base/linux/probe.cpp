/**
 * @file probe.cpp
 * @brief io_uring_probe wrapper implementation.
 */

#include <bnio/base/linux/probe.h>
#include <liburing.h>

#include <utility>

#include "bnio/base/linux/ring.h"
namespace bnio::base {

probe::probe() noexcept = default;

probe::~probe() noexcept { free_probe(); }

probe::probe(probe&& other) noexcept
    : probe_(std::exchange(other.probe_, nullptr)) {}

probe& probe::operator=(probe&& other) noexcept {
  if (this != &other) {
    free_probe();
    probe_ = std::exchange(other.probe_, nullptr);
  }
  return *this;
}

io_uring_probe* probe::get_probe() noexcept {
  free_probe();
  probe_ = io_uring_get_probe();
  return probe_;
}

io_uring_probe* probe::get_probe_ring(ring& source) noexcept {
  free_probe();
  probe_ = io_uring_get_probe_ring(source.raw());
  return probe_;
}

void probe::free_probe() noexcept {
  if (probe_ != nullptr) {
    io_uring_free_probe(probe_);
    probe_ = nullptr;
  }
}

int probe::opcode_supported(int op) const noexcept {
  if (probe_ == nullptr) {
    return 0;
  }
  return io_uring_opcode_supported(probe_, op);
}

io_uring_probe* probe::raw() noexcept { return probe_; }

const io_uring_probe* probe::raw() const noexcept { return probe_; }

bool probe::is_open() const noexcept { return probe_ != nullptr; }

}  // namespace bnio::base
