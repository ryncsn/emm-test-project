#!/usr/bin/env bash
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
source "$SCRIPT_DIR/common.sh"

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

# Bring up the cluster (creates slice, network, and containers)
ensure_cluster

# Build rs.initiate members array from $REPLICAS
# Give r1 the highest priority so it is always elected primary.
# The benchmark connects directly to r1 (port 7001) and needs it to be primary.
members=""
id=0
for replica in $REPLICAS; do
	[[ -n "$members" ]] && members+=","
	if [[ "$replica" == "r1" ]]; then
		members+="{_id:$id,host:\"mongo-$replica\",priority:10}"
	else
		members+="{_id:$id,host:\"mongo-$replica\",priority:1}"
	fi
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
cd "$SCRIPT_DIR"
git submodule update --init py-tpcc

# Build Python 2.7 docker image for py-tpcc
echo "=== Building Python 2.7 docker image ($TPCC_IMAGE) with pymongo..."
docker rm -f py-tpcc-build 2>/dev/null || :
docker run --name py-tpcc-build python:2.7 pip install pymongo
docker commit py-tpcc-build "$TPCC_IMAGE"
docker rm py-tpcc-build
echo "=== Docker image '$TPCC_IMAGE' built successfully."
