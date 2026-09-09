# bnio vs asio Benchmark — io_uring (Linux)

## 1. Test environment

| Item | Value |
| --- | --- |
| Date | 2026-09-09 |
| Topology | Single-host loopback TCP (127.0.0.1) |
| OS | Fedora Linux 44 (Workstation Edition) |
| Kernel | 7.1.9-200.fc44.x86_64 |
| Architecture | x86_64 |
| CPU | 13th Gen Intel(R) Core(TM) i9-13900H |
| Cores / logical CPUs | 14 / 20 |
| Memory | 32,564,592 kB |
| Compiler | g++ (GCC) 16.2.1 20260819 (Red Hat 16.2.1-2) |
| CMake | 4.3.0 |
| liburing | 2.13 |
| Asio | 1.30.2 (local source) |
| bnio | 0.2.0 (branch main, commit f8ad27d) |

Hostnames, usernames, absolute paths, and network addresses are omitted.

> Interactive ECharts: [report.html](charts/io_uring/report.html)

## 2. Methodology

The report follows the same structure as the previous io_uring benchmark:

- **Part A — TCP echo throughput:** a multi-dimensional stress test on a TCP echo server, varying worker threads, concurrent connections, and message sizes.
- **Part B — Timer churn:** a timer lifecycle stress test that creates, resets, cancels, and destroys large numbers of steady timers in tight update rounds.

Both parts compare bnio with standalone Asio. The bnio server uses io_uring; the Asio server uses the epoll reactor. The client is the shared C++ `throughput_benchmark_client`; it runs a strict ping-pong loop and reports total echoes, req/s, and MB/s. It does not expose latency percentiles.

Each server was rebuilt in Release mode with `-march=native -mtune=native` before the run. Every configuration restarts the server, and the same client binary drives both implementations. Each configuration is measured three times.

The order of bnio and asio within each iteration alternates, so a one-off warm-up or thermal drift does not consistently favor the first backend. All client return codes and server process states are recorded. The client binary returns a process exit status but does not count individual connection errors, so the stability claims below are process-level checks rather than per-request error counts.

## 3. Configuration matrix

### Throughput

| Dimension | Values |
| --- | --- |
| Server | bnio, asio |
| Worker threads | 1, 2, 4, 8 |
| Concurrent connections | 64, 256, 1,024 |
| Message size | 64 B, 1 KB, 4 KB, 64 KB |
| Measurement per run | 10 s |
| Iterations | 3 |

This is 48 unique server-configuration cells and 288 measured client runs.

### Timer churn

| Dimension | Values |
| --- | --- |
| Backend | bnio, asio |
| Live timers | 256, 1,024, 4,096, 16,384 |
| Update rounds | 100, 500, 1,000 |
| Replacements per round | timers / 4 |
| Iterations | 3 |

This is 24 unique backend-configuration cells and 72 measured runs.

## 4. Stability

Every server started within the client's readiness window. Process-level failures were 0/288 for throughput and 0/72 for timer churn.

The largest per-configuration coefficient of variation was 5.9% for throughput and 7.9% for timer lifecycle throughput.

## 5. TCP echo throughput

### 5.1 Overview

![Throughput overview](charts/io_uring/overview_bars.png)

Reference point: workers=4, connections=256.

| Message size | bnio req/s | asio req/s | bnio / asio |
| ---: | ---: | ---: | ---: |
| 64 B | 998,217 | 830,431 | 1.20× |
| 1 KB | 932,801 | 787,302 | 1.18× |
| 4 KB | 832,177 | 729,243 | 1.14× |
| 64 KB | 6,303 | 6,221 | 1.01× |

### 5.2 Connection scaling

![Throughput vs connections](charts/io_uring/throughput_vs_connections.png)

Workers=4, faceted by message size.

### 5.3 Worker scaling

![Throughput vs workers](charts/io_uring/throughput_vs_workers.png)

Connections=256, faceted by message size.

| Workers | bnio req/s | asio req/s | bnio / asio |
| ---: | ---: | ---: | ---: |
| 1 | 356,639 | 364,214 | 0.98× |
| 2 | 597,610 | 611,673 | 0.98× |
| 4 | 832,177 | 729,243 | 1.14× |
| 8 | 796,720 | 747,486 | 1.07× |

### 5.4 Connection count at 4 KB

| Connections | bnio req/s | asio req/s | bnio / asio |
| ---: | ---: | ---: | ---: |
| 64 | 827,510 | 699,707 | 1.18× |
| 256 | 832,177 | 729,243 | 1.14× |
| 1,024 | 770,464 | 695,282 | 1.11× |

### 5.5 Ratio heatmaps

![Throughput ratio heatmap](charts/io_uring/heatmap_ratio.png)

Workers=4, by message size and connections.

![Worker-scaling heatmap](charts/io_uring/worker_scaling_heatmap.png)

