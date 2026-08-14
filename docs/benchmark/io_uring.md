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
- Client exit statuses are checked for every run; server startup is checked before each client run.

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

Both servers started successfully for every configuration and all 288 measured client runs completed cleanly.

Overall average throughput ratio (bnio / asio): **1.033×** across all 48 per-configuration ratios.

### Part B — Timer Churn

Both backends completed every configuration successfully. All 72 measured timer runs completed cleanly.

Overall average lifecycle throughput ratio (bnio / asio): **0.966×** across all 12 per-configuration ratios.

---

## Part A — TCP Echo Throughput Results

### 5.1 Throughput Overview

![Throughput Overview](charts/io_uring/overview_bars.png)

**Reference point: workers=4, connections=256**

| Message Size | bnio req/s | bnio err | asio req/s | asio err | Ratio |
| --- | ---: | ---: | ---: | ---: | ---: |
| 64 B | 1,019,081 | 0 | 830,254 | 0 | 1.23× |
| 1 KB | 949,206 | 0 | 810,618 | 0 | 1.17× |
| 4 KB | 838,666 | 0 | 742,754 | 0 | 1.13× |
| 64 KB | 6,282 | 0 | 6,225 | 0 | 1.01× |

### 5.2 Throughput vs Connections

![Throughput vs Connections](charts/io_uring/throughput_vs_connections.png)

*Workers=4, faceted by message size.*

### 5.3 Throughput vs Worker Threads

![Throughput vs Workers](charts/io_uring/throughput_vs_workers.png)

*Connections=256, faceted by message size.*

| Workers | bnio req/s | bnio err | asio req/s | asio err | Ratio |
| ---: | ---: | ---: | ---: | ---: | ---: |
| 1 | 346,461 | 0 | 358,360 | 0 | 0.97× |
| 2 | 583,694 | 0 | 617,810 | 0 | 0.94× |
| 4 | 838,666 | 0 | 742,754 | 0 | 1.13× |
| 8 | 781,538 | 0 | 761,487 | 0 | 1.03× |

**Worker-scaling at 4 KB / 256 connections.** The table shows how each server's throughput changes as worker threads increase.

### 5.4 Connection Scaling

| Connections | bnio req/s | bnio err | asio req/s | asio err | Ratio |
| ---: | ---: | ---: | ---: | ---: | ---: |
| 64 | 804,467 | 0 | 716,554 | 0 | 1.12× |
| 256 | 838,666 | 0 | 742,754 | 0 | 1.13× |
| 1024 | 776,661 | 0 | 712,623 | 0 | 1.09× |

**Connection-scaling at 4 KB / workers=4.** Shows how each server handles increasing concurrency.

### 5.5 bnio / asio Throughput Ratio Heatmap

![Heatmap](charts/io_uring/heatmap_ratio.png)

*Workers=4. Positive values (blue) = bnio faster; negative (red) = asio faster.*

### 5.6 Worker-Scaling Ratio Heatmap

![Worker Scaling](charts/io_uring/worker_scaling_heatmap.png)

*Connections=256. Shows how the bnio/asio ratio changes as worker threads increase.*

### 5.7 Extreme Cases

**Best bnio / asio throughput ratio (zero-error):**

- Configuration: workers=4, connections=256, message_size=64 B
- bnio: 1,019,081 req/s
- asio: 830,254 req/s
- Ratio: 1.23×

**Most challenging bnio / asio throughput ratio (zero-error):**

- Configuration: workers=1, connections=1024, message_size=4 KB
- bnio: 273,510 req/s
- asio: 304,042 req/s
- Ratio: 0.90×

### 5.8 Full Results (workers=4)

#### Message size = 64 B

| Server | Workers | Conns | req/s | MB/s |
| --- | ---: | ---: | ---: | ---: |
| asio | 4 | 64 | 804,364 | 49 |
| asio | 4 | 256 | 830,254 | 50 |
| asio | 4 | 1024 | 920,691 | 56 |
| bnio | 4 | 64 | 964,660 | 58 |
| bnio | 4 | 256 | 1,019,081 | 62 |
| bnio | 4 | 1024 | 1,021,454 | 62 |

#### Message size = 1 KB

| Server | Workers | Conns | req/s | MB/s |
| --- | ---: | ---: | ---: | ---: |
| asio | 4 | 64 | 786,349 | 768 |
| asio | 4 | 256 | 810,618 | 791 |
| asio | 4 | 1024 | 876,083 | 855 |
| bnio | 4 | 64 | 910,687 | 889 |
| bnio | 4 | 256 | 949,206 | 926 |
| bnio | 4 | 1024 | 960,715 | 937 |

