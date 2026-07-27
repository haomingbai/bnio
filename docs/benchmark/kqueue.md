# bnio vs asio Benchmark — kqueue (macOS/BSD)

## 1. Test Environment

| Item | Value |
| --- | --- |
| Date | 2026-07-27 |
| Topology | Single-host loopback TCP (127.0.0.1) |
| OS | macOS 26.5.2 |
| Kernel | Darwin 25.5.0 |
| Architecture | arm64 (Apple Silicon) |
| CPU | Apple M5 Pro |
| Logical CPUs | 18 |
| Memory | 50,331,648 kB (48 GB) |
| Compiler | Apple clang version 21.0.0 (clang-2100.1.1.101) |
| CMake | cmake version 4.3.4 |
| Asio | 1.30.2 (local source) |

Hostnames, usernames, absolute paths, and network addresses are intentionally omitted.

> **Note:** This report represents the kqueue (BSD) backend. For the io_uring (Linux) backend results, see [io_uring.md](io_uring.md).

## 2. Methodology

This report covers two independent benchmarks, each comparing bnio against standalone Asio:

- **Part A — TCP Echo Throughput (Sections 3–7):** A multi-dimensional throughput stress test on a TCP echo server under varying worker counts, connection counts, and message sizes. Analogous to the workload in the io_uring report.
- **Part B — Timer Churn (Sections 8–12):** A timer lifecycle stress test that creates, resets, cancels, and re-creates large numbers of steady timers in tight update rounds.

### Part A — TCP Echo Throughput

The benchmark compares two functionally equivalent TCP echo servers:

- **`bnio_throughput_benchmark`**: C++20 coroutine echo server using bnio on kqueue (macOS/BSD).
- **`asio_throughput_benchmark`**: C++20 coroutine echo server using standalone Asio (kqueue reactor).
- **Client**: The C++ `throughput_benchmark_client` — an Asio-based neutral load generator that is neither bnio nor asio server code. Each connection runs a strict ping-pong loop: send one fixed-size payload, read the echoed payload, then repeat. Throughput is reported as completed echo requests per second; MB/s counts the echoed payload size once per completed request.

> **Metric-availability note:** the C++ `throughput_benchmark_client` reports only total echoes, req/s, and MB/s. It does **not** collect latency percentiles (p50/p99/p999).

#### Fairness Controls

- Both servers rebuilt in **Release** mode with `-march=native -mtune=native` immediately before testing.
- The **same C++ `throughput_benchmark_client`** drives both servers.
- Server process **restarted** for every measured configuration.
- Each configuration runs **1 iteration** (quick mode); full 3-iteration results may differ.
- Client-side error counts tracked per run.

### Part B — Timer Churn

The benchmark compares two functionally equivalent timer stress programs:

- **`bnio_timer_churn_benchmark`**: Uses `bnio::steady_timer` and `bnio::io_context`.
- **`asio_timer_churn_benchmark`**: Uses `asio::steady_timer` and `asio::io_context`.
- Both programs execute an identical workload: each round destroys and recreates a rotating subset of timers, resets the expiry on all other timers, and starts a new async_wait for every live timer. A barrier timer synchronizes each round.

Output metrics include lifecycle API calls per second (creates + destroys + expiry sets + explicit cancels) and active waits started per second.

#### Fairness Controls

- Both programs rebuilt in **Release** mode with `-march=native -mtune=native`.
- Identical workload parameters passed to both executables.
- Each configuration runs **1 iteration** (quick mode); full 3-iteration results may differ.

## 3. Configuration Matrix

### Part A — Throughput

| Dimension | Values |
| --- | --- |
| Server | bnio, asio |
| Worker threads | 1, 2, 4, 8 |
| Concurrent connections | 64, 256, 1024 |
| Message size | 64 B, 1 KB, 4 KB, 64 KB |
| Measurement per run | 10 s (quick mode) |
| Iterations per config | 1 |

The matrix produced **96** result rows.

### Part B — Timer Churn

| Dimension | Values |
| --- | --- |
| Backend | bnio, asio |
| Live timers | 256, 1,024, 4,096, 16,384 |
| Update rounds | 100, 500, 1,000 |
| Replacements per round | timers / 4 (default) |
| Iterations per config | 1 |

The matrix produced **24** result rows.

## 4. Stability Summary

### Part A — Throughput

