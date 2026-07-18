#pragma once

#include "io_context_test_support.h"

namespace {

[[nodiscard]] bool context_available(const bnio::io_context& context) {
  return context.is_open();
}

}  // namespace
