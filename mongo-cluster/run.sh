#!/usr/bin/env bash
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
source "$SCRIPT_DIR/common.sh"

TPCC_DIR="$SCRIPT_DIR/py-tpcc/pytpcc"

# Defaults
WAREHOUSES=500
CLIENTS=30
DURATION=900
CONFIG="mongodb.config"
MEMLIMIT="0"
IOPS="0"

usage() {
	echo "Usage: $0 [options] {load|run|both}"
	echo ""
	echo "Modes:"
	echo "  load  - Load data only (--no-execute), ~200G of space, only need to run once"
	echo "  run   - Run benchmark only (--no-load)"
	echo "  both  - Load data then run benchmark"
	echo ""
	echo "Options:"
	echo "  -w, --warehouses N    Number of warehouses (default: $WAREHOUSES)"
	echo "  -c, --clients N       Number of clients (default: $CLIENTS)"
	echo "  -d, --duration N      Duration in seconds (default: $DURATION)"
	echo "  --config FILE         Config file name (default: $CONFIG)"
	echo "  --memlimit LIMIT      Memory limit for cluster (default: $MEMLIMIT, 0=unlimited)"
	echo "  --iops N              IOPS limit for cluster R+W (default: $IOPS, 0=unlimited)"
	echo "  -h, --help            Show this help"
	exit 1
}

MODE=""
while [[ $# -gt 0 ]]; do
	case "$1" in
		-w|--warehouses) WAREHOUSES="$2"; shift 2 ;;
		-c|--clients)    CLIENTS="$2"; shift 2 ;;
		-d|--duration)   DURATION="$2"; shift 2 ;;
		--config)        CONFIG="$2"; shift 2 ;;
		--memlimit)      MEMLIMIT="$2"; shift 2 ;;
		--iops)          IOPS="$2"; shift 2 ;;
		-h|--help)       usage ;;
		load|run|both)
			[[ -z "$MODE" ]] || { echo "ERROR: multiple modes specified"; usage; }
			MODE="$1"; shift
			;;
		*) echo "ERROR: unknown option: $1"; usage ;;
	esac
done
[[ -n "$MODE" ]] || usage

# Ensure mongo cluster is running (creates slice, network, containers as needed)
echo "=== Checking mongo cluster..."
ensure_cluster

# Apply resource limits to the shared systemd slice
echo "=== Applying resource limits to $MONGO_CLUSTER_SLICE..."
echo "    MemoryMax=${MEMLIMIT} (0=unlimited), IOPS=${IOPS} (0=unlimited)"

if [[ "$MEMLIMIT" != "0" ]]; then
	systemctl set-property "$MONGO_CLUSTER_SLICE" MemoryMax="$MEMLIMIT"
else
	systemctl set-property "$MONGO_CLUSTER_SLICE" MemoryMax=infinity
fi

if [[ "$IOPS" != "0" ]]; then
	SETUP_DEV=$(findmnt -n -o SOURCE --target "$SETUP_BASE")
	SETUP_PARENT="/dev/$(lsblk -no PKNAME "$SETUP_DEV" | head -1)"
	[[ -b "$SETUP_PARENT" ]] || SETUP_PARENT="$SETUP_DEV"
	SETUP_MAJ_MIN=$(lsblk -rno MAJ:MIN "$SETUP_PARENT" | head -1)
	echo "    Block device: $SETUP_PARENT ($SETUP_MAJ_MIN)"
	systemctl set-property "$MONGO_CLUSTER_SLICE" IOReadIOPSMax="$SETUP_MAJ_MIN $IOPS"
	systemctl set-property "$MONGO_CLUSTER_SLICE" IOWriteIOPSMax="$SETUP_MAJ_MIN $IOPS"
else
	SLICE_CGROUP=$(systemctl show -p ControlGroup "$MONGO_CLUSTER_SLICE" --value)
	echo "" | tee /sys/fs/cgroup"$SLICE_CGROUP"/io.max >/dev/null 2>&1 || true
fi

echo "=== Limits applied."

run_tpcc() {
	local extra_args="$*"
	docker run --rm --network host \
		-v "$TPCC_DIR:/pytpcc" \
		-v "$SCRIPT_DIR/$CONFIG:/pytpcc/$CONFIG:ro" \
		-w /pytpcc \
		"$TPCC_IMAGE" \
		python2 tpcc.py --config="$CONFIG" mongodb \
		--duration="$DURATION" --warehouses="$WAREHOUSES" --clients="$CLIENTS" \
		$extra_args
}

case "$MODE" in
	load)
		echo "=== Loading data (--no-execute)..."
		run_tpcc --debug --no-execute
		;;
	run)
		echo "=== Running benchmark (--no-load)..."
		run_tpcc --no-load
		;;
	both)
		echo "=== Loading data..."
		run_tpcc --debug --no-execute
		echo "=== Running benchmark..."
		run_tpcc --no-load
		;;
esac
