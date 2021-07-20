//  Copyright (c) 2011-present, Facebook, Inc.  All rights reserved.
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).

#include "util/trace_replay.h"

#include <chrono>
#include <sstream>
#include <thread>
#include "db/db_impl.h"
#include "rocksdb/slice.h"
#include "rocksdb/write_batch.h"
#include "util/coding.h"
#include "util/string_util.h"
namespace rocksdb {

const std::string kTraceMagic = "feedcafedeadbeef";

namespace {
void EncodeCFAndKey(std::string* dst, uint32_t cf_id, const Slice& key) {
  PutFixed32(dst, cf_id);
  PutLengthPrefixedSlice(dst, key);
}

void DecodeCFAndKey(std::string& buffer, uint32_t* cf_id, Slice* key) {
  Slice buf(buffer);
  GetFixed32(&buf, cf_id);
  GetLengthPrefixedSlice(&buf, key);
}
}  // namespace

Tracer::Tracer(Env* env, const TraceOptions& trace_options,
               std::unique_ptr<TraceWriter>&& trace_writer)
    : env_(env),
      trace_options_(trace_options),
      trace_writer_(std::move(trace_writer)),
      trace_request_count_ (0) {
  WriteHeader();
}

Tracer::~Tracer() { trace_writer_.reset(); }

Status Tracer::Write(WriteBatch* write_batch) {
  TraceType trace_type = kTraceWrite;
  if (ShouldSkipTrace(trace_type)) {
    return Status::OK();
  }
  Trace trace;
  trace.ts = env_->NowMicros();
  trace.type = trace_type;
  trace.payload = write_batch->Data();
  return WriteTrace(trace);
}

Status Tracer::Get(ColumnFamilyHandle* column_family, const Slice& key) {
  TraceType trace_type = kTraceGet;
  if (ShouldSkipTrace(trace_type)) {
    return Status::OK();
  }
  Trace trace;
  trace.ts = env_->NowMicros();
  trace.type = trace_type;
  EncodeCFAndKey(&trace.payload, column_family->GetID(), key);
  return WriteTrace(trace);
}

Status Tracer::IteratorSeek(const uint32_t& cf_id, const Slice& key) {
  TraceType trace_type = kTraceIteratorSeek;
  if (ShouldSkipTrace(trace_type)) {
    return Status::OK();
  }
  Trace trace;
  trace.ts = env_->NowMicros();
  trace.type = trace_type;
  EncodeCFAndKey(&trace.payload, cf_id, key);
  return WriteTrace(trace);
}

Status Tracer::IteratorSeekForPrev(const uint32_t& cf_id, const Slice& key) {
  TraceType trace_type = kTraceIteratorSeekForPrev;
  if (ShouldSkipTrace(trace_type)) {
    return Status::OK();
  }
  Trace trace;
  trace.ts = env_->NowMicros();
  trace.type = trace_type;
  EncodeCFAndKey(&trace.payload, cf_id, key);
  return WriteTrace(trace);
}


bool Tracer::ShouldSkipTrace(const TraceType& trace_type) {
  if (IsTraceFileOverMax()) {
    return true;
  }
  if ((trace_options_.filter & kTraceFilterGet
    && trace_type == kTraceGet)
   || (trace_options_.filter & kTraceFilterWrite
    && trace_type == kTraceWrite)) {
    return true;
  }
  ++trace_request_count_;
  if (trace_request_count_ < trace_options_.sampling_frequency) {
    return true;
  }
  trace_request_count_ = 0;
  return false;
}

bool Tracer::IsTraceFileOverMax() {
  uint64_t trace_file_size = trace_writer_->GetFileSize();
  return (trace_file_size > trace_options_.max_trace_file_size);
}

Status Tracer::WriteHeader() {
  std::ostringstream s;
  s << kTraceMagic << "\t"
    << "Trace Version: 0.1\t"
    << "RocksDB Version: " << kMajorVersion << "." << kMinorVersion << "\t"
    << "Format: Timestamp OpType Payload\n";
  std::string header(s.str());

  Trace trace;
  trace.ts = env_->NowMicros();
  trace.type = kTraceBegin;
  trace.payload = header;
  return WriteTrace(trace);
}

Status Tracer::WriteFooter() {
  Trace trace;
  trace.ts = env_->NowMicros();
  trace.type = kTraceEnd;
  trace.payload = "";
  return WriteTrace(trace);
}


Status Tracer::WriteTrace(const Trace& trace) {
  std::string encoded_trace;
  PutFixed64(&encoded_trace, trace.ts);
  encoded_trace.push_back(trace.type);
  PutFixed32(&encoded_trace, static_cast<uint32_t>(trace.payload.size()));
  encoded_trace.append(trace.payload);
  return trace_writer_->Write(Slice(encoded_trace));
}

Status Tracer::Close() { return WriteFooter(); }

Replayer::Replayer(DB* db, const std::vector<ColumnFamilyHandle*>& handles,
                   std::unique_ptr<TraceReader>&& reader)
    : trace_reader_(std::move(reader)) {
  assert(db != nullptr);
  db_ = static_cast<DBImpl*>(db->GetRootDB());
  for (ColumnFamilyHandle* cfh : handles) {
    cf_map_[cfh->GetID()] = cfh;
  }
}

Replayer::~Replayer() { trace_reader_.reset(); }

Status Replayer::Replay() {
  Status s;
  Trace header;
  s = ReadHeader(&header);
  if (!s.ok()) {
    return s;
  }

  //std::chrono::system_clock::time_point replay_epoch =
  //    std::chrono::system_clock::now();
  WriteOptions woptions;
  ReadOptions roptions;
  Trace trace;
  uint64_t ops = 0;
  Iterator* single_iter = nullptr;
  map<string, string> mangling_map;
  string tmp = "";
  while (s.ok()) {
    trace.reset();
    s = ReadTrace(&trace);
    if (!s.ok()) {
      break;
    }
    /*
    std::this_thread::sleep_until(replay_epoch + std::chrono::microseconds(trace.ts - header.ts));
    */
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
        //fprintf(stdout,"PUT %s %lu\n", key.ToString(true).c_str(), key.size());
        mangling_map.insert(make_pair(key.ToString(true).c_str(), tmp));
        //fprintf(stdout, "Key   | %s | %lu\n", key.ToString(true).c_str(), key.size());
        //fprintf(stdout, "Value | %s | %lu\n", value.ToString(true).c_str(), value.size());
      }
      //jsyeon End
      db_->Write(woptions, &batch);
      ops++;
    } else if (trace.type == kTraceGet) {
      uint32_t cf_id = 0;
      Slice key;
      DecodeCFAndKey(trace.payload, &cf_id, &key);
      //fprintf(stdout,"GET %s\n", key.ToString(true).c_str());
      mangling_map.insert(make_pair(key.ToString(true).c_str(), tmp));
      if (cf_id > 0 && cf_map_.find(cf_id) == cf_map_.end()) {
        return Status::Corruption("Invalid Column Family ID.");
      }

      std::string value;
      if (cf_id == 0) {
        db_->Get(roptions, key, &value);
      } else {
        db_->Get(roptions, cf_map_[cf_id], key, &value);
      }
      ops++;
    } else if (trace.type == kTraceIteratorSeek) {
      uint32_t cf_id = 0;
      Slice key;
      DecodeCFAndKey(trace.payload, &cf_id, &key);
      //fprintf(stdout,"SEEK %s\n", key.ToString(true).c_str());
      mangling_map.insert(make_pair(key.ToString(true).c_str(), tmp));
      if (cf_id > 0 && cf_map_.find(cf_id) == cf_map_.end()) {
        return Status::Corruption("Invalid Column Family ID.");
      }

      if (cf_id == 0) {
        single_iter = db_->NewIterator(roptions);
      } else {
        single_iter = db_->NewIterator(roptions, cf_map_[cf_id]);
      }
      single_iter->Seek(key);
      ops++;
      delete single_iter;
    } else if (trace.type == kTraceIteratorSeekForPrev) {
      // Currently, only support to call the Seek()
      uint32_t cf_id = 0;
      Slice key;
      DecodeCFAndKey(trace.payload, &cf_id, &key);
      //fprintf(stdout,"SEEKforPREV %s\n", key.ToString(true).c_str());
      mangling_map.insert(make_pair(key.ToString(true).c_str(), tmp));
      if (cf_id > 0 && cf_map_.find(cf_id) == cf_map_.end()) {
        return Status::Corruption("Invalid Column Family ID.");
      }

      if (cf_id == 0) {
        single_iter = db_->NewIterator(roptions);
      } else {
        single_iter = db_->NewIterator(roptions, cf_map_[cf_id]);
      }
      single_iter->SeekForPrev(key);
      ops++;
      delete single_iter;
    } else if (trace.type == kTraceEnd) {
      //fprintf(stdout,"END\n");
      // Do nothing for now.
      // TODO: Add some validations later.
      break;
    }
  }

  if (s.IsIncomplete()) {
    // Reaching eof returns Incomplete status at the moment.
    // Could happen when killing a process without calling EndTrace() API.
    // TODO: Add better error handling.
    return Status::OK();
  }

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
    if (reset_prefix){
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
                }else{
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
  init_key[0] = (char)(int)strtol(prev_prefix.c_str(), NULL, 16);
  for ( i = 2; i < prev.size() ; i+=2){
    init_key[i/2] = (char)(int)strtol("FF", NULL, 16); 
  }
  m_key=init_key.substr(0, prev.size()/2); 
  mangling_map[prev] = m_key;
  //////Manling Succes
  /*
  for (it = mangling_map.begin(); it != mangling_map.end(); it++){
      cout << it->first  <<endl ;
  }
  for (it = jsyeon_map.begin() ; it != jsyeon_map.end(); it++){
      //cout << it->second << endl;
  }
  */

  return s;
}

