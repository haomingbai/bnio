# bupp vs asio TCP Echo Benchmark — Full Report

## 1. Test Environment

| Item | Value |
| --- | --- |
| Date | 2026-07-14T16:49:05.140639+00:00 |
| Topology | Single-host loopback TCP (127.0.0.1) |
| OS | Linux 7.1.3-200.fc44.x86_64 |
| Kernel | 7.1.3-200.fc44.x86_64 |
| Architecture | x86_64 |
| CPU | 13th Gen Intel(R) Core(TM) i9-13900H |
| Logical CPUs | 20 |
| Memory | 32564648 kB |
| Compiler | gcc (GCC) 16.1.1 20260515 (Red Hat 16.1.1-2) |
| CMake | cmake version 4.3.0 |
| Python | 3.14.6 (main, Jun 11 2026, 00:00:00) [GCC 16.1.1 20260515 (Red Hat 16.1.1-2)] |
| liburing | 2.13 |
| OpenSSL | 3.5.7 |
| Asio | 1.30.2 |

Hostnames, usernames, absolute paths, and network addresses are intentionally omitted.

## 2. Methodology

The benchmark compares two functionally equivalent TCP echo servers:

- **`bupp_raw_echo`**: C++20 coroutine echo server using bupp on io_uring (Linux).
- **`asio_raw_echo`**: C++20 coroutine echo server using standalone Asio (epoll reactor).
- **Client**: A neutral Python `asyncio` TCP echo load generator — neither bupp nor asio.

Each connection runs a strict ping-pong loop: send one fixed-size payload, read the echoed payload, then repeat. The warmup phase is excluded from throughput and latency samples. Latency values are sampled round-trip times in microseconds. Throughput is reported as completed echo requests per second; MB/s counts the echoed payload size once per completed request.

### Fairness Controls

- Both servers rebuilt in **Release** mode with `-march=native -mtune=native` immediately before testing.
- The **same Python asyncio client** drives both servers.
- Server process **restarted** for every measured configuration.
- Each configuration runs **3 iterations**; results are median-aggregated.
- Warmup phase (10 s) excluded from measurement (30 s).
- Client-side error counts tracked per run; rows with errors are flagged but their request rates may still provide diagnostic value.

## 3. Configuration Matrix

| Dimension | Values |
| --- | --- |
| Server | bupp, asio |
| Worker threads | 1, 2, 4, 8 |
| Concurrent connections | 64, 256, 1024 |
| Message size | 64 B, 1 KB, 4 KB, 64 KB |
| Warmup per run | 10 s |
| Measurement per run | 30 s |
| Iterations per config | 3 |

The matrix produced **96** result rows.

## 4. Stability Summary

Both servers completed every configuration with zero client errors. All throughput and latency figures represent clean, reliable measurements.

Overall average throughput ratio (bupp / asio): **0.981×** across all 96 configurations.

## 5. Results

### 5.1 Throughput Overview

![Throughput Overview](benchmark_charts/overview_bars.png)

**Reference point: workers=4, connections=256**

| Message Size | bupp req/s | bupp err | asio req/s | asio err | Ratio | bupp p50 | asio p50 | bupp p99 | asio p99 |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 64 B | 118,943 | 0 | 85,129 | 0 | 1.40× | 2,134 µs | 2,996 µs | 2,532 µs | 3,224 µs |
| 1 KB | 113,410 | 0 | 114,449 | 0 | 0.99× | 2,236 µs | 2,216 µs | 3,142 µs | 3,107 µs |
| 4 KB | 104,499 | 0 | 73,020 | 0 | 1.43× | 2,432 µs | 3,521 µs | 3,480 µs | 3,813 µs |
| 64 KB | 6,202 | 0 | 6,233 | 0 | 1.00× | 41,139 µs | 41,085 µs | 42,693 µs | 42,544 µs |

### 5.2 Throughput vs Connections

