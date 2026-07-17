# bupp vs asio TCP Echo Benchmark - Full Report

## 1. Test Environment

| Item | Value |
| --- | --- |
| Date | 2026-07-17T00:25:46.000654+00:00 |
| Topology | Single-host loopback TCP (127.0.0.1) |
| OS | Linux 7.1.3-200.fc44.x86_64 |
| Kernel | 7.1.3-200.fc44.x86_64 |
| Architecture | x86_64 |
| CPU | 13th Gen Intel(R) Core(TM) i9-13900H |
| Logical CPUs | 20 |
| Memory | 32564648 kB |
| Compiler | gcc (GCC) 16.1.1 20260515 (Red Hat 16.1.1-2) |
| CMake | cmake version 4.3.0 |
| Python | 3.14.6 (main, Jun 11 2026, 00:00:00) [GCC 16.1.1 20260515 (Red Hat 16.1.1-2)] |
| liburing | 2.13 |
| OpenSSL | 3.5.7 |
| Asio | 1.30.2 |

Hostnames, usernames, absolute paths, and network addresses are intentionally omitted.

## 2. Methodology

The benchmark compares two functionally equivalent TCP echo servers:

- **`bupp_raw_echo`**: C++20 coroutine echo server using bupp on io_uring (Linux).
- **`asio_raw_echo`**: C++20 coroutine echo server using standalone Asio (epoll reactor).
- **Client**: the C++ `raw_echo_client` TCP echo load generator. The same binary drives both servers.

Each connection runs a strict ping-pong loop: send one fixed-size payload, read the echoed payload, then repeat. Throughput is reported as completed echo requests per second; MB/s counts the echoed payload size once per completed request.

### Fairness Controls

- Both servers rebuilt in **Release** mode with `-O3 -DNDEBUG -march=native -mtune=native` immediately before testing.
- The **same C++ `raw_echo_client`** drives both servers.
- Server process **restarted** for every measured configuration and iteration.
- Each configuration runs **3 iterations**; results are median-aggregated.
- Warmup phase (5 s) excluded from measurement (15 s).

> **Metric-availability note:** the C++ `raw_echo_client` reports total echoes, req/s, and MB/s. It does **not** collect latency percentiles or per-connection error counts, so this Linux section reports throughput and payload bandwidth only.

## 3. Configuration Matrix

| Dimension | Values |
| --- | --- |
| Server | bupp, asio |
| Worker threads | 1, 2, 4, 8 |
| Concurrent connections | 64, 256, 1024 |
| Message size | 64 B, 1 KB, 4 KB, 64 KB |
| Warmup per run | 5 s |
| Measurement per run | 15 s |
| Iterations per config | 3 |

The matrix produced **96** result rows.

## 4. Stability Summary

The C++ client completed every measured configuration and produced a median result for all 96 rows.

Overall average throughput ratio (bupp / asio): **1.05×** across all 96 configurations. bupp is ahead in **39 / 48** paired configurations.

## 5. Results

### 5.1 Throughput Overview

![Throughput Overview](benchmark_charts/overview_bars.png)

**Reference point: workers=4, connections=256**

| Message Size | bupp req/s | bupp MB/s | asio req/s | asio MB/s | Ratio |
| --- | ---: | ---: | ---: | ---: | ---: |
| 64 B | 731,057 | 44.6 | 621,176 | 37.9 | 1.18× |
| 1 KB | 696,492 | 680.2 | 608,353 | 594.1 | 1.14× |
| 4 KB | 629,505 | 2,459.0 | 567,564 | 2,217.0 | 1.11× |
| 64 KB | 6,224 | 389.0 | 6,201 | 387.6 | 1.00× |

### 5.2 Throughput vs Connections

![Throughput vs Connections](benchmark_charts/throughput_vs_connections.png)

*Workers=4, faceted by message size.*

### 5.3 Throughput vs Worker Threads

![Throughput vs Workers](benchmark_charts/throughput_vs_workers.png)

*Connections=256, faceted by message size.*

| Workers | bupp req/s | asio req/s | Ratio |
| ---: | ---: | ---: | ---: |
| 1 | 261,105 | 261,968 | 1.00× |
| 2 | 456,545 | 456,172 | 1.00× |
| 4 | 629,505 | 567,564 | 1.11× |
| 8 | 697,976 | 660,973 | 1.06× |

