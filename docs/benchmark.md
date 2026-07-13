# bupp vs asio TCP Echo Benchmark

## 1. Test Environment

| Item | Value |
| --- | --- |
| Date | 2026-07-12 (Asia/Shanghai) |
| Topology | Single-host loopback TCP |
| OS | Fedora Linux 44 (Workstation Edition) |
| Kernel | 7.1.3-200.fc44.x86_64 |
| Architecture | x86_64 |
| CPU | 13th Gen Intel(R) Core(TM) i9-13900H |
| Logical CPUs | 20 |
| Compiler | GCC 16.1.1 |
| CMake | 4.3.0 |
| Python | 3.14.6 |
| liburing | 2.13 |
| OpenSSL | 3.5.7 |
| Asio | 1.30.2 |

Hostnames, usernames, absolute paths, and network addresses are intentionally omitted from this report.

## 2. Methodology

The benchmark compares two functionally equivalent TCP echo servers:

- `bupp_raw_echo`: C++20 coroutine echo server using bupp on io_uring.
- `asio_raw_echo`: C++20 coroutine echo server using standalone Asio.
- Client: a neutral Python `asyncio` TCP echo load generator.

Each connection runs a strict ping-pong loop: send one fixed-size payload, read the echoed payload, then repeat. The warmup phase is excluded from throughput and latency samples. Latency values are sampled round-trip times in microseconds. Throughput is reported as completed echo requests per second; MB/s counts the echoed payload size once per completed request.

Fairness controls:

- Both servers were rebuilt in Release mode immediately before testing.
- The same client drove both servers.
- The server process was restarted for every measured configuration.
- The process `nofile` soft limit was raised before high-concurrency runs.
- Every result row records the client-side error count. Rows with errors are not treated as clean throughput wins.

## 3. Configuration Matrix

| Dimension | Values |
| --- | --- |
| Server | bupp, asio |
| Worker threads | 1, 2, 4, 8 |
| Concurrent connections | 64, 256, 1024 |
| Message size | 64 B, 1 KB, 4 KB, 64 KB |
| Warmup per run | 10 s |
| Measurement per run | 60 s |
| Repeats | 1 |

The main matrix produced 96 rows. Both bupp and Asio completed all 48 rows with zero client errors and no zero-throughput rows. This run therefore compares clean throughput and latency rather than mixing throughput with connection-error behavior.

## 4. Results

### 4.1 Overview (workers=4, connections=256)

![Throughput Overview](benchmark_charts/overview_bars.png)

| Message Size | bupp req/s | bupp errors | asio req/s | asio errors | Ratio | bupp p50 | asio p50 | bupp p99 | asio p99 |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 64 B | 93,333 | 0 | 87,347 | 0 | 1.07x | 2,740 us | 2,920 us | 3,093 us | 3,079 us |
| 1 KB | 90,470 | 0 | 83,290 | 0 | 1.09x | 2,826 us | 3,063 us | 3,208 us | 3,218 us |
| 4 KB | 81,284 | 0 | 73,626 | 0 | 1.10x | 3,145 us | 3,466 us | 3,560 us | 3,660 us |
| 64 KB | 6,127 | 0 | 6,225 | 0 | 0.98x | 41,822 us | 41,072 us | 43,970 us | 42,503 us |

At the reference point, every row completed with zero client errors. bupp was ahead by 7-10% from 64 B through 4 KB, with similar p99 latency. At 64 KB the two implementations were effectively at parity: Asio was 2% higher in request rate and had a lower p99, which is consistent with the workload shifting from event-loop overhead toward TCP buffering, memory copies, and client-side scheduling.

### 4.2 Throughput vs Connections

![Throughput vs Connections](benchmark_charts/throughput_vs_connections.png)

Connection scaling is shown for workers=4. Small and medium payloads stay in a narrow throughput band as connections increase because the ping-pong client and loopback scheduling dominate once enough requests are in flight. The 64 KB rows scale differently: more concurrent connections increase outstanding bytes and therefore MB/s, while per-request latency stays much higher than the small-payload cases.

### 4.3 Throughput vs Worker Threads

![Throughput vs Workers](benchmark_charts/throughput_vs_workers.png)

| Workers | bupp req/s | bupp errors | asio req/s | asio errors | Ratio |
| ---: | ---: | ---: | ---: | ---: | ---: |
| 1 | 87,219 | 0 | 83,036 | 0 | 1.05x |
| 2 | 81,305 | 0 | 76,847 | 0 | 1.06x |
| 4 | 81,284 | 0 | 73,626 | 0 | 1.10x |
| 8 | 80,409 | 0 | 74,675 | 0 | 1.08x |

