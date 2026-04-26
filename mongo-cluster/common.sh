#!/usr/bin/env bash
# Shared configuration and helpers for setup.sh and run.sh

SETUP_BASE="/var/mongo-test"
REPLICAS="r1 r2 r3"
TPCC_IMAGE="py-tpcc-runner"
MONGO_CLUSTER_SLICE="mongo-cluster-limit.slice"

MONGO_INITDB_ROOT_USERNAME=root
MONGO_INITDB_ROOT_PASSWORD=passwd
MONGO_INITDB_DATABASE=testdb

# Ensure the systemd slice exists (creates if missing, idempotent)
ensure_slice() {
	if ! systemctl is-active "$MONGO_CLUSTER_SLICE" &>/dev/null; then
		echo "=== Creating systemd slice ($MONGO_CLUSTER_SLICE)..."
		cat > /etc/systemd/system/"$MONGO_CLUSTER_SLICE" <<-EOF
		[Unit]
		Description=Shared resource slice for mongo cluster
		[Slice]
		EOF
		systemctl daemon-reload
		systemctl start "$MONGO_CLUSTER_SLICE"
	fi
}

# Ensure the docker network exists (idempotent)
ensure_network() {
	if ! docker network inspect mongo-cluster &>/dev/null; then
		echo "=== Creating docker network mongo-cluster..."
		docker network create mongo-cluster
	fi
}

# Start the mongo cluster, creating containers if they don't exist
ensure_cluster() {
	ensure_slice
	ensure_network

	local port=7000
	for replica in $REPLICAS; do
		port=$(( port + 1 ))
		if docker ps --format '{{.Names}}' | grep -q "^mongo-$replica$"; then
			continue
		fi
		if docker start "mongo-$replica" 2>/dev/null; then
			echo "=== Started existing container mongo-$replica."
			sleep 2
			continue
		fi
		echo "=== Creating container mongo-$replica (port $port)..."
		docker run -d --network mongo-cluster --name "mongo-$replica" \
			--cgroup-parent="$MONGO_CLUSTER_SLICE" \
			-e MONGO_INITDB_ROOT_USERNAME=$MONGO_INITDB_ROOT_USERNAME \
			-e MONGO_INITDB_ROOT_PASSWORD=$MONGO_INITDB_ROOT_PASSWORD \
			-e MONGO_INITDB_DATABASE=$MONGO_INITDB_DATABASE \
			-v "$SETUP_BASE/mongo-$replica:/data/db" \
			-p $port:27017 \
			mongo:6 -f /data/db/config.conf
	done
	echo "=== Mongo cluster is running."
}

wait_for_mongosh() {
	local container="$1"
	local db="${2:-test}"
	local max_attempts=30
	echo "=== Waiting for mongosh on $container ($db) to be ready..."
	for (( i=1; i<=max_attempts; i++ )); do
		if docker exec "$container" mongosh "$db" --quiet --eval 'db.runCommand({ping:1})' &>/dev/null; then
			echo "=== $container is ready (attempt $i)."
			return 0
		fi
		sleep 2
	done
	echo "ERROR: $container did not become ready after $((max_attempts * 2))s."
	return 1
}
