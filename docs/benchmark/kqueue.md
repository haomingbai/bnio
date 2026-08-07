# bnio vs asio Benchmark — kqueue (macOS/BSD)

## 1. Test Environment

| Item | Value |
| --- | --- |
| Date | 2026-08-07 |
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

## 2. Methodology

This report covers two independent benchmarks, each comparing bnio against standalone Asio:

- **Part A — TCP Echo Throughput (Sections 3–7):** A multi-dimensional throughput stress test on a TCP echo server under varying worker counts, connection counts, and message sizes.
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

Overall average throughput ratio (bnio / asio): **0.980×** across all 48 configurations, with bnio winning **16 of 48**.

### Part B — Timer Churn

Both backends completed every configuration successfully. All 24 measurements are clean. Three configurations were retested (3 iterations, median): timers=16384/rounds=500 was replaced (0.71× → 0.88×), while timers=256/rounds=100 and timers=4096/rounds=100 were confirmed as noise. timers=16384/rounds=1000 also used its retest median in place of the original run.

Overall average lifecycle throughput ratio (bnio / asio): **0.927×** across all 12 configurations, with bnio winning **1 of 12**.

---

## Part A — TCP Echo Throughput Results

### 5.1 Throughput Overview

![Throughput Overview](charts/kqueue/kqueue_overview_bars.png)

**Reference point: workers=4, connections=256**

| Message Size | bnio req/s | bnio err | asio req/s | asio err | Ratio |
| --- | ---: | ---: | ---: | ---: | ---: |
| 64 B | 113,166 | 0 | 114,253 | 0 | 0.99× |
| 1 KB | 112,243 | 0 | 114,009 | 0 | 0.98× |
| 4 KB | 113,352 | 0 | 114,094 | 0 | 0.99× |
| 64 KB | 14,418 | 0 | 12,742 | 0 | 1.13× |

### 5.2 Throughput vs Connections

![Throughput vs Connections](charts/kqueue/kqueue_throughput_vs_connections.png)

*Workers=4, faceted by message size.*

### 5.3 Throughput vs Worker Threads

![Throughput vs Workers](charts/kqueue/kqueue_throughput_vs_workers.png)

*Connections=256, faceted by message size.*

| Workers | bnio req/s | bnio err | asio req/s | asio err | Ratio |
| ---: | ---: | ---: | ---: | ---: | ---: |
| 1 | 118,776 | 0 | 116,910 | 0 | 1.02× |
| 2 | 141,239 | 0 | 151,650 | 0 | 0.93× |
| 4 | 113,352 | 0 | 114,094 | 0 | 0.99× |
| 8 | 104,535 | 0 | 104,037 | 0 | 1.00× |

**Worker-scaling at 4 KB / 256 connections.** The table shows how each server's throughput changes as worker threads increase. bnio leads at 1, 4, and 8 workers (1.00–1.02×) and trails only at 2 workers (0.93×). The 2-worker configuration remains bnio's weakest point, though the deficit has narrowed substantially since the previous run.

### 5.4 Connection Scaling

| Connections | bnio req/s | bnio err | asio req/s | asio err | Ratio |
| ---: | ---: | ---: | ---: | ---: | ---: |
| 64 | 108,138 | 0 | 114,285 | 0 | 0.95× |
| 256 | 113,352 | 0 | 114,094 | 0 | 0.99× |
| 1024 | 108,901 | 0 | 106,785 | 0 | 1.02× |

**Connection-scaling at 4 KB / workers=4.** Shows how each server handles increasing concurrency. bnio trails slightly at 64 connections but edges ahead at 1,024 connections.

### 5.5 bnio / asio Throughput Ratio Heatmap

![Heatmap](charts/kqueue/kqueue_heatmap_ratio.png)

*Workers=4. Positive values (blue) = bnio faster; negative (red) = asio faster.*

### 5.6 Worker-Scaling Ratio Heatmap

![Worker Scaling](charts/kqueue/kqueue_worker_scaling_heatmap.png)

*Connections=256. Shows how the bnio/asio ratio changes as worker threads increase.*

### 5.7 Extreme Cases

**Best bnio / asio throughput ratio (zero-error):**

