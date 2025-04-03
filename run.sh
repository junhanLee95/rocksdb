sudo ./db_bench -benchmarks=fillrandom -allow_column_family_split=true -db=/mnt/rocksdb_test  -key_size=10 -value_size=1000 -num=5000000 -threads=10 -max_background_jobs=4 --stats_interval_seconds=1 