#### Message size = 4 KB

| Server | Workers | Conns | req/s | MB/s |
| --- | ---: | ---: | ---: | ---: |
| asio | 4 | 64 | 716,554 | 2,798 |
| asio | 4 | 256 | 742,754 | 2,901 |
| asio | 4 | 1024 | 712,623 | 2,783 |
| bnio | 4 | 64 | 804,467 | 3,142 |
| bnio | 4 | 256 | 838,666 | 3,275 |
| bnio | 4 | 1024 | 776,661 | 3,033 |

#### Message size = 64 KB

| Server | Workers | Conns | req/s | MB/s |
| --- | ---: | ---: | ---: | ---: |
| asio | 4 | 64 | 1,560 | 97 |
| asio | 4 | 256 | 6,225 | 389 |
| asio | 4 | 1024 | 24,773 | 1,548 |
| bnio | 4 | 64 | 1,571 | 98 |
| bnio | 4 | 256 | 6,282 | 392 |
| bnio | 4 | 1024 | 25,393 | 1,587 |

### 5.9 Interpretation

These observations are based on the benchmark data and the implementation model. They have not been independently validated with profiling in this run.

1. **bnio leads on average.** The overall throughput ratio is 1.03×. bnio outperforms asio in 31 of 48 configurations.
2. **Worker scaling is uneven.** The strongest average is at 4 workers (1.11×); at 8 workers the average is 1.02×, and at 1 worker it is 0.99×.
3. **Large messages converge.** At 64 KB the average ratio is 1.01×, consistent with both servers being bandwidth-limited on loopback and per-message syscall costs being amortized.
4. **Concurrency has a modest effect.** The average ratio is 1.02× at 1,024 connections, versus 1.05× at 64 connections.
5. **No failures across the full 3-iteration matrix.** All 288 throughput client runs completed with zero failures.

### 5.10 Performance Change vs Previous Run (2026-08-14 release/v0.0.8)

Since the previous release/v0.0.8 io_uring report, this branch has merged scheduler/worker-counter consolidation (`f13ceb7`), io_uring cancellation and EAGAIN inflight tracking (`9b59ea7`), native-availability and worker-TLS cleanup (`30cb59f`), and a non-functional cleanup (`96dea0c`). A worker-scheduling documentation sync (`15c5416`) is also present. These changes are not benchmark-specific optimizations, but they do touch io_uring submit, scheduler, and run-loop paths.

| Metric | Previous (Aug 14 release) | Current (Aug 14 main) | Change |
| --- | ---: | ---: | ---: |
| Overall avg ratio | 1.031× | 1.033× | +0.17% |
| bnio wins | 31/48 | 31/48 | +0 |
| w=1 avg ratio | 0.994× | 0.991× | -0.26% |
| w=8 avg ratio | 1.022× | 1.022× | -0.01% |

Throughput performance is **broadly stable** compared with the previous release run (+0.17%). Both runs use three-iteration means, so small differences should be interpreted as code-behavior rather than sampling-methodology effects.

---

## Part B — Timer Churn Results

### 6.1 Lifecycle Throughput Overview

![Timer Lifecycle Overview](charts/io_uring/timer_lifecycle_overview.png)

**Reference point: update_rounds=500**

| Timer Count | bnio lifecycle/s | asio lifecycle/s | Ratio |
| ---: | ---: | ---: | ---: |
| 256 | 15,820,333 | 17,533,200 | 0.90× |
| 1,024 | 17,515,600 | 18,857,267 | 0.93× |
| 4,096 | 17,956,367 | 17,957,633 | 1.00× |
| 16,384 | 17,998,100 | 17,869,700 | 1.01× |

### 6.2 Active Waits Overview

![Timer Waits Overview](charts/io_uring/timer_waits_overview.png)

**Reference point: update_rounds=500**

| Timer Count | bnio waits/s | asio waits/s | Ratio |
| ---: | ---: | ---: | ---: |
| 256 | 9,017,020 | 9,993,327 | 0.90× |
| 1,024 | 9,983,300 | 10,747,967 | 0.93× |
| 4,096 | 10,234,500 | 10,235,233 | 1.00× |
| 16,384 | 10,258,333 | 10,185,100 | 1.01× |

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
- bnio: 18,130,600 lifecycle calls/s
- asio: 17,721,500 lifecycle calls/s
- Ratio: 1.02×

