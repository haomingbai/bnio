# base examples

These examples are small, direct programs that demonstrate the `bupp::base`
thin wrapper around `liburing`. Each executable keeps the C API shape visible:
allocate an SQE, call `prep_*`, attach `user_data`, submit, wait for a CQE, and
mark the CQE as seen.

## Executables

- `bupp_base_probe` prints kernel opcode support for networking, timers, file
  I/O, polling, and provided buffers.
- `bupp_base_nop` submits the smallest possible operation and validates
  completion `user_data`.
- `bupp_base_timeout` demonstrates kernel timer completions.
- `bupp_base_file_io` writes, fsyncs, reads, and closes a temporary file through
  io_uring.
- `bupp_base_poll` waits for a pipe readiness event.
- `bupp_base_provided_buffers` receives data into a kernel-selected provided
  buffer.
- `bupp_base_echo_server` runs a small echo server event loop built from
  accept, recv, and send completions.

## Build

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build
```

## Run

```sh
./build/examples/base/linux/bupp_base_probe
./build/examples/base/linux/bupp_base_nop
./build/examples/base/linux/bupp_base_timeout
./build/examples/base/linux/bupp_base_file_io
./build/examples/base/linux/bupp_base_poll
./build/examples/base/linux/bupp_base_provided_buffers
```

Run the echo server in one terminal:

```sh
./build/examples/base/linux/bupp_base_echo_server
```

Then connect from another terminal:

```sh
printf 'hello io_uring\n' | nc 127.0.0.1 7000
```
