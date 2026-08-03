#!/bin/bash

LIMIT=300M
CG_NAME=lru-hot-cold-bench

[ -z "$NAME" ] && NAME=demo

if [ "$#" -gt 0 ]; then
	TESTS=("$@")
else
	TESTS=(test_hot_scan_cold_grep.py)
fi

if [ -d /sys/fs/cgroup/memory ]; then
	CG_PATH=/sys/fs/cgroup/memory/spawn-$CG_NAME
else
	CG_PATH=/sys/fs/cgroup/spawn-$CG_NAME
fi

sudo rmdir "$CG_PATH" > /dev/null
sudo sh -c "echo 3 > /proc/sys/vm/drop_caches"
sudo mkdir -p "$CG_PATH"
sudo su -c "echo $$ > $CG_PATH/cgroup.procs"
sudo su -c "echo $PPID > $CG_PATH/cgroup.procs"
sudo su -c "echo $BASHPID > $CG_PATH/cgroup.procs"

if [ -d /sys/fs/cgroup/memory ]; then
	sudo su -c "echo $LIMIT > $CG_PATH/memory.limit_in_bytes"
else
	sudo su -c "echo $LIMIT > $CG_PATH/memory.max"
fi

run_tests() {
	for t in "${TESTS[@]}"; do
		python3 "$t"
	done
}

echo "MGLRU:"
sudo sh -c "echo y > /sys/kernel/mm/lru_gen/enabled"
sudo sh -c "cat /sys/kernel/mm/lru_gen/enabled"
sudo sh -c "echo 3 > /proc/sys/vm/drop_caches"
for i in 1 2 3 4 5; do run_tests; done

echo "CLRU:"
sudo sh -c "echo n > /sys/kernel/mm/lru_gen/enabled"
sudo sh -c "cat /sys/kernel/mm/lru_gen/enabled"
sudo sh -c "echo 3 > /proc/sys/vm/drop_caches"
for i in 1 2 3 4 5; do run_tests; done
