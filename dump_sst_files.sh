#!/bin/bash
DIR="/mnt/rocksdb_test"
SRC_DIR="/home/ceph/junhan/code/rocksdb/./sst_dump"
OUT_DIR="/mnt/sst_dumps/256g_small_load/sst_dump"
ROCK_DIR="/home/ceph/junhan/code/rocksdb/"
TOUT_DIR="/mnt/sst_dumps/256g_small_load/trim_sst_dump"
#TOUT_DIR="/home/ceph/Line_data/Project/Line/data/put_cnt_stat/hash25_zipfcomp_keyrange_num/rocksdb_4bg_250123/cz/rocks_v6_1_2/large/try2/level_kr_"${KRN}"_"${ZIPF}"/"${WHICH}"/cf_0/trim_sst_dump"
#TOUT_DIR="/home/ceph/Line_data/Project/Line/data/put_cnt_stat/zipfcomp_keyrange_num/small_kr_9999_big_16384/run/cf_0/trim_sst_dump"
sudo mkdir -p $OUT_DIR
sudo mkdir -p $TOUT_DIR
# Loop over each .sst file in the directory
for FILE in "$DIR"/*.sst; do
  # Generate the output filename by replacing .sst with _dump.txt
	filename="$(basename $FILE)"
  OUTPUT_FILE="${filename%.sst}_dump.txt"
  # Run the sst_dump command and save the output to the file
	sudo $SRC_DIR --file="$FILE" --command=scan --output_hex |sudo tee "${OUT_DIR}/${OUTPUT_FILE}" > /dev/null 2>&1
done
echo "All .sst files have been processed."
sudo python ${ROCK_DIR}trim.py $OUT_DIR $TOUT_DIR
sudo rm -r $OUT_DIR