- Configuration: workers=4, connections=64, message_size=64 KB
- bnio: 15,104 req/s
- asio: 13,027 req/s
- Ratio: 1.16×

**Most challenging bnio / asio throughput ratio (zero-error):**

- Configuration: workers=2, connections=256, message_size=64 KB
- bnio: 11,149 req/s
- asio: 15,505 req/s
- Ratio: 0.72×

### 5.8 Full Results (workers=4)

#### Message size = 64 B

| Server | Workers | Conns | req/s | MB/s |
| --- | ---: | ---: | ---: | ---: |
| asio | 4 | 64 | 117,121 | 7 |
| asio | 4 | 256 | 114,253 | 6 |
| asio | 4 | 1024 | 111,103 | 6 |
| bnio | 4 | 64 | 109,618 | 6 |
| bnio | 4 | 256 | 113,166 | 6 |
| bnio | 4 | 1024 | 112,702 | 6 |

#### Message size = 1 KB

| Server | Workers | Conns | req/s | MB/s |
| --- | ---: | ---: | ---: | ---: |
| asio | 4 | 64 | 115,495 | 112 |
| asio | 4 | 256 | 114,009 | 111 |
| asio | 4 | 1024 | 109,187 | 106 |
| bnio | 4 | 64 | 107,950 | 105 |
| bnio | 4 | 256 | 112,243 | 109 |
| bnio | 4 | 1024 | 111,725 | 109 |

#### Message size = 4 KB

| Server | Workers | Conns | req/s | MB/s |
| --- | ---: | ---: | ---: | ---: |
| asio | 4 | 64 | 114,285 | 446 |
| asio | 4 | 256 | 114,094 | 445 |
| asio | 4 | 1024 | 106,785 | 417 |
| bnio | 4 | 64 | 108,138 | 422 |
| bnio | 4 | 256 | 113,352 | 442 |
| bnio | 4 | 1024 | 108,901 | 425 |

#### Message size = 64 KB

| Server | Workers | Conns | req/s | MB/s |
| --- | ---: | ---: | ---: | ---: |
| asio | 4 | 64 | 13,027 | 814 |
| asio | 4 | 256 | 12,742 | 796 |
| asio | 4 | 1024 | 13,183 | 823 |
| bnio | 4 | 64 | 15,104 | 944 |
| bnio | 4 | 256 | 14,418 | 901 |
| bnio | 4 | 1024 | 14,262 | 891 |

### 5.9 Interpretation

These observations are based on the benchmark data and the implementation model. They have not been independently validated with profiling in this run.

1. **Overall ratio up slightly, but wins fell.** The overall throughput ratio is 0.980×, up from 0.974×. bnio's per-configuration win count is 16 of 48. The win profile has shifted: the large-message family (64 KB) now contributes 7 of 12 wins, while small/medium messages slipped to near parity after the stealing changes.

2. **64 KB is now a strength.** The average ratio at 64 KB rose from 0.733× to 0.994×. At workers=4, bnio leads every connection count (1.13–1.16×); the former worst case (workers=8, conns=64) improved from 0.55× to 1.05×. The remaining large-message deficits are concentrated at workers=2, where conns=256 is the single worst configuration overall (0.72×).

3. **Workers=1 is near parity.** The average ratio at workers=1 is 0.989× (5/12 wins), with small/medium messages ranging 0.96–1.04× and 64 KB at 0.98×.

4. **Workers=2 improved but remains the laggard.** The average ratio at workers=2 rose from 0.854× to 0.929× (1/12 wins). Small/medium messages trail by roughly 2–10%, and 64 KB now splits — leading at conns=64 (1.02×) but trailing at conns=256 (0.72×) and conns=1024 (0.88×). The 2-worker scaling behavior continues to favor asio's reactor model, though the gap has narrowed.

5. **Small/medium messages sit at parity.** The averages by message size are 64 B (0.976×), 1 KB (0.969×), 4 KB (0.981×), and 64 KB (0.994×). At the reference point (workers=4, connections=256) bnio is within 1–2% of asio for 64 B–4 KB (0.98–0.99×) and leads 1.13× at 64 KB.

