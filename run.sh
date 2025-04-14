#!/bin/bash
sudo umount /mnt/rocksdb_test
sudo mkfs.ext4 -E lazy_itable_init=0,lazy_journal_init=0 /dev/nvme0n1
sudo mount -o barrier=0 /dev/nvme0n1 /mnt/rocksdb_test
sudo rsync -a /mnt/load_database/fillrandom_0414 /mnt/rocksdb_test
sudo chown -Rv ceph:ceph /mnt/rocksdb_test
sudo sh -c "sync"
sudo sh -c "/usr/bin/echo 3 > /proc/sys/vm/drop_caches"
sleep 10
sudo bash -c './db_bench -benchmarks=mixgraph -max_background_jobs=4 -db=/mnt/rocksdb_test/fillrandom_0414 -perf_level=2 -keyrange_dist_a=14.18 -keyrange_dist_b=-2.917 -keyrange_dist_c=0.0164 -keyrange_dist_d=-0.08082 -keyrange_num=30 -value_k=0.2615 -value_sigma=25.45 -iter_k=2.517 -iter_sigma=14.236 -mix_get_ratio=0 -mix_put_ratio=1 -mix_seek_ratio=0 -reads=420000000 -key_size=48 -num=50000000 -cache_size=268435456 -use_direct_io_for_flush_and_compaction=true -use_direct_reads=true -use_existing_db=true -statistics=true -ksl=true &> mixgraph_0414_ksl.txt'
#sudo bash -c './db_bench -benchmarks=fillrandom -max_background_jobs=4 -db=/mnt/rocksdb_test -perf_level=3  -key_size=48 -value_size=43 -num=50000000 -cache_size=268435456 -use_direct_io_for_flush_and_compaction=true -use_direct_reads=true &> fillrandom_0414.txt'
