# Gateway Benchmark Summary

`instance=unknown cpus=12 N=20000 c=10 trials=5`

_Latency = median across 5 trial(s); p99 shows the min–max across trials. rps in the latency table is completed req/s at the fixed concurrency (latency-coupled); see the capacity table for sustained throughput._

## Latency (ms, median of trials)

| target | variant | ok/fail | rps | p50 | p90 | p99 | p99 min–max | ttft p50 | gap p50 | overhead p50 |
|---|---|--:|--:|--:|--:|--:|--:|--:|--:|--:|
| baseline | chat/nonstream | 100000/0 | 44342 | 0.18 | 0.31 | 0.62 | 0.52–0.64 |  |  | 0.00 |
| baseline | chat/stream | 100000/0 | 10281 | 0.84 | 1.44 | 2.23 | 2.18–2.24 | 0.38 | 0.00 | 0.00 |
| baseline | responses/nonstream | 100000/0 | 43565 | 0.18 | 0.31 | 0.63 | 0.54–0.65 |  |  | 0.00 |
| baseline | responses/stream | 100000/0 | 7083 | 1.27 | 2.07 | 2.99 | 2.95–3.15 | 0.63 | 0.00 | 0.00 |
| baseline | messages/nonstream | 100000/0 | 42939 | 0.19 | 0.31 | 0.62 | 0.54–0.64 |  |  | 0.00 |
| baseline | messages/stream | 100000/0 | 7023 | 1.26 | 2.10 | 3.10 | 3.02–3.13 | 0.45 | 0.00 | 0.00 |
| gomodel | chat/nonstream | 100000/0 | 13330 | 0.65 | 1.08 | 1.59 | 1.47–1.63 |  |  | 0.47 |
| gomodel | chat/stream | 100000/0 | 5768 | 1.59 | 2.47 | 3.40 | 3.28–3.43 | 1.24 | 0.00 | 0.75 |
| gomodel | responses/nonstream | 100000/0 | 12638 | 0.69 | 1.06 | 2.14 | 2.02–2.34 |  |  | 0.51 |
| gomodel | responses/stream | 100000/0 | 4429 | 2.06 | 3.26 | 4.73 | 4.54–4.91 | 1.47 | 0.00 | 0.79 |
| gomodel | messages/nonstream | 100000/0 | 14609 | 0.60 | 0.90 | 1.87 | 1.81–1.95 |  |  | 0.41 |
| gomodel | messages/stream | 100000/0 | 3787 | 2.44 | 3.75 | 5.23 | 5.11–5.25 | 1.42 | 0.00 | 1.18 |
| litellm | chat/nonstream | 100000/0 | 684 | 10.95 | 21.35 | 27.12 | 26.37–27.44 |  |  | 10.77 |
| litellm | chat/stream | 57690/0 | 186 | 46.90 | 83.17 | 98.90 | 55.40–100.27 | 46.87 | 0.00 | 46.06 |
| litellm | responses/nonstream | 100000/0 | 633 | 16.63 | 21.04 | 23.14 | 21.80–32.87 |  |  | 16.45 |
| litellm | responses/stream | 100000/0 | 374 | 25.88 | 35.68 | 45.10 | 41.71–53.66 | 25.85 | 0.00 | 24.61 |
| litellm | messages/nonstream | 100000/0 | 606 | 12.63 | 25.00 | 31.49 | 31.28–31.71 |  |  | 12.44 |
| litellm | messages/stream | 99330/0 | 350 | 30.37 | 35.11 | 37.92 | 36.55–39.53 | 14.39 | 0.35 | 29.11 |
| portkey | chat/nonstream | 100000/0 | 1179 | 7.74 | 10.13 | 16.64 | 16.29–16.95 |  |  | 7.56 |
| portkey | chat/stream | 100000/0 | 348 | 28.44 | 30.77 | 33.91 | 33.77–34.99 | 28.43 | 0.00 | 27.60 |
| portkey | responses/nonstream | 100000/0 | 1200 | 7.83 | 10.00 | 13.34 | 12.87–14.33 |  |  | 7.65 |
| portkey | responses/stream | 100000/0 | 351 | 28.23 | 30.56 | 33.58 | 33.36–34.30 | 28.21 | 0.00 | 26.96 |
| portkey | messages/nonstream | 0/100000 | 0 | — | — | — | — |  |  | — |
| portkey | messages/stream | 0/100000 | 0 | — | — | — | — | — | — | — |
| bifrost | chat/nonstream | 100000/0 | 8757 | 0.98 | 1.63 | 3.23 | 2.78–3.36 |  |  | 0.80 |
| bifrost | chat/stream | 100000/0 | 2517 | 3.61 | 5.66 | 8.30 | 7.94–8.56 | 2.39 | 0.00 | 2.77 |
| bifrost | responses/nonstream | 100000/0 | 8510 | 1.02 | 1.65 | 3.26 | 2.89–3.29 |  |  | 0.84 |
| bifrost | responses/stream | 24850/50 | 83 | 4.93 | 7.73 | 13.59 | 12.39–14.33 | 3.97 | 0.00 | 3.66 |
| bifrost | messages/nonstream | 100000/0 | 9145 | 0.92 | 1.52 | 3.70 | 3.57–3.96 |  |  | 0.73 |
| bifrost | messages/stream | 0/50 | 0 | — | — | — | — | — | — | — |
| tensorzero | chat/nonstream | 81207/0 | 271 | 41.92 | 42.06 | 42.68 | 42.66–42.70 |  |  | 41.74 |
| tensorzero | chat/stream | 89339/0 | 297 | 41.87 | 42.16 | 43.03 | 43.03–43.03 | 0.62 | 0.00 | 41.03 |
| tensorzero | responses/nonstream | 0/100000 | 0 | — | — | — | — |  |  | — |
| tensorzero | responses/stream | 0/100000 | 0 | — | — | — | — | — | — | — |
| tensorzero | messages/nonstream | 0/100000 | 0 | — | — | — | — |  |  | — |
| tensorzero | messages/stream | 0/100000 | 0 | — | — | — | — | — | — | — |
| omniroute | chat/nonstream | 17151/0 | 57 | 172.19 | 187.95 | 292.39 | 286.62–302.28 |  |  | 172.01 |
| omniroute | chat/stream | 13991/0 | 47 | 208.21 | 251.42 | 330.48 | 329.15–342.01 | 202.81 | 0.00 | 207.37 |
| omniroute | responses/nonstream | 16611/0 | 56 | 175.91 | 195.10 | 307.18 | 296.46–316.01 |  |  | 175.73 |
| omniroute | responses/stream | 12750/0 | 43 | 231.84 | 257.41 | 353.93 | 345.77–366.48 | 224.38 | 0.00 | 230.57 |
| omniroute | messages/nonstream | 16458/0 | 55 | 177.08 | 198.83 | 316.91 | 309.10–322.55 |  |  | 176.89 |
| omniroute | messages/stream | 12765/0 | 43 | 229.59 | 260.22 | 385.56 | 379.63–393.08 | 221.98 | 0.00 | 228.33 |
| llmbridge | chat/nonstream | 100000/0 | 33597 | 0.25 | 0.38 | 0.72 | 0.56–0.76 |  |  | 0.07 |
| llmbridge | chat/stream | 100000/0 | 8973 | 1.02 | 1.39 | 1.92 | 1.81–2.08 | 0.64 | 0.00 | 0.18 |
| llmbridge | responses/nonstream | 100000/0 | 33301 | 0.26 | 0.39 | 0.71 | 0.57–0.79 |  |  | 0.08 |
| llmbridge | responses/stream | 100000/0 | 6612 | 1.38 | 2.04 | 2.67 | 2.55–2.89 | 0.79 | 0.00 | 0.11 |
| llmbridge | messages/nonstream | 0/100000 | 0 | — | — | — | — |  |  | — |
| llmbridge | messages/stream | 0/100000 | 0 | — | — | — | — | — | — | — |
| llmbridge-anthropic | chat/nonstream | 100000/0 | 32354 | 0.28 | 0.37 | 0.68 | 0.56–0.69 |  |  | 0.10 |
| llmbridge-anthropic | chat/stream | 100000/0 | 6524 | 1.40 | 1.99 | 2.63 | 2.51–2.65 | 0.74 | 0.00 | 0.56 |
| llmbridge-anthropic | responses/nonstream | 0/100000 | 0 | — | — | — | — |  |  | — |
| llmbridge-anthropic | responses/stream | 0/100000 | 0 | — | — | — | — | — | — | — |
| llmbridge-anthropic | messages/nonstream | 100000/0 | 33770 | 0.25 | 0.37 | 0.68 | 0.59–0.71 |  |  | 0.06 |
| llmbridge-anthropic | messages/stream | 100000/0 | 6568 | 1.38 | 2.06 | 2.72 | 2.61–2.75 | 0.69 | 0.00 | 0.12 |
| tcprelay | chat/nonstream | 100000/0 | 34581 | 0.24 | 0.38 | 0.72 | 0.59–0.81 |  |  | 0.06 |
| tcprelay | chat/stream | 100000/0 | 7723 | 1.11 | 1.97 | 3.11 | 3.02–3.34 | 0.66 | 0.00 | 0.27 |
| tcprelay | responses/nonstream | 100000/0 | 33581 | 0.25 | 0.39 | 0.76 | 0.64–0.81 |  |  | 0.07 |
| tcprelay | responses/stream | 100000/0 | 4838 | 1.79 | 3.24 | 4.70 | 4.42–4.92 | 0.97 | 0.00 | 0.52 |
| tcprelay | messages/nonstream | 100000/0 | 33458 | 0.25 | 0.39 | 0.75 | 0.64–0.80 |  |  | 0.06 |
| tcprelay | messages/stream | 100000/0 | 5096 | 1.74 | 2.95 | 4.44 | 4.26–4.58 | 0.77 | 0.00 | 0.48 |

