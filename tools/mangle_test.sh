#!/usr/bin/env bash
# The RocksDB mangle test script.
# REQUIREMENT: must be able to run make {db_bench, ldb, sst_dump} in the current directory
#
# This script will do the following things in order:
#
# 1. build db_bench using the specified commit
# 2. setup result directory $RESULT_DB_PATH.  If not specified, then the test directory
#    will be "/home/junhan/samba_server/data/mangle_test"
# 3. run set of benchmarks on the specified host locally
# 4. generate report in the $RESULT_PATH.  If RESULT_PATH is not specified,
#    RESULT_PATH will be set to $RESULT_DB_PATH/SUMMARY.csv
#
# = Examples =
# * Run the mangle test.
#
#   TEST_MODE=1 \
#   DB_PATH=/db/path TRACE_PATH=/trace/path \
#   RESULT_DB_PATH=/result/db/path RESULT_TRACE_PATH=/result/trace_path \
#   ./tools/mangle_test.sh
#
# * Create mangling data without test
#
#   TEST_MODE=0 \
#   DB_PATH=/db/path TRACE_PATH=/trace/path \
#   RESULT_DB_PATH=/result/db/path RESULT_TRACE_PATH=/result/trace_path \
#   ./tools/mangle_test.sh
#
# = Mangle test environmental parameters =
#   TEST_MODE: If 1, run mangling benchmark and run test code
#       if 0, run mangling benchmark to create mangling data only
#       Default: 0
#   DB_PATH: the path where the rocksdb database will be read
#       for mangle test.  Default: ""
#   TRACE_PATH: the path where the trace file will be read
#       for mangle test.  Default: ""
#   RESULT_DB_PATH: the directory where the mangle db results will be generated.
#       Default: ""
#   RESULT_TRACE_PATH: the path where the mangle trace file results will be generated.
#       Default: $RESULT_DB_PATH/db1
#   RESULT_PATH: the directory where the mangle test summary is written.
#       Default: $RESULT_DB_PATH
#

function main {
  TEST_ROOT_DIR=${RESULT_DB_PATH:-"/home/junhan/samba_server/data/mangle_test"}
  init_arguments $TEST_ROOT_DIR

  setup_test_directory

  build_db_bench_and_ldb_and_sst_dump

  echo "Mangling DB..." &>> $SUMMARY_FILE
  run_db_bench "mangling"
  echo "Mangling DB Done..." &>> $SUMMARY_FILE

  if [ $TEST_MODE -eq 1 ]; then
    echo "[TEST 1] Compare file sizes. We skip to check WAL files since they are recycled" &>> $SUMMARY_FILE
    run_file_size_test

    echo "[TEST 2] Compare sst files." &>> $SUMMARY_FILE
    run_sst_dump_test

    echo "[TEST 3] Compare Scanned keys and values" &>> $SUMMARY_FILE
    run_ldb_scan_test
  fi

  echo "Benchmark completed!  Results are available in $RESULT_PATH">> $SUMMARY_FILE

}

############################################################################
function init_arguments {

  TEST_MODE=${TEST_MODE:-0}
  DB_PATH=${DB_PATH:-"/home/junhan/samba_server/data/osd0_pg_removal2"}
  TRACE_PATH=${TRACE_PATH:-"/home/junhan/samba_server/data/osd0_pg_removal2/db1"}
  RESULT_DB_PATH=${RESULT_DB_PATH:-"/home/junhan/samba_server/data/mangle_test"}
  RESULT_TRACE_PATH=${RESULT_TRACE_PATH:-"$RESULT_DB_PATH/$(basename $TRACE_PATH)"}
  DB_BENCH_DIR=${DB_BENCH_DIR:-"/home/junhan/ceph_rocksdb/rocksdb"}
  current_time=$(date +"%F-%H:%M:%S")
  RESULT_PATH=${RESULT_PATH:-"$1/results/$current_time"}
  SUMMARY_FILE="$RESULT_PATH/SUMMARY.txt"

  SCP=${SCP:-"scp"}
  SSH=${SSH:-"ssh"}
}

# $1 --- benchmark name
function run_db_bench {
  # this will terminate all currently-running db_bench
  find_db_bench_cmd="ps aux | grep db_bench | grep -v grep | grep -v aux | awk '{print \$2}'"

  echo ""
  echo "======================================================================="
  echo "Benchmark $1"
  echo "======================================================================="
  echo ""
  db_bench_error=0
  db_bench_cmd="( $DB_BENCH_DIR/db_bench \
      --benchmarks=$1 --trace_file=$TRACE_PATH --mangling_in_dir=$DB_PATH \
      --mangling_out_dir=$RESULT_DB_PATH \
      --trace_file_result=$RESULT_TRACE_PATH \
      ) 2>&1"
  ps_cmd="ps aux"

  ## make sure no db_bench is running
  # The following statement is necessary make sure "eval $ps_cmd" will success.
  # Otherwise, if we simply check whether "$(eval $ps_cmd | grep db_bench)" is
  # successful or not, then it will always be false since grep will return
  # non-zero status when there's no matching output.
  ps_output="$(eval $ps_cmd)"
  exit_on_error $? "$ps_cmd"

  # perform the actual command to check whether db_bench is running
  grep_output="$(eval $ps_cmd | grep db_bench | grep -v grep)"
  if [ "$grep_output" != "" ]; then
    echo "Stopped mangle_test.sh as there're still db_bench processes running:"
    echo $grep_output
    exit 2
  fi

  ## run the db_bench
  cmd="($db_bench_cmd || db_bench_error=1) | tee -a $RESULT_PATH/$1"
  exit_on_error $?
  echo $cmd
  eval $cmd
  exit_on_error $db_bench_error
}

