//  Copyright (c) 2011-present, Facebook, Inc.  All rights reserved.
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).
//
// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#include "db/flush_job.h"

#ifndef __STDC_FORMAT_MACROS
#define __STDC_FORMAT_MACROS
#endif

#include <inttypes.h>

#include <algorithm>
#include <vector>

#include "db/builder.h"
#include "db/db_iter.h"
#include "db/dbformat.h"
#include "db/event_helpers.h"
#include "db/log_reader.h"
#include "db/log_writer.h"
#include "db/memtable.h"
#include "db/memtable_list.h"
#include "db/merge_context.h"
#include "db/range_tombstone_fragmenter.h"
#include "db/version_set.h"
#include "monitoring/iostats_context_imp.h"
#include "monitoring/perf_context_imp.h"
#include "monitoring/thread_status_util.h"
#include "port/port.h"
#include "rocksdb/db.h"
#include "rocksdb/env.h"
#include "rocksdb/statistics.h"
#include "rocksdb/status.h"
#include "rocksdb/table.h"
#include "table/block.h"
#include "table/block_based_table_factory.h"
#include "table/merging_iterator.h"
#include "table/table_builder.h"
#include "table/two_level_iterator.h"
#include "util/coding.h"
#include "util/event_logger.h"
#include "util/file_util.h"
#include "util/filename.h"
#include "util/log_buffer.h"
#include "util/logging.h"
#include "util/mutexlock.h"
#include "util/stop_watch.h"
#include "util/sync_point.h"

namespace rocksdb {

const char* GetFlushReasonString (FlushReason flush_reason) {
  switch (flush_reason) {
    case FlushReason::kOthers:
      return "Other Reasons";
    case FlushReason::kGetLiveFiles:
      return "Get Live Files";
    case FlushReason::kShutDown:
      return "Shut down";
    case FlushReason::kExternalFileIngestion:
      return "External File Ingestion";
    case FlushReason::kManualCompaction:
      return "Manual Compaction";
    case FlushReason::kWriteBufferManager:
      return "Write Buffer Manager";
    case FlushReason::kWriteBufferFull:
      return "Write Buffer Full";
    case FlushReason::kTest:
      return "Test";
    case FlushReason::kDeleteFiles:
      return "Delete Files";
    case FlushReason::kAutoCompaction:
      return "Auto Compaction";
    case FlushReason::kManualFlush:
      return "Manual Flush";
    case FlushReason::kErrorRecovery:
      return "Error Recovery";
    case FlushReason::kSplitMemtable:
      return "Split Memtable";
    default:
      return "Invalid";
  }
}

// Maintains state for each sub-compaction
struct FlushJob::SubflushState {
  // The boundaries of the key-range this flush is interested in. No two
  // subflushs may have overlapping key-ranges.
  // 'start' is inclusive, 'end' is exclusive, and nullptr means unbounded
  //
  FlushState *flush_state;
  std::string start;
  std::string end;

  // The return status of this subflush
  Status status;

  FileMetaData sub_meta;
  VersionEdit* sub_edit;
  TableProperties sub_table_properties;

  int sub_flush_id;



  SubflushState(FlushState *_flush, std::string _start, 
		  std::string _end, FileMetaData _sub_meta, VersionEdit* _sub_edit, int _sub_flush_id)
      : flush_state(_flush),
		start(_start),
        end(_end),
		sub_meta(_sub_meta), sub_edit(_sub_edit) { 
		sub_flush_id = _sub_flush_id;
	}

  SubflushState(SubflushState&& o) { *this = std::move(o); }

  SubflushState& operator=(SubflushState&& o) {
	flush_state = std::move(o.flush_state);
    start = std::move(o.start);
    end = std::move(o.end);
    status = std::move(o.status);
    sub_meta = std::move(o.sub_meta);
	sub_edit = std::move(o.sub_edit);
	sub_flush_id = std::move(o.sub_flush_id);
	//is_parent = std::move(o.is_parent);
    return *this;
  }

  // Because member std::unique_ptrs do not have these.
  SubflushState(const SubflushState&) = delete;

  SubflushState& operator=(const SubflushState&) = delete;

};

// Maintains state for the entire flush
struct FlushJob::FlushState {
  // REQUIRED: subflush states are stored in order of increasing
  // key-range
  std::vector<FlushJob::SubflushState> sub_flush_states;
  Status status;

  uint64_t total_bytes;
  uint64_t num_input_records;
  uint64_t num_output_records;

  explicit FlushState()
      : total_bytes(0),
        num_input_records(0),
        num_output_records(0) {}

	std::vector<std::string>  GetSubflushStarts() {
		std::vector<std::string> starts;
		for (size_t i = 1; i < sub_flush_states.size(); i++) {
			starts.push_back(sub_flush_states[i].start);
		}
		return starts;
	}

