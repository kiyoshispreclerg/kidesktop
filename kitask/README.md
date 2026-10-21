# kitask

Process list and system monitor for KiDesktop (GTK2), in the spirit of qps
with graphs you choose like plasma-systemmonitor's. Part of
**kidesktop-base**.

The window has optional graphs on top, a filter bar, the process list and a
status line (process count, CPU, RAM).

## Process list

- **Filter** (`Ctrl+F`) by name, command line, user, window title or exact
  PID; `Esc` clears it. Several terms separated by `|` or `,` show the
  processes matching any of them (`kiplay|kitask`). They are plain
  case-insensitive substrings, not regular expressions. The combo next to it shows all processes, only
  yours, or only those with a window.
- **Columns** are picked from *View > Columns*, by right-clicking any header,
  or in *Settings > Columns*; drag headers to reorder, drag their edges to
  resize. Order, widths and the sort column are remembered.
- **Context menu** (right-click, `Menu` or `Shift+F10`): details, switch to
  the process's window, copy the command line, end (`Delete`), kill
  (`Shift+Delete`), pause, continue, any other signal, change priority.
  Several selected rows get the signal together. When the kernel refuses
  (someone else's process, raising priority), kitask offers to retry through
  `pkexec kill` / `pkexec renice`. Ending, killing and pausing ask first
  (can be turned off).
- **Details** (`Enter` or double-click): everything below plus executable,
  working folder, environment and open files, refreshed live.

| Column | What it is |
|---|---|
| Process, PID, Parent, User, State, Nice, Threads | from `/proc/PID/stat` |
| CPU | share of all cores (100% = every core busy) |
| CPU (core) | relative to one core (a busy 4-thread program shows 400%) |
| CPU Time, Started | |
| Resident, Shared, Virtual | `statm` |
| Exclusive | private pages (`smaps_rollup` Private_Clean + Private_Dirty): about what ending the process gives back |
| Proportional, Swap | `smaps_rollup` |
| X11 Memory | pixmap bytes the process's X connections hold in the server (XRes, like xrestop) |
| GPU, GPU Memory | DRM fdinfo (amdgpu, i915, xe... both the old amdgpu format of kernels 5.14-6.x and the `drm-*` keys) and NVML for NVIDIA |
| Net In, Net Out | TCP bytes per second, see below |
| Disk Read/Write, Total Read/Written | `/proc/PID/io` |
| Window Title, Command Line | |

**Network per process** has no root-free source in Linux except TCP's own
counters (`sock_diag`, the ones `ss -i` shows), matched to processes through
`/proc/PID/fd`. So these columns count TCP only (no UDP: QUIC, DNS, games)
and only your own processes. I/O columns are also own-processes-only; they
show a dash for the rest.

## Graphs

Thin vertical bars, newest at the right. Add, remove, reorder them in
*Settings > Graphs* (or right-click a graph), each with its own minimum,
maximum (or automatic) and color:

- CPU: all cores, or any single core
- RAM, from different points of view:
  - *applications*: total − free − buffers − page cache − reclaimable slab
  - *in use*: total − available (what the kernel can't free quickly)
  - *including caches*: total − free
  - *caches and buffers*
  - swap
- per GPU: busy %, VRAM, and GTT (system memory mapped by the GPU) on amdgpu.
  amdgpu reads sysfs; NVIDIA uses NVML (`libnvidia-ml.so.1`, loaded at
  runtime only if present). Other drivers have no global busy/VRAM numbers
  in sysfs, so they get process columns but no graphs.
- network receive/send (all interfaces but loopback), disk read/write (whole
  physical disks)

## Usage pie chart

Double-click the CPU or RAM figure in the status line (or *View > CPU/RAM
Usage Chart*) for a donut whose full circle is 100% of that resource, one slice
per program (or per process, with *Group by program* off), hover for details.
RAM slices use the proportional size, so they add up to what the processes
really hold; the rest is shown as available and kernel/other. It has its own
refresh interval. Nothing is allocated until it is opened: the window, its
timer and its data are freed on close, and while open its timer drives the
sampler in place of the list's.

## Cost

The list and the graphs refresh on separate intervals (default 2 s and 1 s,
*Settings > General*). The list stops refreshing while the window is
minimized. Columns that are expensive to read are only read while shown,
and only for processes that ran since the last refresh; `smaps_rollup`
(which walks page tables) is re-read only when resident memory moved by 2%
and 1 MiB, or after 30 s, with at most 12 reads per refresh. Rows whose
values didn't change aren't touched. A GPU is only queried while a graph
shows it, and never while it is runtime-suspended (NVML would wake up a
laptop's dGPU).

Measured on the author's machine (~410 processes, Ryzen + GTX 1650, 60 s
after a 10 s warm-up, same nested X server, default settings of each):

| | CPU (one core) | PSS | Exclusive memory | Threads |
|---|---|---|---|---|
| kitask 0.1.0, list every 2 s | 0.9% | 13 MiB | 9 MiB | 4 |
| kitask 0.1.0, list every 1 s | 1.4% | 13 MiB | 9 MiB | 4 |
| kitask, every column and 8 graphs, 2 s | 1.3% | 29 MiB | 24 MiB | 5 |
| qps 2.3.0, every 1 s | 2.6% | 38 MiB | 30 MiB | 36 |
| plasma-systemmonitor 5.27, Processes page | 12.7% | 105 MiB | 89 MiB | 50 |

(The jump with every column is mostly `libnvidia-ml`, loaded only for the GPU
columns.)

## Settings

`$XDG_CONFIG_HOME/kitask.conf`, written by the Settings window and whenever
columns, sort order or window size change:

```
list_interval = 2
graph_interval = 1
columns = name:220,pid:70,user:90,cpu:70,uss:100,rss:100,title:0
sort = cpu desc
graph = cpu min=0 max=100 color=#3c7dd9
graph = vram@0000:08:00.0 min=0 max=auto color=#9b59b6
```

Graph limits are in the source's unit: % for CPU/GPU, MiB for memory, KiB/s
for traffic. GPUs are named by PCI address, so a graph keeps pointing at the
same card.

Command line: `kitask [-f TEXT]` starts with TEXT in the filter.

## Not in 0.1.0

Process tree view, per-process history graphs, UDP traffic, global GPU
numbers for non-amdgpu/non-NVIDIA drivers, a kiconf tab, X-DENSITY.