For 4 KB / 256 connections, bupp stayed ahead of Asio across all worker counts, from 1.05x at one worker to 1.10x at four workers. Neither implementation scales linearly in this fixed-client loopback setup; adding server workers mostly redistributes scheduler work after the client and kernel path are already saturated.

### 4.4 Latency

![Latency Comparison](benchmark_charts/latency_comparison.png)

Median latency mostly tracks connection count and payload size. The 64 B through 4 KB rows sit in the low-millisecond range at 256 connections, while 64 KB rows are around 41-44 ms because each request carries much more data through the same ping-pong loop.

### 4.5 bupp/asio Ratio Heatmap

![Heatmap Ratio](benchmark_charts/heatmap_ratio.png)

Positive cells mean bupp has higher measured throughput than asio for that worker/connection point. All cells in this run are zero-error comparisons, so the heatmap can be read directly as relative throughput.

### 4.6 Throughput and Tail Latency Scatter

![Throughput Latency Scatter](benchmark_charts/throughput_latency_scatter.png)

The scatter plot includes the zero-error rows from both implementations. Small payloads cluster at high request rate and low p99 latency; 64 KB rows trade request rate for much higher per-request latency.

### 4.7 Best and Hardest Relative Cases

Best zero-error bupp/asio throughput ratio:

- Configuration: workers=8, connections=256, message_size=64 B
- bupp: 93,532 req/s, errors 0, p99 3,068 us
- asio: 83,870 req/s, errors 0, p99 3,201 us
- Ratio: 1.12x

Most challenging zero-error bupp/asio throughput ratio:

- Configuration: workers=1, connections=1024, message_size=4 KB
- bupp: 80,733 req/s, errors 0, p99 16,442 us
- asio: 88,900 req/s, errors 0, p99 15,094 us
- Ratio: 0.91x

Error summary:

- No bupp or Asio result row reported client errors.

### 4.8 Legacy Queue/Timer-Flush Tuning Sweep

![Queue Tuning Sweep](benchmark_charts/queue_tuning_sweep.png)

This archived sweep predates the passive worker-drain queue. It used the same
10 s warmup and 60 s measurement window as the main matrix to measure the old
queue-length/timer-flush implementation. The timer column is historical and is
not a configuration option in the current runtime.

| Queue / flush | Connections | Size | req/s | errors | p50 | p99 |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| 16 / 100 us | 256 | 64 B | 92,326 | 0 | 2,767 us | 2,993 us |
| 16 / 100 us | 256 | 1 KB | 90,572 | 0 | 2,822 us | 3,044 us |
| 16 / 100 us | 1024 | 64 B | 84,727 | 0 | 12,198 us | 12,921 us |
| 16 / 100 us | 1024 | 1 KB | 78,407 | 0 | 13,190 us | 13,864 us |
| 32 / 250 us | 256 | 64 B | 93,064 | 0 | 2,746 us | 3,031 us |
| 32 / 250 us | 256 | 1 KB | 90,442 | 0 | 2,827 us | 3,110 us |
| 32 / 250 us | 1024 | 64 B | 101,879 | 0 | 8,870 us | 12,834 us |
| 32 / 250 us | 1024 | 1 KB | 77,686 | 0 | 13,296 us | 15,140 us |
| 64 / 1000 us | 256 | 64 B | 93,330 | 0 | 2,738 us | 3,111 us |
| 64 / 1000 us | 256 | 1 KB | 91,135 | 0 | 2,806 us | 3,174 us |
| 64 / 1000 us | 1024 | 64 B | 83,428 | 0 | 12,383 us | 13,600 us |
| 64 / 1000 us | 1024 | 1 KB | 77,657 | 0 | 13,330 us | 14,204 us |

All legacy sweep rows completed with zero client errors. These numbers remain
as historical evidence only. The current implementation has neither a queue
threshold nor a flush timer: workers passively take all published I/O after CPU
work and repeat that check during the pre-sleep handshake.

## 5. Interpretation

These are plausible explanations from the implementation model and the observed benchmark shape; they were not separately validated with profiling in this run.

