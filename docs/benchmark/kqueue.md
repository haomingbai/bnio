# bnio vs asio Benchmark — kqueue (macOS/BSD)

## 1. Test Environment

| Item | Value |
| --- | --- |
| Date | 2026-07-28 |
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

Overall average throughput ratio (bnio / asio): **0.901×** across all 96 configurations.

### Part B — Timer Churn

Both backends completed every configuration successfully. All 24 configurations produced clean measurements.

Overall average lifecycle throughput ratio (bnio / asio): **1.056×** across all 24 configurations.

---

## Part A — TCP Echo Throughput Results

### 5.1 Throughput Overview

![Throughput Overview](charts/kqueue/kqueue_overview_bars.png)

**Reference point: workers=4, connections=256**

| Message Size | bnio req/s | bnio err | asio req/s | asio err | Ratio |
| --- | ---: | ---: | ---: | ---: | ---: |
| 64 B | 110,588 | 0 | 102,384 | 0 | 1.08× |
| 1 KB | 108,570 | 0 | 101,138 | 0 | 1.07× |
| 4 KB | 107,539 | 0 | 99,483 | 0 | 1.08× |
| 64 KB | 9,584 | 0 | 13,316 | 0 | 0.72× |

### 5.2 Throughput vs Connections

![Throughput vs Connections](charts/kqueue/kqueue_throughput_vs_connections.png)

*Workers=4, faceted by message size.*

### 5.3 Throughput vs Worker Threads

![Throughput vs Workers](charts/kqueue/kqueue_throughput_vs_workers.png)

*Connections=256, faceted by message size.*

| Workers | bnio req/s | bnio err | asio req/s | asio err | Ratio |
| ---: | ---: | ---: | ---: | ---: | ---: |
| 1 | 85,283 | 0 | 105,726 | 0 | 0.81× |
| 2 | 120,781 | 0 | 138,059 | 0 | 0.87× |
| 4 | 107,539 | 0 | 99,483 | 0 | 1.08× |
| 8 | 97,451 | 0 | 92,921 | 0 | 1.05× |

**Worker-scaling at 4 KB / 256 connections.** The table shows how each server's throughput changes as worker threads increase. bnio trails at 1–2 workers (0.81–0.87×) but pulls ahead at 4+ workers (1.05–1.08×), indicating the kqueue backend benefits significantly from multi-threaded operation.

### 5.4 Connection Scaling

| Connections | bnio req/s | bnio err | asio req/s | asio err | Ratio |
| ---: | ---: | ---: | ---: | ---: | ---: |
| 64 | 107,099 | 0 | 102,403 | 0 | 1.05× |
| 256 | 107,539 | 0 | 99,483 | 0 | 1.08× |
| 1024 | 103,967 | 0 | 97,014 | 0 | 1.07× |

**Connection-scaling at 4 KB / workers=4.** Shows how each server handles increasing concurrency. bnio leads consistently at all connection counts.

### 5.5 bnio / asio Throughput Ratio Heatmap

![Heatmap](charts/kqueue/kqueue_heatmap_ratio.png)

*Workers=4. Positive values (blue) = bnio faster; negative (red) = asio faster.*

### 5.6 Worker-Scaling Ratio Heatmap

![Worker Scaling](charts/kqueue/kqueue_worker_scaling_heatmap.png)

*Connections=256. Shows how the bnio/asio ratio changes as worker threads increase.*

### 5.7 Extreme Cases

**Best bnio / asio throughput ratio (zero-error):**

- Configuration: workers=8, connections=256, message_size=64 B
- bnio: 104,554 req/s
- asio: 95,006 req/s
- Ratio: 1.10×

**Most challenging bnio / asio throughput ratio (zero-error):**

- Configuration: workers=8, connections=256, message_size=64 KB
- bnio: 9,176 req/s
- asio: 16,399 req/s
- Ratio: 0.56×

### 5.8 Full Results (workers=4)

#### Message size = 64 B

| Server | Workers | Conns | req/s | MB/s |
| --- | ---: | ---: | ---: | ---: |
| asio | 4 | 64 | 103,561 | 6 |
| asio | 4 | 256 | 102,384 | 6 |
| asio | 4 | 1024 | 100,850 | 6 |
| bnio | 4 | 64 | 110,392 | 6 |
| bnio | 4 | 256 | 110,588 | 6 |
| bnio | 4 | 1024 | 107,306 | 6 |

