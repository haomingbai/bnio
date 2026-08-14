# bnio vs asio Benchmark — io_uring (Linux)

## 1. Test Environment

| Item | Value |
| --- | --- |
| Date | 2026-08-14 |
| Topology | Single-host loopback TCP (127.0.0.1) |
| OS | Fedora Linux 44 |
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
- Server process **restarted** for every measured configuration and iteration.
- Each configuration runs **3 iterations**; reported values are arithmetic means.
- Client and server exit statuses are checked for every run.

### Part B — Timer Churn

The benchmark compares two functionally equivalent timer stress programs:

- **`bnio_timer_churn_benchmark`**: Uses `bnio::steady_timer` and `bnio::io_context`.
- **`asio_timer_churn_benchmark`**: Uses `asio::steady_timer` and `asio::io_context`.
- Both programs execute an identical workload: each round destroys and recreates a rotating subset of timers, resets the expiry on all other timers, and starts a new async_wait for every live timer. A barrier timer synchronizes each round.

Output metrics include lifecycle API calls per second (creates + destroys + expiry sets + explicit cancels) and active waits started per second.

#### Fairness Controls

- Both programs rebuilt in **Release** mode with `-march=native -mtune=native`.
- Identical workload parameters passed to both executables.
- Each configuration runs **3 iterations**; reported values are arithmetic means.

## 3. Configuration Matrix

### Part A — Throughput

| Dimension | Values |
| --- | --- |
| Server | bnio, asio |
| Worker threads | 1, 2, 4, 8 |
| Concurrent connections | 64, 256, 1024 |
| Message size | 64 B, 1 KB, 4 KB, 64 KB |
| Measurement per run | 10 s |
| Iterations per config | 3 |

The matrix contains **96** unique server-configuration cells. With 3 iterations, the throughput phase produced **288** measured rows.

### Part B — Timer Churn

| Dimension | Values |
| --- | --- |
| Backend | bnio, asio |
| Live timers | 256, 1,024, 4,096, 16,384 |
| Update rounds | 100, 500, 1,000 |
| Replacements per round | timers / 4 (default) |
| Iterations per config | 3 |

The matrix contains **24** unique backend-configuration cells. With 3 iterations, the timer phase produced **72** measured rows.

## 4. Stability Summary

### Part A — Throughput

Both servers completed every configuration with zero non-zero client or server exits. All 288 measured throughput runs completed cleanly.

Overall average throughput ratio (bnio / asio): **1.031×** across all 48 per-configuration ratios.

### Part B — Timer Churn

Both backends completed every configuration successfully. All 72 measured timer runs completed cleanly.

Overall average lifecycle throughput ratio (bnio / asio): **0.965×** across all 12 per-configuration ratios.

---

## Part A — TCP Echo Throughput Results

### 5.1 Throughput Overview

![Throughput Overview](charts/io_uring/overview_bars.png)

**Reference point: workers=4, connections=256**

| Message Size | bnio req/s | bnio err | asio req/s | asio err | Ratio |
| --- | ---: | ---: | ---: | ---: | ---: |
| 64 B | 926,306 | 0 | 829,170 | 0 | 1.12× |
| 1 KB | 965,037 | 0 | 810,447 | 0 | 1.19× |
| 4 KB | 839,376 | 0 | 746,163 | 0 | 1.12× |
| 64 KB | 6,294 | 0 | 6,223 | 0 | 1.01× |

### 5.2 Throughput vs Connections

![Throughput vs Connections](charts/io_uring/throughput_vs_connections.png)

*Workers=4, faceted by message size.*

### 5.3 Throughput vs Worker Threads

![Throughput vs Workers](charts/io_uring/throughput_vs_workers.png)

*Connections=256, faceted by message size.*

| Workers | bnio req/s | bnio err | asio req/s | asio err | Ratio |
| ---: | ---: | ---: | ---: | ---: | ---: |
| 1 | 365,852 | 0 | 372,552 | 0 | 0.98× |
| 2 | 594,976 | 0 | 623,394 | 0 | 0.95× |
| 4 | 839,376 | 0 | 746,163 | 0 | 1.12× |
| 8 | 787,384 | 0 | 759,767 | 0 | 1.04× |

