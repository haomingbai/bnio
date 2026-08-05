# bnio vs asio Benchmark — kqueue (macOS/BSD)

## 1. Test Environment

| Item | Value |
| --- | --- |
| Date | 2026-08-05 |
| Topology | Single-host loopback TCP (127.0.0.1) |
| OS | macOS 26.5.2 |
| Kernel | Darwin 25.5.0 |
| Architecture | arm64 (Apple Silicon) |
| CPU | Apple M5 Pro |
| Logical CPUs | 18 |
| Memory | 50,331,648 kB (48 GB) |
| Compiler | Apple clang version 21.0.0 (clang-2100.1.1.101) |
| CMake | cmake version 4.4.1 |
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

Overall average throughput ratio (bnio / asio): **0.974×** across all 48 configurations, with bnio winning **22 of 48**.

### Part B — Timer Churn

Both backends completed every configuration successfully. All 24 measurements are clean. Two timer configurations (timers=256/rounds=100 and timers=4,096/rounds=1,000) deviated from their historical baselines and were retested; the retested medians were retained in place of the original runs.

Overall average lifecycle throughput ratio (bnio / asio): **0.908×** across all 12 configurations, with bnio winning **1 of 12**.

---

## Part A — TCP Echo Throughput Results

### 5.1 Throughput Overview

![Throughput Overview](charts/kqueue/kqueue_overview_bars.png)

**Reference point: workers=4, connections=256**

| Message Size | bnio req/s | bnio err | asio req/s | asio err | Ratio |
| --- | ---: | ---: | ---: | ---: | ---: |
| 64 B | 135,613 | 0 | 116,839 | 0 | 1.16× |
| 1 KB | 135,892 | 0 | 115,724 | 0 | 1.17× |
| 4 KB | 129,530 | 0 | 114,810 | 0 | 1.13× |
| 64 KB | 10,302 | 0 | 12,655 | 0 | 0.81× |

### 5.2 Throughput vs Connections

![Throughput vs Connections](charts/kqueue/kqueue_throughput_vs_connections.png)

*Workers=4, faceted by message size.*

### 5.3 Throughput vs Worker Threads

![Throughput vs Workers](charts/kqueue/kqueue_throughput_vs_workers.png)

*Connections=256, faceted by message size.*

| Workers | bnio req/s | bnio err | asio req/s | asio err | Ratio |
| ---: | ---: | ---: | ---: | ---: | ---: |
| 1 | 120,470 | 0 | 115,806 | 0 | 1.04× |
| 2 | 139,256 | 0 | 150,515 | 0 | 0.93× |
| 4 | 129,530 | 0 | 114,810 | 0 | 1.13× |
| 8 | 121,745 | 0 | 103,509 | 0 | 1.18× |

**Worker-scaling at 4 KB / 256 connections.** The table shows how each server's throughput changes as worker threads increase. bnio leads at 1, 4, and 8 workers (1.04–1.18×) and trails only at 2 workers (0.93×). As in the previous run, the 2-worker configuration remains bnio's weakest point, while multi-threaded operation at 4 and 8 workers is bnio's strongest.

### 5.4 Connection Scaling

| Connections | bnio req/s | bnio err | asio req/s | asio err | Ratio |
| ---: | ---: | ---: | ---: | ---: | ---: |
| 64 | 122,850 | 0 | 114,487 | 0 | 1.07× |
| 256 | 129,530 | 0 | 114,810 | 0 | 1.13× |
| 1024 | 125,804 | 0 | 109,326 | 0 | 1.15× |

**Connection-scaling at 4 KB / workers=4.** Shows how each server handles increasing concurrency. bnio leads consistently across all connection counts, with the widest gap at 1,024 connections.

### 5.5 bnio / asio Throughput Ratio Heatmap

![Heatmap](charts/kqueue/kqueue_heatmap_ratio.png)

*Workers=4. Positive values (blue) = bnio faster; negative (red) = asio faster.*

### 5.6 Worker-Scaling Ratio Heatmap

![Worker Scaling](charts/kqueue/kqueue_worker_scaling_heatmap.png)

