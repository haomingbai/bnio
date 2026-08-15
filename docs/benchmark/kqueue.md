# bnio vs asio Benchmark — kqueue (macOS/BSD)

## 1. Test Environment

| Item | Value |
| --- | --- |
| Date | 2026-08-16 |
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

Overall average throughput ratio (bnio / asio): **0.96×** across all 48 per-configuration ratios, with bnio winning **17 of 48**.

### Part B — Timer Churn

Both backends completed every configuration successfully. All 72 measured timer runs completed cleanly; no retests or replacements were required in this 3-iteration run.

Overall average lifecycle throughput ratio (bnio / asio): **0.91×** across all 12 per-configuration ratios, with bnio winning **2 of 12**.

---

## Part A — TCP Echo Throughput Results

### 5.1 Throughput Overview

![Throughput Overview](charts/kqueue/kqueue_overview_bars.png)

**Reference point: workers=4, connections=256**

| Message Size | bnio req/s | bnio err | asio req/s | asio err | Ratio |
| --- | ---: | ---: | ---: | ---: | ---: |
| 64 B | 101,614 | 0 | 108,407 | 0 | 0.94× |
| 1 KB | 102,452 | 0 | 109,226 | 0 | 0.94× |
| 4 KB | 69,663 | 0 | 88,714 | 0 | 0.79× |
| 64 KB | 27,187 | 0 | 22,829 | 0 | 1.19× |

### 5.2 Throughput vs Connections

![Throughput vs Connections](charts/kqueue/kqueue_throughput_vs_connections.png)

*Workers=4, faceted by message size.*

### 5.3 Throughput vs Worker Threads

![Throughput vs Workers](charts/kqueue/kqueue_throughput_vs_workers.png)

*Connections=256, faceted by message size.*

| Workers | bnio req/s | bnio err | asio req/s | asio err | Ratio |
| ---: | ---: | ---: | ---: | ---: | ---: |
| 1 | 161,544 | 0 | 162,566 | 0 | 0.99× |
| 2 | 102,183 | 0 | 101,262 | 0 | 1.01× |
| 4 | 69,663 | 0 | 88,714 | 0 | 0.79× |
| 8 | 56,054 | 0 | 69,060 | 0 | 0.81× |

**Worker-scaling at 4 KB / 256 connections.** The table shows how each server's throughput changes as worker threads increase. Both servers' absolute throughput declines as workers grow at this configuration; bnio leads at 2 workers but trails at 4 and 8.

### 5.4 Connection Scaling

| Connections | bnio req/s | bnio err | asio req/s | asio err | Ratio |
| ---: | ---: | ---: | ---: | ---: | ---: |
| 64 | 85,410 | 0 | 94,149 | 0 | 0.91× |
| 256 | 69,663 | 0 | 88,714 | 0 | 0.79× |
| 1024 | 61,871 | 0 | 63,841 | 0 | 0.97× |

**Connection-scaling at 4 KB / workers=4.** Shows how each server handles increasing concurrency. bnio's throughput declines steadily with connection count, while asio's collapses at 1,024 connections, narrowing the ratio gap.

### 5.5 bnio / asio Throughput Ratio Heatmap

![Heatmap](charts/kqueue/kqueue_heatmap_ratio.png)

*Workers=4. Positive values (blue) = bnio faster; negative (red) = asio faster.*

### 5.6 Worker-Scaling Ratio Heatmap

![Worker Scaling](charts/kqueue/kqueue_worker_scaling_heatmap.png)

*Connections=256. Shows how the bnio/asio ratio changes as worker threads increase.*

### 5.7 Extreme Cases

**Best bnio / asio throughput ratio (zero-error):**

- Configuration: workers=4, connections=256, message_size=64 KB
- bnio: 27,187 req/s
- asio: 22,829 req/s
- Ratio: 1.19×

**Most challenging bnio / asio throughput ratio (zero-error):**

- Configuration: workers=8, connections=1024, message_size=64 B
- Ratio: 0.78×

### 5.8 Full Results (workers=4)

#### Message size = 64 B

| Server | Workers | Conns | req/s | MB/s |
| --- | ---: | ---: | ---: | ---: |
| asio | 4 | 64 | 106,824 | 6 |
| asio | 4 | 256 | 108,407 | 6 |
| asio | 4 | 1024 | 109,713 | 6 |
| bnio | 4 | 64 | 99,753 | 6 |
| bnio | 4 | 256 | 101,614 | 6 |
| bnio | 4 | 1024 | 101,367 | 6 |

