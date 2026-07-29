# bnio vs asio Benchmark — kqueue (macOS/BSD)

## 1. Test Environment

| Item | Value |
| --- | --- |
| Date | 2026-07-29 |
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

Overall average throughput ratio (bnio / asio): **0.972×** across all 96 configurations.

### Part B — Timer Churn

Both backends completed every configuration successfully. All 24 configurations produced clean measurements.

Overall average lifecycle throughput ratio (bnio / asio): **1.051×** across all 24 configurations.

---

## Part A — TCP Echo Throughput Results

### 5.1 Throughput Overview

![Throughput Overview](charts/kqueue/kqueue_overview_bars.png)

**Reference point: workers=4, connections=256**

| Message Size | bnio req/s | bnio err | asio req/s | asio err | Ratio |
| --- | ---: | ---: | ---: | ---: | ---: |
| 64 B | 132,361 | 0 | 110,273 | 0 | 1.20× |
| 1 KB | 127,159 | 0 | 110,042 | 0 | 1.16× |
| 4 KB | 127,947 | 0 | 110,349 | 0 | 1.16× |
| 64 KB | 9,878 | 0 | 12,520 | 0 | 0.79× |

### 5.2 Throughput vs Connections

![Throughput vs Connections](charts/kqueue/kqueue_throughput_vs_connections.png)

*Workers=4, faceted by message size.*

### 5.3 Throughput vs Worker Threads

![Throughput vs Workers](charts/kqueue/kqueue_throughput_vs_workers.png)

*Connections=256, faceted by message size.*

| Workers | bnio req/s | bnio err | asio req/s | asio err | Ratio |
| ---: | ---: | ---: | ---: | ---: | ---: |
| 1 | 119,502 | 0 | 116,485 | 0 | 1.03× |
| 2 | 139,679 | 0 | 151,527 | 0 | 0.92× |
| 4 | 127,947 | 0 | 110,349 | 0 | 1.16× |
| 8 | 114,880 | 0 | 101,413 | 0 | 1.13× |

**Worker-scaling at 4 KB / 256 connections.** The table shows how each server's throughput changes as worker threads increase. bnio now leads at 1, 4, and 8 workers (1.03–1.16×) and trails only at 2 workers (0.92×), indicating the kqueue backend benefits significantly from multi-threaded operation and has closed the single-threaded gap present in the previous run.

### 5.4 Connection Scaling

| Connections | bnio req/s | bnio err | asio req/s | asio err | Ratio |
| ---: | ---: | ---: | ---: | ---: | ---: |
| 64 | 119,549 | 0 | 110,286 | 0 | 1.08× |
| 256 | 127,947 | 0 | 110,349 | 0 | 1.16× |
| 1024 | 123,865 | 0 | 107,532 | 0 | 1.15× |

**Connection-scaling at 4 KB / workers=4.** Shows how each server handles increasing concurrency. bnio leads consistently across all connection counts, with the widest gap at 256 connections.

### 5.5 bnio / asio Throughput Ratio Heatmap

![Heatmap](charts/kqueue/kqueue_heatmap_ratio.png)

*Workers=4. Positive values (blue) = bnio faster; negative (red) = asio faster.*

### 5.6 Worker-Scaling Ratio Heatmap

![Worker Scaling](charts/kqueue/kqueue_worker_scaling_heatmap.png)

*Connections=256. Shows how the bnio/asio ratio changes as worker threads increase.*

### 5.7 Extreme Cases

**Best bnio / asio throughput ratio (zero-error):**

- Configuration: workers=4, connections=1024, message_size=1 KB
- bnio: 131,062 req/s
- asio: 107,790 req/s
- Ratio: 1.22×

**Most challenging bnio / asio throughput ratio (zero-error):**

- Configuration: workers=8, connections=1024, message_size=64 KB
- bnio: 8,848 req/s
- asio: 15,629 req/s
- Ratio: 0.57×