function exit_on_error {
  if [ $1 -ne 0 ]; then
    echo ""
    echo "ERROR: Benchmark did not complete successfully."
    if ! [ -z "$2" ]; then
      echo "Failure command: $2"
    fi
    echo "Partial results are output to $RESULT_PATH"
    echo "ERROR" >> $SUMMARY_FILE
    exit $1
  fi
}

function build_db_bench_and_ldb_and_sst_dump {
  echo "Building db_bench & ldb & sst_dump ..." &>> $SUMMARY_FILE

#  make clean
  exit_on_error $?

  DEBUG_LEVEL=0 make db_bench ldb sst_dump -j32 &>> $SUMMARY_FILE

  exit_on_error $?
}

function run_remote {
  test_remote "$1"
  exit_on_error $? "$1"
}

function test_remote {
  eval $1
}

function run_local {
  eval "$1"
  exit_on_error $?
}

function setup_test_directory {
  echo "Creating new test directories"
  run_local "mkdir -p $RESULT_DB_PATH"
  run_local "mkdir -p $RESULT_PATH"

  exit_on_error $?
}

# check file size of each db
function  run_file_size_test {
  sstfiles="$RESULT_DB_PATH/*.sst"
  pass=1
  for f in $sstfiles
  do
    filename=$(basename $f)
    of="$DB_PATH/db/$filename"

    f_size=$(wc -c $f | awk '{print $1}')
    of_size=$(wc -c $of | awk '{print $1}')
    if [ $f_size != $of_size ]; then
      echo "$f and $of has different size" &>> $SUMMARY_FILE
      pass=0
    fi
  done

  if [ $pass -eq 1 ]; then
    echo "[TEST 1] (PASS) sst file size" &>> $SUMMARY_FILE
  else
    echo "[TEST 1] (FAIL) sst file size" &>> $SUMMARY_FILE
  fi

  manifestfiles="$RESULT_DB_PATH/MANIFEST*"
  pass=1
  for f in $manifestfiles
  do
    filename=$(basename $f)
    of="$DB_PATH/db/$filename"

    f_size=$(wc -c $f | awk '{print $1}')
    of_size=$(wc -c $of | awk '{print $1}')
    if [ $f_size != $of_size ]; then
      echo "$f and $of has different size" &>> $SUMMARY_FILE
      pass=0
    fi
  done

  if [ $pass -eq 1 ]; then
    echo "[TEST 1] (PASS) manifest file size" &>> $SUMMARY_FILE
  else
    echo "[TEST 1] (FAIL) manifest file size" &>> $SUMMARY_FILE
  fi
}

# check sstable properties of each db
function  run_sst_dump_test {
  sstfiles="$RESULT_DB_PATH/*.sst"
  pass=1
  for f in $sstfiles
  do
    filename=$(basename $f)
    of="$DB_PATH/db/$filename"

    sst_dump_o_cmd="( $DB_BENCH_DIR/sst_dump \
      --file=$of \
      --command=check \
      --show_properties
    )"

    sst_dump_m_cmd="( $DB_BENCH_DIR/sst_dump \
      --file=$f \
      --command=check \
      --show_properties
    )"

    o_output=$(eval $sst_dump_o_cmd)
    m_output=$(eval $sst_dump_m_cmd)
    if [ "$m_output" != "$o_output" ]; then
      echo "[TEST 2] $f and $of has different sstable properties" &>> $SUMMARY_FILE
      pass=0
    fi
  done

  if [ $pass -eq 1 ]; then
    echo "[TEST 2] (PASS) sst file dump" &>> $SUMMARY_FILE
  else
    echo "[TEST 2] (FAIL) sst file dump" &>> $SUMMARY_FILE
  fi
}

function  run_ldb_scan_test {
  ldb_o_cmd="( $DB_BENCH_DIR/ldb \
      --hex scan \
      --db=$DB_PATH/db \
      --try_load_options > $RESULT_PATH/original_scan
  )"

  ldb_m_cmd="( $DB_BENCH_DIR/ldb \
      --hex scan \
      --db=$RESULT_DB_PATH \
      --try_load_options > $RESULT_PATH/mangle_scan
    )"

  eval $ldb_o_cmd
  eval $ldb_m_cmd
  python3 $DB_BENCH_DIR/tools/ldb_scan_test.py $RESULT_PATH/original_scan $RESULT_PATH/mangle_scan $SUMMARY_FILE

}

############################################################################

# shellcheck disable=SC2068
main $@