#### Message size = 1 KB

| Server | Workers | Conns | req/s | MB/s |
| --- | ---: | ---: | ---: | ---: |
| asio | 4 | 64 | 106,434 | 103 |
| asio | 4 | 256 | 109,226 | 106 |
| asio | 4 | 1024 | 111,243 | 108 |
| bnio | 4 | 64 | 99,599 | 97 |
| bnio | 4 | 256 | 102,452 | 99 |
| bnio | 4 | 1024 | 102,534 | 99 |

#### Message size = 4 KB

| Server | Workers | Conns | req/s | MB/s |
| --- | ---: | ---: | ---: | ---: |
| asio | 4 | 64 | 94,149 | 367 |
| asio | 4 | 256 | 88,714 | 346 |
| asio | 4 | 1024 | 63,841 | 249 |
| bnio | 4 | 64 | 85,410 | 333 |
| bnio | 4 | 256 | 69,663 | 272 |
| bnio | 4 | 1024 | 61,871 | 241 |

#### Message size = 64 KB

| Server | Workers | Conns | req/s | MB/s |
| --- | ---: | ---: | ---: | ---: |
| asio | 4 | 64 | 22,194 | 1,387 |
| asio | 4 | 256 | 22,829 | 1,426 |
| asio | 4 | 1024 | 25,346 | 1,583 |
| bnio | 4 | 64 | 25,420 | 1,589 |
| bnio | 4 | 256 | 27,187 | 1,699 |
| bnio | 4 | 1024 | 25,818 | 1,613 |

### 5.9 Interpretation

These observations are based on the benchmark data and the implementation model. They have not been independently validated with profiling in this run.

1. **bnio trails slightly on average.** The overall throughput ratio is 0.96×, with bnio winning 17 of 48 configurations. The win profile is uneven: 7 of 12 wins come from the 64 KB family, while small messages contribute only 3 of 12.
2. **Large messages remain the headline strength.** The average ratio at 64 KB is 1.03×, and the single best configuration is 64 KB at workers=4, connections=256 (1.19×). At the reference point, 64 KB is the only message size where bnio leads (1.19×, versus 0.94×/0.94×/0.79× for 64 B/1 KB/4 KB).
3. **Small and medium messages trail.** Averages by message size are 64 B (0.93×, 3/12 wins), 1 KB (0.93×, 3/12), 4 KB (0.96×, 4/12), and 64 KB (1.03×, 7/12). The deep small-message deficits cluster at high worker counts.
4. **Worker scaling is inverted.** workers=2 is bnio's strongest point (1.03×, 10 of 12 wins), while workers=8 is the weakest (0.89×, 1 of 12 wins). At the reference configuration both servers' absolute throughput falls as workers grow, and the ratio gap widens against bnio at 4–8 workers.
5. **Connection count has little aggregate effect.** Averages are 0.97× at 64 connections, 0.97× at 256, and 0.96× at 1,024. The exception is the 4 KB reference family, where bnio's worst cell is connections=256 (0.79×) despite 64 KB leading there by 1.19×.
6. **The overall worst case combines small messages with 8 workers.** The lowest ratio in the matrix is workers=8, connections=1024, message_size=64 B at 0.78×.
7. **No errors across the entire matrix.** All 288 measured client runs completed cleanly — both server implementations and the client are stable under all tested configurations on kqueue.

### 5.10 Performance Change vs Previous Run (2026-08-07 quick mode)

Since the previous kqueue report (quick mode, 1 iteration), the branch has merged a CPU-task work-stealing path behind the `enable_steal` platform option, a fast path for waking up an idle thread, consolidation of the duplicate `running_workers` counters into `global_state`, removal of the always-true `native_available` check together with symmetric restoration of the worker TLS, and a cleanup pass removing dead branches, redundant includes, and duplicated kqueue helpers, plus io_uring-only fixes and docs/version bumps. These changes are not benchmark-specific optimizations, but they do touch kqueue submit, scheduler, run-loop, and worker-TLS paths.

| Metric | Previous (Aug 7 quick) | Current (Aug 16) | Change |
| --- | ---: | ---: | ---: |
| Overall avg ratio | 0.980× | 0.96× | -1.7% |
| bnio wins | 16/48 | 17/48 | +1 |
| Best single-config ratio | 1.16× | 1.19× | +2.7% |

