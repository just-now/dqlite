dqlite-demo --api 127.0.0.1:8001 --db 127.0.0.1:9001 2>dumps_1.txt &
dqlite-demo --api 127.0.0.1:8002 --db 127.0.0.1:9002 --join 127.0.0.1:9001 2>dumps_2.txt &
dqlite-demo --api 127.0.0.1:8003 --db 127.0.0.1:9003 --join 127.0.0.1:9001 2>dumps_3.txt &

