#!/usr/bin/env bash
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
TPCC_IMAGE="py-tpcc-runner"
TPCC_DIR="$SCRIPT_DIR/py-tpcc/pytpcc"

# Default parameters, override via environment variables
WAREHOUSES="${WAREHOUSES:-500}"
CLIENTS="${CLIENTS:-30}"
DURATION="${DURATION:-900}"
CONFIG="${CONFIG:-mongodb.config}"

usage() {
	echo "Usage: $0 {load|run|both}"
	echo ""
	echo "  load  - Load data only (--no-execute), ~200G of space, only need to run once"
	echo "  run   - Run benchmark only (--no-load)"
	echo "  both  - Load data then run benchmark"
	echo ""
	echo "Environment variables:"
	echo "  WAREHOUSES  - Number of warehouses (default: 500)"
	echo "  CLIENTS     - Number of clients (default: 30)"
	echo "  DURATION    - Duration in seconds (default: 900)"
	echo "  CONFIG      - Config file name (default: mongodb.config)"
	echo "               Use mongodb.config.* depending on which replica is master"
	exit 1
}

[[ $# -ge 1 ]] || usage
MODE="$1"

# Ensure mongo cluster is running, start if not
echo "=== Checking mongo cluster..."
for r in r1 r2 r3; do
	if ! docker ps --format '{{.Names}}' | grep -q "^mongo-$r$"; then
		echo "mongo-$r is not running, starting..."
		# Try to start existing stopped container first, fall back to error
		if ! docker start "mongo-$r" 2>/dev/null; then
			echo "ERROR: mongo-$r container does not exist. Run setup.sh first."
			exit 1
		fi
		sleep 2
	fi
done
echo "=== Mongo cluster is running."

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
		run_tpcc --no-execute
		;;
	run)
		echo "=== Running benchmark (--no-load)..."
		run_tpcc --no-load
		;;
	both)
		echo "=== Loading data..."
		run_tpcc --no-execute
		echo "=== Running benchmark..."
		run_tpcc --no-load
		;;
	*)
		usage
		;;
esac