Throughput performance is **broadly stable** compared with the previous quick-mode run: the overall ratio eased 1.7%, the win count rose by one, and the best single-configuration ratio improved from 1.16× to 1.19× (64 KB at workers=4, connections=256). The previous run used single-iteration quick mode while this run uses three-iteration arithmetic means, so small differences should be interpreted with the methodology change in mind.

---

## Part B — Timer Churn Results

### 6.1 Lifecycle Throughput Overview

![Timer Lifecycle Overview](charts/kqueue/kqueue_timer_lifecycle_overview.png)

**Reference point: update_rounds=500**

| Timer Count | bnio lifecycle/s | asio lifecycle/s | Ratio |
| ---: | ---: | ---: | ---: |
| 256 | 34,625,567 | 35,503,733 | 0.98× |
| 1,024 | 34,915,200 | 38,787,867 | 0.90× |
| 4,096 | 34,238,300 | 40,732,833 | 0.84× |
| 16,384 | 35,397,300 | 42,798,233 | 0.83× |

### 6.2 Active Waits Overview

![Timer Waits Overview](charts/kqueue/kqueue_timer_waits_overview.png)

**Reference point: update_rounds=500**

| Timer Count | bnio waits/s | asio waits/s | Ratio |
| ---: | ---: | ---: | ---: |
| 256 | 19,735,400 | 20,235,933 | 0.98× |
| 1,024 | 19,900,467 | 22,107,800 | 0.90× |
| 4,096 | 19,514,633 | 23,216,333 | 0.84× |
| 16,384 | 20,175,267 | 24,393,533 | 0.83× |

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
- bnio: 32,151,667 lifecycle calls/s
- asio: 30,590,833 lifecycle calls/s
- Ratio: 1.05×

**Most challenging bnio / asio lifecycle ratio:**

- Configuration: timers=16,384, rounds=500
- bnio: 35,397,300 lifecycle calls/s
- asio: 42,798,233 lifecycle calls/s
- Ratio: 0.83×

### 6.7 Full Results

#### Timer count = 256

| Backend | Timers | Rounds | lifecycle/s | waits/s |
| --- | ---: | ---: | ---: | ---: |
| asio | 256 | 100 | 30,590,833 | 17,260,767 |
| asio | 256 | 500 | 35,503,733 | 20,235,933 |
| asio | 256 | 1000 | 36,047,400 | 20,572,067 |
| bnio | 256 | 100 | 32,151,667 | 18,141,467 |
| bnio | 256 | 500 | 34,625,567 | 19,735,400 |
| bnio | 256 | 1000 | 36,325,567 | 20,730,833 |

#### Timer count = 1,024

| Backend | Timers | Rounds | lifecycle/s | waits/s |
| --- | ---: | ---: | ---: | ---: |
| asio | 1,024 | 100 | 39,313,667 | 22,182,567 |
| asio | 1,024 | 500 | 38,787,867 | 22,107,800 |
| asio | 1,024 | 1000 | 38,798,467 | 22,142,133 |
| bnio | 1,024 | 100 | 36,440,733 | 20,561,533 |
| bnio | 1,024 | 500 | 34,915,200 | 19,900,467 |
| bnio | 1,024 | 1000 | 33,841,400 | 19,313,167 |

#### Timer count = 4,096

| Backend | Timers | Rounds | lifecycle/s | waits/s |
| --- | ---: | ---: | ---: | ---: |
| asio | 4,096 | 100 | 40,949,233 | 23,105,467 |
| asio | 4,096 | 500 | 40,732,833 | 23,216,333 |
| asio | 4,096 | 1000 | 40,314,633 | 23,007,367 |
| bnio | 4,096 | 100 | 35,548,100 | 20,057,867 |
| bnio | 4,096 | 500 | 34,238,300 | 19,514,633 |
| bnio | 4,096 | 1000 | 35,406,133 | 20,206,133 |

#### Timer count = 16,384

| Backend | Timers | Rounds | lifecycle/s | waits/s |
| --- | ---: | ---: | ---: | ---: |
| asio | 16,384 | 100 | 40,189,600 | 22,676,833 |
| asio | 16,384 | 500 | 42,798,233 | 24,393,533 |
| asio | 16,384 | 1000 | 42,367,133 | 24,178,700 |
| bnio | 16,384 | 100 | 35,223,800 | 19,874,867 |
| bnio | 16,384 | 500 | 35,397,300 | 20,175,267 |
| bnio | 16,384 | 1000 | 35,637,967 | 20,338,433 |