**Worker-scaling at 4 KB / 256 connections.** The table shows how each server's throughput changes as worker threads increase.

### 5.4 Connection Scaling

| Connections | bnio req/s | bnio err | asio req/s | asio err | Ratio |
| ---: | ---: | ---: | ---: | ---: | ---: |
| 64 | 804,426 | 0 | 720,822 | 0 | 1.12× |
| 256 | 839,376 | 0 | 746,163 | 0 | 1.12× |
| 1024 | 787,442 | 0 | 716,523 | 0 | 1.10× |

**Connection-scaling at 4 KB / workers=4.** Shows how each server handles increasing concurrency.

### 5.5 bnio / asio Throughput Ratio Heatmap

![Heatmap](charts/io_uring/heatmap_ratio.png)

*Workers=4. Positive values (blue) = bnio faster; negative (red) = asio faster.*

### 5.6 Worker-Scaling Ratio Heatmap

![Worker Scaling](charts/io_uring/worker_scaling_heatmap.png)

*Connections=256. Shows how the bnio/asio ratio changes as worker threads increase.*

### 5.7 Extreme Cases

**Best bnio / asio throughput ratio (zero-error):**

- Configuration: workers=4, connections=64, message_size=64 B
- bnio: 963,728 req/s
- asio: 804,817 req/s
- Ratio: 1.20×

**Most challenging bnio / asio throughput ratio (zero-error):**

- Configuration: workers=2, connections=64, message_size=4 KB
- bnio: 516,700 req/s
- asio: 569,775 req/s
- Ratio: 0.91×

### 5.8 Full Results (workers=4)

#### Message size = 64 B

| Server | Workers | Conns | req/s | MB/s |
| --- | ---: | ---: | ---: | ---: |
| asio | 4 | 64 | 804,817 | 49 |
| asio | 4 | 256 | 829,170 | 50 |
| asio | 4 | 1024 | 917,765 | 55 |
| bnio | 4 | 64 | 963,728 | 58 |
| bnio | 4 | 256 | 926,306 | 56 |
| bnio | 4 | 1024 | 1,032,600 | 62 |

#### Message size = 1 KB

| Server | Workers | Conns | req/s | MB/s |
| --- | ---: | ---: | ---: | ---: |
| asio | 4 | 64 | 784,097 | 765 |
| asio | 4 | 256 | 810,447 | 791 |
| asio | 4 | 1024 | 878,884 | 858 |
| bnio | 4 | 64 | 919,866 | 898 |
| bnio | 4 | 256 | 965,037 | 942 |
| bnio | 4 | 1024 | 973,704 | 950 |

#### Message size = 4 KB

| Server | Workers | Conns | req/s | MB/s |
| --- | ---: | ---: | ---: | ---: |
| asio | 4 | 64 | 720,822 | 2,815 |
| asio | 4 | 256 | 746,163 | 2,914 |
| asio | 4 | 1024 | 716,523 | 2,798 |
| bnio | 4 | 64 | 804,426 | 3,142 |
| bnio | 4 | 256 | 839,376 | 3,278 |
| bnio | 4 | 1024 | 787,442 | 3,076 |

#### Message size = 64 KB

| Server | Workers | Conns | req/s | MB/s |
| --- | ---: | ---: | ---: | ---: |
| asio | 4 | 64 | 1,561 | 97 |
| asio | 4 | 256 | 6,223 | 389 |
| asio | 4 | 1024 | 24,748 | 1,546 |
| bnio | 4 | 64 | 1,574 | 98 |
| bnio | 4 | 256 | 6,294 | 393 |
| bnio | 4 | 1024 | 25,168 | 1,573 |

### 5.9 Interpretation

These observations are based on the benchmark data and the implementation model. They have not been independently validated with profiling in this run.