	std::vector<std::string>  GetSubflushEnds() {
		std::vector<std::string> ends;
		for (size_t i = 1; i < sub_flush_states.size(); i++) {
			ends.push_back(sub_flush_states[i].end);
		}
		return ends;
	}

};


FlushJob::FlushJob(const std::string& dbname, ColumnFamilyData* cfd,
                   const ImmutableDBOptions& db_options,
                   const MutableCFOptions& mutable_cf_options,
                   const uint64_t* max_memtable_id,
                   const EnvOptions& env_options, VersionSet* versions,
                   InstrumentedMutex* db_mutex,
                   std::atomic<bool>* shutting_down,
                   std::vector<SequenceNumber> existing_snapshots,
                   SequenceNumber earliest_write_conflict_snapshot,
                   SnapshotChecker* snapshot_checker, JobContext* job_context,
                   LogBuffer* log_buffer, Directory* db_directory,
                   Directory* output_file_directory,
                   CompressionType output_compression, Statistics* stats,
                   EventLogger* event_logger, bool measure_io_stats,
                   const bool sync_output_directory, const bool write_manifest,
                   Env::Priority thread_pri)
    : dbname_(dbname),
      cfd_(cfd),
      db_options_(db_options),
      mutable_cf_options_(mutable_cf_options),
      max_memtable_id_(max_memtable_id),
      env_options_(env_options),
      versions_(versions),
      db_mutex_(db_mutex),
      shutting_down_(shutting_down),
      existing_snapshots_(std::move(existing_snapshots)),
      earliest_write_conflict_snapshot_(earliest_write_conflict_snapshot),
      snapshot_checker_(snapshot_checker),
      job_context_(job_context),
      log_buffer_(log_buffer),
      db_directory_(db_directory),
      output_file_directory_(output_file_directory),
      output_compression_(output_compression),
      stats_(stats),
      event_logger_(event_logger),
      measure_io_stats_(measure_io_stats),
      sync_output_directory_(sync_output_directory),
      write_manifest_(write_manifest),
	  flush_(new FlushState()),
      edit_(nullptr),
      base_(nullptr),
      pick_memtable_called(false),
      thread_pri_(thread_pri) {
      //flush_job_stats_ (flush_job_stats) {
  // Update the thread status to indicate flush.
  ReportStartedFlush();
  TEST_SYNC_POINT("FlushJob::FlushJob()");
}

FlushJob::~FlushJob() {
  ThreadStatusUtil::ResetThreadStatus();
}

void FlushJob::ReportStartedFlush() {
  ThreadStatusUtil::SetColumnFamily(cfd_, cfd_->ioptions()->env,
                                    db_options_.enable_thread_tracking);
  ThreadStatusUtil::SetThreadOperation(ThreadStatus::OP_FLUSH);
  ThreadStatusUtil::SetThreadOperationProperty(
      ThreadStatus::COMPACTION_JOB_ID,
      job_context_->job_id);
  IOSTATS_RESET(bytes_written);
}

void FlushJob::ReportFlushInputSize(const autovector<MemTable*>& mems) {
  uint64_t input_size = 0;
  for (auto* mem : mems) {
    input_size += mem->ApproximateMemoryUsage();
  }
  ThreadStatusUtil::IncreaseThreadOperationProperty(
      ThreadStatus::FLUSH_BYTES_MEMTABLES,
      input_size);
}

void FlushJob::RecordFlushIOStats() {
  RecordTick(stats_, FLUSH_WRITE_BYTES, IOSTATS(bytes_written));
  ThreadStatusUtil::IncreaseThreadOperationProperty(
      ThreadStatus::FLUSH_BYTES_WRITTEN, IOSTATS(bytes_written));
  IOSTATS_RESET(bytes_written);
}

void FlushJob::PickMemTable() {
  db_mutex_->AssertHeld();
  assert(!pick_memtable_called);
  pick_memtable_called = true;
  // Save the contents of the earliest memtable as a new Table
  cfd_->imm()->PickMemtablesToFlush(max_memtable_id_, &mems_);
  if (mems_.empty()) {
    return;
  }

  ReportFlushInputSize(mems_);

  // entries mems are (implicitly) sorted in ascending order by their created
  // time. We will use the first memtable's `edit` to keep the meta info for
  // this flush.
  MemTable* m = mems_[0];
  edit_ = m->GetEdits();
  edit_->SetPrevLogNumber(0);
  // SetLogNumber(log_num) indicates logs with number smaller than log_num
  // will no longer be picked up for recovery.
  edit_->SetLogNumber(mems_.back()->GetNextLogNumber());
  edit_->SetColumnFamily(cfd_->GetID());

  // path 0 for level 0 file.
  meta_.fd = FileDescriptor(versions_->NewFileNumber(), 0, 0);
  // path 0 for level 0 file of children nodes
  if (db_options_.allow_column_family_split) {
    ROCKS_LOG_BUFFER(log_buffer_, "Prepare children_metas_ for split-then-flush and its capacity is %d",
                     children_metas_.capacity());
    for (size_t i = 0; i < children_nodes_.size(); i++) {
      FileMetaData meta;
      TableProperties tp;
      meta.fd = FileDescriptor(versions_->NewFileNumber(), 0, 0);
      children_metas_.push_back(meta);
      children_table_properties_.push_back(tp);
      children_edits_.push_back(VersionEdit());
    }
  }
  assert(children_metas_.size() == children_nodes_.size());
  //fprintf(stdout, "children node size : %ld\n", children_nodes_.size() );
  //fprintf(stdout, "children meta size : %ld\n", children_metas_.size() );

  base_ = cfd_->current();
  base_->Ref();  // it is likely that we do not need this reference
}


/*
 *  Made by Kyoungho Koo
 *  for multi-threaded split-then-flush
 */
void FlushJob::Prepare() {
  AutoThreadOperationStageUpdater stage_updater(
      ThreadStatus::STAGE_FLUSH_PREPARE);

  db_mutex_->AssertHeld();
  assert(!pick_memtable_called);
  assert(db_options_.allow_column_family_split);
  pick_memtable_called = true;
  // Save the contents of the earliest memtable as a new Table
  cfd_->imm()->PickMemtablesToFlush(max_memtable_id_, &mems_);
  if (mems_.empty()) {
    return;
  }

  ReportFlushInputSize(mems_);

  // entries mems are (implicitly) sorted in ascending order by their created
  // time. We will use the first memtable's `edit` to keep the meta info for
  // this flush.
  MemTable* m = mems_[0];
  edit_ = m->GetEdits();
  edit_->SetPrevLogNumber(0);
  // SetLogNumber(log_num) indicates logs with number smaller than log_num
  // will no longer be picked up for recovery.
  edit_->SetLogNumber(mems_.back()->GetNextLogNumber());
  edit_->SetColumnFamily(cfd_->GetID());

  int num_boundaries = children_nodes_.size();
  // JH: resize subflush key boundary vectors
  flush_->sub_flush_states.reserve(num_boundaries+1);

  // path 0 for level 0 file.
  meta_.fd = FileDescriptor(versions_->NewFileNumber(), 0, 0);

  //flush_->sub_flush_states.emplace_back(nullptr, nullptr, meta_, nullptr, edit_);
  // path 0 for level 0 file of children nodes
  ROCKS_LOG_BUFFER(log_buffer_, "Prepare children_metas_ for split-then-flush and its capacity is %d",
                     children_metas_.capacity());
  flush_->sub_flush_states.emplace_back(flush_, "", "", meta_, nullptr, 0);

  std::vector<SuperVersionContext>& superversion_contexts = 
      job_context_->superversion_contexts;
  for (int child_idx = 0; child_idx < num_boundaries; child_idx++) {
    FileMetaData sub_meta;
    VersionEdit* sub_edit = new VersionEdit();
		sub_edit->SetPrevLogNumber(0);
		// SetLogNumber(log_num) indicates logs with number smaller than log_num
		// will no longer be picked up for recovery.
		sub_edit->SetLogNumber(mems_.back()->GetNextLogNumber());

    sub_meta.fd = FileDescriptor(versions_->NewFileNumber(), 0, 0);
    std::string start_key = get_lmost_key(children_nodes_[child_idx]);
    std::string end_key = get_rmost_key(children_nodes_[child_idx]);
    sub_edit->SetColumnFamily(children_nodes_[child_idx]->cfd_->GetID());
    flush_->sub_flush_states.emplace_back(flush_, start_key, end_key, sub_meta,
        sub_edit, child_idx+1);
		// JH: prepare superversion_contexts for child nodes
		superversion_contexts.emplace_back(SuperVersionContext(true));  
  }
  /*
  for (int i = 1; i < (int)flush_->sub_flush_states.size(); i++) {
	SubflushState* sub_flush = &flush_->sub_flush_states[i];
    std::cout << "FlushJob::Prepare() check2 sub_flush_start " << i << " " << sub_flush->start << " " << sub_flush->sub_flush_id <<std::endl;
    std::cout << "FlushJob::Prepare() check2 sub_flush_end " << i << " "<< sub_flush->end <<std::endl;
    //std::cout << "FlushJob::Prepare() check child " << flush_->sub_flush_states[i].sub_flush_id <<std::endl;
  }
  */


  base_ = cfd_->current();
  //std::cout << "FlushJob::Prepare() base_->Ref()" << std::endl;
  base_->Ref();  // it is likely that we do not need this reference
}

void FlushJob::SetChildrenNodes() {
  db_mutex_->AssertHeld();
  children_nodes_ = cfd_->GetChildrenNodes();
  children_metas_.reserve(children_nodes_.size());
}


Status FlushJob::Run(LogsWithPrepTracker* prep_tracker,
                     FileMetaData* file_meta) {
  TEST_SYNC_POINT("FlushJob::Start");
  db_mutex_->AssertHeld();
  assert(pick_memtable_called);
  AutoThreadOperationStageUpdater stage_run(
      ThreadStatus::STAGE_FLUSH_RUN);
  //std::cout << "FlushJob::Run() start" << std::endl;
  if (mems_.empty()) {
    ROCKS_LOG_BUFFER(log_buffer_, "[%s] Nothing in memtable to flush",
                     cfd_->GetName().c_str());
    return Status::OK();
  }

  // I/O measurement variables
  PerfLevel prev_perf_level = PerfLevel::kEnableTime;
  uint64_t prev_write_nanos = 0;
  uint64_t prev_fsync_nanos = 0;
  uint64_t prev_range_sync_nanos = 0;
  uint64_t prev_prepare_write_nanos = 0;
  uint64_t prev_cpu_write_nanos = 0;
  uint64_t prev_cpu_read_nanos = 0;
  if (measure_io_stats_) {
    prev_perf_level = GetPerfLevel();
    SetPerfLevel(PerfLevel::kEnableTime);
    prev_write_nanos = IOSTATS(write_nanos);
    prev_fsync_nanos = IOSTATS(fsync_nanos);
    prev_range_sync_nanos = IOSTATS(range_sync_nanos);
    prev_prepare_write_nanos = IOSTATS(prepare_write_nanos);
    prev_cpu_write_nanos = IOSTATS(cpu_write_nanos);
    prev_cpu_read_nanos = IOSTATS(cpu_read_nanos);
  }


  // This will release and re-acquire the mutex.
  Status status;
  if (db_options_.allow_column_family_split) {
    ROCKS_LOG_INFO(
        db_options_.info_log,
        "Flushing [%s]",
        cfd_->GetName().c_str());

		const uint64_t start_micros = db_options_.env->NowMicros();
    const size_t num_threads = flush_->sub_flush_states.size();
    assert(num_threads > 0);
    // Launch a thread for each of subcompactions 1...num_threads-1
    std::vector<port::Thread> thread_pool;
    thread_pool.reserve(num_threads - 1);
    for (size_t i = 1; i < num_threads; i++) {
      thread_pool.emplace_back(&FlushJob::ProcessKeyValueFlush, this,
                        &flush_->sub_flush_states[i]);
    }
    

    // Always schedule the first subflush (whether or not there are also
    // others) in the current thread to be efficient with resources
    ProcessKeyValueFlush(&flush_->sub_flush_states[0]);

    
    for (auto& thread : thread_pool) {
      thread.join();
    }

    // Check if any thread encountered an error during execution
    for (const auto& state : flush_->sub_flush_states) {
      if (!state.status.ok()) {
        status = state.status;
        break;
      }
    }
	 
    if (status.ok() && output_file_directory_) {
      status = output_file_directory_->Fsync();
    }
    base_->Unref();
	//std::cout << "FlushJob::Run() Split-then-flush done" << std::endl;
    //s = WriteLevel0Tables();  
		InternalStats::CompactionStats stats(CompactionReason::kFlush, 1);
		stats.micros = db_options_.env->NowMicros() - start_micros;
		RecordTimeToHistogram(stats_, FLUSH_TIME, stats.micros);
  } else {
    status = WriteLevel0Table();  
  }
  
  /*if (!s.ok()) {
    fprintf(stdout, "WriteLevel0 fail\n");
  }
  if (shutting_down_->load(std::memory_order_acquire)) {
    fprintf(stdout, "shutdown\n");
  }
  if (cfd_->IsDropped()) {
    fprintf(stdout, "dropped\n");
  }*/


  if (status.ok() &&
      (shutting_down_->load(std::memory_order_acquire) || cfd_->IsDropped())) {
    //fprintf(stdout, "WriteLevel0\n");
    status = Status::ShutdownInProgress(
        "Database shutdown or Column family drop during flush");
  }

  if (!status.ok()) {
    cfd_->imm()->RollbackMemtableFlush(mems_, meta_.fd.GetNumber());
    //fprintf(stdout, "Rollback\n");
  } else if (write_manifest_) {
    TEST_SYNC_POINT("FlushJob::InstallResults");
    if (db_options_.allow_column_family_split) {
      // JH: Prepare input args to apply to MANIFEST

      // First, estimate num_entries to apply.
      uint32_t num_entries = 0;
      if (table_properties_.num_entries != 0) {
        num_entries ++;
      }
      for (size_t i = 1; i < flush_->sub_flush_states.size(); i++) {
        SubflushState* sub_flush = &flush_->sub_flush_states[i];
        if (sub_flush->sub_table_properties.num_entries != 0) {
          num_entries ++;
        } 
      }

      //std::cout << "FlushJob::Run() autovector" << std::endl;
      autovector<ColumnFamilyData*> tmp_cfds;
      autovector<const MutableCFOptions*> mutable_cf_options_list;
      autovector<FileMetaData*> tmp_file_meta;
      autovector<autovector<VersionEdit*>> edit_lists;
      autovector<VersionEdit*> edit_list[num_entries];
      size_t edit_idx = 0;

      //std::cout << "FlushJob::Run() table_properties" << std::endl;
      // JH: Scanning table properties and push the infos.
      if (table_properties_.num_entries != 0) {
        tmp_cfds.emplace_back(cfd_);
        mutable_cf_options_list.emplace_back(&mutable_cf_options_);
        tmp_file_meta.emplace_back(&meta_); 

        edit_list[edit_idx].emplace_back(mems_[0]->GetEdits());
        edit_lists.emplace_back(edit_list[edit_idx]);
        edit_idx ++;
      }

      //std::cout << "FlushJob::Run() sub table_properties" << std::endl;
      for (size_t i = 1; i < flush_->sub_flush_states.size(); i++) {
        SubflushState* sub_flush = &flush_->sub_flush_states[i];
        ColumnFamilyData *sub_cfd = children_nodes_[i-1]->cfd_;

        if (sub_flush->sub_table_properties.num_entries != 0) {
          //std::cout << "FlushJob::Run() loop 2" << std::endl;
          tmp_cfds.emplace_back(sub_cfd);
          //std::cout << "FlushJob::Run() children_nodes_[i-1]-> cfd_ " << children_nodes_[i-1]->cfd_->GetName() << std::endl;
          mutable_cf_options_list.emplace_back(&mutable_cf_options_);
          //std::cout << "FlushJob::Run() loop 4" << std::endl;
          tmp_file_meta.emplace_back(&sub_flush->sub_meta);
          //std::cout << "FlushJob::Run() loop 5" << std::endl;
          //autovector<VersionEdit*> edits;
          edit_list[edit_idx].emplace_back(sub_flush->sub_edit);
          edit_lists.emplace_back(edit_list[edit_idx]);
          //std::cout << "FlushJob::Run() sub table_properties" << std::endl;
          edit_idx ++;
        } 
      }

      assert (edit_idx == num_entries);

      // JH: Replace immutable memtable with multiple Tables of corresponding
      // column families.
      // This function involves LogAndApply().

      /*for(auto es: edit_lists) {
        for(auto e: es) {
          fprintf(stdout, "%s\n" , e->DebugString().c_str());
        } 
      }*/

      status = cfd_->imm()->InstallMemtableSplitThenFlushResults(
        edit_lists, cfd_, tmp_cfds, mutable_cf_options_list, mems_, versions_,
        db_mutex_, tmp_file_meta, &job_context_->memtables_to_free,
        db_directory_, log_buffer_);
    } else {
      // Replace immutable memtable with the generated Table
      status = cfd_->imm()->TryInstallMemtableFlushResults(
          cfd_, mutable_cf_options_, mems_, prep_tracker, versions_, db_mutex_,
          meta_.fd.GetNumber(), &job_context_->memtables_to_free, db_directory_,
          log_buffer_);  
    }
    
  }
  //fprintf(stdout, "Inside running flush : %" PRIu64 "\n", meta_.fd.GetNumber());
  if (status.ok() && file_meta != nullptr) {
    //fprintf(stdout, "WriteManifest\n");
    *file_meta = meta_;
  }
  //fprintf(stdout, "Inside running flush(2) : %" PRIu64 "\n", file_meta->fd.GetNumber());
  RecordFlushIOStats();

  auto stream = event_logger_->LogToBuffer(log_buffer_);
  if (db_options_.allow_column_family_split) {
    stream << "job" << job_context_->job_id << "event"
           << "flush_finished";
    stream << "output_compression"
           << CompressionTypeToString(output_compression_);
    stream << "lsm_state";
    stream.StartArray();
    auto vstorage = cfd_->current()->storage_info();
    stream << cfd_->GetName();
    for (int level = 0; level < vstorage->num_levels(); ++level) {
      stream << vstorage->NumLevelFiles(level);
    }
    // also, include children cfds
    for (auto child: children_nodes_) {
      ColumnFamilyData* child_cfd = child->cfd_;
      stream << child_cfd->GetName();
      auto child_vstorage = child_cfd->current()->storage_info();
      for (int level = 0; level < child_vstorage->num_levels(); ++level) {
        stream << child_vstorage->NumLevelFiles(level);
      }
    }
    stream.EndArray();
    stream << "cfd" << cfd_->GetName();  
    stream << "immutable_memtables" << cfd_->imm()->NumNotFlushed();  
    // also, include children cfds
    for (auto child: children_nodes_) {
      ColumnFamilyData* child_cfd = child->cfd_;
      stream << "cfd" << child_cfd->GetName();  
      stream << "immutable_memtables" << child_cfd->imm()->NumNotFlushed();  
    }
  } else {
    stream << "job" << job_context_->job_id << "event"
           << "flush_finished";
    stream << "output_compression"
           << CompressionTypeToString(output_compression_);
    stream << "lsm_state";
    stream.StartArray();
    auto vstorage = cfd_->current()->storage_info();
    for (int level = 0; level < vstorage->num_levels(); ++level) {
      stream << vstorage->NumLevelFiles(level);
    }
    stream.EndArray();
    stream << "immutable_memtables" << cfd_->imm()->NumNotFlushed();  
  }

  if (measure_io_stats_) {
    if (prev_perf_level != PerfLevel::kEnableTime) {
      SetPerfLevel(prev_perf_level);
    }
    stream << "file_write_nanos" << (IOSTATS(write_nanos) - prev_write_nanos);
    stream << "file_range_sync_nanos"
           << (IOSTATS(range_sync_nanos) - prev_range_sync_nanos);
    stream << "file_fsync_nanos" << (IOSTATS(fsync_nanos) - prev_fsync_nanos);
    stream << "file_prepare_write_nanos"
           << (IOSTATS(prepare_write_nanos) - prev_prepare_write_nanos);
    stream << "file_cpu_write_nanos"
           << (IOSTATS(cpu_write_nanos) - prev_cpu_write_nanos);
    stream << "file_cpu_read_nanos"
           << (IOSTATS(cpu_read_nanos) - prev_cpu_read_nanos);
  }

  return status;
}

void FlushJob::Cancel() {
  db_mutex_->AssertHeld();
  assert(base_ != nullptr);
  base_->Unref();
}

Status FlushJob::WriteLevel0Table() {
  AutoThreadOperationStageUpdater stage_updater(
      ThreadStatus::STAGE_FLUSH_WRITE_L0);
  db_mutex_->AssertHeld();
  const uint64_t start_micros = db_options_.env->NowMicros();
  const uint64_t start_cpu_micros = db_options_.env->NowCPUNanos() / 1000;
  ROCKS_LOG_INFO(
          db_options_.info_log,"FlushJob:normal-flush");
  Status s;
  {
    auto write_hint = cfd_->CalculateSSTWriteHint(0);
    db_mutex_->Unlock();
    if (log_buffer_) {
      log_buffer_->FlushBufferToLog();
    }
    // memtables and range_del_iters store internal iterators over each data
    // memtable and its associated range deletion memtable, respectively, at
    // corresponding indexes.
    std::vector<InternalIterator*> memtables;
    std::vector<std::unique_ptr<FragmentedRangeTombstoneIterator>>
        range_del_iters;
    ReadOptions ro;
    ro.total_order_seek = true;
    Arena arena;
    uint64_t total_num_entries = 0, total_num_deletes = 0;
    uint64_t total_data_size = 0;
    size_t total_memory_usage = 0;
    for (MemTable* m : mems_) {
      ROCKS_LOG_INFO(
          db_options_.info_log,
          "[%s] [JOB %d] Flushing memtable with next log file: %" PRIu64 "\n",
          cfd_->GetName().c_str(), job_context_->job_id, m->GetNextLogNumber());
      memtables.push_back(m->NewIterator(ro, &arena));
      auto* range_del_iter =
          m->NewRangeTombstoneIterator(ro, kMaxSequenceNumber);
      if (range_del_iter != nullptr) {
        range_del_iters.emplace_back(range_del_iter);
      }
      total_num_entries += m->num_entries();
      total_num_deletes += m->num_deletes();
      total_data_size += m->get_data_size();
      total_memory_usage += m->ApproximateMemoryUsage();
    }

    event_logger_->Log() << "job" << job_context_->job_id << "event"
                         << "flush_started"
                         << "num_memtables" << mems_.size() << "num_entries"
                         << total_num_entries << "num_deletes"
                         << total_num_deletes << "total_data_size"
                         << total_data_size << "memory_usage"
                         << total_memory_usage << "flush_reason"
                         << GetFlushReasonString(cfd_->GetFlushReason());

    {
      ScopedArenaIterator iter(
          NewMergingIterator(&cfd_->internal_comparator(), &memtables[0],
                             static_cast<int>(memtables.size()), &arena));
      ROCKS_LOG_INFO(db_options_.info_log,
                     "[%s] [JOB %d] Level-0 flush table #%" PRIu64 ": started",
                     cfd_->GetName().c_str(), job_context_->job_id,
                     meta_.fd.GetNumber());

      TEST_SYNC_POINT_CALLBACK("FlushJob::WriteLevel0Table:output_compression",
                               &output_compression_);
      int64_t _current_time = 0;
      auto status = db_options_.env->GetCurrentTime(&_current_time);
      // Safe to proceed even if GetCurrentTime fails. So, log and proceed.
      if (!status.ok()) {
        ROCKS_LOG_WARN(
            db_options_.info_log,
            "Failed to get current time to populate creation_time property. "
            "Status: %s",
            status.ToString().c_str());
      }
      const uint64_t current_time = static_cast<uint64_t>(_current_time);

      uint64_t oldest_key_time =
          mems_.front()->ApproximateOldestKeyTime();

      s = BuildTable(
          dbname_, db_options_.env, *cfd_->ioptions(), mutable_cf_options_,
          env_options_, cfd_->table_cache(), iter.get(),
          std::move(range_del_iters), &meta_, cfd_->internal_comparator(),
          cfd_->int_tbl_prop_collector_factories(), cfd_->GetID(),
          cfd_->GetName(), existing_snapshots_,
          earliest_write_conflict_snapshot_, snapshot_checker_,
          output_compression_, mutable_cf_options_.sample_for_compression,
          cfd_->ioptions()->compression_opts,
          mutable_cf_options_.paranoid_file_checks, cfd_->internal_stats(),
          TableFileCreationReason::kFlush, event_logger_, job_context_->job_id,
          Env::IO_HIGH, &table_properties_, 0 /* level */, current_time,
          oldest_key_time, write_hint);
      LogFlush(db_options_.info_log);
    }
    ROCKS_LOG_INFO(db_options_.info_log,
                   "[%s] [JOB %d] Level-0 flush table #%" PRIu64 ": %" PRIu64
                   " bytes %s"
                   "%s",
                   cfd_->GetName().c_str(), job_context_->job_id,
                   meta_.fd.GetNumber(), meta_.fd.GetFileSize(),
                   s.ToString().c_str(),
                   meta_.marked_for_compaction ? " (needs compaction)" : "");

    if (s.ok() && output_file_directory_ != nullptr && sync_output_directory_) {
      s = output_file_directory_->Fsync();
    }
    TEST_SYNC_POINT("FlushJob::WriteLevel0Table");
    db_mutex_->Lock();
  }
  base_->Unref();

  // Note that if file_size is zero, the file has been deleted and
  // should not be added to the manifest.
  if (s.ok() && meta_.fd.GetFileSize() > 0) {
    // if we have more than 1 background thread, then we cannot
    // insert files directly into higher levels because some other
    // threads could be concurrently producing compacted files for
    // that key range.
    // Add file to L0
    edit_->AddFile(0 /* level */, meta_.fd.GetNumber(), meta_.fd.GetPathId(),
                   meta_.fd.GetFileSize(), meta_.smallest, meta_.largest,
                   meta_.fd.smallest_seqno, meta_.fd.largest_seqno,
                   meta_.marked_for_compaction);
  }

  // Note that here we treat flush as level 0 compaction in internal stats
  InternalStats::CompactionStats stats(CompactionReason::kFlush, 1);
  stats.micros = db_options_.env->NowMicros() - start_micros;
  stats.cpu_micros = db_options_.env->NowCPUNanos() / 1000 - start_cpu_micros;
  stats.bytes_written = meta_.fd.GetFileSize();
  RecordTimeToHistogram(stats_, FLUSH_TIME, stats.micros);
  cfd_->internal_stats()->AddCompactionStats(0 /* level */, thread_pri_, stats);
  cfd_->internal_stats()->AddCFStats(InternalStats::BYTES_FLUSHED,
                                     meta_.fd.GetFileSize());
  RecordFlushIOStats();
  return s;
}

Status FlushJob::WriteLevel0Tables() {
  AutoThreadOperationStageUpdater stage_updater(
      ThreadStatus::STAGE_FLUSH_WRITE_L0);
  db_mutex_->AssertHeld();
  const uint64_t start_micros = db_options_.env->NowMicros();
  const uint64_t start_cpu_micros = db_options_.env->NowCPUNanos() / 1000;
  ROCKS_LOG_INFO(
          db_options_.info_log,"FlushJob:split-then-flush");
  Status s;
  {
    auto write_hint = cfd_->CalculateSSTWriteHint(0);
    db_mutex_->Unlock();
    if (log_buffer_) {
      log_buffer_->FlushBufferToLog();
    }
    // memtables and range_del_iters store internal iterators over each data
    // memtable and its associated range deletion memtable, respectively, at
    // corresponding indexes.
    std::vector<InternalIterator*> memtables;
    std::vector<std::unique_ptr<FragmentedRangeTombstoneIterator>>
        range_del_iters;
    ReadOptions ro;
    ro.total_order_seek = true;
    Arena arena;
    uint64_t total_num_entries = 0, total_num_deletes = 0;
    uint64_t total_data_size = 0;
    size_t total_memory_usage = 0;
    for (MemTable* m : mems_) {
      ROCKS_LOG_INFO(
          db_options_.info_log,
          "[%s] [JOB %d] Flushing memtable with next log file: %" PRIu64 "\n",
          cfd_->GetName().c_str(), job_context_->job_id, m->GetNextLogNumber());
      memtables.push_back(m->NewIterator(ro, &arena));
      auto* range_del_iter =
          m->NewRangeTombstoneIterator(ro, kMaxSequenceNumber);
      if (range_del_iter != nullptr) {
        range_del_iters.emplace_back(range_del_iter);
      }
      total_num_entries += m->num_entries();
      total_num_deletes += m->num_deletes();
      total_data_size += m->get_data_size();
      total_memory_usage += m->ApproximateMemoryUsage();
    }

    event_logger_->Log() << "job" << job_context_->job_id << "event"
                         << "flush_started"
                         << "num_memtables" << mems_.size() << "num_entries"
                         << total_num_entries << "num_deletes"
                         << total_num_deletes << "total_data_size"
                         << total_data_size << "memory_usage"
                         << total_memory_usage << "flush_reason"
                         << GetFlushReasonString(cfd_->GetFlushReason());

    {
      ScopedArenaIterator iter(
          NewMergingIterator(&cfd_->internal_comparator(), &memtables[0],
                             static_cast<int>(memtables.size()), &arena));
      ROCKS_LOG_INFO(db_options_.info_log,
                     "[%s] [JOB %d] Level-0 flush table #%" PRIu64 ": started",
                     cfd_->GetName().c_str(), job_context_->job_id,
                     meta_.fd.GetNumber());

      TEST_SYNC_POINT_CALLBACK("FlushJob::WriteLevel0Tables:output_compression",
                               &output_compression_);
      int64_t _current_time = 0;
      auto status = db_options_.env->GetCurrentTime(&_current_time);
      // Safe to proceed even if GetCurrentTime fails. So, log and proceed.
      if (!status.ok()) {
        ROCKS_LOG_WARN(
            db_options_.info_log,
            "Failed to get current time to populate creation_time property. "
            "Status: %s",
            status.ToString().c_str());
      }
      const uint64_t current_time = static_cast<uint64_t>(_current_time);

      uint64_t oldest_key_time =
          mems_.front()->ApproximateOldestKeyTime();
      // JH: We need to build multiple tables from single iter
      // which is involved from cfd and its children nodes
      // Therefore, we call other function, BuildTables() to achive this.
      if (children_nodes_.empty()) {
        s = BuildTable(
            dbname_, db_options_.env, *cfd_->ioptions(), mutable_cf_options_,
            env_options_, cfd_->table_cache(), iter.get(),
            std::move(range_del_iters), &meta_, cfd_->internal_comparator(),
            cfd_->int_tbl_prop_collector_factories(), cfd_->GetID(),
            cfd_->GetName(), existing_snapshots_,
            earliest_write_conflict_snapshot_, snapshot_checker_,
            output_compression_, mutable_cf_options_.sample_for_compression,
            cfd_->ioptions()->compression_opts,
            mutable_cf_options_.paranoid_file_checks, cfd_->internal_stats(),
            TableFileCreationReason::kFlush, event_logger_, job_context_->job_id,
            Env::IO_HIGH, &table_properties_, 0 /* level */, current_time,
            oldest_key_time, write_hint
            );
      } else {
        s = BuildTables(
            dbname_, db_options_.env, *cfd_->ioptions(), mutable_cf_options_,
            env_options_, cfd_->table_cache(), iter.get(),
            std::move(range_del_iters), &meta_,
            cfd_->internal_comparator(),
            cfd_->int_tbl_prop_collector_factories(), cfd_->GetID(),
            cfd_->GetName(), existing_snapshots_,
            earliest_write_conflict_snapshot_, snapshot_checker_,
            output_compression_, mutable_cf_options_.sample_for_compression,
            cfd_->ioptions()->compression_opts,
            mutable_cf_options_.paranoid_file_checks, cfd_->internal_stats(),
            TableFileCreationReason::kFlush,
            children_metas_,
            children_nodes_,
            children_table_properties_,
            event_logger_, job_context_->job_id,
            Env::IO_HIGH, &table_properties_,
            0 /* level */, current_time,
            oldest_key_time, write_hint);  
      }
      
      LogFlush(db_options_.info_log);
    }

    ROCKS_LOG_INFO(db_options_.info_log,
        "[%s] [JOB %d] Level-0 flush table #%" PRIu64 ": %" PRIu64
        " bytes %s"
        "%s",
        cfd_->GetName().c_str(), job_context_->job_id,
        meta_.fd.GetNumber(), meta_.fd.GetFileSize(),
        s.ToString().c_str(),
        meta_.marked_for_compaction ? " (needs compaction)" : "");
    if (db_options_.allow_column_family_split) {
      for (size_t i = 0; i < children_nodes_.size(); i++) {
        ROCKS_LOG_INFO(db_options_.info_log,
          "[%s] [JOB %d] Level-0 flush table #%" PRIu64 ": %" PRIu64
          " bytes %s"
          "%s",
          children_nodes_[i]->cfd_->GetName().c_str(), job_context_->job_id,
          children_metas_[i].fd.GetNumber(), children_metas_[i].fd.GetFileSize(),
          s.ToString().c_str(),
          children_metas_[i].marked_for_compaction ? " (needs compaction)" : "");
      }
    }

    if (s.ok() && output_file_directory_ != nullptr && sync_output_directory_) {
      s = output_file_directory_->Fsync();
    }
    TEST_SYNC_POINT("FlushJob::WriteLevel0Table");
    db_mutex_->Lock();
  }
  base_->Unref();

  // Note that if file_size is zero, the file has been deleted and
  // should not be added to the manifest.
  if (s.ok() && meta_.fd.GetFileSize() > 0) {
    // if we have more than 1 background thread, then we cannot
    // insert files directly into higher levels because some other
    // threads could be concurrently producing compacted files for
    // that key range.
    // Add file to L0
    edit_->AddFile(0 /* level */, meta_.fd.GetNumber(), meta_.fd.GetPathId(),
                   meta_.fd.GetFileSize(), meta_.smallest, meta_.largest,
                   meta_.fd.smallest_seqno, meta_.fd.largest_seqno,
                   meta_.marked_for_compaction);
  }
  // we also add files to version edits for children nodes
  for (size_t i = 0; i < children_metas_.size(); i++) {
    if (s.ok() && children_metas_[i].fd.GetFileSize() > 0) {
      // if we have more than 1 background thread, then we cannot
      // insert files directly into higher levels because some other
      // threads could be concurrently producing compacted files for
      // that key range.
      // Add file to L0
      children_edits_[i].AddFile(0 /* level */, children_metas_[i].fd.GetNumber(),
                                  children_metas_[i].fd.GetPathId(),
                                  children_metas_[i].fd.GetFileSize(),
                                  children_metas_[i].smallest,
                                  children_metas_[i].largest,
                                  children_metas_[i].fd.smallest_seqno,
                                  children_metas_[i].fd.largest_seqno,
                                  children_metas_[i].marked_for_compaction);
      //fprintf(stdout, "[c]%s\n", children_edits_[i].DebugString().c_str());
    }
  }
  

  // Note that here we treat flush as level 0 compaction in internal stats
  InternalStats::CompactionStats stats(CompactionReason::kFlush, 1);
  stats.micros = db_options_.env->NowMicros() - start_micros;
  stats.cpu_micros = db_options_.env->NowCPUNanos() / 1000 - start_cpu_micros;
  stats.bytes_written = meta_.fd.GetFileSize();
  RecordTimeToHistogram(stats_, FLUSH_TIME, stats.micros);
  cfd_->internal_stats()->AddCompactionStats(0 /* level */, thread_pri_, stats);
  cfd_->internal_stats()->AddCFStats(InternalStats::BYTES_FLUSHED,
                                     meta_.fd.GetFileSize());
  RecordFlushIOStats();
  return s;
}

/*
 * Made by Kyoungho Koo
 * : for multi-threaded split-then-flush
*/
void FlushJob::ProcessKeyValueFlush(SubflushState* sub_flush) {
  assert(sub_flush != nullptr);

  //uint64_t prev_cpu_micros = env_->NowCPUNanos() / 1000;

  // Although the v2 aggregator is what the level iterator(s) know about,
  // the AddTombstones calls will be propagated down to the v1 aggregator.
  //std::unique_ptr<InternalIterator> input(versions_->MakeInputIterator(
   //   sub_compact->compaction, &range_del_agg, env_optiosn_for_read_));

  AutoThreadOperationStageUpdater stage_updater(
      ThreadStatus::STAGE_FLUSH_PROCESS_KV);


  Status status;


  auto write_hint = cfd_->CalculateSSTWriteHint(0);
  //db_mutex_->Unlock(); Maybe invokey by Run method

  ReadOptions ro;
  ro.total_order_seek = true;



  int64_t _current_time = 0;
  status = db_options_.env->GetCurrentTime(&_current_time);
  // Safe to proceed even if GetCurrentTime fails. So, log and proceed.
  if (!status.ok()) {
	ROCKS_LOG_WARN(
		db_options_.info_log,
		"Failed to get current time to populate creation_time property. "
		"Status: %s",
		status.ToString().c_str());
  }

  const uint64_t current_time = static_cast<uint64_t>(_current_time);

  uint64_t oldest_key_time =
	  mems_.front()->ApproximateOldestKeyTime();

  if (sub_flush->sub_flush_id == 0) {
		size_t children_size = children_nodes_.size();

		std::vector<std::vector<InternalIterator*>> memtabless(children_size +1);

		std::vector<Arena> arenas(children_size + 1);
		std::vector<ScopedArenaIterator *> iters;
		std::vector<std::unique_ptr<FragmentedRangeTombstoneIterator>>
					range_del_iters;


		for (size_t i = 0; i < children_size + 1; i++) {
			for (MemTable* m : mems_) {
				InternalIterator * iiter = m->NewIterator(ro, &arenas[i]);
				memtabless[i].push_back(iiter);
				auto* range_del_iter =
				m->NewRangeTombstoneIterator(ro, kMaxSequenceNumber);
				if (range_del_iter != nullptr) {
					range_del_iters.emplace_back(range_del_iter);
				}
			}
			ScopedArenaIterator* iter = new ScopedArenaIterator(NewMergingIterator(&cfd_->internal_comparator(), &memtabless[i][0],
															 static_cast<int>(memtabless[i].size()), &arenas[i]));
			iters.push_back(iter);
		}

    status = BuildParentTable(
            dbname_, db_options_.env, *cfd_->ioptions(), mutable_cf_options_,
            env_options_, cfd_->table_cache(), iters,
            std::move(range_del_iters), &meta_,
            cfd_->internal_comparator(),
            cfd_->int_tbl_prop_collector_factories(), cfd_->GetID(),
            cfd_->GetName(), existing_snapshots_,
            earliest_write_conflict_snapshot_, snapshot_checker_,
            output_compression_, mutable_cf_options_.sample_for_compression,
            cfd_->ioptions()->compression_opts,
            mutable_cf_options_.paranoid_file_checks, cfd_->internal_stats(),
            TableFileCreationReason::kFlush,
            children_nodes_,
            event_logger_, job_context_->job_id,
            Env::IO_HIGH, &table_properties_,
            0 /* level */, current_time,
            oldest_key_time, write_hint,
						sub_flush->flush_state->GetSubflushStarts(),
						sub_flush->flush_state->GetSubflushEnds());  
  } else {
		std::vector<InternalIterator*> memtables;
		std::vector<std::unique_ptr<FragmentedRangeTombstoneIterator>>
					range_del_iters;
		Arena arena;

		for (MemTable* m : mems_) {
			memtables.push_back(m->NewIterator(ro, &arena));
			auto* range_del_iter =
			m->NewRangeTombstoneIterator(ro, kMaxSequenceNumber);
			if (range_del_iter != nullptr) {
				range_del_iters.emplace_back(range_del_iter);
			}
		}
    ColumnFamilyData* sub_cfd =  children_nodes_[sub_flush->sub_flush_id - 1]->cfd_;
    ScopedArenaIterator sub_iter(
          NewMergingIterator(&sub_cfd->internal_comparator(), &memtables[0],
                             static_cast<int>(memtables.size()), &arena));

//    std::cout << "FlushJob::ProcessKeyValueFlush() ->BuildsubTable() " << sub_flush->sub_flush_id<< std::endl;
    //std::cout << "FlushJob::ProcessKeyValueFlush " << sub_flush->sub_flush_id 
//		<< " sub_iter valid " << sub_iter.get()->Valid()<< std::endl;
		status = BuildsubTable(
					dbname_, db_options_.env, *sub_cfd->ioptions(), mutable_cf_options_,
					env_options_, sub_cfd->table_cache(), sub_iter.get(),
					std::move(range_del_iters), &sub_flush->sub_meta, sub_cfd->internal_comparator(),
					sub_cfd->int_tbl_prop_collector_factories(), sub_cfd->GetID(),
					sub_cfd->GetName(), existing_snapshots_,
					earliest_write_conflict_snapshot_, snapshot_checker_,
					output_compression_, mutable_cf_options_.sample_for_compression,
					sub_cfd->ioptions()->compression_opts,
					mutable_cf_options_.paranoid_file_checks, sub_cfd->internal_stats(),
					TableFileCreationReason::kFlush, event_logger_, job_context_->job_id,
					Env::IO_HIGH, &sub_flush->sub_table_properties, 0 , current_time, oldest_key_time, write_hint,
					sub_flush->start, sub_flush->end, sub_flush->sub_flush_id);
    //std::cout << "FlushJob::ProcessKeyValueFlush() " << sub_flush->sub_flush_id  << " <-BuildsubTable()"<< std::endl;
	
//    std::cout << "FlushJob::ProcessKeyValueFlush() <-BuildsubTable() "<< sub_flush->sub_flush_id << std::endl;
  }
  LogFlush(db_options_.info_log);
//    std::cout << "FlushJob::ProcessKeyValueFlush() <- LogFlush()"<< std::endl;

  if (status.ok() && output_file_directory_ != nullptr && sync_output_directory_) {
    status = output_file_directory_->Fsync();
  }
  //db_mutex_->Lock(); Maybe invokey by Run method



  // Note that if file_size is zero, the file has been deleted and
  // should not be added to the manifest.
  VersionEdit* edit = sub_flush->sub_flush_id ? sub_flush->sub_edit: edit_;
  FileMetaData* meta = sub_flush->sub_flush_id ? &sub_flush->sub_meta: &meta_;
 // std::cout << "FlushJob::ProcessKeyValueFlush() <- Fsync() " << sub_flush->sub_flush_id << std::endl;


  assert(edit);
  if (status.ok() && meta->fd.GetFileSize() > 0) {
    // if we have more than 1 background thread, then we cannot
    // insert files directly into higher levels because some other
    // threads could be concurrently producing compacted files for
    // that key range.
    // Add file to L0
    edit->AddFile(0 , meta->fd.GetNumber(), meta->fd.GetPathId(),
        meta->fd.GetFileSize(), meta->smallest, meta->largest,
        meta->fd.smallest_seqno, meta->fd.largest_seqno,
        meta->marked_for_compaction);
  }

  sub_flush->status = status;
}
}  // namespace rocksdb
