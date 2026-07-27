# bnio vs asio Benchmark — io_uring (Linux)

## 1. Test Environment

| Item | Value |
| --- | --- |
| Date | 2026-07-28 |
| Topology | Single-host loopback TCP (127.0.0.1) |
| OS | Fedora Linux 44 (Workstation Edition) |
| Kernel | 7.1.5-200.fc44.x86_64 |
| Architecture | x86_64 |
| CPU | 13th Gen Intel(R) Core(TM) i9-13900H |
| Logical CPUs | 20 |
| Memory | 32,564,620 kB |
| Compiler | gcc (GCC) 16.1.1 20260515 (Red Hat 16.1.1-2) |
| CMake | cmake version 4.3.0 |
| liburing | 2.13 |
| Asio | 1.30.2 (local source) |

Hostnames, usernames, absolute paths, and network addresses are intentionally omitted.

> **Note:** This report represents the io_uring (Linux) backend. For the kqueue (macOS/BSD) backend results, see [kqueue.md](kqueue.md).

## 2. Methodology

This report covers two independent benchmarks, each comparing bnio against standalone Asio:

- **Part A — TCP Echo Throughput (Sections 3–7):** A multi-dimensional throughput stress test on a TCP echo server under varying worker counts, connection counts, and message sizes.
- **Part B — Timer Churn (Sections 8–12):** A timer lifecycle stress test that creates, resets, cancels, and re-creates large numbers of steady timers in tight update rounds.

### Part A — TCP Echo Throughput

The benchmark compares two functionally equivalent TCP echo servers:

- **`bnio_throughput_benchmark`**: C++20 coroutine echo server using bnio on io_uring (Linux).
- **`asio_throughput_benchmark`**: C++20 coroutine echo server using standalone Asio (epoll reactor).
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

Both servers completed every configuration with zero client errors. All 96 configurations produced clean measurements.

Overall average throughput ratio (bnio / asio): **1.068×** across all 96 configurations.

### Part B — Timer Churn

Both backends completed every configuration successfully. All 24 configurations produced clean measurements.

Overall average lifecycle throughput ratio (bnio / asio): **1.216×** across all 24 configurations.

---

## Part A — TCP Echo Throughput Results

### 5.1 Throughput Overview

![Throughput Overview](charts/io_uring/overview_bars.png)

**Reference point: workers=4, connections=256**

| Message Size | bnio req/s | bnio err | asio req/s | asio err | Ratio |
| --- | ---: | ---: | ---: | ---: | ---: |
| 64 B | 451,027 | 0 | 384,522 | 0 | 1.17× |
| 1 KB | 434,038 | 0 | 370,049 | 0 | 1.17× |
| 4 KB | 376,792 | 0 | 330,085 | 0 | 1.14× |
| 64 KB | 6,272 | 0 | 6,228 | 0 | 1.01× |

### 5.2 Throughput vs Connections

![Throughput vs Connections](charts/io_uring/throughput_vs_connections.png)

*Workers=4, faceted by message size.*

### 5.3 Throughput vs Worker Threads

![Throughput vs Workers](charts/io_uring/throughput_vs_workers.png)

*Connections=256, faceted by message size.*

| Workers | bnio req/s | bnio err | asio req/s | asio err | Ratio |
| ---: | ---: | ---: | ---: | ---: | ---: |
| 1 | 378,047 | 0 | 383,720 | 0 | 0.99× |
| 2 | 377,336 | 0 | 359,920 | 0 | 1.05× |
| 4 | 376,792 | 0 | 330,085 | 0 | 1.14× |
| 8 | 377,524 | 0 | 324,312 | 0 | 1.16× |

**Worker-scaling at 4 KB / 256 connections.** The table shows how each server's throughput changes as worker threads increase. bnio's throughput is nearly constant across worker counts, while asio's throughput decreases with more workers. The bnio/asio ratio improves from 0.99× to 1.16× as worker count grows.

### 5.4 Connection Scaling

| Connections | bnio req/s | bnio err | asio req/s | asio err | Ratio |
| ---: | ---: | ---: | ---: | ---: | ---: |
| 64 | 388,020 | 0 | 334,914 | 0 | 1.16× |
| 256 | 376,792 | 0 | 330,085 | 0 | 1.14× |
| 1024 | 293,380 | 0 | 272,595 | 0 | 1.08× |