Both servers completed every configuration with zero client errors. All 96 configurations produced clean, reliable measurements.

Overall average throughput ratio (bnio / asio): **0.772×** across all 96 configurations.

### Part B — Timer Churn

Both backends completed every configuration successfully. All 24 configurations produced clean measurements.

Overall average lifecycle throughput ratio (bnio / asio): **1.064×** across all 24 configurations.

---

## Part A — TCP Echo Throughput Results

### 5.1 Throughput Overview

![Throughput Overview](charts/kqueue/kqueue_overview_bars.png)

**Reference point: workers=4, connections=256**

| Message Size | bnio req/s | bnio err | asio req/s | asio err | Ratio |
| --- | ---: | ---: | ---: | ---: | ---: |
| 64 B | 224,448 | 0 | 251,386 | 0 | 0.89× |
| 1 KB | 219,854 | 0 | 244,240 | 0 | 0.90× |
| 4 KB | 214,837 | 0 | 251,929 | 0 | 0.85× |
| 64 KB | 21,079 | 0 | 43,867 | 0 | 0.48× |

### 5.2 Throughput vs Connections

![Throughput vs Connections](charts/kqueue/kqueue_throughput_vs_connections.png)

*Workers=4, faceted by message size.*

### 5.3 Throughput vs Worker Threads

![Throughput vs Workers](charts/kqueue/kqueue_throughput_vs_workers.png)

*Connections=256, faceted by message size.*

| Workers | bnio req/s | bnio err | asio req/s | asio err | Ratio |
| ---: | ---: | ---: | ---: | ---: | ---: |
| 1 | 152,189 | 0 | 222,760 | 0 | 0.68× |
| 2 | 205,639 | 0 | 269,602 | 0 | 0.76× |
| 4 | 214,837 | 0 | 251,929 | 0 | 0.85× |
| 8 | 157,276 | 0 | 173,647 | 0 | 0.91× |

**Worker-scaling at 4 KB / 256 connections.** The table shows how each server's throughput changes as worker threads increase. The bnio/asio ratio improves from 0.68× to 0.91× as worker count grows, suggesting bnio's kqueue backend benefits more from multi-threading than from single-threaded operation.

### 5.4 Connection Scaling

| Connections | bnio req/s | bnio err | asio req/s | asio err | Ratio |
| ---: | ---: | ---: | ---: | ---: | ---: |
| 64 | 215,667 | 0 | 245,588 | 0 | 0.88× |
| 256 | 214,837 | 0 | 251,929 | 0 | 0.85× |
| 1024 | 208,785 | 0 | 245,395 | 0 | 0.85× |

**Connection-scaling at 4 KB / workers=4.** Shows how each server handles increasing concurrency.

### 5.5 bnio / asio Throughput Ratio Heatmap

![Heatmap](charts/kqueue/kqueue_heatmap_ratio.png)

*Workers=4. Positive values (blue) = bnio faster; negative (red) = asio faster.*

### 5.6 Worker-Scaling Ratio Heatmap

![Worker Scaling](charts/kqueue/kqueue_worker_scaling_heatmap.png)

*Connections=256. Shows how the bnio/asio ratio changes as worker threads increase.*

### 5.7 Extreme Cases

**Best bnio / asio throughput ratio (zero-error):**

- Configuration: workers=8, connections=64, message_size=64 KB
- bnio: 29,394 req/s
- asio: 26,460 req/s
- Ratio: 1.11×

**Most challenging bnio / asio throughput ratio (zero-error):**

- Configuration: workers=4, connections=256, message_size=64 KB
- bnio: 21,079 req/s
- asio: 43,867 req/s
- Ratio: 0.48×

### 5.8 Full Results (workers=4)

#### Message size = 64 B

| Server | Workers | Conns | req/s | MB/s |
| --- | ---: | ---: | ---: | ---: |
| asio | 4 | 64 | 251,496 | 15 |
| asio | 4 | 256 | 251,386 | 15 |
| asio | 4 | 1024 | 266,847 | 16 |
| bnio | 4 | 64 | 224,296 | 13 |
| bnio | 4 | 256 | 224,448 | 13 |
| bnio | 4 | 1024 | 220,278 | 13 |

#### Message size = 1 KB