**Worker-scaling at 4 KB / 256 connections.** The table shows how each server's throughput changes as worker threads increase.

### 5.4 Connection Scaling

| Connections | bupp req/s | asio req/s | Ratio |
| ---: | ---: | ---: | ---: |
| 64 | 616,103 | 503,973 | 1.22× |
| 256 | 629,505 | 567,564 | 1.11× |
| 1024 | 570,707 | 560,230 | 1.02× |

**Connection-scaling at 4 KB / workers=4.** Shows how each server handles increasing concurrency.

### 5.5 bupp / asio Throughput Ratio Heatmap

![Heatmap](benchmark_charts/heatmap_ratio.png)

*Workers=4. Positive values (blue) = bupp faster; negative (red) = asio faster.*

### 5.6 Worker-Scaling Ratio Heatmap

![Worker Scaling](benchmark_charts/worker_scaling_heatmap.png)

*Connections=256. Shows how the bupp/asio ratio changes as worker threads increase.*

### 5.7 Throughput & Payload Bandwidth Scatter

![Scatter](benchmark_charts/throughput_latency_scatter.png)

*Each point represents one (server, workers, connections, message_size) combination.*

### 5.8 Extreme Cases

**Best bupp / asio throughput ratio:**

- Configuration: workers=4, connections=64, message_size=64 B
- bupp: 718,766 req/s, 43.9 MB/s
- asio: 554,198 req/s, 33.8 MB/s
- Ratio: 1.30×

**Most challenging bupp / asio throughput ratio:**

- Configuration: workers=1, connections=1024, message_size=4 KB
- bupp: 207,666 req/s, 811.2 MB/s
- asio: 236,100 req/s, 922.3 MB/s
- Ratio: 0.88×

## 6. Interpretation

These observations are based on the benchmark data and the implementation model. They have not been independently validated with profiling in this run.

### 6.1 Full Matrix Analysis

This full matrix covered 96 configurations with 3 iterations each.

1. **Linux now shows an average lead for bupp.** The previous submitted Linux benchmark reported an average bupp/asio ratio of 0.981×; this run reports 1.05×. That moves the headline result from slightly behind parity to ahead overall.
2. **The strongest plausible implementation change is io_uring setup and submission ownership.** Since the previous Linux benchmark, raw echo stopped overriding the Linux setup flags and now inherits the backend defaults. The current defaults enable `IORING_SETUP_SINGLE_ISSUER` and conditionally use `IORING_SETUP_R_DISABLED`, so the run-loop thread becomes the issuer before submissions begin. That reduces submission-side synchronization in the kernel on supported systems.
3. **The task-queue hot path is shorter.** The io_uring backend no longer reverses popped I/O task lists before preparing SQEs. That removes linked-list work from the echo hot path and can improve cache locality, especially in throughput-oriented loopback tests.
4. **CQE-first dispatch fits this workload.** The current run loop drains ready completions before local task execution. A ping-pong echo server is completion-driven, so reducing completion backlog is consistent with the higher request rate in several small and mid-sized payload configurations.
5. **Large-payload cases are still bandwidth-shaped.** At 64 KB the ratios are closer to parity than the best small/mid-sized cases because loopback payload movement dominates per-operation scheduling overhead.

## 7. Full Results (workers=4)

#### Message size = 64 B

| Server | Workers | Conns | req/s | MB/s |
| --- | ---: | ---: | ---: | ---: |
| asio | 4 | 64 | 554,198 | 33.8 |
| asio | 4 | 256 | 621,176 | 37.9 |
| asio | 4 | 1024 | 690,657 | 42.2 |
| bupp | 4 | 64 | 718,766 | 43.9 |
| bupp | 4 | 256 | 731,057 | 44.6 |
| bupp | 4 | 1024 | 730,175 | 44.6 |

#### Message size = 1 KB

| Server | Workers | Conns | req/s | MB/s |
| --- | ---: | ---: | ---: | ---: |
| asio | 4 | 64 | 549,884 | 537.0 |
| asio | 4 | 256 | 608,353 | 594.1 |
| asio | 4 | 1024 | 670,902 | 655.2 |
| bupp | 4 | 64 | 691,341 | 675.1 |
| bupp | 4 | 256 | 696,492 | 680.2 |
| bupp | 4 | 1024 | 688,698 | 672.6 |