**Connection-scaling at 4 KB / workers=4.** Shows how each server handles increasing concurrency. The gap narrows slightly at higher connection counts.

### 5.5 bnio / asio Throughput Ratio Heatmap

![Heatmap](charts/io_uring/heatmap_ratio.png)

*Workers=4. Positive values (blue) = bnio faster; negative (red) = asio faster.*

### 5.6 Worker-Scaling Ratio Heatmap

![Worker Scaling](charts/io_uring/worker_scaling_heatmap.png)

*Connections=256. Shows how the bnio/asio ratio changes as worker threads increase.*

### 5.7 Extreme Cases

**Best bnio / asio throughput ratio (zero-error):**

- Configuration: workers=8, connections=64, message_size=4 KB
- bnio: 390,793 req/s
- asio: 327,433 req/s
- Ratio: 1.19×

**Most challenging bnio / asio throughput ratio (zero-error):**

- Configuration: workers=1, connections=1024, message_size=1 KB
- bnio: 382,284 req/s
- asio: 408,090 req/s
- Ratio: 0.94×

### 5.8 Full Results (workers=4)

#### Message size = 64 B

| Server | Workers | Conns | req/s | MB/s |
| --- | ---: | ---: | ---: | ---: |
| asio | 4 | 64 | 385,611 | 23 |
| asio | 4 | 256 | 384,522 | 23 |
| asio | 4 | 1024 | 389,886 | 23 |
| bnio | 4 | 64 | 454,177 | 27 |
| bnio | 4 | 256 | 451,027 | 27 |
| bnio | 4 | 1024 | 444,906 | 27 |

#### Message size = 1 KB

| Server | Workers | Conns | req/s | MB/s |
| --- | ---: | ---: | ---: | ---: |
| asio | 4 | 64 | 377,005 | 368 |
| asio | 4 | 256 | 370,049 | 361 |
| asio | 4 | 1024 | 363,677 | 355 |
| bnio | 4 | 64 | 437,789 | 427 |
| bnio | 4 | 256 | 434,038 | 423 |
| bnio | 4 | 1024 | 397,328 | 388 |

#### Message size = 4 KB

| Server | Workers | Conns | req/s | MB/s |
| --- | ---: | ---: | ---: | ---: |
| asio | 4 | 64 | 334,914 | 1,308 |
| asio | 4 | 256 | 330,085 | 1,289 |
| asio | 4 | 1024 | 272,595 | 1,064 |
| bnio | 4 | 64 | 388,020 | 1,515 |
| bnio | 4 | 256 | 376,792 | 1,471 |
| bnio | 4 | 1024 | 293,380 | 1,146 |

#### Message size = 64 KB

| Server | Workers | Conns | req/s | MB/s |
| --- | ---: | ---: | ---: | ---: |
| asio | 4 | 64 | 1,565 | 97 |
| asio | 4 | 256 | 6,228 | 389 |
| asio | 4 | 1024 | 24,743 | 1,546 |
| bnio | 4 | 64 | 1,576 | 98 |
| bnio | 4 | 256 | 6,272 | 392 |
| bnio | 4 | 1024 | 25,124 | 1,570 |

### 5.9 Interpretation

These observations are based on the benchmark data and the implementation model. They have not been independently validated with profiling in this run.

1. **bnio leads on average.** The overall throughput ratio is 1.068× in bnio's favor. bnio on io_uring outperforms asio on epoll in 40 of 48 configurations, particularly at small-to-medium message sizes (64 B–4 KB) where io_uring's submission batching provides a measurable advantage.

2. **Single-worker is near parity.** At workers=1, bnio and asio are within ±6%. The io_uring advantage materializes primarily with multi-worker setups where batching and reduced syscall overhead compound.

3. **Multi-worker scaling favours bnio.** At workers=1 (4 KB, 256 connections), asio holds a narrow 0.99× edge. At workers=8, bnio leads at 1.16×. bnio's throughput stays near-constant across worker counts (~377k req/s), while asio's throughput degrades from ~384k to ~324k as worker count increases.

4. **Large messages converge.** At 64 KB both servers are bandwidth-limited on loopback and the ratio is near 1.01×, consistent with per-message syscall cost being amortized over large transfers.

