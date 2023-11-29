#!/bin/bash

set -eux

killall dqlite-demo || echo OK
rm -rf dumps_*

export LIBDQLITE_TRACE=1
dqlite-demo --api 127.0.0.1:8001 --db 127.0.0.1:9001 2>dumps_1.txt &
dqlite-demo --api 127.0.0.1:8002 --db 127.0.0.1:9002 --join 127.0.0.1:9001 2>dumps_2.txt &
dqlite-demo --api 127.0.0.1:8003 --db 127.0.0.1:9003 --join 127.0.0.1:9001 2>dumps_3.txt &

sleep 5

curl -X PUT -d my-value http://127.0.0.1:8001/my-key0
curl http://127.0.0.1:8001/my-key0

sleep 1

curl -X PUT -d my-value http://127.0.0.1:8001/my-key1
curl http://127.0.0.1:8001/my-key1

sleep 10

killall dqlite-demo

cat dumps_1.txt  | grep '@@@' | awk '{ print "*", $2, $8, $9, $10, $11, $12}' > dumps_11.txt