### 5.8 Full Results (workers=4)

#### Message size = 64 B

| Server | Workers | Conns | req/s | MB/s |
| --- | ---: | ---: | ---: | ---: |
| asio | 4 | 64 | 115,019 | 7 |
| asio | 4 | 256 | 110,273 | 6 |
| asio | 4 | 1024 | 107,729 | 6 |
| bnio | 4 | 64 | 126,648 | 7 |
| bnio | 4 | 256 | 132,361 | 8 |
| bnio | 4 | 1024 | 130,617 | 7 |

#### Message size = 1 KB

| Server | Workers | Conns | req/s | MB/s |
| --- | ---: | ---: | ---: | ---: |
| asio | 4 | 64 | 114,261 | 111 |
| asio | 4 | 256 | 110,042 | 107 |
| asio | 4 | 1024 | 107,790 | 105 |
| bnio | 4 | 64 | 123,259 | 120 |
| bnio | 4 | 256 | 127,159 | 124 |
| bnio | 4 | 1024 | 131,062 | 127 |

#### Message size = 4 KB

| Server | Workers | Conns | req/s | MB/s |
| --- | ---: | ---: | ---: | ---: |
| asio | 4 | 64 | 110,286 | 430 |
| asio | 4 | 256 | 110,349 | 431 |
| asio | 4 | 1024 | 107,532 | 420 |
| bnio | 4 | 64 | 119,549 | 466 |
| bnio | 4 | 256 | 127,947 | 499 |
| bnio | 4 | 1024 | 123,865 | 483 |

#### Message size = 64 KB

| Server | Workers | Conns | req/s | MB/s |
| --- | ---: | ---: | ---: | ---: |
| asio | 4 | 64 | 13,307 | 831 |
| asio | 4 | 256 | 12,520 | 782 |
| asio | 4 | 1024 | 13,065 | 816 |
| bnio | 4 | 64 | 9,360 | 585 |
| bnio | 4 | 256 | 9,878 | 617 |
| bnio | 4 | 1024 | 9,279 | 579 |

### 5.9 Interpretation

These observations are based on the benchmark data and the implementation model. They have not been independently validated with profiling in this run.

1. **bnio now nearly reaches parity with asio on the kqueue backend.** The overall throughput ratio is 0.972×, up from 0.901× in the previous run (+7.9%). bnio wins 50 of 96 configurations, up from 42 previously. This improvement is attributable to the two recent commits `0979ac0` (*perf: modified the check method of kqueue*) and `eeb6805` (*perf: remove the fixed array in kqueue branch*), which streamlined the kqueue event-check path and eliminated per-event fixed-array overhead.

2. **Single-worker throughput improved the most.** At workers=1, the average ratio jumped from 0.873 to 0.993 (+0.12), nearly closing the gap. Small/medium messages at w=1 now range 0.98–1.03× (previously 0.79–0.82×). The check-method optimization particularly benefits the single-threaded event loop, where per-event overhead dominates.

3. **The multi-worker lead has expanded.** At workers=4 and 8, bnio's lead on small/medium messages widened to 1.07–1.22× (previously 1.02–1.10×). Removing the fixed array likely reduced contention on the multi-threaded submission path and improved cache locality.

4. **Large messages (64 KB) remain the weakest area.** At 64 KB, the average ratio is essentially flat at ~0.74×. Three single-threaded 64 KB configurations regressed: w=1/c=64/m=65536 dropped sharply from 1.04 to 0.84 (−0.20) — bnio's absolute throughput also fell from 10,737 to 9,392 req/s while asio rose from 10,283 to 11,139 req/s, a bidirectional worsening — w=1/c=1024/m=65536 narrowed from 1.085 to 1.021 (−0.06), and w=1/c=256/m=65536 narrowed from 1.063 to 1.024 (−0.04). The w=1/c=64 case is the only severe regression in this run and warrants targeted investigation.