Connections=256, by message size and worker count.

### 5.6 Extreme cases

Best bnio / asio ratio:

- Configuration: workers=4, connections=64, message=64 B
- bnio: 1,007,660 req/s
- asio: 797,957 req/s
- Ratio: 1.26×

Lowest bnio / asio ratio:

- Configuration: workers=1, connections=1,024, message=4 KB
- bnio: 270,769 req/s
- asio: 296,851 req/s
- Ratio: 0.91×

### 5.7 Full results at workers=4

#### 64 B

| Server | Workers | Connections | req/s | MB/s |
| --- | ---: | ---: | ---: | ---: |
| asio | 4 | 64 | 797,957 | 48 |
| asio | 4 | 256 | 830,431 | 50 |
| asio | 4 | 1,024 | 909,826 | 55 |
| bnio | 4 | 64 | 1,007,660 | 61 |
| bnio | 4 | 256 | 998,217 | 60 |
| bnio | 4 | 1,024 | 1,001,160 | 61 |

#### 1 KB

| Server | Workers | Connections | req/s | MB/s |
| --- | ---: | ---: | ---: | ---: |
| asio | 4 | 64 | 779,306 | 760 |
| asio | 4 | 256 | 787,302 | 768 |
| asio | 4 | 1,024 | 848,531 | 828 |
| bnio | 4 | 64 | 935,146 | 913 |
| bnio | 4 | 256 | 932,801 | 910 |
| bnio | 4 | 1,024 | 932,331 | 910 |

#### 4 KB

| Server | Workers | Connections | req/s | MB/s |
| --- | ---: | ---: | ---: | ---: |
| asio | 4 | 64 | 699,707 | 2,733 |
| asio | 4 | 256 | 729,243 | 2,848 |
| asio | 4 | 1,024 | 695,282 | 2,715 |
| bnio | 4 | 64 | 827,510 | 3,232 |
| bnio | 4 | 256 | 832,177 | 3,250 |
| bnio | 4 | 1,024 | 770,464 | 3,009 |

#### 64 KB

| Server | Workers | Connections | req/s | MB/s |
| --- | ---: | ---: | ---: | ---: |
| asio | 4 | 64 | 1,561 | 97 |
| asio | 4 | 256 | 6,221 | 388 |
| asio | 4 | 1,024 | 24,773 | 1,548 |
| bnio | 4 | 64 | 1,572 | 98 |
| bnio | 4 | 256 | 6,303 | 394 |
| bnio | 4 | 1,024 | 25,074 | 1,567 |

### 5.8 Patterns

- Overall bnio / asio throughput ratio: **1.048×**, with bnio winning 39 of 48 configurations.
- Worker averages: 1 worker 1.00×, 2 workers 1.01×, 4 workers 1.13×, 8 workers 1.05×.
- Message averages: 64 B 1.09×, 1 KB 1.06×, 4 KB 1.04×, 64 KB 1.01×.
- Connection averages: 64 1.07×, 256 1.04×, 1,024 1.02×.

## 6. Timer churn

### 6.1 Lifecycle throughput

![Timer lifecycle overview](charts/io_uring/timer_lifecycle_overview.png)

Reference point: update rounds=500.

| Timer count | bnio lifecycle/s | asio lifecycle/s | bnio / asio |
| ---: | ---: | ---: | ---: |
| 256 | 17,973,600 | 16,705,300 | 1.08× |
| 1,024 | 18,288,933 | 18,958,033 | 0.96× |
| 4,096 | 19,034,767 | 17,778,433 | 1.07× |
| 16,384 | 18,939,533 | 18,060,333 | 1.05× |

### 6.2 Active waits

![Timer waits overview](charts/io_uring/timer_waits_overview.png)

Reference point: update rounds=500.

| Timer count | bnio waits/s | asio waits/s | bnio / asio |
| ---: | ---: | ---: | ---: |
| 256 | 10,244,347 | 9,521,450 | 1.08× |
| 1,024 | 10,424,067 | 10,805,433 | 0.96× |
| 4,096 | 10,849,167 | 10,133,113 | 1.07× |
| 16,384 | 10,794,867 | 10,293,733 | 1.05× |

### 6.3 Timer count scaling

![Timer lifecycle vs timers](charts/io_uring/timer_lifecycle_vs_timers.png)

Faceted by update rounds.

### 6.4 Ratio heatmaps

![Timer lifecycle ratio heatmap](charts/io_uring/timer_lifecycle_heatmap.png)

![Timer waits ratio heatmap](charts/io_uring/timer_waits_heatmap.png)

### 6.5 Extreme cases

Best bnio / asio lifecycle ratio:

- Configuration: timers=256, rounds=500, bnio 17,973,600 lifecycle/s, asio 16,705,300 lifecycle/s
- Ratio: 1.08×