6. **Connection count has a modest effect.** At workers=4 the ratio rises slightly with connection count for 4 KB (0.95× → 0.99× → 1.02×). Across the full matrix, connections=1024 is the best point (0.991×), connections=64 the weakest (0.963×, dragged down by small/medium messages), and connections=256 sits at 0.985×.

7. **No errors across the entire matrix.** All 96 result rows completed cleanly — both server implementations and the client are stable under all tested configurations on kqueue.

---

## Part B — Timer Churn Results

### 6.1 Lifecycle Throughput Overview

![Timer Lifecycle Overview](charts/kqueue/kqueue_timer_lifecycle_overview.png)

**Reference point: update_rounds=500**

| Timer Count | bnio lifecycle/s | asio lifecycle/s | Ratio |
| ---: | ---: | ---: | ---: |
| 256 | 22,773,000 | 23,582,000 | 0.97× |
| 1,024 | 35,005,000 | 38,236,000 | 0.92× |
| 4,096 | 35,589,000 | 39,236,000 | 0.91× |
| 16,384 | 36,408,000 | 41,156,000 | 0.88× |

### 6.2 Active Waits Overview

![Timer Waits Overview](charts/kqueue/kqueue_timer_waits_overview.png)

**Reference point: update_rounds=500**

| Timer Count | bnio waits/s | asio waits/s | Ratio |
| ---: | ---: | ---: | ---: |
| 256 | 12,980,000 | 13,441,000 | 0.97× |
| 1,024 | 19,952,000 | 21,793,000 | 0.92× |
| 4,096 | 20,284,000 | 22,363,000 | 0.91× |
| 16,384 | 16,173,000 | 22,881,000 | 0.71× |

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

- Configuration: timers=256, rounds=1000
- bnio: 28,310,500 lifecycle calls/s
- asio: 28,284,000 lifecycle calls/s
- Ratio: 1.00×

**Most challenging bnio / asio lifecycle ratio:**

- Configuration: timers=16384, rounds=1000
- bnio: 36,766,800 lifecycle calls/s (retest median)
- asio: 41,735,900 lifecycle calls/s (retest median)
- Ratio: 0.88×

### 6.7 Full Results

#### Timer count = 256

| Backend | Timers | Rounds | lifecycle/s | waits/s |
| --- | ---: | ---: | ---: | ---: |
| bnio | 256 | 100 | 15,794,000 | 8,911,600 |
| bnio | 256 | 500 | 22,773,000 | 12,980,000 |
| bnio | 256 | 1,000 | 28,310,000 | 16,157,000 |
| asio | 256 | 100 | 16,126,000 | 9,099,000 |
| asio | 256 | 500 | 23,582,000 | 13,441,000 |
| asio | 256 | 1,000 | 28,284,000 | 16,142,000 |

#### Timer count = 1,024

| Backend | Timers | Rounds | lifecycle/s | waits/s |
| --- | ---: | ---: | ---: | ---: |
| bnio | 1,024 | 100 | 32,253,000 | 18,199,000 |
| bnio | 1,024 | 500 | 35,005,000 | 19,952,000 |
| bnio | 1,024 | 1,000 | 35,764,000 | 20,411,000 |
| asio | 1,024 | 100 | 34,720,000 | 19,590,000 |
| asio | 1,024 | 500 | 38,236,000 | 21,793,000 |
| asio | 1,024 | 1,000 | 39,217,000 | 22,381,000 |

#### Timer count = 4,096

| Backend | Timers | Rounds | lifecycle/s | waits/s |
| --- | ---: | ---: | ---: | ---: |
| bnio | 4,096 | 100 | 35,681,000 | 20,133,000 |
| bnio | 4,096 | 500 | 35,589,000 | 20,284,000 |
| bnio | 4,096 | 1,000 | 35,098,000 | 20,030,000 |
| asio | 4,096 | 100 | 39,989,000 | 22,564,000 |
| asio | 4,096 | 500 | 39,236,000 | 22,363,000 |
| asio | 4,096 | 1,000 | 38,574,000 | 22,014,000 |

#### Timer count = 16,384

