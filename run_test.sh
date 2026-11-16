#!/bin/bash
# Run the frequency sweep in both modes, then regenerate all plots.
#
# Usage:
#   ./run_test.sh            # assume the environment is already tuned
#   ./run_test.sh --lock     # also set cpu0/cpu15 to the performance governor
#
# Expects: sudo for sysctl / SCHED_FIFO; python3 with numpy/matplotlib.
set -e
cd "$(dirname "$0")"

if [ "$1" = "--lock" ]; then
	echo performance | sudo tee /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor \
		/sys/devices/system/cpu/cpu15/cpufreq/scaling_governor >/dev/null
fi

gov0=$(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor)
gov15=$(cat /sys/devices/system/cpu/cpu15/cpufreq/scaling_governor)
if [ "$gov0" != performance ] || [ "$gov15" != performance ]; then
	echo "WARNING: governor cpu0=$gov0 cpu15=$gov15 (expected performance)"
	echo "         cold-workqueue backlog may dominate the results"
fi

make -s tty_isig_freq

echo "=== mode 1 (UINTR) ==="
sudo sysctl -w kernel.uintr_isig_mode=1
sudo ./tty_isig_freq

echo "=== mode 0 (SIGINT) ==="
sudo sysctl -w kernel.uintr_isig_mode=0
sudo ./tty_isig_freq

echo "=== plots ==="
env -u LD_LIBRARY_PATH python3 plot_freq_verdict.py
env -u LD_LIBRARY_PATH python3 plot_hist_pair.py