5. **The 2-worker case is now the laggard.** At workers=2, the average ratio is 0.851, the only worker count still clearly below parity. This contrasts with w=1, which nearly closed the gap. The 2-thread scaling mechanism appears to benefit asio's reactor model more than bnio's current kqueue path.

6. **Connection count has minimal impact on the ratio.** The bnio/asio ratio is stable across 64, 256, and 1,024 connections (1.08–1.16× at workers=4), indicating both backends scale similarly with connection count. At workers=8, bnio maintains its lead across all connection counts for small/medium messages.

7. **No errors across the entire matrix.** All 96 configurations completed cleanly — both server implementations and the client are stable under all tested configurations on kqueue.

8. **The kqueue implementation continues to improve.** The single-threaded and small/medium-message paths are now competitive with asio. Remaining areas: the 64 KB large-buffer I/O path, 2-thread scaling, and the specific w=1/c=64/m=65536 regression. See the [kqueue roadmap](../design/kqueue-roadmap.md) for optimization plans.

---

## Part B — Timer Churn Results

### 6.1 Lifecycle Throughput Overview

![Timer Lifecycle Overview](charts/kqueue/kqueue_timer_lifecycle_overview.png)

**Reference point: update_rounds=500**

| Timer Count | bnio lifecycle/s | asio lifecycle/s | Ratio |
| ---: | ---: | ---: | ---: |
| 256 | 21,333,800 | 23,552,500 | 0.91× |
| 1,024 | 38,093,200 | 37,503,700 | 1.02× |
| 4,096 | 38,033,100 | 38,890,800 | 0.98× |
| 16,384 | 38,226,100 | 37,159,800 | 1.03× |

### 6.2 Active Waits Overview

![Timer Waits Overview](charts/kqueue/kqueue_timer_waits_overview.png)

**Reference point: update_rounds=500**

| Timer Count | bnio waits/s | asio waits/s | Ratio |
| ---: | ---: | ---: | ---: |
| 256 | 12,159,500 | 13,424,100 | 0.91× |
| 1,024 | 21,711,800 | 21,375,800 | 1.02× |
| 4,096 | 21,677,600 | 22,166,400 | 0.98× |
| 16,384 | 21,787,600 | 21,179,800 | 1.03× |

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
- bnio: 18,876,100 lifecycle calls/s
- asio: 14,688,200 lifecycle calls/s
- Ratio: 1.29×

**Most challenging bnio / asio lifecycle ratio:**

- Configuration: timers=256, rounds=500
- bnio: 21,333,800 lifecycle calls/s
- asio: 23,552,500 lifecycle calls/s
- Ratio: 0.91×

### 6.7 Full Results

#### Timer count = 256

| Backend | Timers | Rounds | lifecycle/s | waits/s |
| --- | ---: | ---: | ---: | ---: |
| bnio | 256 | 100 | 18,876,100 | 10,650,700 |
| bnio | 256 | 500 | 21,333,800 | 12,159,500 |
| bnio | 256 | 1,000 | 29,951,700 | 17,093,300 |
| asio | 256 | 100 | 14,688,200 | 8,287,730 |
| asio | 256 | 500 | 23,552,500 | 13,424,100 |
| asio | 256 | 1,000 | 29,499,500 | 16,835,200 |

#### Timer count = 1,024

| Backend | Timers | Rounds | lifecycle/s | waits/s |
| --- | ---: | ---: | ---: | ---: |
| bnio | 1,024 | 100 | 36,161,100 | 20,403,700 |
| bnio | 1,024 | 500 | 38,093,200 | 21,711,800 |
| bnio | 1,024 | 1,000 | 37,051,700 | 21,145,200 |
| asio | 1,024 | 100 | 35,849,600 | 20,228,000 |
| asio | 1,024 | 500 | 37,503,700 | 21,375,800 |
| asio | 1,024 | 1,000 | 37,068,200 | 21,154,700 |

