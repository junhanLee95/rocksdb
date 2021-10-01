//  Copyright (c) 2011-present, Facebook, Inc.  All rights reserved.
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).

#include "util/trace_replay.h"
#include "util/mangler.h"
#include <sys/types.h>
#include <inttypes.h>
#include <chrono>
#include <sstream>
#include <thread>
#include <iostream>
#include <fstream>
#include "db/db_impl.h"
#include "rocksdb/slice.h"
#include "rocksdb/write_batch.h"
#include "rocksdb/sst_file_writer.h"
#include "rocksdb/sst_file_reader.h"
#include "rocksdb/merge_operator.h"
#include "utilities/merge_operators.h"
#include "util/coding.h"
#include "util/string_util.h"
#include "port/port_dirent.h"
#include "tools/sst_dump_tool_imp.h"
#include "table/block_based_table_factory.h"
namespace rocksdb {


namespace {

struct StdErrReporter : public log::Reader::Reporter {
  void Corruption(size_t /*bytes*/, const Status& s) override {
    std::cerr << "Corruption detected in log file " << s.ToString() << "\n";
  }
};

void EncodeCFAndKey(std::string* dst, uint32_t cf_id, const Slice& key) {
  PutFixed32(dst, cf_id);
  PutLengthPrefixedSlice(dst, key);
}

void DecodeCFAndKey(std::string& buffer, uint32_t* cf_id, Slice* key) {
  Slice buf(buffer);
  GetFixed32(&buf, cf_id);
  GetLengthPrefixedSlice(&buf, key);
}
} // namespace

Mangler::Mangler(rocksdb::Env* env, DB* db, const std::vector<ColumnFamilyHandle*>& handles, std::string trace_path, std::string db_path, std::string mangling_out_dir, std::string trace_file_result, bool apply) :
                apply_(apply), trace_file_path_(trace_path), trace_file_result_(trace_file_result), mangling_out_dir_(mangling_out_dir), db_path_(db_path), env_(env) {
  assert(db != nullptr);
  db_ = static_cast<DBImpl*>(db->GetRootDB());
  cfh_default_ =  db->DefaultColumnFamily();

  for (ColumnFamilyHandle* cfh : handles) {
    cf_map_[cfh->GetID()] = cfh;
  } // this is not called and not be used.

  /* [Step 1] Mangler Initialization */
  /* 1.1. Get File Paths and Options */
  GetDBFilePaths();
  GetDBOptions();

  /* [Step 2] Trace File Initialization */
  /* 2.1. Trace Reader Initialization */
  Status s;
  s = NewFileTraceReader(env_, EnvOptions(), trace_file_path_,
                         &trace_reader_);
  if (!s.ok()) {
    fprintf(
        stderr,
        "Encountered an error creating a TraceReader from the trace file. "
        "Error: %s\n",
        s.ToString().c_str());
    exit(1);
  }

  /* 2.2. Trace Writer Initialization */
  s = NewFileTraceWriter(env_, EnvOptions(), trace_file_result_,
                         &trace_writer_);
  if (!s.ok()) {
    fprintf(
        stderr,
        "Encountered an error creating a TraceWriter from the trace file result. "
        "Error: %s\n",
        s.ToString().c_str());
    exit(1);
  }

  /* [Step 3] SST File Initialization */
  /* 3.1. sst_dumper for sstable files Initialization */
  for (size_t i = 0; i < sst_file_paths_.size(); i++) {
    std::unique_ptr<SstFileDumper> dumper( new SstFileDumper(loaded_db_opt_, sst_file_paths_[i], false, false));
    if(!dumper->getStatus().ok()) {
      fprintf(
          stderr,
          "Encountered an error creating a SstFileDumper from the trace file. "
          "Error: %s\n",
          sst_file_paths_[i].c_str());
      exit(1);
    }
    else {
      sst_file_dumpers_.push_back(std::move(dumper));
    } 
  }

  /* 3.2. sst_writer for sstable files Initialization */
  for (size_t i = 0; i < sst_file_paths_.size(); i++) {
    std::unique_ptr<SstFileWriter> sst_file_writer( new SstFileWriter(EnvOptions(), loaded_db_opt_, cfh_default_));
    sst_file_writers_.push_back(std::move(sst_file_writer));
  }

  /* [Step 4] log::Reader and log::Writer for log file Initialization */
  EnvOptions soptions(loaded_db_opt_);
  for (size_t i = 0; i < wal_file_paths_.size(); i++) {
    std::unique_ptr<SequentialFile> rfile;
    s = env_->NewSequentialFile(wal_file_paths_[i], &rfile, env_->OptimizeForLogRead(soptions));
    if(!s.ok()) {
      fprintf(
          stderr,
          "Encountered an error creating a wal file reader from the wal file. "
          "Error: %s\n",
          wal_file_paths_[i].c_str());
      exit(1);
    }
    else {
      std::unique_ptr<SequentialFileReader> wal_file_reader;
      wal_file_reader.reset(
          new SequentialFileReader(std::move(rfile), wal_file_paths_[i]));

      StdErrReporter reporter;
      uint64_t log_number;
      FileType type;

      // Extract log number from the log file name
      std::string sanitized = wal_file_paths_[i];
      std::string log_filename;
      size_t lastslash = sanitized.rfind('/');

      if (lastslash != std::string::npos)
        log_filename = sanitized.substr(lastslash + 1);

      if (!ParseFileName(log_filename, &log_number, &type)) {
        // bogus input, carry on as best we can
        log_number = 0;
      }

      std::unique_ptr<log::Reader> reader(new log::Reader(loaded_db_opt_.info_log, std::move(wal_file_reader), &reporter, true , log_number));
      wal_readers_.push_back(std::move(reader));


      // make output file name
      std::string wal_file_out_paths;
      wal_file_out_paths = mangling_out_dir_ + "/" + sanitized.substr(lastslash + 1);

      std::unique_ptr<WritableFile> wfile;
      env_->NewWritableFile(wal_file_out_paths, &wfile, EnvOptions());
      std::unique_ptr<WritableFileWriter> file_writer(
          new WritableFileWriter(std::move(wfile), wal_file_out_paths, EnvOptions()));

      std::unique_ptr<log::Writer> writer(new log::Writer(std::move(file_writer), log_number, loaded_db_opt_.recycle_log_file_num > 0));
      wal_writers_.push_back(std::move(writer));

    } 
  }


  /* [Step 5] log::Reader and log::Writer for manifest file Initialization */
  for (size_t i = 0; i < manifest_file_paths_.size(); i++) {
    std::unique_ptr<SequentialFile> rfile;
    s = env_->NewSequentialFile(manifest_file_paths_[i], &rfile, env_->OptimizeForManifestRead(soptions));
    if(!s.ok()) {
      fprintf(
          stderr,
          "Encountered an error creating a manifest file reader from the manifest file. "
          "Error: %s\n",
          manifest_file_paths_[i].c_str());
      exit(1);
    }
    else {
      std::unique_ptr<SequentialFileReader> manifest_file_reader;
      manifest_file_reader.reset(
          new SequentialFileReader(std::move(rfile), manifest_file_paths_[i]));

      StdErrReporter reporter;

      std::unique_ptr<log::Reader> reader(new log::Reader(loaded_db_opt_.info_log, std::move(manifest_file_reader), &reporter, true, 0));
      manifest_readers_.push_back(std::move(reader));


      // Extract log number from the log file name
      std::string sanitized = manifest_file_paths_[i];
      std::string manifest_filename;
      size_t lastslash = sanitized.rfind('/');

      if (lastslash != std::string::npos)
        manifest_filename = sanitized.substr(lastslash + 1);


      // make output file name
      std::string manifest_file_out_paths;
      manifest_file_out_paths = mangling_out_dir_ + "/" + sanitized.substr(lastslash + 1);

      std::unique_ptr<WritableFile> wfile;
      env_->NewWritableFile(manifest_file_out_paths, &wfile, env_->OptimizeForManifestWrite(soptions));
      std::unique_ptr<WritableFileWriter> file_writer(
          new WritableFileWriter(std::move(wfile), manifest_file_out_paths, soptions));

      std::unique_ptr<log::Writer> writer(new log::Writer(std::move(file_writer), 0, false));
      manifest_writers_.push_back(std::move(writer));
    } 
  }


}

Mangler::~Mangler() {
  size_t i;

  trace_reader_.reset();
  trace_writer_.reset();
  for (i = 0; i < sst_file_dumpers_.size(); i++) {
    sst_file_dumpers_[i].reset();
  }
  for (i = 0; i < sst_file_writers_.size(); i++) {
    sst_file_writers_[i].reset();
  }
  for (i = 0; i < wal_readers_.size(); i++) {
    wal_readers_[i].reset();
  }
  for (i = 0; i < wal_writers_.size(); i++) {
    wal_writers_[i].reset();
  }
  for (i = 0; i < manifest_readers_.size(); i++) {
    manifest_readers_[i].reset();
  }
  for (i = 0; i < manifest_writers_.size(); i++) {
    manifest_writers_[i].reset();
  }
}

void Mangler::GetDBFilePaths(void) {
  bool found = false;
  std::string foundfile;
  auto CloseDir = [](DIR* p) { closedir(p); };
  struct dirent* entry;

  // [STEP 1]. Get SST file paths
  std::string dir_sst = db_path_ + "/db";
  std::unique_ptr<DIR, decltype(CloseDir)> d_sst(opendir(dir_sst.c_str()),
                                                 CloseDir);
  if (d_sst == nullptr) {
    std::cout << "[ERR] opening directory " << dir_sst << " is failed\n";
    std::cout << "[ERR] please retype FLAGS_db.\n";
    return;
  }

  while ((entry = readdir(d_sst.get())) != nullptr) {
    unsigned int match;
    uint64_t num;
    if (sscanf(entry->d_name, "%" PRIu64 ".sst%n", &num, &match) &&
        match == strlen(entry->d_name)) {
      foundfile = dir_sst + "/" + std::string(entry->d_name);
      sst_file_paths_.push_back(foundfile);
      found = true;
    }
  }
  if (!found) {
    std::cout << "[ERR] finding sst file in the directory " << dir_sst << " is failed\n";
    std::cout << "[ERR] please retype FLAGS_db.\n";
    return;
  }
  
  // [STEP 2]. Get Manifest file paths
  found = false;
  std::string dir_manifest = db_path_ + "/db";
  std::unique_ptr<DIR, decltype(CloseDir)> d_manifest(opendir(dir_manifest.c_str()),
                                                 CloseDir);
  if (d_manifest == nullptr) {
    std::cout << "[ERR] opening directory " << dir_manifest << " is failed\n";
    std::cout << "[ERR] please retype FLAGS_db.\n";
    return;
  }

  while ((entry = readdir(d_manifest.get())) != nullptr) {
    unsigned int match;
    uint64_t num;
    if (sscanf(entry->d_name, "MANIFEST-%" PRIu64 "%n", &num, &match) &&
        match == strlen(entry->d_name)) {
      foundfile = dir_manifest + "/" + std::string(entry->d_name);
      manifest_file_paths_.push_back(foundfile);
      found = true;
    }
  }
  if (!found) {
    std::cout << "[ERR] finding manifest file in the directory " << dir_manifest << " is failed\n";
    std::cout << "[ERR] please retype FLAGS_db.\n";
    return;
  }
  
  // [STEP 3]. Get WAL file paths
  found = false;
  std::string dir_wal = db_path_ + "/db.wal";
  std::unique_ptr<DIR, decltype(CloseDir)> d_wal(opendir(dir_wal.c_str()),
                                             CloseDir);
  if (d_wal == nullptr) {
    std::cout << "[ERR] opening directory " << dir_wal << " is failed\n";
    std::cout << "[ERR] please retype FLAGS_db.\n";
    return;
  }

  while ((entry = readdir(d_wal.get())) != nullptr) {
    unsigned int match;
    uint64_t num;
    if (sscanf(entry->d_name, "%" PRIu64 ".log%n", &num, &match) &&
        match == strlen(entry->d_name)) {
      foundfile = dir_wal + "/" + std::string(entry->d_name);
      wal_file_paths_.push_back(foundfile);
      found = true;
    }
  }
  if (!found) {
    std::cout << "[ERR] finding wal file in the directory " << dir_wal << " is failed\n";
    std::cout << "[ERR] please retype FLAGS_db.\n";
    return;
  }
}

void Mangler::GetDBOptions(void) {
  Status s;
  std::string option_path = db_path_ + "/db";
  s = LoadLatestOptions(option_path, env_, &loaded_db_opt_,
                        &loaded_cf_descs_);

  // NOTE: LoadLatestOptions cannot bring all the settings from the option file.
  //       Thus, we need to set the option as the same as the bluestore_rocksdb_options of Ceph Nautilus.
  //       merge_operator is configured as Bluestore, which acts same as BytesXOR but the name is the default settings of
  //       Ceph bluestore
  //       Applying merge_operator as BytesXOR is fine, since values are padded as zero and have fixed length,
  //       merge operations of mangled KVs always return zeroes regardless of the type of the merge_operator.
  //

  BlockBasedTableOptions table_options;
  table_options.cache_index_and_filter_blocks = true;
  table_options.filter_policy.reset(NewBloomFilterPolicy(20));
  loaded_db_opt_.table_factory.reset(new BlockBasedTableFactory(table_options));
  loaded_db_opt_.compression = kNoCompression;
  loaded_db_opt_.merge_operator = MergeOperators::CreateBluestoreOperator();
  loaded_db_opt_.wal_recovery_mode = WALRecoveryMode::kPointInTimeRecovery;

  if (!s.ok()) {
    fprintf(
        stderr,
        "Encountered an error loading the latest options from the trace file. "
        "Error: %s\n",
        option_path.c_str());
    exit(1);
  }
}

Status Mangler::mangle() {
  Status s;
  
  s = MangleTraceFile();
  if (!s.ok()) {
    return Status::Corruption("Error mangling trace files");
  }
  

  s = MangleSSTableFiles();
  if (!s.ok()) {
    return Status::Corruption("Error mangling sstable files");
  }
  
  s = MangleWALFiles();
  if (!s.ok()) {
    return Status::Corruption("Error mangling wal files");
  }

  s = MangleManifestFiles();
  if (!s.ok()) {
    return Status::Corruption("Error mangling manifest files");
  }
 
  PrintManglingMap("/home/junhan/ceph_rocksdb/rocksdb/mangle_before.txt");

  s = ApplyManglingProcessToManglingMap();
  if (!s.ok()) {
    return Status::Corruption("Error applying mangling process to the mangling map");
  } 

  PrintManglingMap("/home/junhan/ceph_rocksdb/rocksdb/mangle_after.txt");

  return s;
}

void Mangler::PrintManglingMap(std::string filename) {
  ofstream ofile;
  ofile.open(filename);

  map<std::string, std::string>::iterator it;
  for (it = mangling_map.begin(); it != mangling_map.end(); it++) {
    std::stringstream ss;

    ofile << "key : " << it->first << " => "
          << "value : ";// << it->second << std::endl;
    for (size_t i = 0; i < it->second.size(); i++) {
      ss << std::hex << (unsigned int)(unsigned char)(it->second[i]);
    }
    ofile << ss.str() << ", ";
    ofile << it->second << std::endl;
  }

  ofile.close();
  return;
}

Status Mangler::mangle_write(void) {
  Status s;

  /* [Step 1] write trace files */
  // reset before reading trace file again
  
  trace_reader_.reset();
  s = NewFileTraceReader(env_, EnvOptions(), trace_file_path_,
                         &trace_reader_);
  if (!s.ok()) {
    fprintf(
        stderr,
        "Encountered an error creating a TraceReader from the trace file. "
        "Error: %s\n",
        s.ToString().c_str());
    exit(1);
  }

  s = NewFileTraceReader(env_, EnvOptions(), trace_file_path_,
                         &trace_reader_);

  // write trace file
  s = WriteMangledTraceFile();
  if (!s.ok()) {
    return Status::Corruption("Error writing mangled trace files");
  }

  // reset before reading SST file again
  sst_file_dumpers_.clear();
  for (size_t i = 0; i < sst_file_paths_.size(); i++) {
    std::unique_ptr<SstFileDumper> dumper( new SstFileDumper(loaded_db_opt_, sst_file_paths_[i], false, false));
    if(!dumper->getStatus().ok()) {
      fprintf(
          stderr,
          "Encountered an error creating a SstFileDumper(2) from the trace file. "
          "Error: %s\n",
          sst_file_paths_[i].c_str());
      exit(1);
    }
    else {
      sst_file_dumpers_.push_back(std::move(dumper));
    } 
  }

  /* [Step 2] write sst files */
  
  s = WriteMangledSSTableFiles();
  if (!s.ok()) {
    fprintf(stderr,
        "Error: %s\n",
        s.ToString().c_str());
    return Status::Corruption("Error writing mangled sstable files");
  }
  

  /* [Step 3] write WAL files */
  
  // reset wal readers before reading WAL file again
  wal_readers_.clear();
  EnvOptions soptions(loaded_db_opt_);
  for (size_t i = 0; i < wal_file_paths_.size(); i++) {
    std::unique_ptr<SequentialFile> rfile;
    s = env_->NewSequentialFile(wal_file_paths_[i], &rfile, soptions);
    if(!s.ok()) {
      fprintf(
          stderr,
          "Encountered an error creating a wal file reader from the wal file(2). "
          "Error: %s\n",
          wal_file_paths_[i].c_str());
      exit(1);
    }
    else {
      std::unique_ptr<SequentialFileReader> wal_file_reader;
      wal_file_reader.reset(
          new SequentialFileReader(std::move(rfile), wal_file_paths_[i]));

      StdErrReporter reporter;
      uint64_t log_number;
      FileType type;

      // Extract log number from the log file name
      std::string sanitized = wal_file_paths_[i];
      std::string log_filename;
      size_t lastslash = sanitized.rfind('/');
      if (lastslash != std::string::npos)
        log_filename = sanitized.substr(lastslash + 1);
      if (!ParseFileName(log_filename, &log_number, &type)) {
        // bogus input, carry on as best we can
        log_number = 0;
      }
      std::unique_ptr<log::Reader> reader(new log::Reader(loaded_db_opt_.info_log, std::move(wal_file_reader), &reporter, true , log_number));
      wal_readers_.push_back(std::move(reader));
    }
  }
 
  s = WriteMangledWALFiles();
  if (!s.ok()) {
    return Status::Corruption("Error writing mangled wal files");
  }

  /* [Step 4] write Manifest files */

  // reset manifest readers before reading MANIFEST file again
  manifest_readers_.clear();
  for (size_t i = 0; i < manifest_file_paths_.size(); i++) {
    std::unique_ptr<SequentialFile> rfile;
    s = env_->NewSequentialFile(manifest_file_paths_[i], &rfile, env_->OptimizeForManifestRead(soptions));
    if(!s.ok()) {
      fprintf(
          stderr,
          "Encountered an error creating a manifest file reader from the manifest file. "
          "Error: %s\n",
          manifest_file_paths_[i].c_str());
      exit(1);
    }
    else {
      std::unique_ptr<SequentialFileReader> manifest_file_reader;
      manifest_file_reader.reset(
          new SequentialFileReader(std::move(rfile), manifest_file_paths_[i]));

      StdErrReporter reporter;

      std::unique_ptr<log::Reader> reader(new log::Reader(loaded_db_opt_.info_log, std::move(manifest_file_reader), &reporter, true, 0));
      manifest_readers_.push_back(std::move(reader));
    }
  }

  s = WriteMangledManifestFiles();
  if (!s.ok()) {
    return Status::Corruption("Error writing mangled manifest files");
  }
   
  return s;
}

Status Mangler::ApplyManglingProcessToManglingMap(void) {
  Status s;
  if (apply_) {
    const size_t prefix_size = 2; 
    map<string, string>::iterator it = mangling_map.begin();
    size_t i = 0;
    string prev, prev_prefix, prev_payload;
    string next, next_prefix, next_payload;
    string m_key;
    int r = 0 ;
    bool reset_prefix=false;

    prev = it->first;
    prev_prefix = prev.substr(0, prefix_size); //4D=M
    prev_payload = prev.substr(2, prev.size() - prefix_size);
  
    string init_key;
    for (i = 0; i< 1000; i++){
      init_key.push_back((char) (int)strtol("00",NULL,16));
    }

    string prev_tmp, next_tmp;
    string rand_tmp;
    size_t start_index = 1000;
    while(it != mangling_map.end()){
      next = it->first;
      next_prefix = next.substr(0, prefix_size);
      next_payload = next.substr(2, next.size() - prefix_size);
      size_t min_len = (prev.size() < next.size()) ? prev.size() : next.size();

      //prefix hex check add
      init_key[0] = (char)(int)strtol(next_prefix.c_str(), NULL, 16);
      bool find = false;
      if (reset_prefix || prev.size() <= 4){
        for(int j=2; j < 2000; j+=2){
          init_key[j/2] = ((char) (int)strtol("00",NULL,16));
        }
      } else {
        for ( i = 0; i < min_len; i+=2){
          prev_tmp = prev.substr(i, 2);
          next_tmp = next.substr(i, 2);
          if(!find){
            r = compare(&prev_tmp, &next_tmp, (size_t)2); // 1byte compare
            if (r != 0) {
              find = true;
              if (start_index != i){
                start_index=i;
                for(int j=i; j < 2000; j+=2){
                  if(j == 0){
                    reset_prefix=true;
                    find = false;
                    break;
                  } else{
                    init_key[j/2] = ((char) (int)strtol("00",NULL,16));
                  }
                }
              }
              if (i != 0)
                init_key[i/2] = (char) (int)strtol(next_tmp.c_str(),NULL,16);
            }
          }
        }
      }
      m_key=init_key.substr(0, next.size()/2); 
      mangling_map[next] = m_key;
      m_key.clear();
      rand_tmp.clear();
      prev_tmp.clear();
      next_tmp.clear();
      prev = next;
      reset_prefix=false;
      prev_prefix = next_prefix;
      prev_payload = next_payload;
      it++;
    }
  }
  else {
    map<string, string>::iterator it = mangling_map.begin();
    for(; it != mangling_map.end(); it ++) {
      std::string hex(it->first.size()/2, ' ');
      std::string piece;
      for (size_t j=0; j<it->first.size(); j+=2) {
        piece = it->first.substr(j, 2);
        hex[j/2] =  (char) (int)strtol(piece.c_str(),NULL,16);
      }
      mangling_map[it->first] = hex;
    }
  }

  return s;
  /*
  Status s;
  return s;
  */
}

// Methods for trace files

Status Mangler::MangleTraceFile() {

  Status s;
  Trace header;
  s = ReadHeader(&header);
  if (!s.ok()) {
    return s;
  }
  Trace trace;
  string tmp = "";
  while (s.ok()) {
    trace.reset();
    s = ReadTrace(&trace);
    if (!s.ok()) {
      break;
    }
    if (trace.type == kTraceWrite) {
      WriteBatch batch(trace.payload);
      //jsyeon
      Slice input(trace.payload);
      if (input.size() < WriteBatchInternal::kHeader) {
        return Status::Corruption("malformed WriteBatch (too small)");
      }
      input.remove_prefix(WriteBatchInternal::kHeader);
      Slice key, value, blob, xid;
      uint32_t column_family = 0;  // default
      char tag = 0;
      while (((s.ok() && !input.empty()) || UNLIKELY(s.IsTryAgain()))) {
        s = ReadRecordFromWriteBatch(&input, &tag, &column_family, &key, &value,
                                           &blob, &xid);
        if (!s.ok()) {
            return s;
        }
        mangling_map.insert(make_pair(key.ToString(true).c_str(), tmp));
      }
      //jsyeon End
    } else if (trace.type == kTraceGet) {
      uint32_t cf_id = 0;
      Slice key;
      DecodeCFAndKey(trace.payload, &cf_id, &key);
      mangling_map.insert(make_pair(key.ToString(true).c_str(), tmp));
      if (cf_id > 0 && cf_map_.find(cf_id) == cf_map_.end()) {
        return Status::Corruption("Invalid Column Family ID.");
      }
    } else if (trace.type == kTraceIteratorSeek) {
      uint32_t cf_id = 0;
      Slice key;
      DecodeCFAndKey(trace.payload, &cf_id, &key);
      mangling_map.insert(make_pair(key.ToString(true).c_str(), tmp));
      if (cf_id > 0 && cf_map_.find(cf_id) == cf_map_.end()) {
        return Status::Corruption("Invalid Column Family ID.");
      }
    } else if (trace.type == kTraceIteratorSeekForPrev) {
      // Currently, only support to call the Seek()
      uint32_t cf_id = 0;
      Slice key;
      DecodeCFAndKey(trace.payload, &cf_id, &key);
      mangling_map.insert(make_pair(key.ToString(true).c_str(), tmp));
      if (cf_id > 0 && cf_map_.find(cf_id) == cf_map_.end()) {
        return Status::Corruption("Invalid Column Family ID.");
      }
    } else if (trace.type == kTraceEnd) {
      break;
    }
  }
  if (s.IsIncomplete()) {
    // Reaching eof returns Incomplete status at the moment.
    // Could happen when killing a process without calling EndTrace() API.
    // TODO: Add better error handling.
    return Status::OK();
  }

  return s;
}

Status Mangler::WriteMangledTraceFile(void) {
  Status s;
  Trace header;
  s = ReadHeader(&header);
  if (!s.ok()) {
    return s;
  }
  WriteHeader(header.ts);
  WriteOptions woptions;
  ReadOptions roptions;
  Trace trace;
  uint64_t ops = 0;
  Iterator* single_iter = nullptr;
  string tmp = "";
  while (s.ok()) {
    trace.reset();
    s = ReadTrace(&trace);
    if (!s.ok()) {
      break;
    }
    if (trace.type == kTraceWrite) {
      WriteBatch batch(trace.payload);
      Slice input(trace.payload);
      if (input.size() < WriteBatchInternal::kHeader) {
        return Status::Corruption("malformed WriteBatch (too small)");
      }
      input.remove_prefix(WriteBatchInternal::kHeader);
      Slice key, value, blob, xid;
      uint32_t column_family = 0;  // default
      char tag = 0;
      WriteBatch m_batch;
      while (((s.ok() && !input.empty()) || UNLIKELY(s.IsTryAgain()))) {
        s = ReadRecordFromWriteBatch(&input, &tag, &column_family, &key, &value,
                                           &blob, &xid);
        if (!s.ok()) {
            return s;
        }
        Slice m_key(mangling_map[key.ToString(true).c_str()]);
        string z_value;
        if (apply_) {
          for (size_t i=0; i< value.size() ; i++){
            z_value.push_back('0'); 
          }
        } else {
          z_value = string(value.data());
        }
        
        Slice m_value(z_value);

        switch(tag){
          case kTypeDeletion:
            m_batch.Delete(m_key);
            break;
          case kTypeValue:
            m_batch.Put(m_key, m_value);
            break;
          case kTypeMerge:
            m_batch.Merge(m_key, m_value);
            break;
          case kTypeSingleDeletion:
            m_batch.SingleDelete(m_key);
            break;
          default:
            fprintf(stdout,"Unknown Tag: %d\n", tag);
            break;
        }

      }
      Write(&m_batch, trace.ts);
      ops++;
    } else if (trace.type == kTraceGet) {
      uint32_t cf_id = 0;
      Slice key;
      DecodeCFAndKey(trace.payload, &cf_id, &key);
      if (cf_id > 0 && cf_map_.find(cf_id) == cf_map_.end()) {
        return Status::Corruption("Invalid Column Family ID.");
      }
      Slice m_key(mangling_map[key.ToString(true).c_str()]);
      std::string value;
      if (cf_id == 0) {
        Get(cf_id, m_key, trace.ts);
      } else {
        Get(cf_id, m_key, trace.ts);
      }
      ops++;
    } else if (trace.type == kTraceIteratorSeek) {
      uint32_t cf_id = 0;
      Slice key;
      DecodeCFAndKey(trace.payload, &cf_id, &key);
      Slice m_key(mangling_map[key.ToString(true).c_str()]);
      if (cf_id > 0 && cf_map_.find(cf_id) == cf_map_.end()) {
        return Status::Corruption("Invalid Column Family ID.");
      }
      IteratorSeek(cf_id, m_key, trace.ts);
      ops++;
      delete single_iter;
    } else if (trace.type == kTraceIteratorSeekForPrev) {
      uint32_t cf_id = 0;
      Slice key;
      DecodeCFAndKey(trace.payload, &cf_id, &key);
      Slice m_key(mangling_map[key.ToString(true).c_str()]);
      if (cf_id > 0 && cf_map_.find(cf_id) == cf_map_.end()) {
        return Status::Corruption("Invalid Column Family ID.");
      }
      IteratorSeekForPrev(cf_id, m_key, trace.ts);
      ops++;
      delete single_iter;
    } else if (trace.type == kTraceEnd) {
      WriteFooter(trace.ts);
      break;
    }
  }
  if (s.IsIncomplete()) {
    return Status::OK();
  }
  return s;
}

Status Mangler::ReadHeader(Trace* header) {
  assert(header != nullptr);
  Status s = ReadTrace(header);
  if (!s.ok()) {
    return s;
  }
  if (header->type != kTraceBegin) {
    return Status::Corruption("Corrupted trace file. Incorrect header.");
  }
  if (header->payload.substr(0, kTraceMagic.length()) != kTraceMagic) {
    return Status::Corruption("Corrupted trace file. Incorrect magic.");
  }

  return s;
}

Status Mangler::ReadFooter(Trace* footer) {
  assert(footer != nullptr);
  Status s = ReadTrace(footer);
  if (!s.ok()) {
    return s;
  }
  if (footer->type != kTraceEnd) {
    return Status::Corruption("Corrupted trace file. Incorrect footer.");
  }
  // TODO: Add more validations later
  return s;
}

Status Mangler::ReadTrace(Trace* trace) {
  assert(trace != nullptr);
  std::string encoded_trace;
  Status s = trace_reader_->Read(&encoded_trace);
  if (!s.ok()) {
    return s;
  }

  Slice enc_slice = Slice(encoded_trace);
  GetFixed64(&enc_slice, &trace->ts);
  trace->type = static_cast<TraceType>(enc_slice[0]);
  enc_slice.remove_prefix(kTraceTypeSize + kTracePayloadLengthSize);
  trace->payload = enc_slice.ToString();
  return s;
}

int Mangler::compare(string* a, string* b, size_t min_len){
  int r = strncmp(a->c_str(), b->c_str(), min_len);
  if (r==0) {
    if(a->size() < b->size())
      r = -1;
    else if (a->size() > b->size())
      r = +1;
  }
  return r;
}

Status Mangler::Write(WriteBatch* write_batch, uint64_t ts) {
  TraceType trace_type = kTraceWrite;
  if (ShouldSkipTrace()) {
    return Status::OK();
  }
  Trace trace;
  trace.ts = ts;
  trace.type = trace_type;
  trace.payload = write_batch->Data();
  return WriteTrace(trace);
}

Status Mangler::Get(uint32_t cf_id, const Slice& key, uint64_t ts) {
  TraceType trace_type = kTraceGet;
  if (ShouldSkipTrace()) {
    return Status::OK();
  }
  Trace trace;
  trace.ts = ts;
  trace.type = trace_type;
  EncodeCFAndKey(&trace.payload, cf_id, key);
  return WriteTrace(trace);
}

Status Mangler::IteratorSeek(const uint32_t& cf_id, const Slice& key, uint64_t ts) {
  TraceType trace_type = kTraceIteratorSeek;
  if (ShouldSkipTrace()) {
    return Status::OK();
  }
  Trace trace;
  trace.ts = ts;
  trace.type = trace_type;
  EncodeCFAndKey(&trace.payload, cf_id, key);
  return WriteTrace(trace);
}

Status Mangler::IteratorSeekForPrev(const uint32_t& cf_id, const Slice& key, uint64_t ts) {
  TraceType trace_type = kTraceIteratorSeekForPrev;
  if (ShouldSkipTrace()) {
    return Status::OK();
  }
  Trace trace;
  trace.ts = ts;
  trace.type = trace_type;
  EncodeCFAndKey(&trace.payload, cf_id, key);
  return WriteTrace(trace);
}
Status Mangler::WriteHeader(uint64_t ts) {
  std::ostringstream s;
  s << kTraceMagic << "\t"
    << "Trace Version: 0.1\t"
    << "RocksDB Version: " << kMajorVersion << "." << kMinorVersion << "\t"
    << "Format: Timestamp OpType Payload\n";
  std::string header(s.str());

  Trace trace;
  trace.ts = ts;
  trace.type = kTraceBegin;
  trace.payload = header;
  return WriteTrace(trace);
}

Status Mangler::WriteFooter(uint64_t ts) {
  Trace trace;
  trace.ts = ts;
  trace.type = kTraceEnd;
  trace.payload = "";
  return WriteTrace(trace);
}
Status Mangler::WriteTrace(const Trace& trace) {
  std::string encoded_trace;
  PutFixed64(&encoded_trace, trace.ts);
  encoded_trace.push_back(trace.type);
  PutFixed32(&encoded_trace, static_cast<uint32_t>(trace.payload.size()));
  encoded_trace.append(trace.payload);
  return trace_writer_->Write(Slice(encoded_trace));
}

bool Mangler::IsTraceFileOverMax() {
  uint64_t trace_file_size = trace_writer_->GetFileSize();
  uint64_t max_trace_file_size = uint64_t{64} * 1024 * 1024 * 1024;
  return (trace_file_size > max_trace_file_size);
}

bool Mangler::ShouldSkipTrace() {
  if (IsTraceFileOverMax()) {
    return true;
  }
  return false;
}

// Methods for sstable files

Status Mangler::MangleSSTableFiles(void) {
  Status s;
  // Read SSTable Files
  // Update Mangled Map
  size_t i;
  for (i = 0; i < sst_file_dumpers_.size(); i++) {
    std::cout << "[INFO] Mangle sst : " << sst_file_paths_[i] << std::endl;
    sst_file_dumpers_[i]->UpdateManglingMap(mangling_map);
  }

  return s;
}

Status Mangler::WriteMangledSSTableFiles(void) {
  Status s;
  size_t i;

  for (i = 0; i < sst_file_paths_.size(); i++) {
    std::cout << "[INFO] Sst Write : " << sst_file_paths_[i] << std::endl;
    // make output file name
    std::string sst_file_out_paths;
    std::string sanitized;
    ReadOptions ropts;

    sanitized = sst_file_paths_[i];
    size_t lastslash = sanitized.rfind('/');
    if (lastslash != std::string::npos) {
      sst_file_out_paths = mangling_out_dir_ + "/" + sanitized.substr(lastslash + 1);
    }
    else {
      return Status::Corruption("WriteMangledSSTableFiles error : Failed to parse sst file path");
    }

    sst_file_writers_[i]->Open(sst_file_out_paths);

    /* reset creation time */
    std::shared_ptr<const TableProperties> tpptr;
    const TableProperties* tp;

    sst_file_dumpers_[i]->ReadTableProperties(&tpptr);
    tp = tpptr.get();

    sst_file_writers_[i]->ResetTableProperties(tp);
    s = sst_file_dumpers_[i]->WriteMangledSSTableFiles(mangling_map, sst_file_writers_[i], apply_);

    if (!s.ok()) {
      fprintf(
        stderr,
        "Encountered an error writing a Mangled SST file from the sst file path"
        "Error: %s\n",
        s.ToString().c_str());

      return s;
    }

    ExternalSstFileInfo sst_file_infoi;
    s = sst_file_writers_[i]->Finish(&sst_file_infoi);
    if (!s.ok()) {
      fprintf(
        stderr,
        "Encountered an error finishing a Mangled SST file from the sst file path"
        "Error: %s\n",
        s.ToString().c_str());

      return s;
    }
  }

  return s;
}

// Methods for wal files

Status Mangler::MangleWALFiles(void) {
  Status s;

  // Read WAL Files
  // Update Mangled Map
  size_t i;
  for (i = 0; i < wal_readers_.size(); i++) {
    std::cout << "[INFO] Mangle wal : " << wal_file_paths_[i] << std::endl;
    s = wal_readers_[i]->UpdateManglingMap(mangling_map);
    if (!s.ok()) {
      fprintf(
        stderr,
        "Encountered an error mangling wal files"
        "Error: %s\n",
        s.ToString().c_str());
      return s;
    }
  }

  return s;
}

Status Mangler::WriteMangledWALFiles(void) {
  Status s;
  size_t i;

  int record_num = 0;
  int batch_num = 0;

  for (i = 0; i < wal_readers_.size(); i++) {
    std::cout << "[INFO] Wal Write " << wal_file_paths_[i] << std::endl;
    std::string scratch;
    WriteBatch ibatch;
    WriteBatch obatch;
    Slice record;
    std::stringstream row;
    record_num = 0;
    batch_num = 0;

    while (wal_readers_[i]->ReadRecord(&record, &scratch)) {
      record_num ++;

      row.str("");
      if (record.size() < WriteBatchInternal::kHeader) {
        std::cerr << "[ERR] log record too small : " <<  record.size() << std::endl;
        return Status::Corruption();
      } else if (record.size() == WriteBatchInternal::kHeader) {
          obatch.Clear();
          WriteBatchInternal::SetContents(&obatch, record);
          wal_writers_[i]->AddRecord(WriteBatchInternal::Contents(&obatch));
      }
      else {
        // get ibatch
        WriteBatchInternal::SetContents(&ibatch, record);
        // clear obatch
        obatch.Clear();
        // replace keys and values in the batched to mangled keys and values
        Slice input(ibatch.Data());
        if (input.size() < WriteBatchInternal::kHeader) {
          return Status::Corruption("malformed WriteBatch (too small)");
        }

        input.remove_prefix(WriteBatchInternal::kHeader);
        Slice key, value, blob, xid;

        // Sometimes a sub-batch starts with a Noop. We want to exclude such Noops as
        // the batch boundary symbols otherwise we would mis-count the number of
        // batches. We do that by checking whether the accumulated batch is empty
        // before seeing the next Noop.
        char tag = 0;
        uint32_t column_family = 0;  // default
        bool last_was_try_again = false;
        batch_num = 0;

        while (((s.ok() && !input.empty()) || UNLIKELY(s.IsTryAgain()))) {
          batch_num ++;
          if (LIKELY(!s.IsTryAgain())) {
            last_was_try_again = false;
            tag = 0;
            column_family = 0;  // default
      
            s = ReadRecordFromWriteBatch(&input, &tag, &column_family, &key, &value,
                                         &blob, &xid);
           
            if (!s.ok()) {
              return s;
            }
          } else {
            assert(s.IsTryAgain());
            assert(!last_was_try_again); // to detect infinite loop bugs
            if (UNLIKELY(last_was_try_again)) {
              return Status::Corruption(
                  "two consecutive TryAgain in WriteBatch handler; this is either a "
                  "software bug or data corruption.");
            }
            last_was_try_again = true;
            s = Status::OK();
          }
          // set sequence of obatch to the sequence of ibatch
          WriteBatchInternal::SetSequence(&obatch, WriteBatchInternal::Sequence(&ibatch));

          switch (tag) {
            case kTypeColumnFamilyValue:
            case kTypeValue: {
              Slice m_key(mangling_map[key.ToString(true).c_str()]);
              string z_value;
              if (apply_) {
                for (size_t j=0; j< value.size() ; j++){
                  z_value.push_back('0'); 
                } 
              } else {
                z_value = string(value.data());
              }
              Slice m_value(z_value);
 
              obatch.Put(m_key, m_value);
              break;
            }
            case kTypeColumnFamilyDeletion:
            case kTypeDeletion: {
              Slice m_key(mangling_map[key.ToString(true).c_str()]);
              obatch.Delete(m_key);
              break;
            }
            case kTypeColumnFamilySingleDeletion:
            case kTypeSingleDeletion: {
              Slice m_key(mangling_map[key.ToString(true).c_str()]);

              s = obatch.SingleDelete(m_key);
              break;
            }
            case kTypeColumnFamilyRangeDeletion: // NOT_USED
            case kTypeRangeDeletion: {
              Slice m_key(mangling_map[key.ToString(true).c_str()]);
              s = obatch.DeleteRange(m_key, m_key);
              break;
            }
            case kTypeColumnFamilyMerge:
            case kTypeMerge: {
              Slice m_key(mangling_map[key.ToString(true).c_str()]);
              string z_value;
              if (apply_) {
                for (size_t j=0; j< value.size() ; j++){
                  z_value.push_back('0'); 
                }
              } else {
                z_value = string(value.data());
              }

              Slice m_value(z_value);
 
              s = obatch.Merge(m_key, m_value);
              break;
            }
            case kTypeColumnFamilyBlobIndex:
            case kTypeBlobIndex:
              std::cout << "[ERROR] not supported\n";
              break;
           case kTypeLogData:    
              s = obatch.PutLogData(blob);
              break;
           case kTypeBeginPrepareXID:
           case kTypeBeginPersistedPrepareXID:
           case kTypeBeginUnprepareXID:
             std::cout << "[ERROR] not supported\n";
             break;
           case kTypeEndPrepareXID:
             std::cout << "[ERROR] not supported\n";
             break;
           case kTypeCommitXID:
             std::cout << "[ERROR] not supported\n";
             break;
           case kTypeRollbackXID:
             std::cout << "[ERROR] not supported\n";
             break;
           case kTypeNoop:
             std::cout << "[ERROR] not supported\n";
             break;
           default:
             std::cout << "[ERROR] unknown tag\n";
             break;
          }
        }
        wal_writers_[i]->AddRecord(WriteBatchInternal::Contents(&obatch));
      } 
    }
  }
  return s;
}

// Methods for manifest files

Status Mangler::MangleManifestFiles(void) {
  Status s;

  EnvOptions sopt;
  std::shared_ptr<Cache> tc(NewLRUCache(loaded_db_opt_.max_open_files - 10,
                                        loaded_db_opt_.table_cache_numshardbits));
  // Notice we are using the default options not through SanitizeOptions(),
  // if VersionSet::DumpManifest() depends on any option done by
  // SanitizeOptions(), we need to initialize it manually.
  WriteController wc(loaded_db_opt_.delayed_write_rate);
  WriteBufferManager wb(loaded_db_opt_.db_write_buffer_size);
  ImmutableDBOptions immutable_db_options(loaded_db_opt_);
  VersionSet versions(cfh_default_->GetName(), &immutable_db_options, sopt, tc.get(), &wb, &wc);

  size_t i;
  std::string tmp = "";
  Slice record;
  std::string scratch;

  for (i = 0; i < manifest_file_paths_.size(); i++) {
    std::cout << "[INFO] Mangle Manifest : " << manifest_file_paths_[i] << std::endl;
    while (manifest_readers_[i]->ReadRecord(&record, &scratch) && s.ok()) {
      VersionEdit edit;
      s = edit.DecodeFrom(record);
      if (!s.ok()) {
        break;
      }
      
      // update Mangled Map
      edit.UpdateManglingMap(mangling_map);
    }
  }

  return s;
}

Status Mangler::WriteMangledManifestFiles(void) {
  Status s;

  // Read Manifest Files
  size_t i;
  for (i = 0; i < manifest_readers_.size(); i++) {
    std::cout << "[INFO] Manifest Write " << manifest_file_paths_[i] << std::endl;
    Slice record;
    std::string scratch;

    while (manifest_readers_[i]->ReadRecord(&record, &scratch) && s.ok()) {
      VersionEdit edit;
      std::string new_record_str;
      s = edit.DecodeFrom(record);
      if (!s.ok()) {
        break;
      }
      
      // if edit contains FileMetadata and key data, update the value by looking at the mangling map
      // if not, write the original edit to the log file
      VersionEdit new_edit;
      edit.WriteMangledVersionEdit(new_edit, mangling_map);

      if (!new_edit.EncodeTo(&new_record_str)) {
        return Status::Corruption("cannot encode new record from manifest file");
      }
 
      manifest_writers_[i]->AddRecord(Slice(new_record_str));
    }   
  }

  return s;
}


}  // namespace rocksdb