Status Replayer::ReadHeader(Trace* header) {
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

Status Replayer::ReadFooter(Trace* footer) {
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

Status Replayer::ReadTrace(Trace* trace) {
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

int Replayer::compare(string* a, string* b, size_t min_len){
  int r = strncmp(a->c_str(), b->c_str(), min_len);
  if (r==0) {
    if(a->size() < b->size())
      r = -1;
    else if (a->size() > b->size())
      r = +1;
  }
  return r;
}
Mangler::Mangler(DB* db, const std::vector<ColumnFamilyHandle*>& handles, 
                   std::unique_ptr<TraceReader>&& reader)
    : trace_reader_(std::move(reader)) {
  assert(db != nullptr);
  db_ = static_cast<DBImpl*>(db->GetRootDB());
  for (ColumnFamilyHandle* cfh : handles) {
    cf_map_[cfh->GetID()] = cfh;
  }
}

Mangler::~Mangler() { trace_reader_.reset(); trace_writer_.reset();}

Status Mangler::mangle() {
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
    if (reset_prefix){
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
                }else{
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
  init_key[0] = (char)(int)strtol(prev_prefix.c_str(), NULL, 16);
  for ( i = 2; i < prev.size() ; i+=2){
    init_key[i/2] = (char)(int)strtol("FF", NULL, 16); 
  }
  m_key=init_key.substr(0, prev.size()/2); 
  mangling_map[prev] = m_key;
  /*
  for (it = mangling_map.begin(); it != mangling_map.end(); it++){
      cout << it->second  <<endl ;
  }
  */
  //////Manling Succes
  /*
  map<string, string> jsyeon_map;

    for (it = jsyeon_map.begin() ; it != jsyeon_map.end(); it++){
      //cout << it->second << endl;
  }
  */
  return s;
}

Status Mangler::mangle_write(std::unique_ptr<TraceReader>&& reader, std::unique_ptr<TraceWriter>&& writer){
  trace_reader_ = std::move(reader);
  trace_writer_ = std::move(writer);
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
        for (size_t i=0; i< value.size() ; i++){
          z_value.push_back('0'); 
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

}  // namespace rocksdb