#### Timer count = 4,096

| Backend | Timers | Rounds | lifecycle/s | waits/s |
| --- | ---: | ---: | ---: | ---: |
| bnio | 4,096 | 100 | 38,616,000 | 21,788,900 |
| bnio | 4,096 | 500 | 38,033,100 | 21,677,600 |
| bnio | 4,096 | 1,000 | 39,235,900 | 22,391,700 |
| asio | 4,096 | 100 | 32,581,600 | 18,384,000 |
| asio | 4,096 | 500 | 38,890,800 | 22,166,400 |
| asio | 4,096 | 1,000 | 36,759,100 | 20,978,300 |

#### Timer count = 16,384

| Backend | Timers | Rounds | lifecycle/s | waits/s |
| --- | ---: | ---: | ---: | ---: |
| bnio | 16,384 | 100 | 37,985,400 | 21,433,100 |
| bnio | 16,384 | 500 | 38,226,100 | 21,787,600 |
| bnio | 16,384 | 1,000 | 38,220,700 | 21,812,400 |
| asio | 16,384 | 100 | 33,544,000 | 18,927,100 |
| asio | 16,384 | 500 | 37,159,800 | 21,179,800 |
| asio | 16,384 | 1,000 | 38,761,100 | 22,120,800 |

### 6.8 Interpretation

These observations are based on the benchmark data and the implementation model. They have not been independently validated with profiling in this run.

1. **bnio retains a slight edge on timer churn.** The average lifecycle ratio is 1.051× in bnio's favor (down slightly from 1.056× previously, −0.5%), with bnio winning 16 of 24 configurations (down from 18). At steady-state configurations (500+ rounds), both backends produce near-identical throughput. The recent kqueue commits (`0979ac0`, `eeb6805`) target the I/O event path and do not modify timer code, so timer churn behavior is largely unchanged.

2. **Kqueue timer throughput continues to excel.** Both backends achieve ~37–39 million lifecycle calls/s at peak, consistent with the lightweight nature of kqueue's `EVFILT_TIMER` mechanism.

3. **Low-round results are mixed.** At rounds=100, the 4,096-timer case improved significantly (1.038 → 1.185, +0.147) and the 256-timer case strengthened (1.187 → 1.285, +0.098). However, the 1,024-timer case regressed severely (1.227 → 1.009, −0.218) — the single worst regression in this run.

4. **The 1,024-timer count is the primary regression area.** The average ratio at t=1024 fell from 1.101 to 1.008 (−0.093). Two of three round configurations regressed; only t=1024/r=500 retains a slim lead (1.016). This suggests the recent kqueue changes interact poorly with this specific timer scale.

5. **Setup-cost amortization favors asio at 256 timers / 500 rounds.** At 256/500, asio leads at 0.91×. This configuration represents the case where per-round overhead dominates, and asio's path is marginally faster once warm-up is complete.

6. **No clear scaling pattern with timer count.** The ratio is relatively flat or slightly decreasing with more timers. Both backends use kqueue's native timer mechanism, so per-timer costs are very similar.