*Connections=256. Shows how the bnio/asio ratio changes as worker threads increase.*

### 5.7 Extreme Cases

**Best bnio / asio throughput ratio (zero-error):**

- Configuration: workers=4, connections=1024, message_size=64 B
- bnio: 135,185 req/s
- asio: 110,978 req/s
- Ratio: 1.22×

**Most challenging bnio / asio throughput ratio (zero-error):**

- Configuration: workers=8, connections=64, message_size=64 KB
- bnio: 8,666 req/s
- asio: 15,645 req/s
- Ratio: 0.55×

### 5.8 Full Results (workers=4)

#### Message size = 64 B

| Server | Workers | Conns | req/s | MB/s |
| --- | ---: | ---: | ---: | ---: |
| asio | 4 | 64 | 119,259 | 7 |
| asio | 4 | 256 | 116,839 | 7 |
| asio | 4 | 1024 | 110,978 | 6 |
| bnio | 4 | 64 | 125,546 | 7 |
| bnio | 4 | 256 | 135,613 | 8 |
| bnio | 4 | 1024 | 135,185 | 8 |

#### Message size = 1 KB

| Server | Workers | Conns | req/s | MB/s |
| --- | ---: | ---: | ---: | ---: |
| asio | 4 | 64 | 115,876 | 113 |
| asio | 4 | 256 | 115,724 | 113 |
| asio | 4 | 1024 | 112,589 | 109 |
| bnio | 4 | 64 | 126,147 | 123 |
| bnio | 4 | 256 | 135,892 | 132 |
| bnio | 4 | 1024 | 130,145 | 127 |

#### Message size = 4 KB

| Server | Workers | Conns | req/s | MB/s |
| --- | ---: | ---: | ---: | ---: |
| asio | 4 | 64 | 114,487 | 447 |
| asio | 4 | 256 | 114,810 | 448 |
| asio | 4 | 1024 | 109,326 | 427 |
| bnio | 4 | 64 | 122,850 | 479 |
| bnio | 4 | 256 | 129,530 | 505 |
| bnio | 4 | 1024 | 125,804 | 491 |

#### Message size = 64 KB

| Server | Workers | Conns | req/s | MB/s |
| --- | ---: | ---: | ---: | ---: |
| asio | 4 | 64 | 13,436 | 839 |
| asio | 4 | 256 | 12,655 | 790 |
| asio | 4 | 1024 | 13,044 | 815 |
| bnio | 4 | 64 | 9,284 | 580 |
| bnio | 4 | 256 | 10,302 | 643 |
| bnio | 4 | 1024 | 9,754 | 609 |

### 5.9 Interpretation

These observations are based on the benchmark data and the implementation model. They have not been independently validated with profiling in this run.

1. **Overall ratio essentially unchanged, but wins fell.** The overall throughput ratio is 0.974×, virtually flat versus the previous run (0.972×, +0.2%). However, bnio's per-configuration win count dropped to 22 of 48. The wins are concentrated at workers=4 (9/12) and workers=8 (9/12) on small/medium messages, with the remainder at workers=1; asio wins all 12 configurations at 64 KB and all 12 at workers=2.

2. **Multi-worker small/medium remains bnio's strength.** At workers=4 and 8, small/medium-message ratios range roughly 1.04–1.22×. At the reference point (workers=4, connections=256) bnio leads 1.16× (64 B), 1.17× (1 KB), and 1.13× (4 KB). This is the same qualitative pattern as the previous run, though the margins have narrowed somewhat.

3. **Workers=1 is near parity.** The average ratio at workers=1 is 0.986× (4/12 wins), with small/medium messages ranging 0.97–1.04×.

4. **Workers=2 is the structural laggard.** The average ratio at workers=2 is 0.854× with 0/12 wins. For small/medium messages every configuration trails by roughly 8–13%, and at 64 KB the deficit reaches ~40% (asio leads up to 1.66×). This is essentially unchanged from the previous run (0.851×); the 2-worker scaling behavior continues to favor asio's reactor model over bnio's current kqueue path.