#### Message size = 4 KB

| Server | Workers | Conns | req/s | MB/s |
| --- | ---: | ---: | ---: | ---: |
| asio | 4 | 64 | 503,973 | 1,968.6 |
| asio | 4 | 256 | 567,564 | 2,217.0 |
| asio | 4 | 1024 | 560,230 | 2,188.4 |
| bupp | 4 | 64 | 616,103 | 2,406.7 |
| bupp | 4 | 256 | 629,505 | 2,459.0 |
| bupp | 4 | 1024 | 570,707 | 2,229.3 |

#### Message size = 64 KB

| Server | Workers | Conns | req/s | MB/s |
| --- | ---: | ---: | ---: | ---: |
| asio | 4 | 64 | 1,553 | 97.1 |
| asio | 4 | 256 | 6,201 | 387.6 |
| asio | 4 | 1024 | 24,623 | 1,538.9 |
| bupp | 4 | 64 | 1,553 | 97.1 |
| bupp | 4 | 256 | 6,224 | 389.0 |
| bupp | 4 | 1024 | 24,677 | 1,542.3 |

---


## 8. macOS / kqueue Results (Supplementary)

> **Supplementary platform.** macOS (and BSD) use the kqueue backend, which is a
> secondary, community-supported target — the primary backend is io_uring on Linux
> (Sections 1–7). This section is provided as a best-effort bonus so kqueue users can
> see how bupp compares to Asio on their platform. It follows the same methodology and
> includes charts, but is not part of the main supported matrix.

This section reports the same TCP echo benchmark on **macOS**, where both participants use the **kqueue** event backend: bupp's `kqueue_context` and standalone Asio's kqueue reactor. Unlike the Linux run (bupp on io_uring vs asio on epoll — two different kernel mechanisms), on macOS both sides ride the same kqueue subsystem, so the comparison isolates **user-space dispatch and framework overhead** rather than kernel-mechanism differences.

### 8.1 Test Environment

| Item | Value |
| --- | --- |
| Date | 2026-07-15 |
| Topology | Single-host loopback TCP (127.0.0.1) |
| OS | macOS 26.5.2 |
| Kernel | Darwin 25.5.0 (arm64) |
| Architecture | arm64 (Apple Silicon) |
| CPU | Apple M-series (18 logical CPUs) |
| Logical CPUs | 18 |
| Memory | system |
| Compiler | Apple clang 21.0.0 |
| CMake | 4.3.4 |
| Python | 3.9.6 |
| Backend | kqueue (bupp `kqueue_context` and Asio kqueue reactor) |
| OpenSSL | 3.6.3 |
| Asio | 1.30.2 |

Hostnames, usernames, absolute paths, and network addresses are intentionally omitted.

### 8.2 Methodology

The benchmark compares two functionally equivalent TCP echo servers, identical in source to the Linux run:

- **`bupp_raw_echo`**: C++20 coroutine echo server using bupp on kqueue (macOS).
- **`asio_raw_echo`**: C++20 coroutine echo server using standalone Asio (kqueue reactor).
- **Client**: the neutral C++ `raw_echo_client` TCP echo load generator (the same binary drives both servers; it is neither bupp nor asio).

Each connection runs a strict ping-pong loop: send one fixed-size payload, read the echoed payload, then repeat. Throughput is reported as completed echo requests per second; MB/s counts the echoed payload size once per completed request.

### Fairness Controls

- Both servers rebuilt in **Release** mode with `-march=native` immediately before testing.
- The **same C++ `raw_echo_client`** drives both servers.
- Server process **restarted** for every measured configuration.
- Each configuration runs **3 iterations**; results are median-aggregated.
- Warmup phase (5 s) excluded from measurement (15 s).

> **Metric-availability note:** the C++ `raw_echo_client` reports only total echoes, req/s, and MB/s. It does **not** collect latency percentiles (p50/p99/p999), so those columns from the Linux section are **omitted here** (marked N/A where a comparison would otherwise expect them). All figures below are clean throughput measurements.

### 8.3 Configuration Matrix