#### Message size = 1 KB

| Server | Workers | Conns | req/s | MB/s |
| --- | ---: | ---: | ---: | ---: |
| asio | 4 | 64 | 103,190 | 100 |
| asio | 4 | 256 | 101,138 | 98 |
| asio | 4 | 1024 | 98,097 | 95 |
| bnio | 4 | 64 | 109,229 | 106 |
| bnio | 4 | 256 | 108,570 | 106 |
| bnio | 4 | 1024 | 107,536 | 105 |

#### Message size = 4 KB

| Server | Workers | Conns | req/s | MB/s |
| --- | ---: | ---: | ---: | ---: |
| asio | 4 | 64 | 102,403 | 400 |
| asio | 4 | 256 | 99,483 | 388 |
| asio | 4 | 1024 | 97,014 | 378 |
| bnio | 4 | 64 | 107,099 | 418 |
| bnio | 4 | 256 | 107,539 | 420 |
| bnio | 4 | 1024 | 103,967 | 406 |

#### Message size = 64 KB

| Server | Workers | Conns | req/s | MB/s |
| --- | ---: | ---: | ---: | ---: |
| asio | 4 | 64 | 13,997 | 874 |
| asio | 4 | 256 | 13,316 | 832 |
| asio | 4 | 1024 | 13,064 | 816 |
| bnio | 4 | 64 | 9,694 | 605 |
| bnio | 4 | 256 | 9,584 | 599 |
| bnio | 4 | 1024 | 9,024 | 564 |

### 5.9 Interpretation

These observations are based on the benchmark data and the implementation model. They have not been independently validated with profiling in this run.

1. **bnio now leads throughput on the kqueue backend at higher worker counts.** The overall throughput ratio is 0.901×, a significant improvement from the previous 0.772× (+16.7%). bnio wins 42 of 96 configurations, up from only 6 previously. This improvement is attributable to the "perf: optimize the performance on bsd side" commit.

2. **The gap reverses at 4+ workers.** At workers=1, bnio trails at ~0.81×. At workers=2, it improves to ~0.87×. At workers=4 and 8, bnio leads at 1.05–1.08× for small/medium messages. This suggests the recent BSD-side optimizations primarily benefit multi-threaded operation, likely through improved task queue submission and event batching.

3. **Large messages (64 KB) remain the weakest area.** At 64 KB, bnio trails across all worker counts. The gap narrows at workers=1–2 (0.65–1.06×, mixed) but widens at workers=8 (as low as 0.56×). This suggests the kqueue large-buffer I/O path has additional optimization opportunities distinct from the small/medium message path.

4. **Small and medium message performance is now competitive.** At 64 B–4 KB with workers ≥ 4, bnio leads asio by 5–8%. For these workloads, the recent optimizations have brought bnio's per-message overhead below asio's on kqueue.

5. **Connection count has minimal impact on the ratio.** The bnio/asio ratio is stable across 64, 256, and 1,024 connections (1.05–1.08× at workers=4), indicating both backends scale similarly with connection count. At workers=8, bnio maintains its lead across all connection counts for small/medium messages.

6. **No errors across the entire matrix.** All 96 configurations completed cleanly — both server implementations and the client are stable under all tested configurations on kqueue.

7. **The kqueue implementation continues to improve.** bnio's kqueue backend has been actively optimized for BSD-side performance. Areas of ongoing improvement include the large-buffer I/O path and further reductions in per-message overhead at low worker counts. See the [kqueue roadmap](../design/kqueue-roadmap.md) for optimization plans.

---

## Part B — Timer Churn Results

### 6.1 Lifecycle Throughput Overview

![Timer Lifecycle Overview](charts/kqueue/kqueue_timer_lifecycle_overview.png)

**Reference point: update_rounds=500**

| Timer Count | bnio lifecycle/s | asio lifecycle/s | Ratio |
| ---: | ---: | ---: | ---: |
| 256 | 22,974,300 | 23,979,200 | 0.96× |
| 1,024 | 39,289,100 | 36,808,500 | 1.07× |
| 4,096 | 37,660,000 | 38,814,700 | 0.97× |
| 16,384 | 37,756,200 | 37,081,800 | 1.02× |

### 6.2 Active Waits Overview