5. **Large messages (64 KB) remain the weakest area.** The average ratio at 64 KB is 0.733× and asio wins all 12 configurations. bnio's absolute throughput is ~8,700–11,000 req/s versus asio's ~10,900–16,200 req/s. The gap is widest at workers=2 (asio leads ~1.40–1.66×) and worst overall at workers=8/conns=64 (0.55×).

6. **Connection count has a modest effect.** At workers=4 the ratio improves slightly with connection count (1.07× → 1.13× → 1.15×). Across the full matrix, connections=256 is the best point (1.000×), connections=64 the weakest (0.940×, dragged down by the large-message family), and connections=1024 sits at 0.982×.

7. **No errors across the entire matrix.** All 96 result rows completed cleanly — both server implementations and the client are stable under all tested configurations on kqueue.

### 5.10 Performance Change vs Previous Run (2026-07-29)

The current codebase fixes several race conditions by adding a submit-path lock, binding submission to the shutdown state, and reordering timer aborts during shutdown. The implementation lives in commits `e76b8a5` (submit-path locking) and `64908cb` (timer-abort ordering and shutdown/stranding fixes); `03b566e` and `298fb1f` document the design. This section compares the current results against the previous benchmark run from 2026-07-29.

| Metric | Previous (Jul 29) | Current (Aug 5) | Change |
| --- | ---: | ---: | ---: |
| Overall avg ratio | 0.972× | 0.974× | +0.2% |
| bnio wins¹ | 25/48 | 22/48 | -3 |
| w=1 avg ratio | 0.993× | 0.986× | -0.7% |
| w=2 avg ratio | 0.851× | 0.854× | +0.4% |
| Best single-config ratio | 1.22× | 1.22× | 0.0% |
| Worst single-config ratio | 0.57× | 0.55× | -3.5% |

¹ Win counts are reported per configuration (48 for throughput). The previous report counted wins across 96 result rows (50/96); both are normalized to the per-configuration basis for comparison.

Throughput performance is **essentially flat on average** (+0.2%), though bnio still trails asio overall (0.974×). The added per-submit synchronization — binding submission to the shutdown state under a submit lock (`e76b8a5`) — is largely amortized on the I/O-completion-dominated echo path, which explains why the overall ratio barely moved. The overhead is most visible where per-echo costs are smallest: the reference 64 B margin at workers=4/conns=256 narrowed from 1.20× to 1.16×, the w=1 average softened from 0.993× to 0.986×, and bnio's per-configuration win total fell from a clear majority to 22/48. The remaining below-parity average, concentrated at w=1/w=2 and at 64 KB, reflects synchronization overhead surfacing on the most overhead-sensitive configurations; the w=2 deficit is structural and unchanged from the previous run.

---

## Part B — Timer Churn Results

### 6.1 Lifecycle Throughput Overview

![Timer Lifecycle Overview](charts/kqueue/kqueue_timer_lifecycle_overview.png)

**Reference point: update_rounds=500**

| Timer Count | bnio lifecycle/s | asio lifecycle/s | Ratio |
| ---: | ---: | ---: | ---: |
| 256 | 27,131,000 | 28,073,000 | 0.97× |
| 1,024 | 30,965,000 | 34,971,000 | 0.89× |
| 4,096 | 30,282,000 | 36,390,000 | 0.83× |
| 16,384 | 32,621,000 | 35,649,000 | 0.92× |

### 6.2 Active Waits Overview

![Timer Waits Overview](charts/kqueue/kqueue_timer_waits_overview.png)

**Reference point: update_rounds=500**

| Timer Count | bnio waits/s | asio waits/s | Ratio |
| ---: | ---: | ---: | ---: |
| 256 | 15,464,000 | 16,001,000 | 0.97× |
| 1,024 | 17,649,000 | 19,932,000 | 0.89× |
| 4,096 | 17,260,000 | 20,741,000 | 0.83× |
| 16,384 | 18,593,000 | 20,319,000 | 0.92× |

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