| Dimension | Values |
| --- | --- |
| Server | bupp, asio |
| Worker threads | 1, 2, 4, 8 |
| Concurrent connections | 64, 256, 1024 |
| Message size | 64 B, 1 KB, 4 KB, 64 KB |
| Warmup per run | 5 s |
| Measurement per run | 15 s |
| Iterations per config | 3 |

The matrix produced **96** result rows (2 servers × 4 workers × 3 connections × 4 message sizes).

### 8.4 Stability Summary

Both servers completed every configuration with zero client errors. All throughput figures represent clean, reliable measurements.

Overall average throughput ratio (bupp / asio): **0.934×** across all 96 configurations.

### 8.5 Results

### 8.5.1 Throughput Overview

![kqueue Throughput Overview](benchmark_charts/kqueue_overview_bars.png)

**Reference point: workers=4, connections=256**

| Message Size | bupp req/s | asio req/s | Ratio | bupp MB/s | asio MB/s |
| --- | ---: | ---: | ---: | ---: | ---: |
| 64 B | 102,512 | 103,874 | 0.99× | 6 | 6 |
| 1 KB | 102,404 | 102,932 | 0.99× | 100 | 100 |
| 4 KB | 99,455 | 100,969 | 0.99× | 388 | 394 |
| 64 KB | 14,721 | 13,439 | 1.10× | 920 | 839 |

### 8.5.2 Throughput vs Connections

![kqueue Throughput vs Connections](benchmark_charts/kqueue_throughput_vs_connections.png)

*Workers=4, faceted by message size.*

| Connections | bupp req/s | asio req/s | Ratio |
| ---: | ---: | ---: | ---: |
| 64 | 90,011 | 94,605 | 0.95× |
| 256 | 99,455 | 100,969 | 0.99× |
| 1024 | 94,438 | 96,082 | 0.98× |

**Connection-scaling at 4 KB / workers=4.**

### 8.5.3 Throughput vs Worker Threads

![kqueue Throughput vs Workers](benchmark_charts/kqueue_throughput_vs_workers.png)

*Connections=256, faceted by message size.*

| Workers | bupp req/s | asio req/s | Ratio |
| ---: | ---: | ---: | ---: |
| 1 | 81,753 | 101,290 | 0.81× |
| 2 | 104,836 | 122,100 | 0.86× |
| 4 | 99,455 | 100,969 | 0.99× |
| 8 | 94,412 | 92,910 | 1.02× |

**Worker-scaling at 4 KB / 256 connections.**

### 8.5.4 bupp / asio Throughput Ratio Heatmap

![kqueue Heatmap](benchmark_charts/kqueue_heatmap_ratio.png)

*Workers=4. Values > 1.0 (blue) = bupp faster; < 1.0 (red) = asio faster.*

### 8.5.5 Worker-Scaling Ratio Heatmap

![kqueue Worker Scaling](benchmark_charts/kqueue_worker_scaling_heatmap.png)

*Connections=256. Shows how the bupp/asio ratio changes as worker threads increase.*

### 8.6 Extreme Cases

**Best bupp / asio throughput ratio (zero-error):**

- Configuration: workers=4, connections=64, message_size=64 KB
- bupp: 15,702 req/s, 981 MB/s
- asio: 14,148 req/s, 884 MB/s
- Ratio: 1.11×

**Most challenging bupp / asio throughput ratio (zero-error):**

- Configuration: workers=1, connections=1024, message_size=1 KB
- bupp: 70,714 req/s, 69 MB/s
- asio: 97,342 req/s, 95 MB/s
- Ratio: 0.73×

### 8.7 Interpretation

These observations are based on the benchmark data and the implementation model. They have not been independently validated with profiling in this run.