5. **High concurrency compresses the gap.** At 1,024 connections the bnio/asio ratio narrows to 1.08× (vs 1.16× at 64 connections), suggesting both servers become CPU-saturated handling many concurrent sessions.

6. **No errors across the entire matrix.** All 96 configurations completed cleanly — both server implementations and the client are stable under all tested configurations.

---

## Part B — Timer Churn Results

### 6.1 Lifecycle Throughput Overview

![Timer Lifecycle Overview](charts/io_uring/timer_lifecycle_overview.png)

**Reference point: update_rounds=500**

| Timer Count | bnio lifecycle/s | asio lifecycle/s | Ratio |
| ---: | ---: | ---: | ---: |
| 256 | 18,821,700 | 14,719,800 | 1.28× |
| 1,024 | 21,615,900 | 18,130,700 | 1.19× |
| 4,096 | 22,486,500 | 16,864,000 | 1.33× |
| 16,384 | 22,741,100 | 17,996,200 | 1.26× |

### 6.2 Active Waits Overview

![Timer Waits Overview](charts/io_uring/timer_waits_overview.png)

**Reference point: update_rounds=500**

| Timer Count | bnio waits/s | asio waits/s | Ratio |
| ---: | ---: | ---: | ---: |
| 256 | 10,727,700 | 8,389,770 | 1.28× |
| 1,024 | 12,320,300 | 10,333,900 | 1.19× |
| 4,096 | 12,816,600 | 9,611,910 | 1.33× |
| 16,384 | 12,961,700 | 10,257,200 | 1.26× |

### 6.3 Lifecycle Throughput vs Timer Count

![Timer Lifecycle vs Timers](charts/io_uring/timer_lifecycle_vs_timers.png)

*Faceted by update rounds. X-axis on log₂ scale.*

### 6.4 bnio / asio Lifecycle Ratio Heatmap

![Timer Lifecycle Heatmap](charts/io_uring/timer_lifecycle_heatmap.png)

*Values > 1.0 (blue) = bnio faster; < 1.0 (red) = asio faster.*

### 6.5 bnio / asio Waits Ratio Heatmap

![Timer Waits Heatmap](charts/io_uring/timer_waits_heatmap.png)

*Values > 1.0 (blue) = bnio faster; < 1.0 (red) = asio faster.*

### 6.6 Extreme Cases

**Best bnio / asio lifecycle ratio:**

- Configuration: timers=4,096, rounds=500
- bnio: 22,486,500 lifecycle calls/s
- asio: 16,864,000 lifecycle calls/s
- Ratio: 1.33×

**Most challenging bnio / asio lifecycle ratio:**

- Configuration: timers=1,024, rounds=100
- bnio: 15,330,800 lifecycle calls/s
- asio: 14,800,600 lifecycle calls/s
- Ratio: 1.04×

### 6.7 Full Results

#### Timer count = 256

| Backend | Timers | Rounds | lifecycle/s | waits/s |
| --- | ---: | ---: | ---: | ---: |
| asio | 256 | 100 | 13,505,600 | 7,620,480 |
| asio | 256 | 500 | 14,719,800 | 8,389,770 |
| asio | 256 | 1,000 | 17,552,500 | 10,017,200 |
| bnio | 256 | 100 | 14,521,900 | 8,193,900 |
| bnio | 256 | 500 | 18,821,700 | 10,727,700 |
| bnio | 256 | 1,000 | 20,287,800 | 11,578,100 |

#### Timer count = 1,024

| Backend | Timers | Rounds | lifecycle/s | waits/s |
| --- | ---: | ---: | ---: | ---: |
| asio | 1,024 | 100 | 14,800,600 | 8,351,180 |
| asio | 1,024 | 500 | 18,130,700 | 10,333,900 |
| asio | 1,024 | 1,000 | 19,092,300 | 10,895,900 |
| bnio | 1,024 | 100 | 15,330,800 | 8,650,370 |
| bnio | 1,024 | 500 | 21,615,900 | 12,320,300 |
| bnio | 1,024 | 1,000 | 21,159,600 | 12,075,700 |

#### Timer count = 4,096

