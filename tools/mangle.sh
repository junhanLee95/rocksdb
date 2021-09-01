#!/bin/bash
#./db_bench --benchmarks=mangling --trace_file=/home/junhan/samba_server/data/osd0_pg_removal2/db1 --mangling_out_dir=/home/junhan/samba_server/data/mangle_osd0_pg_removal2 --mangling_in_dir=/home/junhan/samba_server/data/osd0_pg_removal2 --trace_file_result=/home/junhan/samba_server/data/mangle_osd0_pg_removal2/db1 > dump.txt
./ldb --hex scan --no_value --db=/home/junhan/samba_server/data/mangle_osd0_pg_removal2 --try_load_options > mangle_scan
#./ldb --hex scan --no_value --db=/home/junhan/samba_server/data/osd0_pg_removal2/db --try_load_options > on_scan