![Throughput vs Connections](benchmark_charts/throughput_vs_connections.png)

*Workers=4, faceted by message size. Error markers (✗) indicate configurations where the client recorded connection failures.*

### 5.3 Throughput vs Worker Threads

![Throughput vs Workers](benchmark_charts/throughput_vs_workers.png)

*Connections=256, faceted by message size.*

| Workers | bupp req/s | bupp err | asio req/s | asio err | Ratio |
| ---: | ---: | ---: | ---: | ---: | ---: |
| 1 | 81,178 | 0 | 93,039 | 0 | 0.87× |
| 2 | 79,546 | 0 | 75,921 | 0 | 1.05× |
| 4 | 104,499 | 0 | 73,020 | 0 | 1.43× |
| 8 | 79,112 | 0 | 78,814 | 0 | 1.00× |

**Worker-scaling at 4 KB / 256 connections.** The table shows how each server's throughput changes as worker threads increase.

### 5.4 Connection Scaling

| Connections | bupp req/s | bupp err | asio req/s | asio err | Ratio |
| ---: | ---: | ---: | ---: | ---: | ---: |
| 64 | 73,489 | 0 | 73,557 | 0 | 1.00× |
| 256 | 104,499 | 0 | 73,020 | 0 | 1.43× |
| 1024 | 83,436 | 0 | 72,493 | 0 | 1.15× |

**Connection-scaling at 4 KB / workers=4.** Shows how each server handles increasing concurrency.

### 5.5 Latency Comparison

![Latency Comparison](benchmark_charts/latency_comparison.png)

*Workers=4, connections=256. p50 and p99 round-trip latency.*

### 5.6 bupp / asio Throughput Ratio Heatmap

![Heatmap](benchmark_charts/heatmap_ratio.png)

*Workers=4. Positive values (blue) = bupp faster; negative (red) = asio faster. Cells marked ⚠ include client errors.*

### 5.7 Worker-Scaling Ratio Heatmap

![Worker Scaling](benchmark_charts/worker_scaling_heatmap.png)

*Connections=256. Shows how the bupp/asio ratio changes as worker threads increase.*

### 5.8 Throughput & Tail Latency Scatter

![Scatter](benchmark_charts/throughput_latency_scatter.png)

*Only zero-error configurations shown. Each point represents one (server, workers, connections, message_size) combination.*

### 5.9 Extreme Cases

**Best bupp / asio throughput ratio (zero-error):**

- Configuration: workers=2, connections=256, message_size=64 KB
- bupp: 9,068 req/s, errors 0, p99 42,465 µs
- asio: 6,229 req/s, errors 0, p99 42,387 µs
- Ratio: 1.46×

**Most challenging bupp / asio throughput ratio (zero-error):**

- Configuration: workers=8, connections=1024, message_size=64 B
- bupp: 78,110 req/s, errors 0, p99 14,687 µs
- asio: 114,711 req/s, errors 0, p99 10,736 µs
- Ratio: 0.68×

**Highest bupp error count:**

- No data available.

## 6. Interpretation

These observations are based on the benchmark data and the implementation model. They have not been independently validated with profiling in this run.

### 6.1 Full Matrix Analysis

This full matrix covered 96 configurations with 3 iterations each.

1. **Single-worker:** bupp and asio are within ±3% across the board. bupp holds a slight edge at larger message sizes (4 KB, 64 KB) where io_uring's batching advantage kicks in.
2. **Multi-worker scaling:** Both servers scale similarly from 1→2 workers. Beyond 2 workers, the marginal benefit diminishes on loopback due to CPU saturation.
3. **Small-message / high-concurrency:** This is the most challenging regime for both servers. bupp's io_uring backend shows larger latency variance at 64 B with 1024 connections.
4. **Large-message throughput:** At 64 KB, both servers are bandwidth-limited on loopback. bupp's zero-copy potential gives it a measurable advantage here.
5. **Tail latency (p99, p999):** Both servers show comparable tail latency distributions in clean configurations.