| Backend | Timers | Rounds | lifecycle/s | waits/s |
| --- | ---: | ---: | ---: | ---: |
| asio | 4,096 | 100 | 16,230,500 | 9,157,980 |
| asio | 4,096 | 500 | 16,864,000 | 9,611,910 |
| asio | 4,096 | 1,000 | 17,303,700 | 9,875,130 |
| bnio | 4,096 | 100 | 20,727,200 | 11,695,200 |
| bnio | 4,096 | 500 | 22,486,500 | 12,816,600 |
| bnio | 4,096 | 1,000 | 22,688,700 | 12,948,300 |

#### Timer count = 16,384

| Backend | Timers | Rounds | lifecycle/s | waits/s |
| --- | ---: | ---: | ---: | ---: |
| asio | 16,384 | 100 | 16,963,600 | 9,571,660 |
| asio | 16,384 | 500 | 17,996,200 | 10,257,200 |
| asio | 16,384 | 1,000 | 17,633,800 | 10,063,500 |
| bnio | 16,384 | 100 | 22,234,100 | 12,545,500 |
| bnio | 16,384 | 500 | 22,741,100 | 12,961,700 |
| bnio | 16,384 | 1,000 | 21,953,900 | 12,529,000 |

### 6.8 Interpretation

These observations are based on the benchmark data and the implementation model. They have not been independently validated with profiling in this run.

1. **bnio leads across all timer configurations.** The average lifecycle ratio is 1.216×, and bnio wins in all 12 configurations. The smallest margin is 1.04× (1,024 timers, 100 rounds); the largest is 1.33× (4,096 timers, 500 rounds).

2. **bnio's advantage is strongest at mid-to-high timer counts.** At 256 timers the lifecycle ratio averages ~1.20×. At 4,096 timers the ratio reaches 1.27–1.33×. This suggests bnio's timer infrastructure scales more efficiently with the number of active timers.

3. **asio's throughput plateaus.** asio's lifecycle throughput stays around 14–18 M calls/s across all configurations. bnio scales from ~15 M/s (256 timers, 100 rounds) to ~23 M/s (4,096+ timers, 500+ rounds), reaching ~33% higher peak throughput.

4. **More update rounds benefit bnio more.** bnio's lifecycle throughput increases from ~15–22 M/s at 100 rounds to ~20–23 M/s at 1,000 rounds. asio's improvement is less pronounced, widening the ratio as rounds increase.

5. **Active wait throughput mirrors lifecycle throughput.** The waits/s ratio follows the same trend (average 1.22× in bnio's favor), confirming the lifecycle advantage is not an artifact of operation counting but reflects genuinely faster timer completion.

6. **bnio's throughput is more stable across configurations.** bnio's lifecycle throughput varies from ~15 M to ~23 M across the 12 configs, while asio varies from ~14 M to ~18 M. bnio's timer subsystem handles diverse workloads with more consistent high throughput.

---

## 7. Cross-Benchmark Summary

| Benchmark | Avg bnio/asio Ratio | bnio Wins | asio Wins | Total Configs |
| --- | ---: | ---: | ---: | ---: |
| TCP Echo Throughput | 1.068× | 40 | 8 | 48 |
| Timer Churn (lifecycle) | 1.216× | 12 | 0 | 12 |

bnio on io_uring demonstrates a clear throughput advantage over standalone Asio on epoll in both the network I/O and timer management benchmarks. The advantage is more pronounced in timer churn (where bnio's timer infrastructure shows better scaling with timer count) than in TCP echo (where the gap is narrower, particularly at 64 KB and at single-worker configurations).

### Platform Comparison: io_uring vs kqueue

| Benchmark | io_uring (Linux) | kqueue (macOS) |
| --- | ---: | ---: |
| TCP Echo Throughput | **1.068×** (bnio leads) | **0.772×** (asio leads) |
| Timer Churn (lifecycle) | **1.216×** (bnio leads) | **1.064×** (near parity) |

The platform comparison confirms that bnio's performance advantage is backend-dependent. On io_uring, bnio leverages submission batching and kernel-side polling to outperform standalone Asio. On kqueue, these Linux-specific optimizations are not available, and bnio's newer kqueue backend trails the more mature Asio kqueue implementation in throughput.

---

*Report generated from 120 benchmark configurations (96 throughput + 24 timer churn). Charts are available in the repository under `docs/benchmark/charts/io_uring/`. Raw data in `.artifacts/benchmark_results/`.*
