# Ctrl-C 投递延迟测试指南

## 1. 测试目的

对比 Ctrl-C 触发时两种投递机制的延迟分布：

- **信号**：`kill_pgrp()` → SIGINT，内核在返回用户态时由 `do_signal` 投递
- **用户态中断 (UINTR)**：`uintr_notify_pgrp()` → `uintr_kern_notify()` → APIC 直接投递到用户态

通过 sysctl `kernel.uintr_isig_mode` 互斥切换两者，测量从父进程 `write(0x03)` 到接收方 handler 记录时间戳的耗时。

## 2. 测量方法

### 2.1 程序结构

```
父进程 (trigger)                    子进程 (receiver)
─────────────────                   ─────────────────
1. sh->round = i                    1. spin 等 sh->round == i
2. usleep(300)  ← 给接收方准备时间  2. _stui() 开启 UIF
3. t0[i] = rdtsc()                  3. spin 等 t_sig[i] | t_uintr[i]
4. write(master, 0x03)              ┌──────────────────────┐
   └─ workqueue → __isig            │ handler:             │
      ├─ mode 0: kill_pgrp → SIGINT │   t = rdtsc()        │
      └─ mode 1: uintr_notify_pgrp  │   nrecv[i]++         │
5. 等 nrecv[i] != 0                 └──────────────────────┘
6. i++
```

**测量窗口**：`t_handler - t0`，即从 `write()` 前一刻到 handler 第一条指令。

### 2.2 时间戳

- 使用 `rdtsc`（x86 Time Stamp Counter），不变频率（invariant TSC）
- 校准：启动时用 `usleep(20ms)` + `clock_gettime(CLOCK_MONOTONIC)` 算出 `cycles/ns`
- handler 里用 `rdtsc` 而非 `clock_gettime`：UINTR handler 编译时带 `-mgeneral-regs-only`，不能碰 SSE/FPU

### 2.3 轮次

- 默认 `ROUNDS = 10000`
- 每轮独立，互不影响
- 数据文件：`latency_mode<N>_YYYYMMDD_HHMMSS.csv`，每行一个延迟值（ns）

## 3. 运行步骤

### 3.1 前置条件

- 内核已打补丁[内核补丁](https://github.com/XIPCAcc/uintr-linux-kernel-official/tree/signal2uintr)

### 3.2 基本运行

```bash
# 测信号路径
sudo sysctl kernel.uintr_isig_mode=0
sudo ./tty_isig_latency

# 测用户态中断路径
sudo sysctl kernel.uintr_isig_mode=1
sudo ./tty_isig_latency
```

### 3.3 推荐配置（获得更干净的数据）

| 配置 | 作用 | 方法 |
|---|---|---|
| CPU 绑核 | 减少迁移、缓存抖动 | 程序自动：父进程 cpu0，接收方末核/isolated 核 |
| isolcpus | 接收方独占一个核 | grub `GRUB_CMDLINE_LINUX="isolcpus=15"` |
| SCHED_FIFO | 接收方不被 CFS 抢占 | 有 isolcpus 时自动启用 prio 50 |
| nice -20 | 无 isolcpus 时尽量不被抢占 | 程序自动设置 |

## 4. 数据产出与绘图

### 4.1 数据文件

每次运行生成 `latency_mode<N>_YYYYMMDD_HHMMSS.csv`：

```
latency_ns
7752
8431
5920
...
```

### 4.2 一键绘图

```bash
python3 plot_latency.py [目录]
```

自动扫描目录下所有 `latency_mode*.csv`，生成四张图：

| 图 | 文件名 | 用途 |
|---|---|---|
| CDF | `latency_cdf_<ts>.png` | 读百分位（p50/p90/p99） |
| 对数直方图 | `latency_hist_<ts>.png` | 看分布形状、双峰、重尾 |
| 小提琴图 | `latency_violin_<ts>.png` | 各次运行分布对比 |
| 散点抖动图 | `latency_strip_<ts>.png` | 原始样本真实分布 |

注意：运行脚本时可能需要 `env -u LD_LIBRARY_PATH python3 plot_latency.py`（避免环境变量里的 libstdc++ 与 PIL 冲突）。

## 5. 注意事项

### 5.1 SCHED_FIFO 会饿死同核 kworker

**现象**：`[FAIL] round N never armed` 或测试卡死。

**原因**：接收方用 `SCHED_FIFO` 占满一个 CPU 核，tty 的 unbound kworker 恰好被调度到同核时，CFS worker 永远抢不过 RT 接收方 → `__isig` 永远不执行 → 通知永远发不出去 → 死锁。

**处理**：
- 有 `isolcpus`：接收方绑 isolated 核 + SCHED_FIFO（isolated 核上 kworker 基本不去）
- 无 `isolcpus`：接收方用 `nice(-20)`，不用 SCHED_FIFO（程序自动判断）

### 5.2 两次采样需连续运行

系统负载随时间变化，间隔几小时的两次数据不可比。建议：

```bash
echo 1 | sudo tee /proc/sys/kernel/uintr_isig_mode && sudo ./tty_isig_latency
echo 0 | sudo tee /proc/sys/kernel/uintr_isig_mode && sudo ./tty_isig_latency
```

连续跑两次，保证负载相近。

### 5.3 p50 比 avg 更有意义

延迟分布是重尾的（workqueue 唤醒抖动），avg 会被 ms 级长尾拉飞。看分布用 **p50 / p90 / p99**，avg 仅作参考。

### 5.4 接收方不是 100% 纯用户态

即使绑核 + SCHED_FIFO，以下情况仍会让接收方进内核：

- 时钟中断（tick，每 1-4ms，`nohz_full` 可减少）
- IPI（核间中断，如 `flush_tlb`）
- 硬件中断（接收方 CPU 上的外设 IRQ）

这些是 UINTR 理论优势无法完全体现的原因。

### 5.5 延迟的组成

```
总延迟 = write() 开销
       + queue_work 入队
       + kworker 唤醒延迟        ← 抖动主要来源，几 µs ~ 几百 µs
       + flush_to_ldisc → __isig
       + 投递机制开销
         ├─ 信号: IPI + 进内核 + 构信号帧 + IRET (~1-3 µs)
         └─ UINTR: APIC 直接投递 (~几百 ns)
```

kworker 唤醒延迟占主导，所以 p90/p99 上两者差异不大；p50 上 UINTR 能体现优势。

### 5.6 偶发通知丢失

程序对单轮通知丢失有容忍（跟随 trigger 的 round 推进，不终止测试），最后打印 `N rounds skipped/lost`。如果丢失率高，检查：

- 是否 kworker 被饿死（见 5.1）
- UINTR 的 UPID ON 位竞争（极端情况）
