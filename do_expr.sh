#!/bin/bash

#BLUEFS_BENCH="/home/junhan/ceph_rocksdb/rocksdb/db_bench"
#BASE_DIR="/home/junhan/ceph_rocksdb/rocksdb_result/"
BLUEFS_BENCH="./db_bench"
BASE_DIR="/home/junhan/ceph_rocksdb/rocksdb_result/"
TEST_DIR="$BASE_DIR/$(date "+%Y%m%d%H%M")"

fixed_option="--db=db --key_size=128 --value_size=1024 --statistics"

fixed_prebmrk="fillseq,stats,levelstats"
fixed_postbmrk="stats,levelstats,sstables"

benchmarks="readrandom,stats,levelstats,readrandommergerandom"
options="--write_buffer_size=268435456 --num=10000000 --threads 1 --reads=10000000 --stats_interval_seconds=2 --batch_size=10 --merge_operator=stringappend --merge_operator=put --merge_operator=uint64add --compaction_queue_stat=true --latency_stat=true"

mkdir -p ${TEST_DIR}

$BLUEFS_BENCH ${fixed_option} --benchmarks=${fixed_prebmrk},${benchmarks},${fixed_postbmrk} ${options} 2>&1 | tee ${TEST_DIR}/data.dat

#kill ${BLK_PID}

#blkparse -i ${BLK_DIR}/result -o ${TEST_DIR}/blk.dat &> /dev/null

chown -R junhan:junhan ${BASE_DIR}