1. **asio leads on average on kqueue.** The average bupp/asio ratio is 0.934× — i.e. asio's mature kqueue reactor outperforms bupp's kqueue backend in the vast majority of configurations (bupp wins only at 64 KB, where per-message syscall cost is amortized). This is the inverse of the Linux result, where bupp (io_uring) often matched or beat asio (epoll). The difference is explained by the backend: on macOS both use kqueue, so bupp's newer `kqueue_context` is compared directly against Asio's long-optimized kqueue reactor.
2. **Best case for bupp (globally): workers=4, conns=64, 64 KB → ratio 1.11×.** bupp's single winning configuration only narrowly beats asio.
3. **Most challenging for bupp (globally): workers=1, conns=1024, 1 KB → ratio 0.73×.** Small messages at high concurrency show the widest gap, consistent with kevent syscall overhead dominating per-operation cost.
4. **Large messages converge.** At 64 KB both servers are bandwidth-limited on loopback and the ratio is near 1.0×, because per-message syscall cost is amortized over a large transfer.
5. **Worker scaling is flat-to-degrading on loopback.** Adding worker threads beyond 1 does not help and sometimes hurts, as expected for single-host loopback where there is no NIC to parallelize and scheduler/lock contention grows.

### 8.8 Full Results (workers=4)

#### Message size = 64 B

| Server | Workers | Conns | req/s | MB/s |
| --- | ---: | ---: | ---: | ---: |
| asio | 4 | 64 | 172,917 | 10 |
| asio | 4 | 256 | 103,874 | 6 |
| asio | 4 | 1024 | 99,162 | 6 |
| bupp | 4 | 64 | 158,487 | 9 |
| bupp | 4 | 256 | 102,512 | 6 |
| bupp | 4 | 1024 | 101,694 | 6 |

#### Message size = 1 KB

| Server | Workers | Conns | req/s | MB/s |
| --- | ---: | ---: | ---: | ---: |
| asio | 4 | 64 | 96,019 | 93 |
| asio | 4 | 256 | 102,932 | 100 |
| asio | 4 | 1024 | 96,538 | 94 |
| bupp | 4 | 64 | 105,049 | 102 |
| bupp | 4 | 256 | 102,404 | 100 |
| bupp | 4 | 1024 | 100,245 | 97 |

#### Message size = 4 KB

| Server | Workers | Conns | req/s | MB/s |
| --- | ---: | ---: | ---: | ---: |
| asio | 4 | 64 | 94,605 | 369 |
| asio | 4 | 256 | 100,969 | 394 |
| asio | 4 | 1024 | 96,082 | 375 |
| bupp | 4 | 64 | 90,011 | 351 |
| bupp | 4 | 256 | 99,455 | 388 |
| bupp | 4 | 1024 | 94,438 | 368 |

#### Message size = 64 KB

| Server | Workers | Conns | req/s | MB/s |
| --- | ---: | ---: | ---: | ---: |
| asio | 4 | 64 | 14,148 | 884 |
| asio | 4 | 256 | 13,439 | 839 |
| asio | 4 | 1024 | 12,807 | 800 |
| bupp | 4 | 64 | 15,702 | 981 |
| bupp | 4 | 256 | 14,721 | 920 |
| bupp | 4 | 1024 | 13,603 | 850 |

---

*Charts in this section were generated from the macOS/kqueue benchmark run; see `docs/benchmark_charts/`.*

### 8.9 Dual-Platform Overview (Linux / io_uring vs macOS / kqueue)

The two platform sections measure the same workload with the same server/client
sources, but on different event backends:

| Aspect | Linux (Section 1-7) | macOS (Section 8) |
| --- | --- | --- |
| bupp backend | io_uring (submission/completion rings) | kqueue (`kqueue_context`) |
| Asio backend | epoll reactor | kqueue reactor |
| Kernel mechanisms compared | **two different** (io_uring vs epoll) | **the same** (kqueue vs kqueue) |
| What the gap isolates | framework + kernel-mechanism overhead | **user-space framework overhead only** |
| Avg bupp/asio throughput ratio | 1.05× (bupp faster overall) | 0.934× (asio faster across the board) |
| Latency percentiles | not collected by the C++ client | **not collected** (client limitation) |

**Takeaway:** on Linux, bupp's io_uring backend now beats Asio's epoll
reactor on average in this C++-client throughput run. On macOS, where both ride kqueue, the comparison is purely about
user-space dispatch — and Asio's long-optimized kqueue reactor currently holds a
small but consistent edge (~7% on average). The 64 KB case is the lone bright spot
for bupp on macOS (occasionally >1.0×), where per-message syscall cost is amortized.

---

*Report generated from 192 benchmark result rows (96 Linux/io_uring + 96 macOS/kqueue). Charts are available in the repository under `docs/benchmark_charts/`.*