1. **bnio leads on average.** The overall throughput ratio is 1.03× in bnio's favor. bnio outperforms asio in 31 of 48 configurations.
2. **Worker scaling is uneven.** The strongest average is at 4 workers (1.11×); at 8 workers the edge narrows to 1.02×, and at 1 worker the two are effectively level (0.99×).
3. **Large messages converge.** At 64 KB the average ratio is 1.01×, consistent with both servers being bandwidth-limited on loopback and per-message syscall costs being amortized.
4. **High concurrency remains the tightest regime.** The average ratio is 1.02× at 1,024 connections, versus 1.04× at 64 connections.
5. **No failures across the full 3-iteration matrix.** All 288 throughput runs completed with zero non-zero exits.

### 5.10 Performance Change vs Previous Run (2026-08-04)

Since the previous io_uring report, this branch has changed eager read/write probing (`f107f34`, `1bf9155`, `1a4ef0c`, `15a9710`), timer-clock and timer wake-up locking (`2aca001`, `2bdedc5`, `e688866`), worker-state/wake-up scheduling (`d0dada1`, `88da040`, `2c441c4`, `97d456d`, `6aeadfc`), and io_uring context/CQE dispatch clustering (`d4285a2`, `600e206`). A CPU-task work-stealing option was also added (`428318e`) but is not exercised by these two benchmarks.

| Metric | Previous (Aug 4) | Current (Aug 14) | Change |
| --- | ---: | ---: | ---: |
| Overall avg ratio | 1.073× | 1.031× | -3.91% |
| bnio wins | 43/48 | 31/48 | -12 |
| w=1 avg ratio | 0.998× | 0.994× | -0.44% |
| w=8 avg ratio | 1.151× | 1.022× | -11.24% |

Throughput performance **narrowed slightly** compared with the previous quick run (-3.91%). The multi-worker advantage weakened at 8 workers, while the 4-worker configuration remains the strongest bnio point. Because the previous report used one iteration per configuration and this report uses three-iteration means, part of this movement may reflect sampling rather than code behavior.

---

## Part B — Timer Churn Results

### 6.1 Lifecycle Throughput Overview

![Timer Lifecycle Overview](charts/io_uring/timer_lifecycle_overview.png)

**Reference point: update_rounds=500**

| Timer Count | bnio lifecycle/s | asio lifecycle/s | Ratio |
| ---: | ---: | ---: | ---: |
| 256 | 16,239,333 | 16,415,000 | 0.99× |
| 1,024 | 17,039,100 | 18,917,667 | 0.90× |
| 4,096 | 17,859,867 | 17,916,933 | 1.00× |
| 16,384 | 17,953,000 | 17,981,367 | 1.00× |

### 6.2 Active Waits Overview

![Timer Waits Overview](charts/io_uring/timer_waits_overview.png)

**Reference point: update_rounds=500**

| Timer Count | bnio waits/s | asio waits/s | Ratio |
| ---: | ---: | ---: | ---: |
| 256 | 9,255,847 | 9,356,003 | 0.99× |
| 1,024 | 9,711,693 | 10,782,433 | 0.90× |
| 4,096 | 10,179,500 | 10,212,000 | 1.00× |
| 16,384 | 10,232,567 | 10,248,800 | 1.00× |

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

- Configuration: timers=16,384, rounds=100
- bnio: 17,746,500 lifecycle calls/s
- asio: 17,280,167 lifecycle calls/s
- Ratio: 1.03×

**Most challenging bnio / asio lifecycle ratio:**

- Configuration: timers=1,024, rounds=500
- bnio: 17,039,100 lifecycle calls/s
- asio: 18,917,667 lifecycle calls/s
- Ratio: 0.90×

### 6.7 Full Results

#### Timer count = 256