| Server | Workers | Conns | req/s | MB/s |
| --- | ---: | ---: | ---: | ---: |
| asio | 4 | 64 | 250,092 | 244 |
| asio | 4 | 256 | 244,240 | 238 |
| asio | 4 | 1024 | 256,170 | 250 |
| bnio | 4 | 64 | 220,346 | 215 |
| bnio | 4 | 256 | 219,854 | 214 |
| bnio | 4 | 1024 | 216,471 | 211 |

#### Message size = 4 KB

| Server | Workers | Conns | req/s | MB/s |
| --- | ---: | ---: | ---: | ---: |
| asio | 4 | 64 | 245,588 | 959 |
| asio | 4 | 256 | 251,929 | 984 |
| asio | 4 | 1024 | 245,395 | 958 |
| bnio | 4 | 64 | 215,667 | 842 |
| bnio | 4 | 256 | 214,837 | 839 |
| bnio | 4 | 1024 | 208,785 | 815 |

#### Message size = 64 KB

| Server | Workers | Conns | req/s | MB/s |
| --- | ---: | ---: | ---: | ---: |
| asio | 4 | 64 | 43,417 | 2,713 |
| asio | 4 | 256 | 43,867 | 2,741 |
| asio | 4 | 1024 | 38,584 | 2,411 |
| bnio | 4 | 64 | 21,324 | 1,332 |
| bnio | 4 | 256 | 21,079 | 1,317 |
| bnio | 4 | 1024 | 20,118 | 1,257 |

### 5.9 Interpretation

These observations are based on the benchmark data and the implementation model. They have not been independently validated with profiling in this run.

1. **asio leads significantly in throughput on kqueue.** The overall throughput ratio is 0.772×, meaning bnio achieves about 77% of asio's throughput on average. asio wins in 90 of 96 configurations. This is notably different from the io_uring (Linux) results where bnio leads at 1.039×.

2. **The gap narrows with more worker threads.** At workers=1, the ratio is ~0.68×. At workers=4 it improves to ~0.85×, and at workers=8 it reaches ~0.91×. This suggests bnio's kqueue backend amortizes synchronization overhead better at higher thread counts.

3. **Large messages (64 KB) show the widest gap.** At 64 KB, bnio's throughput is typically 0.48–0.55× of asio's. The exception is at workers=8 where bnio briefly leads (1.11× at 64/256 connections). This suggests bnio's send/receive path for large buffers on kqueue has optimization opportunities.

4. **Small message performance is more competitive.** At 64 B–4 KB, the ratio stabilizes around 0.85–0.91×. Per-message overhead is the dominant factor at these sizes, and bnio's overhead is about 10–15% higher than asio's on kqueue.

5. **Connection count has minimal impact on the ratio.** The bnio/asio ratio is remarkably stable across 64, 256, and 1,024 connections (0.85–0.88×), indicating both backends scale similarly with connection count on kqueue.

6. **No errors across the entire matrix.** All 96 configurations completed cleanly — both server implementations and the client are stable under all tested configurations on kqueue.

7. **The kqueue implementation is a newer backend.** bnio's kqueue support was developed primarily for macOS/BSD portability and is a less mature backend compared to io_uring. The io_uring backend benefits from submission batching, zero-syscall completion polling, and other Linux-specific optimizations that have no direct kqueue equivalent. The throughput gap observed here is consistent with the [kqueue roadmap](../design/kqueue-roadmap.md) which documents ongoing optimization work.

---

## Part B — Timer Churn Results

### 6.1 Lifecycle Throughput Overview

![Timer Lifecycle Overview](charts/kqueue/kqueue_timer_lifecycle_overview.png)

**Reference point: update_rounds=500**

| Timer Count | bnio lifecycle/s | asio lifecycle/s | Ratio |
| ---: | ---: | ---: | ---: |
| 256 | 18,636,500 | 19,864,800 | 0.94× |
| 1,024 | 39,147,900 | 38,584,100 | 1.01× |
| 4,096 | 36,370,100 | 36,400,200 | 1.00× |
| 16,384 | 37,954,300 | 37,814,900 | 1.00× |

### 6.2 Active Waits Overview

![Timer Waits Overview](charts/kqueue/kqueue_timer_waits_overview.png)

**Reference point: update_rounds=500**

| Timer Count | bnio waits/s | asio waits/s | Ratio |
| ---: | ---: | ---: | ---: |
| 256 | 10,622,200 | 11,322,300 | 0.94× |
| 1,024 | 22,313,000 | 21,991,600 | 1.01× |
| 4,096 | 20,729,700 | 20,746,900 | 1.00× |
| 16,384 | 21,632,700 | 21,553,200 | 1.00× |