**Most challenging bnio / asio lifecycle ratio:**

- Configuration: timers=1,024, rounds=100
- bnio: 13,880,633 lifecycle calls/s
- asio: 15,652,467 lifecycle calls/s
- Ratio: 0.89×

### 6.7 Full Results

#### Timer count = 256

| Backend | Timers | Rounds | lifecycle/s | waits/s |
| --- | ---: | ---: | ---: | ---: |
| asio | 256 | 100 | 13,310,833 | 7,510,573 |
| asio | 256 | 500 | 17,533,200 | 9,993,327 |
| asio | 256 | 1000 | 18,006,733 | 10,276,340 |
| bnio | 256 | 100 | 12,479,233 | 7,041,357 |
| bnio | 256 | 500 | 15,820,333 | 9,017,020 |
| bnio | 256 | 1000 | 17,488,833 | 9,980,780 |

#### Timer count = 1,024

| Backend | Timers | Rounds | lifecycle/s | waits/s |
| --- | ---: | ---: | ---: | ---: |
| asio | 1,024 | 100 | 15,652,467 | 8,831,847 |
| asio | 1,024 | 500 | 18,857,267 | 10,747,967 |
| asio | 1,024 | 1000 | 19,189,300 | 10,951,200 |
| bnio | 1,024 | 100 | 13,880,633 | 7,832,093 |
| bnio | 1,024 | 500 | 17,515,600 | 9,983,300 |
| bnio | 1,024 | 1000 | 17,672,967 | 10,085,860 |

#### Timer count = 4,096

| Backend | Timers | Rounds | lifecycle/s | waits/s |
| --- | ---: | ---: | ---: | ---: |
| asio | 4,096 | 100 | 16,906,267 | 9,539,287 |
| asio | 4,096 | 500 | 17,957,633 | 10,235,233 |
| asio | 4,096 | 1000 | 18,187,400 | 10,379,467 |
| bnio | 4,096 | 100 | 17,210,733 | 9,711,077 |
| bnio | 4,096 | 500 | 17,956,367 | 10,234,500 |
| bnio | 4,096 | 1000 | 18,067,600 | 10,311,100 |

#### Timer count = 16,384

| Backend | Timers | Rounds | lifecycle/s | waits/s |
| --- | ---: | ---: | ---: | ---: |
| asio | 16,384 | 100 | 17,721,500 | 9,999,273 |
| asio | 16,384 | 500 | 17,869,700 | 10,185,100 |
| asio | 16,384 | 1000 | 17,997,133 | 10,270,867 |
| bnio | 16,384 | 100 | 18,130,600 | 10,230,087 |
| bnio | 16,384 | 500 | 17,998,100 | 10,258,333 |
| bnio | 16,384 | 1000 | 18,020,267 | 10,284,100 |

### 6.8 Interpretation

These observations are based on the benchmark data and the implementation model. They have not been independently validated with profiling in this run.

1. **bnio trails in timer churn.** The average lifecycle ratio is 0.97×, with bnio winning 4 of 12 configurations.
2. **The backend gap is configuration-dependent.** The tightest timer-count family is 1,024 (0.91×); the strongest is 16,384 (1.01×).
3. **Active waits mirror lifecycle throughput.** The waits/s ratios follow the same ordering as the lifecycle ratios, confirming that the measurements reflect real timer-completion work.
4. **All timer configurations completed cleanly.** No retries or replacements were required in this 3-iteration run.

### 6.9 Performance Change vs Previous Run (2026-08-14 release/v0.0.8)

The previous release/v0.0.8 report measured three-iteration means on 2026-08-14. This run uses the same three-iteration method on main. The code changes listed in Section 5.10 affect submit, scheduler, and run-loop paths, and are the main candidates for any timer-throughput movement.

| Metric | Previous (Aug 14 release) | Current (Aug 14 main) | Change |
| --- | ---: | ---: | ---: |
| Overall avg lifecycle ratio | 0.965× | 0.966× | +0.09% |
| bnio wins | 1/12 | 4/12 | +3 |
| Best single-config ratio | 1.03× | 1.02× | -0.67% |
| bnio peak lifecycle/s | 18,051,167 | 18,130,600 | +0.44% |
| asio peak lifecycle/s | 18,917,667 | 19,189,300 | +1.44% |

Timer performance is **broadly stable** compared with the previous release run (+0.09%). Both runs use three-iteration means, so small differences should be interpreted as code-behavior rather than sampling-methodology noise.