### 6.8 Interpretation

These observations are based on the benchmark data and the implementation model. They have not been independently validated with profiling in this run.

1. **bnio trails in timer churn.** The average lifecycle ratio is 0.91×, with bnio winning 2 of 12 configurations. The deficit is concentrated at large timer counts rather than in small-timer workloads.
2. **Small-timer workloads are at parity or better.** timers=256 averages 1.01×, and both wins come from this family: rounds=100 (1.05×) and rounds=1000 (1.01×).
3. **The deficit widens with timer count.** Averages by timer count are 256 (1.01×), 1,024 (0.90×), 4,096 (0.86×), and 16,384 (0.85×). The deepest deficits are timers=16384/rounds=500 (0.83×) and timers=16384/rounds=1000 (0.84×).
4. **The 500-round reference spans 0.83–0.98×.** The ratios at the reference point are 256 (0.98×), 1,024 (0.90×), 4,096 (0.84×), and 16,384 (0.83×), with the deficit widening as the timer count grows.
5. **The gap is an asio absolute advantage at high timer counts.** bnio's lifecycle throughput stays flat at roughly 35M/s from 4,096 timers upward, while asio scales from 40.3M to 42.8M in the same range — so the ratio gap reflects an asio throughput increase rather than a bnio collapse.
6. **Active waits mirror lifecycle throughput.** The waits/s ratios match the lifecycle ratios at every reference cell (0.98×/0.90×/0.84×/0.83×), confirming the measurements reflect real timer-completion work. All 72 timer runs completed cleanly with no retests required.

### 6.9 Performance Change vs Previous Run (2026-08-07 quick mode)

The previous kqueue report measured single-iteration quick mode on 2026-08-07. This run uses the same 3-iteration arithmetic-mean method as the io_uring report. The code changes listed in Section 5.10 affect kqueue submit, scheduler, run-loop, and worker-TLS paths, and are the main candidates for any timer-throughput movement.

| Metric | Previous (Aug 7 quick) | Current (Aug 16) | Change |
| --- | ---: | ---: | ---: |
| Overall avg lifecycle ratio | 0.927× | 0.91× | -2.3% |
| bnio wins | 1/12 | 2/12 | +1 |
| Best single-config ratio | 1.00× | 1.05× | +5.1% |

Timer performance is **broadly stable** compared with the previous quick-mode run: the overall lifecycle ratio eased 2.3%, the win count rose from 1 to 2, and the best single-configuration ratio improved from 1.00× to 1.05× (timers=256, rounds=100). The previous run required retesting of several configurations; this 3-iteration run needed none. The previous run used single-iteration quick mode, so small differences should be interpreted with the methodology change in mind.

---

## 7. Cross-Benchmark Summary

| Benchmark | Avg bnio/asio Ratio | bnio Wins | asio Wins | Total Configs |
| --- | ---: | ---: | ---: | ---: |
| TCP Echo Throughput | 0.96× | 17 | 31 | 48 |
| Timer Churn (lifecycle) | 0.91× | 2 | 10 | 12 |

On the kqueue (macOS/BSD) backend:

- **Throughput**: bnio achieves 96% of asio's throughput on average (0.96×, 17 of 48 wins), roughly flat versus the previous quick-mode run (0.980×). The large-message path remains the headline strength: the 64 KB family averages 1.03× with 7 of 12 wins, including the overall best configuration (workers=4, connections=256) at 1.19×. Small and medium messages trail at 0.93–0.96×, and workers=8 is now the weakest worker count (0.89×, 1 of 12 wins), while workers=2 leads (1.03×, 10 of 12 wins).

- **Timer churn**: bnio's lifecycle ratio is 0.91× (2 of 12 wins), down slightly from the previous 0.927×. Small-timer workloads are at parity or better (timers=256 averages 1.01×), but large timer counts (4,096 and 16,384) trail by roughly 14–15%, an asio absolute advantage at scale rather than a bnio collapse.

The kqueue backend remains under active optimization: the timer control-plane at large timer counts, 8-worker scaling, and the small-message steady-state all sit on the roadmap, along with further synchronization reduction. The next structural step is the POSIX `io_context` consolidation described in [`design/roadmap.md`](../design/roadmap.md).
