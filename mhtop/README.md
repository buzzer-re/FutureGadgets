# mhtop

A better `htop` for macOS: per-core CPU, memory, GPU, power, disk and network in one view. Plain C, no dependencies.

![mhtop](screenshot.png)

## GPU view

Press `g` (or start with `mhtop -g`) for a full GPU inspector:

- **Overview**: device, renderer and tiler utilization, active residency, clock, GPU power, temperature, GPU memory and DRAM bandwidth, each with session stats (avg, p95, max), history graphs, the GPU processes and a focus process.
- **Processes**: every process with GPU work: GPU% now / average / peak, GPU time, estimated power (its share of GPU time × GPU power), command queues, graphics memory, footprint, time since its last submission, API. `Enter` pins the focus, `s` changes the sort, `k` sends a signal.
- **Clocks & Power**: residency per performance state (MHz), GPU / DRAM / ANE power, energy, power target, clock limiting by power or thermal controllers, throttle counters, GPU temperature.
- **Memory & Bandwidth**: driver memory (in use, allocated, parameter buffer), the `iogpu.wired_limit_mb` wired limit and headroom, unified memory and pressure, DRAM read/write bandwidth, GPU memory latency, graphics memory per process.
- **Events**: processes starting or stopping GPU work or exiting, clock limits, thermal limiting, throttling, GPU restarts and recoveries, memory pressure, GPU memory reaching 90% of the wired limit.

`r` resets the session stats. Once opened, telemetry keeps sampling, so stats survive closing the view.

## GPU telemetry from the command line

`mhtop --gpu` prints the same data without the UI. It needs no terminal, so scripts and agents can call it:

```sh
mhtop --gpu                          # one report after a 1 s sample
mhtop --gpu -n 30 -d 1 -p llama      # 30 s profile of processes matching "llama" (or a pid)
mhtop --gpu --json -n 0 -d 0.5       # JSON Lines, one object per sample, until interrupted
```

Each JSON object has `device`, `utilization`, `clock` (with `pstates`), `power`, `temperature`, `memory`, `bandwidth`, `limits`, `health`, `session` (avg/min/max/p95 per metric since start), `processes` and the `events` since the previous line. Metrics this Mac does not report are `null`.

Sources: IOAccelerator statistics and user clients, IOReport (energy, performance states, limiters, memory controllers), SMC GPU temperature sensors, and task ledgers for graphics memory. Bandwidth and latency are estimates from the memory controller histograms. Graphics memory of other users' processes needs `sudo`.