1. The current bupp single-worker and multi-worker paths are stable across the tested matrix. The absence of client errors on every bupp row is the most important stability signal in this run.
2. bupp's advantage on 64 B through 4 KB at the reference point is consistent with naturally accumulated io_uring submission batches. The fixed ping-pong workload gives repeated read/write pairs that can benefit from lower submission overhead.
3. The 64 KB rows are near parity because the bottleneck shifts away from event-loop dispatch and toward payload movement through TCP buffers, memory copies, and the Python client.
4. Worker scaling is intentionally read as a shape, not a linear speedup claim. At 4 KB / 256 connections, adding server workers does not raise bupp throughput after one worker, which suggests the single-host client/kernel path is already the limiting factor.
5. The legacy queue/timer sweep is not directly comparable to the current
   passive-drain state machine and should be rerun before drawing current
   tuning conclusions.

## 6. Key Logs and Checks

- Build: Release configure and raw echo target build completed successfully.
- Smoke test: bupp and Asio each completed a 16-connection / 64 B short run with zero errors before the main matrix.
- Main matrix: completed 96/96 result rows.
- Error summary: Asio error rows 0/48; bupp error rows 0/48; bupp total client errors 0; bupp zero-throughput rows 0.
- Focused tuning sweep: completed 12/12 bupp rows with zero client errors.

## 7. Full Results

The table below shows the aggregated rows for workers=4. Error counts are included to make correctness visible; all rows shown here were clean.

#### message_size = 64 B

| server | workers | connections | req/s | MB/s | errors | p50 | p99 | p999 |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| asio | 4 | 64 | 86,995 | 5.3 | 0 | 733 us | 784 us | 838 us |
| asio | 4 | 256 | 87,347 | 5.3 | 0 | 2,920 us | 3,079 us | 3,361 us |
| asio | 4 | 1024 | 80,130 | 4.9 | 0 | 12,907 us | 13,736 us | 15,781 us |
| bupp | 4 | 64 | 92,167 | 5.6 | 0 | 694 us | 964 us | 1,016 us |
| bupp | 4 | 256 | 93,333 | 5.7 | 0 | 2,740 us | 3,093 us | 3,371 us |
| bupp | 4 | 1024 | 80,848 | 4.9 | 0 | 12,776 us | 13,632 us | 15,806 us |

#### message_size = 1 KB

| server | workers | connections | req/s | MB/s | errors | p50 | p99 | p999 |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| asio | 4 | 64 | 84,284 | 82.3 | 0 | 756 us | 817 us | 873 us |
| asio | 4 | 256 | 83,290 | 81.3 | 0 | 3,063 us | 3,218 us | 3,500 us |
| asio | 4 | 1024 | 76,742 | 74.9 | 0 | 13,601 us | 14,267 us | 15,827 us |
| bupp | 4 | 64 | 89,377 | 87.3 | 0 | 715 us | 1,006 us | 1,067 us |
| bupp | 4 | 256 | 90,470 | 88.3 | 0 | 2,826 us | 3,208 us | 3,519 us |
| bupp | 4 | 1024 | 77,638 | 75.8 | 0 | 13,321 us | 14,207 us | 15,698 us |

#### message_size = 4 KB

| server | workers | connections | req/s | MB/s | errors | p50 | p99 | p999 |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| asio | 4 | 64 | 75,142 | 293.5 | 0 | 848 us | 926 us | 1,006 us |
| asio | 4 | 256 | 73,626 | 287.6 | 0 | 3,466 us | 3,660 us | 3,980 us |
| asio | 4 | 1024 | 86,143 | 336.5 | 0 | 11,944 us | 16,161 us | 16,828 us |
| bupp | 4 | 64 | 80,788 | 315.6 | 0 | 791 us | 1,050 us | 1,130 us |
| bupp | 4 | 256 | 81,284 | 317.5 | 0 | 3,145 us | 3,560 us | 3,746 us |
| bupp | 4 | 1024 | 85,914 | 335.6 | 0 | 11,914 us | 15,997 us | 16,895 us |

#### message_size = 64 KB

| server | workers | connections | req/s | MB/s | errors | p50 | p99 | p999 |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| asio | 4 | 64 | 1,558 | 97.4 | 0 | 41,012 us | 42,183 us | 42,430 us |
| asio | 4 | 256 | 6,225 | 389.1 | 0 | 41,072 us | 42,503 us | 43,136 us |
| asio | 4 | 1024 | 24,513 | 1,532.0 | 0 | 41,777 us | 45,696 us | 51,627 us |
| bupp | 4 | 64 | 1,514 | 94.6 | 0 | 42,179 us | 44,417 us | 45,441 us |
| bupp | 4 | 256 | 6,127 | 382.9 | 0 | 41,822 us | 43,970 us | 45,912 us |
| bupp | 4 | 1024 | 22,852 | 1,428.3 | 0 | 42,852 us | 63,653 us | 71,068 us |