7. **Active wait throughput mirrors lifecycle throughput.** The waits/s ratio follows the same pattern (average ~1.051× in bnio's favor), confirming consistency across both metrics.

---

## 7. Cross-Benchmark Summary

| Benchmark | Avg bnio/asio Ratio | bnio Wins | asio Wins | Total Configs |
| --- | ---: | ---: | ---: | ---: |
| TCP Echo Throughput | 0.972× | 50 | 46 | 96 |
| Timer Churn (lifecycle) | 1.051× | 16 | 8 | 24 |

On the kqueue (macOS/BSD) backend, the picture has continued to improve from the previous run:

- **Throughput**: bnio now achieves 97.2% of asio's throughput on average, up from 90.1% (+7.9%). At 1, 4, and 8 workers, bnio leads asio for small and medium messages (1.03–1.22×). The large-message (64 KB) path remains the weakest area, with two single-threaded 64 KB configurations regressing.

- **Timer churn**: bnio maintains a slight edge (1.051×), essentially unchanged from the previous run (1.056×, −0.5%). Both backends achieve near-identical steady-state throughput, with the 1,024-timer scale the only notable regression area.

### Comparison with Previous Run (2026-07-28)

| Benchmark | Previous (0.901× avg) | Current (0.972× avg) | Change |
| --- | ---: | ---: | ---: |
| TCP Echo Throughput | 0.901× (42 wins) | 0.972× (50 wins) | **+7.9%** |
| Timer Churn | 1.056× (18 wins) | 1.051× (16 wins) | −0.5% |

The throughput improvement is attributable to the two recent commits `0979ac0` (*perf: modified the check method of kqueue*) and `eeb6805` (*perf: remove the fixed array in kqueue branch*). The check-method change streamlined the per-event verification path in the kqueue reactor, particularly benefiting the single-threaded event loop. Removing the fixed array eliminated per-event array overhead and reduced contention on the multi-threaded submission path. Together these changes lifted the overall ratio by +0.071 and converted 4 net configurations from asio wins to bnio wins, with the largest gains concentrated in single-threaded small/medium-message scenarios (delta +0.17 to +0.22). The only notable regression is two single-threaded 64 KB configurations (worst: w=1/c=64/m=65536 at −0.20), which warrants separate investigation.

### Platform Comparison: kqueue vs io_uring

| Benchmark | io_uring (Linux) | kqueue (macOS) |
| --- | ---: | ---: |
| TCP Echo Throughput | **1.039×** (bnio leads) | **0.972×** (bnio near parity) |
| Timer Churn (lifecycle) | **1.179×** (bnio leads) | **1.051×** (near parity) |

The platform comparison reveals that bnio's performance on kqueue continues to close the gap with asio, though it still trails its io_uring performance. On io_uring, bnio leverages submission batching and zero-syscall completion polling to outperform standalone Asio. On kqueue, the recent BSD-side optimizations (`0979ac0`, `eeb6805`) have brought small/medium-message throughput to near-parity or better across most worker counts, but large-message I/O remains an area for further improvement. The timer churn results are closer across platforms because both backends ultimately rely on the same underlying kqueue timer mechanism.

The kqueue backend is actively under development (see [kqueue roadmap](../design/kqueue-roadmap.md)). Areas of ongoing optimization include the large-buffer I/O path, 2-thread scaling, the single-threaded 64 KB regression, and further reductions in synchronization overhead.

---

## 8. Cross-Platform Absolute Throughput Comparison

A comparison of absolute throughput (not ratio) between platforms:

### Throughput (workers=4, connections=256, 4 KB)

| Backend | io_uring (Linux) | kqueue (macOS) | Ratio |
| --- | ---: | ---: | ---: |
| bnio | 788,413 req/s | 127,947 req/s | 0.16× |
| asio | 723,093 req/s | 110,349 req/s | 0.15× |

### Timer Churn (timers=1,024, rounds=500)

| Backend | io_uring (Linux) | kqueue (macOS) | Ratio |
| --- | ---: | ---: | ---: |
| bnio | 20,690,800 lifecycle/s | 38,093,200 lifecycle/s | 1.84× |
| asio | 17,375,400 lifecycle/s | 37,503,700 lifecycle/s | 2.16× |

> **Note:** Absolute throughput comparisons across platforms are not apples-to-apples. Different hardware (i9-13900H x86_64 vs M5 Pro arm64), operating systems, and kernel implementations all affect baseline performance. The io_uring run used 60 s measurements over 3 iterations; the kqueue run used 10 s measurements over 1 iteration. System load and thermal conditions may differ between runs. Treat these cross-platform figures as directional only.
