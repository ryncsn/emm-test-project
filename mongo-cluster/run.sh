#!/usr/bin/env bash

port=7000
for r in r1 r2 r3; do
	sudo docker run -d --replace --network mongo-cluster --name "mongo-$r" -e MONGO_INITDB_ROOT_USERNAME=root -e MONGO_INITDB_ROOT_PASSWORD=passwd -e MONGO_INITDB_DATABASE=testdb -v /var/data/mongo-$r:/data/db -p $port:27017 mongo -f /data/db/config.conf
	sleep 2
done
