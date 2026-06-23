# FlashDB Benchmark Report

Date: Tue Jun 23 13:36:05 2026
Target: 127.0.0.1:6379
Threads: 2

## Write Benchmark
| Metric | Value |
|--------|-------|
| Operations | 1000 |
| Throughput | 5276 ops/sec |
| Avg Latency | 0.321 ms |
| P99 Latency | 1.922 ms |
| Errors | 0 |

## Read Benchmark
| Metric | Value |
|--------|-------|
| Operations | 1000 |
| Throughput | 4307 ops/sec |
| Avg Latency | 0.324 ms |
| P99 Latency | 0.598 ms |
| Errors | 0 |

## Mixed Benchmark (80% Read / 20% Write)
| Metric | Value |
|--------|-------|
| Operations | 1000 |
| Throughput | 8136 ops/sec |
| Avg Latency | 0.225 ms |
| P99 Latency | 1.115 ms |
| Errors | 0 |