Lowest bnio / asio lifecycle ratio:

- Configuration: timers=256, rounds=100, bnio 12,986,167 lifecycle/s, asio 13,645,200 lifecycle/s
- Ratio: 0.95×

### 6.6 Full results

#### 256 timers

| Server | Timers | Rounds | lifecycle/s | waits/s |
| --- | ---: | ---: | ---: | ---: |
| asio | 256 | 100 | 13,645,200 | 7,699,253 |
| asio | 256 | 500 | 16,705,300 | 9,521,450 |
| asio | 256 | 1,000 | 17,374,367 | 9,915,483 |
| bnio | 256 | 100 | 12,986,167 | 7,327,397 |
| bnio | 256 | 500 | 17,973,600 | 10,244,347 |
| bnio | 256 | 1,000 | 17,589,600 | 10,038,300 |

#### 1,024 timers

| Server | Timers | Rounds | lifecycle/s | waits/s |
| --- | ---: | ---: | ---: | ---: |
| asio | 1,024 | 100 | 15,815,833 | 8,924,023 |
| asio | 1,024 | 500 | 18,958,033 | 10,805,433 |
| asio | 1,024 | 1,000 | 19,059,133 | 10,876,967 |
| bnio | 1,024 | 100 | 15,784,367 | 8,906,260 |
| bnio | 1,024 | 500 | 18,288,933 | 10,424,067 |
| bnio | 1,024 | 1,000 | 18,250,533 | 10,415,507 |

#### 4,096 timers

| Server | Timers | Rounds | lifecycle/s | waits/s |
| --- | ---: | ---: | ---: | ---: |
| asio | 4,096 | 100 | 17,026,567 | 9,607,177 |
| asio | 4,096 | 500 | 17,778,433 | 10,133,113 |
| asio | 4,096 | 1,000 | 18,221,733 | 10,399,100 |
| bnio | 4,096 | 100 | 17,323,033 | 9,774,457 |
| bnio | 4,096 | 500 | 19,034,767 | 10,849,167 |
| bnio | 4,096 | 1,000 | 19,039,100 | 10,865,500 |

#### 16,384 timers

| Server | Timers | Rounds | lifecycle/s | waits/s |
| --- | ---: | ---: | ---: | ---: |
| asio | 16,384 | 100 | 17,597,133 | 9,929,093 |
| asio | 16,384 | 500 | 18,060,333 | 10,293,733 |
| asio | 16,384 | 1,000 | 18,028,767 | 10,288,967 |
| bnio | 16,384 | 100 | 18,605,900 | 10,498,300 |
| bnio | 16,384 | 500 | 18,939,533 | 10,794,867 |
| bnio | 16,384 | 1,000 | 19,189,367 | 10,951,300 |

### 6.7 Patterns

- Overall lifecycle ratio: **1.022×**, with bnio winning 8 of 12 configurations.
- Timer-count averages: 256 1.01×, 1,024 0.97×, 4,096 1.04×, 16,384 1.06×.
- Peak lifecycle/s: bnio 19,189,367, asio 19,059,133.

## 7. Compared with the previous run

The previous io_uring report ran on 2026-08-14 at commit 96dea0c. This run uses commit f8ad27d on branch main.

| Metric | Previous (Aug 14) | Current (2026-09-09) | Change |
| --- | ---: | ---: | ---: |
| Throughput average ratio | 1.033× | 1.048× | +1.46% |
| Throughput wins | 31/48 | 39/48 | +8 |
| 1-worker ratio | 0.99× | 1.00× | +0.65% |
| 2-worker ratio | 1.01× | 1.01× | +0.88% |
| 4-worker ratio | 1.11× | 1.13× | +1.27% |
| 8-worker ratio | 1.02× | 1.05× | +3.01% |
| Timer average ratio | 0.966× | 1.022× | +5.81% |
| Timer wins | 4/12 | 8/12 | +4 |

The main code changes in this interval touch the hot paths this benchmark exercises: the defer scheduler always publishes through the shared queue (`1c376b0`), schedule-kind publish paths were composed (`0750191`), the throughput benchmark converged on the defer-accept scheme (`ab473f1`), success paths reuse a shared empty error code (`3088bf2`), and FIFO batch-order restoration was removed on both backends (`f8ad27d`). These changes are not benchmark-specific, but they do touch worker scheduling, shared-queue publication, and timer/socket success-path construction.

## 8. Conclusion

On this 20-thread Fedora Linux machine, bnio leads in TCP echo throughput with an overall ratio of 1.048×, and leads in timer churn with an overall ratio of 1.022×. The scheduler and I/O publication changes are visible in the worker-scaling numbers. In timer churn the only remaining deficit is the 1,024-timer family (0.97×), while 4,096 and 16,384 timers lead by 1.04× and 1.06×.
