#include <bupp/base/linux/probe.h>

#include <array>
#include <iostream>

namespace {

struct opcode_row {
  const char* name;
  int opcode;
};

}  // namespace

int main() {
  bupp::base::probe probe;
  if (probe.get_probe() == nullptr) {
    std::cerr << "io_uring probe is not available\n";
    return 0;
  }

  constexpr std::array<opcode_row, 14> k_opcodes{{
      {"NOP", IORING_OP_NOP},
      {"TIMEOUT", IORING_OP_TIMEOUT},
      {"ACCEPT", IORING_OP_ACCEPT},
      {"CONNECT", IORING_OP_CONNECT},
      {"SEND", IORING_OP_SEND},
      {"RECV", IORING_OP_RECV},
      {"POLL_ADD", IORING_OP_POLL_ADD},
      {"READ", IORING_OP_READ},
      {"WRITE", IORING_OP_WRITE},
      {"OPENAT", IORING_OP_OPENAT},
      {"FSYNC", IORING_OP_FSYNC},
      {"STATX", IORING_OP_STATX},
      {"PROVIDE_BUFFERS", IORING_OP_PROVIDE_BUFFERS},
      {"REMOVE_BUFFERS", IORING_OP_REMOVE_BUFFERS},
  }};

  for (const opcode_row& row : k_opcodes) {
    std::cout << row.name
              << " supported: " << probe.opcode_supported(row.opcode) << '\n';
  }

  return 0;
}