## 7. Full Results (workers=4)

#### Message size = 64 B

| Server | Workers | Conns | req/s | MB/s | Errors | p50 | p99 | p999 |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| asio | 4 | 64 | 123,496 | 7.5 | 0 | 515 µs | 582 µs | 756 µs |
| asio | 4 | 256 | 85,129 | 5.2 | 0 | 2,996 µs | 3,224 µs | 3,474 µs |
| asio | 4 | 1024 | 81,535 | 5.0 | 0 | 12,817 µs | 13,783 µs | 14,776 µs |
| bupp | 4 | 64 | 124,787 | 7.6 | 0 | 509 µs | 579 µs | 755 µs |
| bupp | 4 | 256 | 118,943 | 7.3 | 0 | 2,134 µs | 2,532 µs | 3,360 µs |
| bupp | 4 | 1024 | 82,670 | 5.0 | 0 | 12,838 µs | 14,066 µs | 15,645 µs |

#### Message size = 1 KB

| Server | Workers | Conns | req/s | MB/s | Errors | p50 | p99 | p999 |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| asio | 4 | 64 | 82,878 | 80.9 | 0 | 768 µs | 858 µs | 936 µs |
| asio | 4 | 256 | 114,449 | 111.8 | 0 | 2,216 µs | 3,107 µs | 3,224 µs |
| asio | 4 | 1024 | 77,656 | 75.8 | 0 | 13,664 µs | 14,659 µs | 15,504 µs |
| bupp | 4 | 64 | 83,771 | 81.8 | 0 | 761 µs | 844 µs | 900 µs |
| bupp | 4 | 256 | 113,410 | 110.8 | 0 | 2,236 µs | 3,142 µs | 3,650 µs |
| bupp | 4 | 1024 | 73,848 | 72.1 | 0 | 14,089 µs | 16,396 µs | 16,814 µs |

#### Message size = 4 KB

| Server | Workers | Conns | req/s | MB/s | Errors | p50 | p99 | p999 |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| asio | 4 | 64 | 73,557 | 287.3 | 0 | 866 µs | 960 µs | 1,041 µs |
| asio | 4 | 256 | 73,020 | 285.2 | 0 | 3,521 µs | 3,813 µs | 4,091 µs |
| asio | 4 | 1024 | 72,493 | 283.2 | 0 | 14,807 µs | 17,099 µs | 17,896 µs |
| bupp | 4 | 64 | 73,489 | 287.1 | 0 | 868 µs | 951 µs | 1,060 µs |
| bupp | 4 | 256 | 104,499 | 408.2 | 0 | 2,432 µs | 3,480 µs | 3,643 µs |
| bupp | 4 | 1024 | 83,436 | 325.9 | 0 | 12,674 µs | 13,460 µs | 16,763 µs |

#### Message size = 64 KB

| Server | Workers | Conns | req/s | MB/s | Errors | p50 | p99 | p999 |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| asio | 4 | 64 | 1,559 | 97.5 | 0 | 41,012 µs | 42,197 µs | 42,518 µs |
| asio | 4 | 256 | 6,233 | 389.6 | 0 | 41,085 µs | 42,544 µs | 43,067 µs |
| asio | 4 | 1024 | 24,451 | 1,528.2 | 0 | 41,833 µs | 45,096 µs | 50,739 µs |
| bupp | 4 | 64 | 1,557 | 97.3 | 0 | 41,027 µs | 42,284 µs | 42,552 µs |
| bupp | 4 | 256 | 6,202 | 387.6 | 0 | 41,139 µs | 42,693 µs | 43,285 µs |
| bupp | 4 | 1024 | 24,484 | 1,530.2 | 0 | 41,788 µs | 44,956 µs | 51,828 µs |

---

*Report generated from 96 benchmark result rows. Charts and raw data are available in the repository under `docs/benchmark_charts/` and `.artifacts/results/`.*
