#!/bin/bash

#BLUEFS_BENCH="/home/junhan/ceph_rocksdb/rocksdb/db_bench"
#BASE_DIR="/home/junhan/ceph_rocksdb/rocksdb_result/"
BLUEFS_BENCH="./db_bench"
BASE_DIR="/home/junhan/ceph_rocksdb/rocksdb_result/"
TEST_DIR="$BASE_DIR/$(date "+%Y%m%d%H%M")"

fixed_option="--db=db --key_size=128 --value_size=1024 --statistics"

#fixed_prebmrk="fillseq,stats,levelstats"

#benchmarks="readrandom,stats,levelstats,readrandommergerandom"
benchmarks="ycsbwklda,stats,levelstats"
options="--write_buffer_size=134217728 --max_background_jobs=10 --num=100000000 --threads 32 --reads=100000000 --duration=60 --stats_interval_seconds=1 --batch_size=1 --merge_operator=stringappend --merge_operator=put --merge_operator=uint64add --compaction_queue_stat=true --latency_stat=true"

mkdir -p ${TEST_DIR}

$BLUEFS_BENCH ${fixed_option} --benchmarks=${fixed_prebmrk},${benchmarks},${fixed_postbmrk} ${options} 2>&1 | tee ${TEST_DIR}/data.dat

#kill ${BLK_PID}

#blkparse -i ${BLK_DIR}/result -o ${TEST_DIR}/blk.dat &> /dev/null

chown -R junhan:junhan ${BASE_DIR}
