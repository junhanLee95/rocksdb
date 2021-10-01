//  Copyright (c) 2011-present, Facebook, Inc.  All rights reserved.
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).

#pragma once

#include <memory>
#include <unordered_map>
#include <utility>

#include "db/log_writer.h"
#include "db/log_reader.h"
#include "rocksdb/env.h"
#include "rocksdb/options.h"
#include "rocksdb/trace_reader_writer.h"
#include "rocksdb/sst_file_writer.h"
#include "rocksdb/sst_file_reader.h"
#include "rocksdb/utilities/options_util.h"
#include "tools/sst_dump_tool_imp.h"
#include <map>
#include <iostream>
#include <string.h>
using namespace std;

namespace rocksdb {

class ColumnFamilyHandle;
class ColumnFamilyData;
class DB;
class DBImpl;
class Slice;
class WriteBatch;

class Mangler {
 public:
  Mangler(Env*, DB*, const std::vector<ColumnFamilyHandle*>&, std::string, std::string, std::string, std::string, bool);
  ~Mangler();
  Status mangle();
  Status mangle_write();

  bool IsTraceFileOverMax();

 private:
  // decide whether we apply mangling algorithm
  bool apply_;
  // file paths
  std::string trace_file_path_;
  std::string trace_file_result_;
  std::string mangling_out_dir_;
  std::string db_path_;
  std::vector<std::string> sst_file_paths_;
  std::vector<std::string> wal_file_paths_;
  std::vector<std::string> manifest_file_paths_;

  // Members for DB
  DBImpl* db_;
  ColumnFamilyHandle* cfh_default_;
  Env* env_;
  Options loaded_db_opt_;
  std::vector<ColumnFamilyDescriptor> loaded_cf_descs_;
  std::unordered_map<uint32_t, ColumnFamilyHandle*> cf_map_;
  void GetDBFilePaths(void);
  void GetDBOptions(void);

  // Members for modifying trace file
  std::unique_ptr<TraceReader> trace_reader_;
  std::unique_ptr<TraceWriter> trace_writer_;

  // Members for modifying sstable files
  std::vector<std::unique_ptr<SstFileDumper>> sst_file_dumpers_;
  std::vector<std::unique_ptr<SstFileWriter>> sst_file_writers_;
  std::vector<uint64_t> sst_file_creation_time;
  std::vector<uint64_t> sst_file_oldest_key_time;

  // Members for modifying wal files
  std::vector<std::unique_ptr<log::Reader>> wal_readers_;
  std::vector<std::unique_ptr<log::Writer>> wal_writers_;

  // Members for modifying manifest files
  std::vector<std::unique_ptr<log::Reader>> manifest_readers_;
  std::vector<std::unique_ptr<log::Writer>> manifest_writers_;

  // Members for manging mangled key
  map<std::string, std::string> mangling_map;

  // Methods for trace files
  Status MangleTraceFile(void);
  Status WriteMangledTraceFile(void);
  // Read Trace 
  Status ReadHeader(Trace* header);
  Status ReadFooter(Trace* footer);
  Status ReadTrace(Trace* trace);
  int compare(std::string* a, std::string* b, size_t min_len);
  // Write Trace
  Status Write(WriteBatch* write_batch, uint64_t ts);
  Status Get(uint32_t cf_id, const Slice& key, uint64_t ts);
  Status IteratorSeek(const uint32_t& cf_id, const Slice& key, uint64_t ts);
  Status IteratorSeekForPrev(const uint32_t& cf_id, const Slice& key, uint64_t ts);

  Status WriteHeader(uint64_t ts);
  Status WriteFooter(uint64_t ts);
  Status WriteTrace(const Trace& trace);
  bool ShouldSkipTrace();

  // Methods for sstable files
  Status MangleSSTableFiles(void);
  Status WriteMangledSSTableFiles(void);

  // Methods for wal files
  Status MangleWALFiles(void);
  Status WriteMangledWALFiles(void);

  // Methods for manifest files
  Status MangleManifestFiles(void);
  Status WriteMangledManifestFiles(void);

  // Methods for mangling algorithm
  Status ApplyManglingProcessToManglingMap(void); 


  void PrintManglingMap(std::string filename);

  // UNUSED
  uint64_t start_ts;
  uint64_t end_ts;
};

}  // namespace rocksdb