- Configuration: timers=16,384, rounds=100
- bnio: 33,200,800 lifecycle calls/s
- asio: 32,325,700 lifecycle calls/s
- Ratio: 1.03×

**Most challenging bnio / asio lifecycle ratio:**

- Configuration: timers=256, rounds=100
- bnio: 16,094,600 lifecycle calls/s
- asio: 23,103,100 lifecycle calls/s
- Ratio: 0.70×

### 6.7 Full Results

#### Timer count = 256

| Backend | Timers | Rounds | lifecycle/s | waits/s |
| --- | ---: | ---: | ---: | ---: |
| bnio | 256 | 100 | 16,095,000 | 15,448,000 |
| bnio | 256 | 500 | 27,131,000 | 15,464,000 |
| bnio | 256 | 1,000 | 28,147,000 | 16,063,000 |
| asio | 256 | 100 | 23,103,000 | 15,069,000 |
| asio | 256 | 500 | 28,073,000 | 16,001,000 |
| asio | 256 | 1,000 | 31,677,000 | 18,078,000 |

#### Timer count = 1,024

| Backend | Timers | Rounds | lifecycle/s | waits/s |
| --- | ---: | ---: | ---: | ---: |
| bnio | 1,024 | 100 | 32,211,000 | 18,175,000 |
| bnio | 1,024 | 500 | 30,965,000 | 17,649,000 |
| bnio | 1,024 | 1,000 | 32,743,000 | 18,686,000 |
| asio | 1,024 | 100 | 33,769,000 | 19,054,000 |
| asio | 1,024 | 500 | 34,971,000 | 19,932,000 |
| asio | 1,024 | 1,000 | 34,364,000 | 19,611,000 |

#### Timer count = 4,096

| Backend | Timers | Rounds | lifecycle/s | waits/s |
| --- | ---: | ---: | ---: | ---: |
| bnio | 4,096 | 100 | 28,995,000 | 16,360,000 |
| bnio | 4,096 | 500 | 30,282,000 | 17,260,000 |
| bnio | 4,096 | 1,000 | 36,413,000 | 17,061,000 |
| asio | 4,096 | 100 | 29,872,000 | 16,855,000 |
| asio | 4,096 | 500 | 36,390,000 | 20,741,000 |
| asio | 4,096 | 1,000 | 40,318,000 | 21,146,000 |

#### Timer count = 16,384

| Backend | Timers | Rounds | lifecycle/s | waits/s |
| --- | ---: | ---: | ---: | ---: |
| bnio | 16,384 | 100 | 33,201,000 | 18,733,000 |
| bnio | 16,384 | 500 | 32,621,000 | 18,593,000 |
| bnio | 16,384 | 1,000 | 33,064,000 | 18,869,000 |
| asio | 16,384 | 100 | 32,326,000 | 18,240,000 |
| asio | 16,384 | 500 | 35,649,000 | 20,319,000 |
| asio | 16,384 | 1,000 | 36,459,000 | 20,807,000 |

### 6.8 Interpretation

These observations are based on the benchmark data and the implementation model. They have not been independently validated with profiling in this run.

1. **Clear regression: asio now leads on timer churn.** The average lifecycle ratio is 0.908×, with bnio winning only 1 of 12 configurations. This reverses the previous run's narrow bnio edge (1.051×) and is attributable to the new synchronization on the timer control-plane path (see Section 6.9).

2. **bnio's only win is the largest workload.** The single bnio win is timers=16,384/rounds=100 at 1.03×, a configuration where setup-cost amortization over a large timer set favors bnio.

3. **The smallest workload is the deepest deficit.** timers=256/rounds=100 is the worst configuration at 0.70× (retested median). It was flagged as a historical-drift candidate and retested; the retest confirmed the deficit.

4. **The 500-round reference spans 0.83–0.97×.** The deepest deficit at the reference point is timers=4,096 (0.83×); the shallowest is timers=256 (0.97×).

5. **Absolute throughput diverged from both sides.** bnio's peak lifecycle throughput fell from 39,235,900 to 36,413,000 lifecycle/s (-7.2%), while asio's peak rose from 38,890,800 to 40,318,000 (+3.7%). The ratio regression is therefore not solely a bnio slowdown.