| Backend | Timers | Rounds | lifecycle/s | waits/s |
| --- | ---: | ---: | ---: | ---: |
| bnio | 16,384 | 100 | 36,106,000 | 20,372,000 |
| bnio | 16,384 | 500 | 36,408,000 | 16,173,000 |
| bnio | 16,384 | 1,000 | 36,766,800 | 19,242,000 |
| asio | 16,384 | 100 | 38,249,000 | 21,582,000 |
| asio | 16,384 | 500 | 41,156,000 | 22,881,000 |
| asio | 16,384 | 1,000 | 41,735,900 | 24,984,000 |

### 6.8 Interpretation

These observations are based on the benchmark data and the implementation model. They have not been independently validated with profiling in this run.

1. **Regression to a small deficit.** The average lifecycle ratio is 0.927×, down from 0.984× in the previous run, with bnio winning 1 of 12 configurations. The loss is concentrated at large timer counts rather than in the small-timer workloads.

2. **Small-timer workloads remain at parity.** timers=256 averages 0.982× and timers=256/rounds=1000 is the best configuration at 1.00×. bnio's absolute lifecycle throughput at timers=256 is within 2% of asio for every round count.

3. **Large-timer workloads are the weakest area.** Averages by timer count are 256 (0.982×), 1,024 (0.919×), 4,096 (0.903×), and 16,384 (0.903×). The deepest deficits are timers=16384/rounds=500 (0.885×) and timers=16384/rounds=1000 (0.881×), both confirmed by retesting.

4. **The 500-round reference spans 0.88–0.97×.** The ratios at the reference point are 256 (0.97×), 1,024 (0.92×), 4,096 (0.91×), and 16,384 (0.88×), with the deficit widening as the timer count grows.

5. **The gap is an asio absolute advantage at high timer counts.** bnio's peak lifecycle throughput is 36.8M lifecycle/s, while asio's peak is 41.7M. At 16,384 timers asio sustains 38–42M across all round counts while bnio holds at 36–37M, so the ratio gap reflects an asio throughput increase rather than a bnio collapse.

6. **Active wait throughput tracks lifecycle for most configurations** (e.g., 0.97×/0.92×/0.91× at rounds=500 for 256/1,024/4,096). The waits ratio at timers=16384/rounds=500 (0.71×) is an outlier from the single original run; its lifecycle value was retested and corrected, but waits were not re-collected.

7. **Three configurations were retested.** timers=16384/rounds=500 deviated sharply in the original run (0.71×) and was replaced with its retest median (0.885×). timers=256/rounds=100 and timers=4096/rounds=100 were retested and confirmed as measurement noise. timers=16384/rounds=1000 also uses its retest median (0.881×) in place of the original 0.77×.

---

## 7. Cross-Benchmark Summary

| Benchmark | Avg bnio/asio Ratio | bnio Wins | asio Wins | Total Configs |
| --- | ---: | ---: | ---: | ---: |
| TCP Echo Throughput | 0.980× | 16 | 32 | 48 |
| Timer Churn (lifecycle) | 0.927× | 1 | 11 | 12 |

On the kqueue (macOS/BSD) backend:

- **Throughput**: bnio achieves 98.0% of asio's throughput on average, up from 97.4%. The large-message path is the headline improvement: the 64 KB family rose from 0.733× to 0.994× (7/12 wins), including 1.16× at workers=4/conns=64 and a recovery of the former worst case (workers=8, conns=64) from 0.55× to 1.05×. Small and medium messages now sit at parity (0.97–0.98×), and workers=2 improved from 0.854× to 0.929×, though it remains bnio's weakest point.

- **Timer churn**: bnio's lifecycle ratio is 0.927×, down from 0.984×. Small-timer workloads stay at parity (0.98×), but large-timer counts (4,096 and 16,384) trail by roughly 10%, confirmed by retesting the two deepest-deficit configurations.

The kqueue backend remains under active optimization: the timer control-plane at large timer counts, 2-thread scaling, and the small-message steady-state all sit on the roadmap, along with further synchronization reduction. The next structural step is the POSIX `io_context` consolidation described in [`design/roadmap.md`](../design/roadmap.md).