| Backend | Timers | Rounds | lifecycle/s | waits/s |
| --- | ---: | ---: | ---: | ---: |
| asio | 256 | 100 | 14,061,700 | 7,934,260 |
| asio | 256 | 500 | 16,415,000 | 9,356,003 |
| asio | 256 | 1000 | 17,095,667 | 9,756,440 |
| bnio | 256 | 100 | 12,722,767 | 7,178,767 |
| bnio | 256 | 500 | 16,239,333 | 9,255,847 |
| bnio | 256 | 1000 | 16,800,133 | 9,587,743 |

#### Timer count = 1,024

| Backend | Timers | Rounds | lifecycle/s | waits/s |
| --- | ---: | ---: | ---: | ---: |
| asio | 1,024 | 100 | 17,038,800 | 9,614,083 |
| asio | 1,024 | 500 | 18,917,667 | 10,782,433 |
| asio | 1,024 | 1000 | 18,793,000 | 10,725,100 |
| bnio | 1,024 | 100 | 15,492,767 | 8,741,717 |
| bnio | 1,024 | 500 | 17,039,100 | 9,711,693 |
| bnio | 1,024 | 1000 | 17,504,033 | 9,989,473 |

#### Timer count = 4,096

| Backend | Timers | Rounds | lifecycle/s | waits/s |
| --- | ---: | ---: | ---: | ---: |
| asio | 4,096 | 100 | 16,809,633 | 9,484,767 |
| asio | 4,096 | 500 | 17,916,933 | 10,212,000 |
| asio | 4,096 | 1000 | 18,166,300 | 10,367,433 |
| bnio | 4,096 | 100 | 16,146,600 | 9,110,650 |
| bnio | 4,096 | 500 | 17,859,867 | 10,179,500 |
| bnio | 4,096 | 1000 | 18,051,167 | 10,301,700 |

#### Timer count = 16,384

| Backend | Timers | Rounds | lifecycle/s | waits/s |
| --- | ---: | ---: | ---: | ---: |
| asio | 16,384 | 100 | 17,280,167 | 9,750,267 |
| asio | 16,384 | 500 | 17,981,367 | 10,248,800 |
| asio | 16,384 | 1000 | 18,031,667 | 10,290,567 |
| bnio | 16,384 | 100 | 17,746,500 | 10,013,407 |
| bnio | 16,384 | 500 | 17,953,000 | 10,232,567 |
| bnio | 16,384 | 1000 | 17,723,367 | 10,114,633 |

### 6.8 Interpretation

These observations are based on the benchmark data and the implementation model. They have not been independently validated with profiling in this run.

1. **bnio trails in timer churn.** The average lifecycle ratio is 0.96×, with bnio winning 1 of 12 configurations.
2. **The backend gap is configuration-dependent.** At small timer counts the ratios are noisy, while asio holds the broader edge across the 1,024–16,384-timer cells in this run.
3. **Active waits mirror lifecycle throughput.** The waits/s ratios follow the same ordering as the lifecycle ratios, confirming that the measurements reflect real timer-completion work.
4. **All timer configurations completed cleanly.** No retries or replacements were required in this 3-iteration run.

### 6.9 Performance Change vs Previous Run (2026-08-04)

The previous report measured a 1-iteration quick run on 2026-08-04. This run uses 3-iteration means. The code changes listed in Section 5.10 affect both control-plane and wake-up paths, and are the main candidates for any timer-throughput movement.

| Metric | Previous (Aug 4) | Current (Aug 14) | Change |
| --- | ---: | ---: | ---: |
| Overall avg lifecycle ratio | 1.081× | 0.965× | -10.75% |
| bnio wins | 11/12 | 1/12 | -10 |
| Best single-config ratio | 1.179× | 1.03× | -12.89% |
| bnio peak lifecycle/s | 20,040,900 | 18,051,167 | -9.93% |
| asio peak lifecycle/s | 18,837,600 | 18,917,667 | +0.43% |

Timer performance **regressed** compared with the previous quick run (-10.75%), with bnio's win count falling from 11/12 to 1/12. The comparison combines code changes and the transition from one iteration to three-iteration means, so it is not a clean A/B run; however, the lifecycle-ratio drop is consistent across most timer counts.
