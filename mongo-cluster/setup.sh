#!/usr/bin/env bash

SETUP_BASE="/var/mongo-test"
REPLICAS="r1 r2 r3"
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

echo "Install packages? (y/n)"
read -r RES
if [[ "$RES" == y ]]; then
	# XXX: Or maybe you just want "dnf install docker" here.
	dnf install podman-docker
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

for replica in $REPLICAS; do
	docker stop "mongo-$replica" || :
	docker rm "mongo-$replica" || :
done
docker network remove mongo-cluster || :

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

echo "=== Sleep 10s and executing cluster setup... if this failed, you can try manually later."
set -x
sleep 10
# XXX: Need to change the setup command if REPLICAS changes.
docker exec -it mongo-r1 mongosh --eval \
'"rs.initiate({
    _id: "issa-tpcc_0",
    members: [
      {_id: 0, host: "mongo-r1"},
      {_id: 1, host: "mongo-r2"},
      {_id: 2, host: "mongo-r3"}
    ]
})"' || echo "Cluster setup failed, check the error log."
set +x

echo "=== Setting up user/password... if this failed, you can try manually later."
set -x
sleep 10
docker exec -it mongo-r1 mongosh admin --eval 'db.createUser({user:"root",pwd:"passwd",roles:["root"]})'
set +x