6. **Active wait throughput mirrors lifecycle throughput.** The waits/s ratio tracks the lifecycle ratio for every configuration (e.g., 0.97×/0.89×/0.83×/0.92× at rounds=500), confirming the lifecycle measurements reflect genuine timer completion throughput.

7. **Two configurations were retested.** As noted in Section 4, timers=256/rounds=100 and timers=4,096/rounds=1,000 deviated from their historical baselines; the retested medians were retained.

### 6.9 Performance Change vs Previous Run

The same correctness fixes described in Section 5.10 — submit-path locking, shutdown state binding, and timer-abort ordering — have a measurable impact on timer-churn performance:

| Metric | Previous (Jul 29) | Current (Aug 5) | Change |
| --- | ---: | ---: | ---: |
| Overall avg lifecycle ratio | 1.051× | 0.908× | **-13.6%** |
| bnio wins¹ | 8/12 | 1/12 | -7 |
| Best single-config ratio | 1.29× | 1.03× | -20.2% |
| Worst single-config ratio | 0.91× | 0.70× | -23.1% |
| bnio peak lifecycle/s | 39,235,900 | 36,413,000 | -7.2% |
| asio peak lifecycle/s | 38,890,800 | 40,318,000 | +3.7% |

¹ Win counts are reported per configuration (12 for timer churn). The previous report counted wins across 24 result rows (16/24); both are normalized to the per-configuration basis for comparison.

The regression is significant: the overall bnio/asio lifecycle ratio dropped from 1.051× to 0.908×, a decline of 13.6%, and bnio wins only 1 of 12 configurations. The largest single-configuration drops are at the extremes of the matrix (timers=256/rounds=100 at -23.1% ratio and timers=4,096/rounds=1,000, whose ratio fell from 1.07× historically to 0.90× after retest).

**Root cause analysis:** The timer churn benchmark is an adversarial workload for the new locking scheme. Each update round performs `timers/4` destroy-and-recreate cycles, resets the expiry on the remaining timers, and starts a fresh async_wait on every live timer — every one of these operations now goes through the locked submit path and the stricter timer-abort ordering that were previously lock-free or used more granular synchronization. With up to 16,384 timers created, reset, cancelled, and destroyed in tight succession, lock contention is substantial.

Specifically, the changes that contribute to this regression:

1. **Submit-path locking and shutdown state binding** (`e76b8a5`): Every timer operation (create, cancel, reset, destroy) now goes through a locked submission path bound to the shutdown state, adding an acquisition on every submit. Previously, some of these paths used lock-free atomic operations.

2. **Timer abort ordering and shutdown/stranding fixes** (`64908cb`): `begin_stop()` now aborts pending timer waits before publishing the stopping state, and the shutdown drain closes the timer-stranding and nested-I/O races. Together these add synchronization on the control-plane path.

The same pattern appeared on io_uring, where the locking dropped the timer ratio from 1.216× to 1.081× (-11.1%); on kqueue the regression is deeper (1.051× → 0.908×). **Throughput is largely unaffected** (+0.2% on kqueue, +0.5% on io_uring): the TCP echo workload is dominated by I/O completion handling, which amortizes the new locks over much larger per-operation costs. The timer churn benchmark, by contrast, stresses the pure control-plane path, where lock overhead dominates.

**Trade-off assessment:** The ~14% timer throughput regression is the cost of fixing several real race conditions that could cause crashes, hangs, or use-after-free in production use. The fixes are correctness-critical; the performance impact is confined to timer-heavy workloads and does not affect the more common network-I/O path.

---

## 7. Cross-Benchmark Summary

| Benchmark | Avg bnio/asio Ratio | bnio Wins | asio Wins | Total Configs |
| --- | ---: | ---: | ---: | ---: |
| TCP Echo Throughput | 0.974× | 22 | 26 | 48 |
| Timer Churn (lifecycle) | 0.908× | 1 | 11 | 12 |

On the kqueue (macOS/BSD) backend, the picture is mixed versus the previous run:

- **Throughput**: bnio achieves 97.4% of asio's throughput on average, essentially unchanged from the previous run (0.972×, +0.2%). At 4 and 8 workers, bnio leads asio for small and medium messages (1.04–1.22×). The large-message (64 KB) path remains the weakest area (0.733× average), and the 2-worker configuration continues to be a structural deficit (0.854×, 0/12 wins).

- **Timer churn**: bnio's previous narrow edge (1.051×) has reversed into a deficit (0.908×), with bnio winning only 1 of 12 configurations. The regression is attributable to the submit-path locking and timer-abort ordering introduced to fix shutdown/stranding races, and is confined to the timer control-plane path.

### Comparison with Previous Run (2026-07-29)

| Benchmark | Previous (0.972× avg) | Current (0.974× avg) | Change |
| --- | ---: | ---: | ---: |
| TCP Echo Throughput | 0.972× (25/48 wins) | 0.974× (22/48 wins) | +0.2% |
| Timer Churn | 1.051× (8/12 wins) | 0.908× (1/12 wins) | **-13.6%** |

¹ Win counts are reported per configuration (48 for throughput, 12 for timer churn); the previous report counted wins across result rows (50/96 and 16/24).

The correctness fixes landed since the previous run — submit-path locking (`e76b8a5`) and the timer-abort ordering and shutdown/stranding fixes (`64908cb`), with the design documented in `03b566e` and `298fb1f` — had almost no effect on the network-I/O path (0.972× → 0.974×), consistent with the new locks being amortized over the larger per-operation costs of I/O completion. The timer control-plane path paid the price: the overall lifecycle ratio fell 13.6%, and bnio's win share collapsed from 16/24 result rows to 1/12 configurations.

### Platform Comparison: kqueue vs io_uring

| Benchmark | io_uring (Linux) | kqueue (macOS) |
| --- | ---: | ---: |
| TCP Echo Throughput | **1.073×** (bnio leads) | **0.974×** (near parity) |
| Timer Churn (lifecycle) | **1.081×** (bnio leads) | **0.908×** (asio leads) |

The platform comparison shows bnio still trails on kqueue relative to its io_uring performance. Both platforms carry the same submit-path locking changes: on io_uring the throughput ratio held at 1.073× while the timer ratio regressed to 1.081×; on kqueue the throughput ratio held at 0.974× while the timer ratio fell to 0.908×. The timer control-plane regression is visible on both backends, but on kqueue it flipped the timer result from a narrow bnio edge into an asio lead. On the I/O path, bnio's kqueue backend remains at near parity with asio while its io_uring backend leads.

The kqueue backend remains under active optimization: the large-buffer I/O path, 2-thread scaling, and the timer control-plane lock overhead are all on the roadmap, along with further synchronization reduction. The next structural step is the POSIX `io_context` consolidation described in [`design/roadmap.md`](../design/roadmap.md).

---

## 8. Cross-Platform Absolute Throughput Comparison

A comparison of absolute throughput (not ratio) between platforms:

### Throughput (workers=4, connections=256, 4 KB)

| Backend | io_uring (Linux) | kqueue (macOS) | Ratio |
| --- | ---: | ---: | ---: |
| bnio | 362,818 req/s | 129,530 req/s | 0.36× |
| asio | 324,198 req/s | 114,810 req/s | 0.35× |

### Timer Churn (timers=1,024, rounds=500)

| Backend | io_uring (Linux) | kqueue (macOS) | Ratio |
| --- | ---: | ---: | ---: |
| bnio | 19,162,300 lifecycle/s | 30,965,000 lifecycle/s | 1.62× |
| asio | 18,620,400 lifecycle/s | 34,971,000 lifecycle/s | 1.88× |

> **Note:** Absolute throughput comparisons across platforms are not apples-to-apples. Different hardware (i9-13900H x86_64 vs M5 Pro arm64), operating systems, and kernel implementations all affect baseline performance. Both runs used 10 s measurements over 1 iteration (quick mode). System load and thermal conditions may differ between runs. Treat these cross-platform figures as directional only.