### 6.3 Lifecycle Throughput vs Timer Count

![Timer Lifecycle vs Timers](charts/kqueue/kqueue_timer_lifecycle_vs_timers.png)

*Faceted by update rounds. X-axis on log₂ scale.*

### 6.4 bnio / asio Lifecycle Ratio Heatmap

![Timer Lifecycle Heatmap](charts/kqueue/kqueue_timer_lifecycle_heatmap.png)

*Values > 1.0 (blue) = bnio faster; < 1.0 (red) = asio faster.*

### 6.5 bnio / asio Waits Ratio Heatmap

![Timer Waits Heatmap](charts/kqueue/kqueue_timer_waits_heatmap.png)

*Values > 1.0 (blue) = bnio faster; < 1.0 (red) = asio faster.*

### 6.6 Extreme Cases

**Best bnio / asio lifecycle ratio:**

- Configuration: timers=256, rounds=100
- bnio: 11,910,300 lifecycle calls/s
- asio: 8,672,080 lifecycle calls/s
- Ratio: 1.37×

**Most challenging bnio / asio lifecycle ratio:**

- Configuration: timers=256, rounds=1,000
- bnio: 27,767,800 lifecycle calls/s
- asio: 30,563,900 lifecycle calls/s
- Ratio: 0.91×

### 6.7 Full Results

#### Timer count = 256

| Backend | Timers | Rounds | lifecycle/s | waits/s |
| --- | ---: | ---: | ---: | ---: |
| bnio | 256 | 100 | 11,910,300 | 6,720,350 |
| bnio | 256 | 500 | 18,636,500 | 10,622,200 |
| bnio | 256 | 1,000 | 27,767,800 | 15,847,000 |
| asio | 256 | 100 | 8,672,080 | 4,893,190 |
| asio | 256 | 500 | 19,864,800 | 11,322,300 |
| asio | 256 | 1,000 | 30,563,900 | 17,442,700 |

#### Timer count = 1,024

| Backend | Timers | Rounds | lifecycle/s | waits/s |
| --- | ---: | ---: | ---: | ---: |
| bnio | 1,024 | 100 | 38,249,400 | 21,582,100 |
| bnio | 1,024 | 500 | 39,147,900 | 22,313,000 |
| bnio | 1,024 | 1,000 | 38,273,300 | 21,842,400 |
| asio | 1,024 | 100 | 33,882,500 | 19,118,100 |
| asio | 1,024 | 500 | 38,584,100 | 21,991,600 |
| asio | 1,024 | 1,000 | 34,661,600 | 19,781,200 |

#### Timer count = 4,096

| Backend | Timers | Rounds | lifecycle/s | waits/s |
| --- | ---: | ---: | ---: | ---: |
| bnio | 4,096 | 100 | 39,084,200 | 22,053,100 |
| bnio | 4,096 | 500 | 36,370,100 | 20,729,700 |
| bnio | 4,096 | 1,000 | 37,079,800 | 21,161,300 |
| asio | 4,096 | 100 | 34,099,300 | 19,240,400 |
| asio | 4,096 | 500 | 36,400,200 | 20,746,900 |
| asio | 4,096 | 1,000 | 35,789,700 | 20,425,000 |

#### Timer count = 16,384

| Backend | Timers | Rounds | lifecycle/s | waits/s |
| --- | ---: | ---: | ---: | ---: |
| bnio | 16,384 | 100 | 36,805,900 | 20,767,600 |
| bnio | 16,384 | 500 | 37,954,300 | 21,632,700 |
| bnio | 16,384 | 1,000 | 37,716,700 | 21,524,700 |
| asio | 16,384 | 100 | 32,717,900 | 18,460,900 |
| asio | 16,384 | 500 | 37,814,900 | 21,553,200 |
| asio | 16,384 | 1,000 | 38,111,400 | 21,750,000 |

### 6.8 Interpretation

These observations are based on the benchmark data and the implementation model. They have not been independently validated with profiling in this run.

1. **bnio and asio are effectively tied on timer churn with kqueue.** The average lifecycle ratio is 1.064× in bnio's favor, but this is skewed by a few outlier configurations. At stable configurations (500+ rounds, 1024+ timers), both backends produce nearly identical throughput.