![Timer Waits Overview](charts/kqueue/kqueue_timer_waits_overview.png)

**Reference point: update_rounds=500**

| Timer Count | bnio waits/s | asio waits/s | Ratio |
| ---: | ---: | ---: | ---: |
| 256 | 13,094,600 | 13,667,300 | 0.96× |
| 1,024 | 22,393,500 | 20,979,600 | 1.07× |
| 4,096 | 21,464,900 | 22,123,100 | 0.97× |
| 16,384 | 21,519,700 | 21,135,300 | 1.02× |

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

- Configuration: timers=1,024, rounds=100
- bnio: 38,037,400 lifecycle calls/s
- asio: 30,999,900 lifecycle calls/s
- Ratio: 1.23×

**Most challenging bnio / asio lifecycle ratio:**

- Configuration: timers=256, rounds=500
- bnio: 22,974,300 lifecycle calls/s
- asio: 23,979,200 lifecycle calls/s
- Ratio: 0.96×

### 6.7 Full Results

#### Timer count = 256

| Backend | Timers | Rounds | lifecycle/s | waits/s |
| --- | ---: | ---: | ---: | ---: |
| bnio | 256 | 100 | 18,586,400 | 10,487,300 |
| bnio | 256 | 500 | 22,974,300 | 13,094,600 |
| bnio | 256 | 1,000 | 31,476,700 | 17,963,600 |
| asio | 256 | 100 | 15,652,300 | 8,831,730 |
| asio | 256 | 500 | 23,979,200 | 13,667,300 |
| asio | 256 | 1,000 | 31,727,400 | 18,106,700 |

#### Timer count = 1,024

| Backend | Timers | Rounds | lifecycle/s | waits/s |
| --- | ---: | ---: | ---: | ---: |
| bnio | 1,024 | 100 | 38,037,400 | 21,462,500 |
| bnio | 1,024 | 500 | 39,289,100 | 22,393,500 |
| bnio | 1,024 | 1,000 | 37,840,100 | 21,595,200 |
| asio | 1,024 | 100 | 30,999,900 | 17,491,600 |
| asio | 1,024 | 500 | 36,808,500 | 20,979,600 |
| asio | 1,024 | 1,000 | 37,553,900 | 21,431,800 |

#### Timer count = 4,096

| Backend | Timers | Rounds | lifecycle/s | waits/s |
| --- | ---: | ---: | ---: | ---: |
| bnio | 4,096 | 100 | 39,278,900 | 22,163,000 |
| bnio | 4,096 | 500 | 37,660,000 | 21,464,900 |
| bnio | 4,096 | 1,000 | 38,790,300 | 22,137,500 |
| asio | 4,096 | 100 | 37,845,400 | 21,354,100 |
| asio | 4,096 | 500 | 38,814,700 | 22,123,100 |
| asio | 4,096 | 1,000 | 38,017,000 | 21,696,100 |

#### Timer count = 16,384

| Backend | Timers | Rounds | lifecycle/s | waits/s |
| --- | ---: | ---: | ---: | ---: |
| bnio | 16,384 | 100 | 38,795,800 | 21,890,400 |
| bnio | 16,384 | 500 | 37,756,200 | 21,519,700 |
| bnio | 16,384 | 1,000 | 37,525,800 | 21,415,800 |
| asio | 16,384 | 100 | 33,089,200 | 18,670,400 |
| asio | 16,384 | 500 | 37,081,800 | 21,135,300 |
| asio | 16,384 | 1,000 | 37,026,200 | 21,130,700 |

### 6.8 Interpretation

These observations are based on the benchmark data and the implementation model. They have not been independently validated with profiling in this run.

1. **bnio and asio remain effectively tied on timer churn.** The average lifecycle ratio is 1.056× in bnio's favor (down slightly from 1.064× previously), with bnio winning 18 of 24 configurations. At steady-state configurations (500+ rounds), both backends produce near-identical throughput.

2. **Kqueue timer throughput continues to excel.** Both backends achieve ~37–39 million lifecycle calls/s at peak, consistent with the lightweight nature of kqueue's `EVFILT_TIMER` mechanism.

3. **bnio's advantage at low round counts has strengthened.** At rounds=100, bnio leads across all timer counts with ratios of 1.04–1.23× (previously 1.10–1.37×). The lead is most pronounced at 1,024 timers (1.23×), suggesting continued advantage in timer setup/initialization overhead.

