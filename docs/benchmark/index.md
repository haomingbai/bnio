# bnio vs asio Benchmark

This directory contains benchmark reports comparing bnio against standalone [Asio](https://think-async.com/Asio/)
on different platform backends.

## Reports

| Backend | Platform | Summary |
| --- | --- | --- |
| [io_uring (Linux)](io_uring.md) | Linux x86_64 | bnio leads in throughput (1.07×) and timer churn (1.22×) |
| [kqueue (macOS/BSD)](kqueue.md) | macOS arm64 (Apple Silicon) | bnio leads in timer churn (1.06×); trails in throughput (0.77×) |

## Methodology

Each backend report covers two independent benchmarks:

- **Part A — TCP Echo Throughput:** A multi-dimensional throughput stress test on a TCP echo server under varying worker counts, connection counts, and message sizes.
- **Part B — Timer Churn:** A timer lifecycle stress test that creates, resets, cancels, and re-creates large numbers of steady timers in tight update rounds.

All benchmarks compare functionally equivalent implementations: a bnio server/program and a standalone Asio server/program, driven by the same neutral workload.

### Fairness Controls

- Both implementations rebuilt in **Release** mode with `-march=native -mtune=native` immediately before testing.
- The **same neutral client** drives both servers (for throughput benchmarks).
- Server process **restarted** for every measured configuration.
- Each configuration runs **3 iterations**; results are median-aggregated.

## Charts

Charts for each backend are in:

- `charts/io_uring/` — Linux io_uring benchmark charts
- `charts/kqueue/` — macOS/BSD kqueue benchmark charts

## Running Benchmarks

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