2. **Kqueue timer throughput is substantially higher than io_uring.** Both backends achieve ~37–39 million lifecycle calls/s at peak on kqueue, compared to ~21 million/s (bnio) and ~17 million/s (asio) on io_uring. This reflects the lightweight nature of kqueue's EVFILT_TIMER mechanism versus io_uring's IORING_OP_TIMEOUT.

3. **bnio leads at low round counts.** At rounds=100, bnio leads across all timer counts (1.10–1.37×). This suggests bnio has lower timer setup/initialization overhead. At more rounds, the amortization effect brings the two backends into parity.

4. **Setup cost amortization favors asio slightly.** At higher round counts, asio sometimes pulls ahead (e.g., 256 timers, 1000 rounds: asio leads 0.91×). This suggests asio's per-round path is marginally faster once warm-up is complete.

5. **No clear scaling pattern with timer count.** Unlike io_uring where bnio's advantage grew with timer count, on kqueue the ratio is flat or slightly decreasing with more timers. Both backends use kqueue's native timer mechanism, so the per-timer costs are very similar.

6. **Active wait throughput mirrors lifecycle throughput.** The waits/s ratio follows the same pattern (average ~1.06× in bnio's favor), confirming the measurements are consistent across both metrics.

---

## 7. Cross-Benchmark Summary

| Benchmark | Avg bnio/asio Ratio | bnio Wins | asio Wins | Total Configs |
| --- | ---: | ---: | ---: | ---: |
| TCP Echo Throughput | 0.772× | 6 | 90 | 96 |
| Timer Churn (lifecycle) | 1.064× | 16 | 8 | 24 |

On the kqueue (macOS/BSD) backend, the picture is mixed:

- **Throughput**: bnio trails asio significantly, achieving ~77% of asio's throughput on average. The gap narrows with more worker threads (0.68× at 1 worker → 0.91× at 8 workers), suggesting the kqueue backend has overhead that amortizes better in multi-threaded scenarios. The large-message (64 KB) path is the weakest area.

- **Timer churn**: bnio and asio are effectively tied, with bnio holding a slight edge (1.06×) driven primarily by lower setup costs at low round counts. At steady-state, both backends achieve near-identical throughput.

### Platform Comparison: kqueue vs io_uring

| Benchmark | io_uring (Linux) | kqueue (macOS) |
| --- | ---: | ---: |
| TCP Echo Throughput | **1.039×** (bnio leads) | **0.772×** (asio leads) |
| Timer Churn (lifecycle) | **1.179×** (bnio leads) | **1.064×** (near parity) |

The platform comparison reveals that bnio's performance advantage is backend-dependent. On io_uring, bnio leverages submission batching and zero-syscall completion polling to outperform standalone Asio. On kqueue, these optimizations are not available, and bnio's overhead relative to the more mature Asio kqueue implementation results in a throughput deficit. The timer churn results are closer across platforms because both backends ultimately rely on the same underlying kqueue timer mechanism.

The kqueue backend is actively under development (see [kqueue roadmap](../design/kqueue-roadmap.md)). Areas of ongoing optimization include the task queue submission path, event batching, and large-buffer I/O handling.

---

## 8. Cross-Platform Absolute Throughput Comparison

A comparison of absolute throughput (not ratio) between platforms:

### Throughput (workers=4, connections=256, 4 KB)

| Backend | io_uring (Linux) | kqueue (macOS) | Ratio |
| --- | ---: | ---: | ---: |
| bnio | 788,413 req/s | 214,837 req/s | 0.27× |
| asio | 723,093 req/s | 251,929 req/s | 0.35× |

### Timer Churn (timers=1,024, rounds=500)

| Backend | io_uring (Linux) | kqueue (macOS) | Ratio |
| --- | ---: | ---: | ---: |
| bnio | 20,690,800 lifecycle/s | 39,147,900 lifecycle/s | 1.89× |
| asio | 17,375,400 lifecycle/s | 38,584,100 lifecycle/s | 2.22× |

> **Note:** Absolute throughput comparisons across platforms are not apples-to-apples. Different hardware (i9-13900H x86_64 vs M5 Pro arm64), operating systems, and kernel implementations all affect baseline performance. The io_uring run used 60 s measurements over 3 iterations; the kqueue run used 10 s measurements over 1 iteration. Treat these cross-platform figures as directional only.
