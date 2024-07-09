#!/bin/bash
arr_lvl=("/mnt/rocksdb_test/"*.sst)
for input_file in "${arr_lvl[@]}"; do
  	output_file="scan_$(basename "$input_file")"
    ./sst_dump --file="$input_file" --command=scan >> "$output_file"
done