## Capacity (chat non-stream, sustained req/s by concurrency)

| target | c=1 | c=2 | c=4 | c=8 | c=16 | c=32 | c=64 | c=128 | c=256 | peak rps | @c | knee c |
|---|--:|--:|--:|--:|--:|--:|--:|--:|--:|--:|--:|--:|
| baseline | 10128 | 18714 | 30696 | 43648 | 51916 | 60155 | 63351 | 66029 | 66199 | 66199 | 256 | 64 |
| gomodel | 3422 | 5404 | 8760 | 11605 | 14152 | 15478 | 16417 | 16468 | 14887 | 16468 | 128 | 64 |
| litellm | 181 | 328 | 499 | 670 | 689 | 822 | 880 | 878 | 877 | 880 | 64 | 64 |
| portkey | 959 | 1214 | 1232 | 1232 | 1212 | 1206 | 1170 | 1143 | 1170 | 1232 | 4 | 2 |
| bifrost | 2083 | 3666 | 5953 | 8381 | 9522 | 9930 | 10035 | 9999 | 10043 | 10043 | 256 | 32 |
| tensorzero | 24 | 48 | 100 | 214 | 415 | 832 | 1626 | 3300 | 6834 | 6834 | 256 | 256 |
| omniroute | 53 | 56 | 56 | 57 | 57 | 58 | 55 | 53 | 49 | 58 | 32 | 2 |
| llmbridge | 7967 | 13702 | 22391 | 32599 | 36490 | 39767 | 40677 | 38576 | 36646 | 40677 | 64 | 32 |
| llmbridge-anthropic | 7769 | 12959 | 21263 | 28617 | 32051 | 33815 | 35298 | 33703 | 32536 | 35298 | 64 | 32 |
| tcprelay | 8369 | 14674 | 23225 | 32278 | 38991 | 43099 | 43675 | 43858 | 43973 | 43973 | 256 | 32 |

