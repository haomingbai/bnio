# bnio vs asio Benchmark

This directory contains benchmark reports comparing bnio against standalone [Asio](https://think-async.com/Asio/)
on different platform backends.

## Reports

| Backend | Platform | Summary |
| --- | --- | --- |
| [io_uring (Linux)](io_uring.md) | Linux | bnio leads in throughput (1.05×); timer churn at parity (1.00×) |
| [kqueue (macOS/BSD)](kqueue.md) | macOS | bnio leads in throughput (1.03×); at parity in timer churn (1.02×) |

## Methodology

Each backend report covers two independent benchmarks:

- **Part A — TCP Echo Throughput:** a multi-dimensional throughput stress test on a TCP echo server under varying worker counts, connection counts, and message sizes.
- **Part B — Timer Churn:** a timer lifecycle stress test that creates, resets, cancels, and re-creates large numbers of steady timers in tight update rounds.

All benchmarks compare functionally equivalent implementations: a bnio server/program and a standalone Asio server/program, driven by the same neutral workload.

### Fairness controls

- Both implementations rebuilt in **Release** mode with `-march=native -mtune=native` immediately before testing.
- The **same neutral client** drives both servers (for throughput benchmarks).
- Server process **restarted** for every measured configuration and iteration.
- Each configuration runs **3 iterations**; reported values are arithmetic means.

## Charts

Charts for each backend are in:

- `charts/io_uring/` — Linux io_uring benchmark charts
- `charts/kqueue/` — macOS/BSD kqueue benchmark charts

Interactive ECharts for the io_uring report are in `charts/io_uring/report.html`.

## Running benchmarks

```bash
# Build (benchmarks require -DBNIO_BUILD_BENCHMARKS=ON)
cmake -S . -B build-bench -DCMAKE_BUILD_TYPE=Release \
  -DBNIO_BUILD_BENCHMARKS=ON

# Throughput benchmark
cmake --build build-bench --target bnio_throughput_benchmark \
  asio_throughput_benchmark throughput_benchmark_client
scripts/benchmark.sh --build-dir build-bench

# Timer churn benchmark
cmake --build build-bench --target bnio_timer_churn_benchmark \
  asio_timer_churn_benchmark
scripts/timer_benchmark.sh --build-dir build-bench
```