4. **Setup cost amortization favors asio at some configurations.** At 256 timers with 500+ rounds, asio slightly leads (0.96–0.99×). This configuration represents the case where per-round overhead dominates, and asio's path is marginally faster once warm-up is complete.

5. **No clear scaling pattern with timer count.** The ratio is relatively flat or slightly decreasing with more timers. Both backends use kqueue's native timer mechanism, so per-timer costs are very similar.

6. **Active wait throughput mirrors lifecycle throughput.** The waits/s ratio follows the same pattern (average ~1.05× in bnio's favor), confirming consistency across both metrics.

---

## 7. Cross-Benchmark Summary

| Benchmark | Avg bnio/asio Ratio | bnio Wins | asio Wins | Total Configs |
| --- | ---: | ---: | ---: | ---: |
| TCP Echo Throughput | 0.901× | 42 | 54 | 96 |
| Timer Churn (lifecycle) | 1.056× | 18 | 6 | 24 |

On the kqueue (macOS/BSD) backend, the picture has shifted significantly from the previous run:

- **Throughput**: bnio now achieves 90% of asio's throughput on average, up from 77% (+16.7%). At 4+ workers, bnio leads asio for small and medium messages (1.05–1.10×). The large-message (64 KB) path remains the weakest area, particularly at higher worker counts.

- **Timer churn**: bnio maintains a slight edge (1.056×), essentially unchanged from the previous run (1.064×). Both backends achieve near-identical steady-state throughput.

### Comparison with Previous Run (2026-07-27)

| Benchmark | Previous (0.772× avg) | Current (0.901× avg) | Change |
| --- | ---: | ---: | ---: |
| TCP Echo Throughput | 0.772× (6 wins) | 0.901× (42 wins) | **+16.7%** |
| Timer Churn | 1.064× (16 wins) | 1.056× (18 wins) | −0.8% |

The throughput improvement is attributable to the "perf: optimize the performance on bsd side" commit (d9d9c12), which improved bnio's kqueue backend performance, particularly in multi-threaded operation.

### Platform Comparison: kqueue vs io_uring

| Benchmark | io_uring (Linux) | kqueue (macOS) |
| --- | ---: | ---: |
| TCP Echo Throughput | **1.039×** (bnio leads) | **0.901×** (bnio narrows gap) |
| Timer Churn (lifecycle) | **1.179×** (bnio leads) | **1.056×** (near parity) |

The platform comparison reveals that bnio's performance on kqueue has improved significantly but still lags behind its io_uring performance. On io_uring, bnio leverages submission batching and zero-syscall completion polling to outperform standalone Asio. On kqueue, the recent BSD-side optimizations have closed much of the gap, but large-message I/O remains an area for further improvement. The timer churn results are closer across platforms because both backends ultimately rely on the same underlying kqueue timer mechanism.

The kqueue backend is actively under development (see [kqueue roadmap](../design/kqueue-roadmap.md)). Areas of ongoing optimization include the large-buffer I/O path, single-worker throughput, and further reductions in synchronization overhead.

---

## 8. Cross-Platform Absolute Throughput Comparison

A comparison of absolute throughput (not ratio) between platforms:

### Throughput (workers=4, connections=256, 4 KB)

| Backend | io_uring (Linux) | kqueue (macOS) | Ratio |
| --- | ---: | ---: | ---: |
| bnio | 788,413 req/s | 107,539 req/s | 0.14× |
| asio | 723,093 req/s | 99,483 req/s | 0.14× |

### Timer Churn (timers=1,024, rounds=500)

| Backend | io_uring (Linux) | kqueue (macOS) | Ratio |
| --- | ---: | ---: | ---: |
| bnio | 20,690,800 lifecycle/s | 39,289,100 lifecycle/s | 1.90× |
| asio | 17,375,400 lifecycle/s | 36,808,500 lifecycle/s | 2.12× |

> **Note:** Absolute throughput comparisons across platforms are not apples-to-apples. Different hardware (i9-13900H x86_64 vs M5 Pro arm64), operating systems, and kernel implementations all affect baseline performance. The io_uring run used 60 s measurements over 3 iterations; the kqueue run used 10 s measurements over 1 iteration. System load and thermal conditions may differ between runs. Treat these cross-platform figures as directional only.