## Resources

| gateway | image MB (compressed) | image MB (on-disk) | startup s | idle MB | peak MB | avg CPU % | load rps | rps/CPU% |
|---|--:|--:|--:|--:|--:|--:|--:|--:|
| gomodel | 19.0 | 19.2 | 0.53 | 76.6 | 36.2 | 595.6 | 13606 | 22.8 |
| litellm | 359.2 | 361.4 | 40.69 | 9528.3 | 9529.3 | 705.3 | 688 | 1.0 |
| portkey | 58.6 | 59.1 | 0.98 | 197.7 | 164.6 | 119.5 | 1242 | 10.4 |
| bifrost | 82.9 | 83.3 | 7.67 | 492.7 | 404.6 | 790.0 | 8736 | 11.1 |
| tensorzero | 90.9 | 91.4 | 0.52 | 131.7 | 119.9 | 16.7 | 273 | 16.3 |
| omniroute | 1174.8 | 1179.0 | 7.43 | 928.5 | 930.2 | 108.3 | 48 | 0.4 |
| llmbridge | 31.5 | 31.8 | 0.30 | 30.3 | 32.1 | 97.8 | 34626 | 354.1 |
| llmbridge-anthropic | 31.5 | 31.8 | 0.30 | 29.4 | 30.4 | 101.6 | 30493 | 300.1 |
| tcprelay | 6.5 | 6.6 | 0.30 | 2.6 | 5.1 | 217.3 | 34208 | 157.4 |
