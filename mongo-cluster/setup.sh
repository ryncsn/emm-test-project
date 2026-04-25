#!/usr/bin/env bash

SETUP_BASE="/var/mongo-test"
REPLICAS="r1 r2 r3"
TPCC_IMAGE="py-tpcc-runner"
TEMPLATE='
{
    "net": {
        "bindIpAll": true,
        "ipv6": false,
        "maxIncomingConnections": 10000,
    },
    "setParameter": {
        "disabledSecureAllocatorDomains": "*"
    },
    "replication": {
        "oplogSizeMB": 10480,
        "replSetName": "issa-tpcc_0"
    },
    "security": {
        "keyFile": "/data/db/keyfile"
    },
    "storage": {
        "dbPath": "/data/db/",
        "syncPeriodSecs": 60,
        "directoryPerDB": true,
        "wiredTiger": {
            "engineConfig": {
                "cacheSizeGB": 5
            }
        }
    },
    "systemLog": {
        "destination": "file",
        "logAppend": true,
        "logRotate": "rename",
        "path": "/data/db/mongod.log",
        "verbosity": 0
    }
}
'

MONGO_INITDB_ROOT_USERNAME=root
MONGO_INITDB_ROOT_PASSWORD=passwd
MONGO_INITDB_DATABASE=testdb

set -e

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

echo "Install packages? (y/n)"
read -r RES
if [[ "$RES" == y ]]; then
	# XXX: Or maybe you just want "dnf install docker" here.
	dnf install docker
fi

echo "Running this script will destroy all previous data under $SETUP_BASE, and may destroy all containers named mongo-*, this is only for standalone test environment, continue? (y/n)"
read -r RES
[[ "$RES" == y ]] || exit 0

echo "Disabling SELinux... If this failed you may try do it manually."
setenforce 0 || :

KEY=$(openssl rand -base64 756)
for replica in $REPLICAS; do
	rm -rf "$SETUP_BASE/mongo-$replica"
	mkdir -p "$SETUP_BASE/mongo-$replica"
	echo "$KEY" > "$SETUP_BASE/mongo-$replica/keyfile"
	# Less security, only for test
	chmod 0600 "$SETUP_BASE/mongo-$replica/keyfile"
	echo "$TEMPLATE" > "$SETUP_BASE/mongo-$replica/config.conf"
done

echo "=== Cleaning up old containers and network..."
for replica in $REPLICAS; do
	docker stop "mongo-$replica" 2>/dev/null || true
	docker rm "mongo-$replica" 2>/dev/null || true
done
docker network remove mongo-cluster 2>/dev/null || true

# XXX: If MongoDB Failed to setup, you may remove the --rm below to retrive failure log
port=7000
docker network create mongo-cluster
for replica in $REPLICAS; do
	port=$(( port + 1 ))
	docker run -d --network mongo-cluster --name "mongo-$replica" \
		-e MONGO_INITDB_ROOT_USERNAME=$MONGO_INITDB_ROOT_USERNAME \
		-e MONGO_INITDB_ROOT_PASSWORD=$MONGO_INITDB_ROOT_PASSWORD \
		-e MONGO_INITDB_DATABASE=$MONGO_INITDB_DATABASE \
		-v "$SETUP_BASE/mongo-$replica:/data/db" \
		-p $port:27017 \
		mongo:6 -f /data/db/config.conf
done

# Build rs.initiate members array from $REPLICAS
members=""
id=0
for replica in $REPLICAS; do
	[[ -n "$members" ]] && members+=","
	members+="{_id:$id,host:\"mongo-$replica\"}"
	id=$(( id + 1 ))
done

echo "=== Initiating replica set..."
wait_for_mongosh mongo-r1
docker exec mongo-r1 mongosh --quiet --eval \
	"rs.initiate({_id:\"issa-tpcc_0\",members:[$members]})" \
	|| echo "Cluster setup failed, check the error log."

echo "=== Setting up user/password..."
wait_for_mongosh mongo-r1 admin
docker exec mongo-r1 mongosh admin --quiet --eval \
	'db.createUser({user:"root",pwd:"passwd",roles:["root"]})'

# Initialize py-tpcc submodule
echo "=== Initializing py-tpcc submodule..."
cd "$(dirname "$0")"
git submodule update --init py-tpcc

# Build Python 2.7 docker image for py-tpcc
echo "=== Building Python 2.7 docker image ($TPCC_IMAGE) with pymongo..."
docker rm -f py-tpcc-build 2>/dev/null || :
docker run --name py-tpcc-build python:2.7 pip install pymongo
docker commit py-tpcc-build "$TPCC_IMAGE"
docker rm py-tpcc-build
echo "=== Docker image '$TPCC_IMAGE' built successfully."
