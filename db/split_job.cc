//  Copyright (c) 2011-present, Facebook, Inc.  All rights reserved.
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).
//
// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#include "db/split_job.h"

#ifndef __STDC_FORMAT_MACROS
#define __STDC_FORMAT_MACROS
#endif

#include <inttypes.h>
#include <algorithm>
#include <functional>
#include <list>
#include <memory>
#include <random>
#include <set>
#include <thread>
#include <utility>
#include <vector>
#include <fstream>

#include "db/builder.h"
#include "db/db_impl.h"
#include "db/db_iter.h"
#include "db/dbformat.h"
#include "db/error_handler.h"
#include "db/event_helpers.h"
#include "db/log_reader.h"
#include "db/log_writer.h"
#include "db/memtable.h"
#include "db/memtable_list.h"
#include "db/merge_context.h"
#include "db/merge_helper.h"
#include "db/range_del_aggregator.h"
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
#include "util/coding.h"
#include "util/file_reader_writer.h"
#include "util/filename.h"
#include "util/log_buffer.h"
#include "util/logging.h"
#include "util/mutexlock.h"
#include "util/random.h"
#include "util/sst_file_manager_impl.h"
#include "util/stop_watch.h"
#include "util/string_util.h"
#include "util/sync_point.h"

namespace rocksdb {

const char* GetCompactionReasonString(CompactionReason compaction_reason) {
  switch (compaction_reason) {
    case CompactionReason::kUnknown:
      return "Unknown";
    case CompactionReason::kLevelL0FilesNum:
      return "LevelL0FilesNum";
    case CompactionReason::kLevelMaxLevelSize:
      return "LevelMaxLevelSize";
    case CompactionReason::kUniversalSizeAmplification:
      return "UniversalSizeAmplification";
    case CompactionReason::kUniversalSizeRatio:
      return "UniversalSizeRatio";
    case CompactionReason::kUniversalSortedRunNum:
      return "UniversalSortedRunNum";
    case CompactionReason::kFIFOMaxSize:
      return "FIFOMaxSize";
    case CompactionReason::kFIFOReduceNumFiles:
      return "FIFOReduceNumFiles";
    case CompactionReason::kFIFOTtl:
      return "FIFOTtl";
    case CompactionReason::kSplit:
      return "Split";
    case CompactionReason::kManualSplit:
      return "ManualSplit";
    case CompactionReason::kFilesMarkedForSplit:
      return "FilesMarkedForSplit";
    case CompactionReason::kBottommostFiles:
      return "BottommostFiles";
    case CompactionReason::kTtl:
      return "Ttl";
    case CompactionReason::kFlush:
      return "Flush";
    case CompactionReason::kExternalSstIngestion:
      return "ExternalSstIngestion";
    case CompactionReason::kNumOfReasons:
      // fall through
    default:
      assert(false);
      return "Invalid";
  }
}

// Maintains state for each subsplit; this is the technical dept.
struct SplitJob::SubsplitState {
  const Compaction* compaction;
  std::vector<ColumnFamilyData*> children_cfds;
  Slice median_key;
  size_t child_idx;
  std::unique_ptr<CompactionIterator> c_iter;

  // The boundaries of the key-range this compaction is interested in. No two
  // subcompactions may have overlapping key-ranges.
  // 'start' is inclusive, 'end' is exclusive, and nullptr means unbounded
  Slice *start, *end;

  // The return status of this subcompaction
  Status status;

  // Files produced by this subcompaction
  struct Output {
    FileMetaData meta;
    bool finished;
    size_t child_idx;
    std::shared_ptr<const TableProperties> table_properties;
  };

  // State kept for output being generated
  std::vector<Output> parent_outputs;
  std::vector<Output> child_outputs;
  std::unique_ptr<WritableFileWriter> outfile;
  std::unique_ptr<TableBuilder> parent_builder;
  std::unique_ptr<TableBuilder> child_builder;
  Output* child_current_output() {
    if (child_outputs.empty()) {
      // This subcompaction's output could be empty if compaction was aborted
      // before this subcompaction had a chance to generate any output files.
      // When subcompactions are executed sequentially this is more likely and
      // will be particulalry likely for the later subcompactions to be empty.
      // Once they are run in parallel however it should be much rarer.
      return nullptr;
    } else {
      return &child_outputs.back();
    }
  }
  Output* parent_current_output() {
    if (parent_outputs.empty()) {
      // This subcompaction's output could be empty if compaction was aborted
      // before this subcompaction had a chance to generate any output files.
      // When subcompactions are executed sequentially this is more likely and
      // will be particulalry likely for the later subcompactions to be empty.
      // Once they are run in parallel however it should be much rarer.
      return nullptr;
    } else {
      return &parent_outputs.back();
    }
  }

  uint64_t parent_current_output_file_size;
  uint64_t child_current_output_file_size;

  // State during the subcompaction
  uint64_t total_bytes;
  uint64_t num_input_records;
  uint64_t parent_num_output_records;
  uint64_t child_num_output_records;
  SplitJobStats split_job_stats;
  uint64_t approx_size;
  // An index that used to speed up ShouldStopBefore().
  size_t grandparent_index = 0;
  // The number of bytes overlapping between the current output and
  // grandparent files used in ShouldStopBefore().
  uint64_t overlapped_bytes = 0;
  // A flag determine whether the key has been seen in ShouldStopBefore()
  bool seen_key = false;

  SubsplitState(Compaction* c, Slice _median_key, Slice* _start, Slice* _end,
                     uint64_t size = 0)
      : compaction(c),
        children_cfds(c->column_family_data()->children_cfds),
        median_key(_median_key),
        child_idx(0),
        start(_start),
        end(_end),
        outfile(nullptr),
        parent_builder(nullptr),
        child_builder(nullptr),
        parent_current_output_file_size(0),
        child_current_output_file_size(0),
        total_bytes(0),
        num_input_records(0),
        parent_num_output_records(0),
        child_num_output_records(0),
        approx_size(size),
        grandparent_index(0),
        overlapped_bytes(0),
        seen_key(false) {
    assert(compaction != nullptr);
  }

  SubsplitState(SubsplitState&& o) { *this = std::move(o); }

  SubsplitState& operator=(SubsplitState&& o) {
    compaction = std::move(o.compaction);
    median_key = std::move(o.median_key);
    start = std::move(o.start);
    end = std::move(o.end);
    status = std::move(o.status);
    parent_outputs = std::move(o.parent_outputs);
    child_outputs = std::move(o.child_outputs);
    outfile = std::move(o.outfile);
    parent_builder = std::move(o.parent_builder);
    child_builder = std::move(o.child_builder);
    parent_current_output_file_size = std::move(o.parent_current_output_file_size);
    child_current_output_file_size = std::move(o.child_current_output_file_size);
    total_bytes = std::move(o.total_bytes);
    num_input_records = std::move(o.num_input_records);
    child_num_output_records = std::move(o.child_num_output_records);
    parent_num_output_records = std::move(o.parent_num_output_records);
    split_job_stats = std::move(o.split_job_stats);
    approx_size = std::move(o.approx_size);
    grandparent_index = std::move(o.grandparent_index);
    overlapped_bytes = std::move(o.overlapped_bytes);
    seen_key = std::move(o.seen_key);
    return *this;
  }

  // Because member std::unique_ptrs do not have these.
  SubsplitState(const SubsplitState&) = delete;

  SubsplitState& operator=(const SubsplitState&) = delete;

  // Returns true iff we should stop building the current output
  // before processing "internal_key".
  bool ShouldStopBefore(const Slice& internal_key, uint64_t curr_file_size) {
    const InternalKeyComparator* icmp =
        &compaction->column_family_data()->internal_comparator();
    const std::vector<FileMetaData*>& grandparents = compaction->grandparents();

    // Scan to find earliest grandparent file that contains key.
    while (grandparent_index < grandparents.size() &&
           icmp->Compare(internal_key,
                         grandparents[grandparent_index]->largest.Encode()) >
               0) {
      if (seen_key) {
        overlapped_bytes += grandparents[grandparent_index]->fd.GetFileSize();
      }
      assert(grandparent_index + 1 >= grandparents.size() ||
             icmp->Compare(
                 grandparents[grandparent_index]->largest.Encode(),
                 grandparents[grandparent_index + 1]->smallest.Encode()) <= 0);
      grandparent_index++;
    }
    seen_key = true;

    if (overlapped_bytes + curr_file_size >
        compaction->max_compaction_bytes()) {
      // Too much overlap for current output; start new output
      overlapped_bytes = 0;
      return true;
    }

    return false;
  }
};

// Maintains state for the entire compaction
struct SplitJob::SplitState {
  Compaction* const compaction;
  std::vector<FileMetaData*> metas;
  Slice median_key;

  // REQUIRED: subcompaction states are stored in order of increasing
  // key-range
  std::vector<SplitJob::SubsplitState> sub_split_states;
  Status status;

  uint64_t total_bytes;
  uint64_t num_input_records;
  uint64_t num_output_records;

  explicit SplitState(Compaction* c, std::vector<FileMetaData*> m)
      : compaction(c),
        metas(m),
        median_key(c->column_family_data()->current()->storage_info()->GetMedianKey()),
        total_bytes(0),
        num_input_records(0),
        num_output_records(0) {}

  size_t NumOutputFiles() {
    size_t total = 0;
    for (auto& s : sub_split_states) {
      total += s.parent_outputs.size();
      total += s.child_outputs.size();
    }
    return total;
  }

  Slice SmallestUserKey() {
    for (auto it = sub_split_states.rbegin(); it < sub_split_states.rend();
         ++it) {
      Slice p;
      Slice c;
      if (!it->parent_outputs.empty() && it->parent_current_output()->finished) {
        assert(it->parent_current_output() != nullptr);
        p = it->parent_current_output()->meta.smallest.user_key();
      }
      if (!it->child_outputs.empty() && it->child_current_output()->finished) {
        assert(it->child_current_output() != nullptr);
        c = it->child_current_output()->meta.smallest.user_key();
      }
      return p.compare(c) > 0 ? c : p;
    }
    // If there is no finished output, return an empty slice.
    return Slice(nullptr, 0);
  }

  Slice LargestUserKey() {
    for (auto it = sub_split_states.rbegin(); it < sub_split_states.rend();
         ++it) {
      Slice p;
      Slice c;
      if (!it->parent_outputs.empty() && it->parent_current_output()->finished) {
        assert(it->parent_current_output() != nullptr);
        p = it->parent_current_output()->meta.largest.user_key();
      }
      if (!it->child_outputs.empty() && it->child_current_output()->finished) {
        assert(it->child_current_output() != nullptr);
        c = it->child_current_output()->meta.largest.user_key();
      }
      return p.compare(c) > 0 ? p : c;
    }
    // If there is no finished output, return an empty slice.
    return Slice(nullptr, 0);
  }
};

void SplitJob::AggregateStatistics() {
  for (SubsplitState& sc : split_->sub_split_states) {
    split_->total_bytes += sc.total_bytes;
    split_->num_input_records += sc.num_input_records;
    split_->num_output_records += sc.parent_num_output_records;
    split_->num_output_records += sc.child_num_output_records;
  }
  if (split_job_stats_) {
    for (SubsplitState& sc : split_->sub_split_states) {
      split_job_stats_->Add(sc.split_job_stats);
    }
  }
}

SplitJob::SplitJob(
    int job_id, Compaction* compaction, std::vector<FileMetaData*> metas,
    const ImmutableDBOptions& db_options,
    const EnvOptions env_options, VersionSet* versions,
    const std::atomic<bool>* shutting_down,
    const SequenceNumber preserve_deletes_seqnum, LogBuffer* log_buffer,
    Directory* db_directory, Directory* output_directory, Statistics* stats,
    InstrumentedMutex* db_mutex, ErrorHandler* db_error_handler,
    std::vector<SequenceNumber> existing_snapshots,
    SequenceNumber earliest_write_conflict_snapshot,
    const SnapshotChecker* snapshot_checker, std::shared_ptr<Cache> table_cache,
    EventLogger* event_logger, bool paranoid_file_checks, bool measure_io_stats,
    const std::string& dbname, SplitJobStats* split_job_stats,
    Env::Priority thread_pri)
    : job_id_(job_id),
      split_(new SplitState(compaction, metas)),
      children_cnt_(metas.size()),
      split_job_stats_(split_job_stats),
      compaction_stats_(compaction->compaction_reason(), 1),
      dbname_(dbname),
      db_options_(db_options),
      env_options_(env_options),
      env_(db_options.env),
      env_optiosn_for_read_(
          env_->OptimizeForCompactionTableRead(env_options, db_options_)),
      versions_(versions),
      shutting_down_(shutting_down),
      preserve_deletes_seqnum_(preserve_deletes_seqnum),
      log_buffer_(log_buffer),
      db_directory_(db_directory),
      output_directory_(output_directory),
      stats_(stats),
      db_mutex_(db_mutex),
      db_error_handler_(db_error_handler),
      existing_snapshots_(std::move(existing_snapshots)),
      earliest_write_conflict_snapshot_(earliest_write_conflict_snapshot),
      snapshot_checker_(snapshot_checker),
      table_cache_(std::move(table_cache)),
      event_logger_(event_logger),
      bottommost_level_(false),
      paranoid_file_checks_(paranoid_file_checks),
      measure_io_stats_(measure_io_stats),
      write_hint_(Env::WLTH_NOT_SET),
      thread_pri_(thread_pri) {
  assert(log_buffer_ != nullptr);
  const auto* cfd = split_->compaction->column_family_data();
  ThreadStatusUtil::SetColumnFamily(cfd, cfd->ioptions()->env,
                                    db_options_.enable_thread_tracking);
  ThreadStatusUtil::SetThreadOperation(ThreadStatus::OP_COMPACTION);
  ReportStartedSplit(compaction);
}

SplitJob::~SplitJob() {
  assert(split_ == nullptr);
  ThreadStatusUtil::ResetThreadStatus();
}

void SplitJob::ReportStartedSplit(Compaction* compaction) {
  const auto* cfd = split_->compaction->column_family_data();
  ThreadStatusUtil::SetColumnFamily(cfd, cfd->ioptions()->env,
                                    db_options_.enable_thread_tracking);

  ThreadStatusUtil::SetThreadOperationProperty(ThreadStatus::COMPACTION_JOB_ID,
                                               job_id_);
  /*
  ThreadStatusUtil::SetThreadOperationProperty(
      ThreadStatus::COMPACTION_INPUT_OUTPUT_LEVEL,
      (static_cast<uint64_t>(compact_->compaction->start_level()) << 32) +
          compact_->compaction->output_level());*/

  // In the current design, a SplitJob is always created
  // for non-trivial compaction.
  /*
  assert(compaction->IsTrivialMove() == false ||
         compaction->is_manual_compaction() == true);*/
  /*
  ThreadStatusUtil::SetThreadOperationProperty(
      ThreadStatus::COMPACTION_PROP_FLAGS,
      compaction->is_manual_compaction() +
          (compaction->deletion_compaction() << 1));*/

  /*
  ThreadStatusUtil::SetThreadOperationProperty(
      ThreadStatus::COMPACTION_TOTAL_INPUT_BYTES,
      compaction->CalculateTotalInputSize());*/

  IOSTATS_RESET(bytes_written);
  IOSTATS_RESET(bytes_read);
  ThreadStatusUtil::SetThreadOperationProperty(
      ThreadStatus::COMPACTION_BYTES_WRITTEN, 0);
  ThreadStatusUtil::SetThreadOperationProperty(
      ThreadStatus::COMPACTION_BYTES_READ, 0);

  // Set the thread operation after operation properties
  // to ensure GetThreadList() can always show them all together.
  ThreadStatusUtil::SetThreadOperation(ThreadStatus::OP_COMPACTION);

  if (split_job_stats_) {
    split_job_stats_->is_manual_split =
        compaction->is_manual_compaction();
  }
}

void SplitJob::Prepare() {
  AutoThreadOperationStageUpdater stage_updater(
      ThreadStatus::STAGE_COMPACTION_PREPARE);

  // Generate file_levels_ for compaction berfore making Iterator
  auto* c = split_->compaction;
  assert(c->column_family_data() != nullptr);

  if (c->compaction_reason() == CompactionReason::kSplit) {
    ROCKS_LOG_INFO(db_options_.info_log, "SplitJob::kSplit");
    write_hint_ = Env::WriteLifeTimeHint::WLTH_NONE;
    split_->sub_split_states.emplace_back(c, nullptr, nullptr, nullptr);
  }
  else {
    ROCKS_LOG_INFO(db_options_.info_log, "SplitJob::kSplitManual Prepare median_key : %s\n",
                   split_->median_key.ToString().c_str());

    write_hint_ = Env::WriteLifeTimeHint::WLTH_NONE;
    //    c->column_family_data()->CalculateSSTWriteHint(c->output_level());
    // Is this compaction producing files at the bottommost level?
    //bottommost_level_ = c->bottommost_level();
    split_->sub_split_states.emplace_back(c, split_->median_key, nullptr, nullptr);
  }
}

struct RangeWithSize {
  Range range;
  uint64_t size;

  RangeWithSize(const Slice& a, const Slice& b, uint64_t s = 0)
      : range(a, b), size(s) {}
};


Status SplitJob::Run() {
  AutoThreadOperationStageUpdater stage_updater(
      ThreadStatus::STAGE_COMPACTION_RUN);
  TEST_SYNC_POINT("SplitJob::Run():Start");
  log_buffer_->FlushBufferToLog();
  LogSplit();

  const size_t num_threads = split_->sub_split_states.size();
  assert(num_threads > 0);
  const uint64_t start_micros = env_->NowMicros();

  // Launch a thread for each of subcompactions 1...num_threads-1
  //
  assert(num_threads == 1);
  std::vector<port::Thread> thread_pool;
  thread_pool.reserve(num_threads - 1);
  for (size_t i = 1; i < split_->sub_split_states.size(); i++) {
    thread_pool.emplace_back(&SplitJob::ProcessKeyValueSplit, this,
                             &split_->sub_split_states[i]);
  }

  // Always schedule the first subcompaction (whether or not there are also
  // others) in the current thread to be efficient with resources
  ProcessKeyValueSplit(&split_->sub_split_states[0]);

  // Wait for all other threads (if there are any) to finish execution
  for (auto& thread : thread_pool) {
    thread.join();
  }

  compaction_stats_.micros = env_->NowMicros() - start_micros;
  compaction_stats_.cpu_micros = 0;
  for (size_t i = 0; i < split_->sub_split_states.size(); i++) {
    compaction_stats_.cpu_micros +=
        split_->sub_split_states[i].split_job_stats.cpu_micros;
  }

  RecordTimeToHistogram(stats_, COMPACTION_TIME, compaction_stats_.micros);
  RecordTimeToHistogram(stats_, COMPACTION_CPU_TIME,
                        compaction_stats_.cpu_micros);

  TEST_SYNC_POINT("SplitJob::Run:BeforeVerify");

  // Check if any thread encountered an error during execution
  Status status;
  for (const auto& state : split_->sub_split_states) {
    if (!state.status.ok()) {
      status = state.status;
      break;
    }
  }

  if (status.ok() && output_directory_) {
    status = output_directory_->Fsync();
  }

  if (status.ok()) {
    thread_pool.clear();
    std::vector<const FileMetaData*> files_meta;
    for (const auto& state : split_->sub_split_states) {
      for (const auto& output : state.parent_outputs) {
        files_meta.emplace_back(&output.meta);
      }
      for (const auto& output : state.child_outputs) {
        files_meta.emplace_back(&output.meta);
      }
    }
    ColumnFamilyData* cfd = split_->compaction->column_family_data();
    auto prefix_extractor =
        split_->compaction->mutable_cf_options()->prefix_extractor.get();
    std::atomic<size_t> next_file_meta_idx(0);
    auto verify_table = [&](Status& output_status) {
      while (true) {
        size_t file_idx = next_file_meta_idx.fetch_add(1);
        if (file_idx >= files_meta.size()) {
          break;
        }
        // Verify that the table is usable
        // We set for_compaction to false and don't OptimizeForCompactionTableRead
        // here because this is a special case after we finish the table building
        // No matter whether use_direct_io_for_flush_and_compaction is true,
        // we will regard this verification as user reads since the goal is
        // to cache it here for further user reads
        InternalIterator* iter = cfd->table_cache()->NewIterator(
            ReadOptions(), env_options_, cfd->internal_comparator(),
            *files_meta[file_idx], nullptr /* range_del_agg */,
            prefix_extractor, nullptr,
            cfd->internal_stats()->GetFileReadHist(
                split_->compaction->output_level()),
            false, nullptr /* arena */, false /* skip_filters */,
            split_->compaction->output_level());
        auto s = iter->status();

        if (s.ok() && paranoid_file_checks_) {
          for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {}
          s = iter->status();
        }

        delete iter;

        if (!s.ok()) {
          output_status = s;
          break;
        }
      }
    };
    for (size_t i = 1; i < split_->sub_split_states.size(); i++) {
      thread_pool.emplace_back(verify_table,
                               std::ref(split_->sub_split_states[i].status));
    }
    verify_table(split_->sub_split_states[0].status);
    for (auto& thread : thread_pool) {
      thread.join();
    }
    for (const auto& state : split_->sub_split_states) {
      if (!state.status.ok()) {
        status = state.status;
        break;
      }
    }
  }

  TablePropertiesCollection tp;
  for (const auto& state : split_->sub_split_states) {
    for (const auto& output : state.child_outputs) {
      auto fn =
          TableFileName(state.compaction->immutable_cf_options()->cf_paths,
                        output.meta.fd.GetNumber(), output.meta.fd.GetPathId());
      tp[fn] = output.table_properties;
    }
    for (const auto& output : state.parent_outputs) {
      auto fn =
          TableFileName(state.compaction->immutable_cf_options()->cf_paths,
                        output.meta.fd.GetNumber(), output.meta.fd.GetPathId());
      tp[fn] = output.table_properties;
    }

  }
  split_->compaction->SetOutputTableProperties(std::move(tp));

  // Finish up all book-keeping to unify the subcompaction results
  AggregateStatistics();
  UpdateSplitStats();
  RecordSplitIOStats();
  LogFlush(db_options_.info_log);
  TEST_SYNC_POINT("SplitJob::Run():End");
  split_->status = status;
  return status;
}

Status SplitJob::Install(void) {
  AutoThreadOperationStageUpdater stage_updater(
      ThreadStatus::STAGE_COMPACTION_INSTALL);
  db_mutex_->AssertHeld();
  Status status = split_->status;
  ColumnFamilyData* cfd = split_->compaction->column_family_data();
  cfd->internal_stats()->AddCompactionStats(
      split_->compaction->output_level(), thread_pri_, compaction_stats_);

  if (status.ok()) {
    status = InstallSplitResults();
  }
  VersionStorageInfo::LevelSummaryStorage tmp;
  auto vstorage = cfd->current()->storage_info();
  const auto& stats = compaction_stats_;

  double read_write_amp = 0.0;
  double write_amp = 0.0;
  double bytes_read_per_sec = 0;
  double bytes_written_per_sec = 0;

  if (stats.bytes_read_non_output_levels > 0) {
    read_write_amp = (stats.bytes_written + stats.bytes_read_output_level +
                      stats.bytes_read_non_output_levels) /
                     static_cast<double>(stats.bytes_read_non_output_levels);
    write_amp = stats.bytes_written /
                static_cast<double>(stats.bytes_read_non_output_levels);
  }
  if (stats.micros > 0) {
    bytes_read_per_sec =
        (stats.bytes_read_non_output_levels + stats.bytes_read_output_level) /
        static_cast<double>(stats.micros);
    bytes_written_per_sec =
        stats.bytes_written / static_cast<double>(stats.micros);
  }

  ROCKS_LOG_BUFFER(
      log_buffer_,
      "[%s] compacted to: %s, MB/sec: %.1f rd, %.1f wr, level %d, "
      "files in(%d, %d) out(%d) "
      "MB in(%.1f, %.1f) out(%.1f), read-write-amplify(%.1f) "
      "write-amplify(%.1f) %s, records in: %" PRIu64
      ", records dropped: %" PRIu64 " output_compression: %s\n",
      cfd->GetName().c_str(), vstorage->LevelSummary(&tmp), bytes_read_per_sec,
      bytes_written_per_sec, split_->compaction->output_level(),
      stats.num_input_files_in_non_output_levels,
      stats.num_input_files_in_output_level, stats.num_output_files,
      stats.bytes_read_non_output_levels / 1048576.0,
      stats.bytes_read_output_level / 1048576.0,
      stats.bytes_written / 1048576.0, read_write_amp, write_amp,
      status.ToString().c_str(), stats.num_input_records,
      stats.num_dropped_records,
      CompressionTypeToString(split_->compaction->output_compression())
          .c_str());

  UpdateSplitJobStats(stats);

  auto stream = event_logger_->LogToBuffer(log_buffer_);
  stream << "job" << job_id_ << "event"
         << "split_finished"
         << "split_time_micros" << compaction_stats_.micros
         << "split_time_cpu_micros" << compaction_stats_.cpu_micros
         << "output_level" << split_->compaction->output_level()
         << "num_output_files" << split_->NumOutputFiles()
         << "total_output_size" << split_->total_bytes << "num_input_records"
         << split_->num_input_records << "num_output_records"
         << split_->num_output_records << "num_subcompactions"
         << split_->sub_split_states.size() << "output_compression"
         << CompressionTypeToString(split_->compaction->output_compression());

  if (split_job_stats_ != nullptr) {
    stream << "num_single_delete_mismatches"
           << split_job_stats_->num_single_del_mismatch;
    stream << "num_single_delete_fallthrough"
           << split_job_stats_->num_single_del_fallthru;
  }

  if (measure_io_stats_ && split_job_stats_ != nullptr) {
    stream << "file_write_nanos" << split_job_stats_->file_write_nanos;
    stream << "file_range_sync_nanos"
           << split_job_stats_->file_range_sync_nanos;
    stream << "file_fsync_nanos" << split_job_stats_->file_fsync_nanos;
    stream << "file_prepare_write_nanos"
           << split_job_stats_->file_prepare_write_nanos;
  }

  stream << "lsm_state";
  stream.StartArray();
  for (int level = 0; level < vstorage->num_levels(); ++level) {
    stream << vstorage->NumLevelFiles(level);
  }
  stream.EndArray();

  CleanupSplit();

  return status;
}

void SplitJob::ProcessKeyValueSplit(SubsplitState* sub_split) {
  assert(sub_split != nullptr);

  uint64_t prev_cpu_micros = env_->NowCPUNanos() / 1000;

  ColumnFamilyData* cfd = sub_split->compaction->column_family_data();

  // Create compaction filter and fail the compaction if
  // IgnoreSnapshots() = false because it is not supported anymore
  const CompactionFilter* compaction_filter =
      cfd->ioptions()->compaction_filter;
  std::unique_ptr<CompactionFilter> compaction_filter_from_factory = nullptr;
  if (compaction_filter == nullptr) {
    compaction_filter_from_factory =
        sub_split->compaction->CreateCompactionFilter();
    compaction_filter = compaction_filter_from_factory.get();
  }
  if (compaction_filter != nullptr && !compaction_filter->IgnoreSnapshots()) {
    sub_split->status = Status::NotSupported(
        "SplitFilter::IgnoreSnapshots() = false is not supported "
        "anymore.");
    return;
  }

  CompactionRangeDelAggregator range_del_agg(&cfd->internal_comparator(),
                                             existing_snapshots_);

  // Although the v2 aggregator is what the level iterator(s) know about,
  // the AddTombstones calls will be propagated down to the v1 aggregator.
  std::unique_ptr<InternalIterator> input(versions_->MakeInputIterator(
      sub_split->compaction, &range_del_agg, env_optiosn_for_read_));

  AutoThreadOperationStageUpdater stage_updater(
      ThreadStatus::STAGE_COMPACTION_PROCESS_KV);

  // I/O measurement variables
  PerfLevel prev_perf_level = PerfLevel::kEnableTime;
  const uint64_t kRecordStatsEvery = 1000;
  uint64_t prev_write_nanos = 0;
  uint64_t prev_fsync_nanos = 0;
  uint64_t prev_range_sync_nanos = 0;
  uint64_t prev_prepare_write_nanos = 0;
  uint64_t prev_cpu_write_nanos = 0;
  uint64_t prev_cpu_read_nanos = 0;
  if (measure_io_stats_) {
    prev_perf_level = GetPerfLevel();
    SetPerfLevel(PerfLevel::kEnableTimeAndCPUTimeExceptForMutex);
    prev_write_nanos = IOSTATS(write_nanos);
    prev_fsync_nanos = IOSTATS(fsync_nanos);
    prev_range_sync_nanos = IOSTATS(range_sync_nanos);
    prev_prepare_write_nanos = IOSTATS(prepare_write_nanos);
    prev_cpu_write_nanos = IOSTATS(cpu_write_nanos);
    prev_cpu_read_nanos = IOSTATS(cpu_read_nanos);
  }

  MergeHelper merge(
      env_, cfd->user_comparator(), cfd->ioptions()->merge_operator,
      compaction_filter, db_options_.info_log.get(),
      false /* internal key corruption is expected */,
      existing_snapshots_.empty() ? 0 : existing_snapshots_.back(),
      snapshot_checker_, split_->compaction->level(),
      db_options_.statistics.get(), shutting_down_);

  TEST_SYNC_POINT("SplitJob::Run():Inprogress");

  Slice* start = sub_split->start;
  Slice* end = sub_split->end;
  if (start != nullptr) {
    IterKey start_iter;
    start_iter.SetInternalKey(*start, kMaxSequenceNumber, kValueTypeForSeek);
    input->Seek(start_iter.GetInternalKey());
  } else {
    input->SeekToFirst();
  }

  Status status;
  sub_split->c_iter.reset(new CompactionIterator(
      input.get(), cfd->user_comparator(), &merge, versions_->LastSequence(),
      &existing_snapshots_, earliest_write_conflict_snapshot_,
      snapshot_checker_, env_, ShouldReportDetailedTime(env_, stats_), false,
      &range_del_agg, sub_split->compaction, compaction_filter,
      shutting_down_, preserve_deletes_seqnum_));
  auto c_iter = sub_split->c_iter.get();
  c_iter->SeekToFirst();
  if (c_iter->Valid() && sub_split->compaction->output_level() != 0) {
    // ShouldStopBefore() maintains state based on keys processed so far. The
    // compaction loop always calls it on the "next" key, thus won't tell it the
    // first key. So we do that here.
    sub_split->ShouldStopBefore(c_iter->key(),
                                  sub_split->parent_current_output_file_size);
  }
  const auto& c_iter_stats = c_iter->iter_stats();

  while (status.ok() && !cfd->IsDropped() && c_iter->Valid()) {
    // Invariant: c_iter.status() is guaranteed to be OK if c_iter->Valid()
    // returns true.
    const Slice& key = c_iter->key();
    const Slice& value = c_iter->value();
    bool is_child = false; // determine whether we should add items to child cfd or not

    // If an end key (exclusive) is specified, check if the current key is
    // >= than it and exit if it is because the iterator is out of its range
    if (end != nullptr &&
        cfd->user_comparator()->Compare(c_iter->user_key(), *end) >= 0) {
      break;
    }
    if (c_iter_stats.num_input_records % kRecordStatsEvery ==
        kRecordStatsEvery - 1) {
      RecordDroppedKeys(c_iter_stats, &sub_split->split_job_stats);
      c_iter->ResetRecordCounts();
      RecordSplitIOStats();
    }

    // Open output file if necessary
    if (sub_split->parent_builder == nullptr) {
      status = OpenSplitOutputFile(sub_split, false);
      if (!status.ok()) {
        break;
      }
    }
    if (sub_split->child_builder == nullptr && sub_split->child_idx < children_cnt_) {
      status = OpenSplitOutputFile(sub_split, true);
      if (!status.ok()) {
        break;
      }
    }

    assert(sub_split->parent_builder != nullptr);
    assert(sub_split->child_builder != nullptr || sub_split->child_idx==children_cnt_);
    assert(sub_split->parent_current_output() != nullptr);
    assert(sub_split->child_current_output() != nullptr || sub_split->child_idx==children_cnt_);

    Slice child_smallest;
    Slice child_largest;

    if (sub_split->child_builder != nullptr) {
      ColumnFamilyData* child_cfd = sub_split->children_cfds[sub_split->child_idx];
      child_smallest = child_cfd->GetSmallestKey();
      child_largest = child_cfd->GetLargestKey();
  
      if ((child_smallest.empty() &&
           key.compare(child_largest) <= 0) ||
          (child_largest.empty() &&
           key.compare(child_smallest) >= 0) ||
          (key.compare(child_smallest) >= 0 &&
           key.compare(child_largest) <= 0)) { // key is in child cfds boundaries
        is_child = true;
        sub_split->child_builder->Add(key, value);
        sub_split->child_current_output_file_size = sub_split->child_builder->FileSize();
        sub_split->child_current_output()->meta.UpdateBoundaries(
            key, c_iter->ikey().sequence);
        sub_split->child_num_output_records++;
      }
    } else {
        is_child = false;
        sub_split->parent_builder->Add(key, value);
        sub_split->parent_current_output_file_size = sub_split->parent_builder->FileSize();
        sub_split->parent_current_output()->meta.UpdateBoundaries(
            key, c_iter->ikey().sequence);
        sub_split->parent_num_output_records++;
    }

    

    // Close output file if it is big enough. Two possibilities determine it's
    // time to close it: (1) the current key should be this file's last key, (2)
    // the next key should not be in this file.
    //
    // TODO(aekmekji): determine if file should be closed earlier than this
    // during subcompactions (i.e. if output size, estimated by input size, is
    // going to be 1.2MB and max_output_file_size = 1MB, prefer to have 0.6MB
    // and 0.6MB instead of 1MB and 0.2MB)
    bool output_file_ended = false;
    bool child_file_ended = false;
    uint64_t current_output_file_size = is_child ? sub_split->child_current_output_file_size :
                                                   sub_split->parent_current_output_file_size;
    Status input_status;
    if (sub_split->compaction->output_level() != 0 &&
        current_output_file_size >=
        sub_split->compaction->max_output_file_size()) {
      // (1) this key terminates the file. For historical reasons, the iterator
      // status before advancing will be given to FinishSplitOutputFile().
      input_status = input->status();
      output_file_ended = true;
      ROCKS_LOG_INFO(
          db_options_.info_log,
          "SplitJob::ProcessKeyValueSplit output_file_ended(1)"
      );
    }
    c_iter->Next();
    if (!output_file_ended && c_iter->Valid() &&
        sub_split->compaction->output_level() != 0 &&
        sub_split->ShouldStopBefore(c_iter->key(),
                                    current_output_file_size) &&
        (sub_split->parent_builder != nullptr ||
        sub_split->child_builder != nullptr)
       ) {
      // (2) this key belongs to the next file. For historical reasons, the
      // iterator status after advancing will be given to
      // FinishSplitOutputFile().
      input_status = input->status();
      output_file_ended = true;
      ROCKS_LOG_INFO(
          db_options_.info_log,
          "SplitJob::ProcessKeyValueSplit output_file_ended(2)"
      );
    }
    if (is_child && !child_largest.empty() && cfd->user_comparator()->Compare(c_iter->user_key(), child_largest) >=0) {
      // (3) if key is greater than the child's largest key, terminates the file and switch to the next column family.
      output_file_ended = true;
      child_file_ended = true;
    }

    if (output_file_ended) {
      const Slice* next_key = nullptr;
      if (c_iter->Valid()) {
        next_key = &c_iter->key();
      }
      CompactionIterationStats range_del_out_stats;
      status =
          FinishSplitOutputFile(input_status, sub_split, &range_del_agg,
                               &range_del_out_stats, next_key, is_child);
      RecordDroppedKeys(range_del_out_stats,
                        &sub_split->split_job_stats);
      if (child_file_ended) {
        sub_split->child_idx ++;
      }
    }
  }

  sub_split->num_input_records = c_iter_stats.num_input_records;
  sub_split->split_job_stats.num_input_deletion_records =
      c_iter_stats.num_input_deletion_records;
  sub_split->split_job_stats.num_corrupt_keys =
      c_iter_stats.num_input_corrupt_records;
  sub_split->split_job_stats.num_single_del_fallthru =
      c_iter_stats.num_single_del_fallthru;
  sub_split->split_job_stats.num_single_del_mismatch =
      c_iter_stats.num_single_del_mismatch;
  sub_split->split_job_stats.total_input_raw_key_bytes +=
      c_iter_stats.total_input_raw_key_bytes;
  sub_split->split_job_stats.total_input_raw_value_bytes +=
      c_iter_stats.total_input_raw_value_bytes;

  RecordTick(stats_, FILTER_OPERATION_TOTAL_TIME,
             c_iter_stats.total_filter_time);
  RecordDroppedKeys(c_iter_stats, &sub_split->split_job_stats);
  RecordSplitIOStats();

  if (status.ok() &&
      (shutting_down_->load(std::memory_order_relaxed) || cfd->IsDropped())) {
    status = Status::ShutdownInProgress(
        "Database shutdown or Column family drop during compaction");
  }
  if (status.ok()) {
    status = input->status();
  }
  if (status.ok()) {
    status = c_iter->status();
  }

  if (status.ok() && sub_split->parent_builder == nullptr &&
      sub_split->parent_outputs.size() == 0 && !range_del_agg.IsEmpty()) {
    // handle subcompaction containing only range deletions
    // range_del is for parent column family
    status = OpenSplitOutputFile(sub_split, false);
  }

  // Call FinishSplitOutputFile() even if status is not ok: it needs to
  // close the output file.
  if (sub_split->parent_builder != nullptr) {
    CompactionIterationStats range_del_out_stats;
    Status s = FinishSplitOutputFile(status, sub_split, &range_del_agg,
                                     &range_del_out_stats, nullptr, false);
    if (status.ok()) {
      status = s;
    }
    RecordDroppedKeys(range_del_out_stats, &sub_split->split_job_stats);
  }
  if (sub_split->child_builder != nullptr) {
    CompactionIterationStats range_del_out_stats;
    Status s = FinishSplitOutputFile(status, sub_split, &range_del_agg,
                                     &range_del_out_stats, nullptr, true);
    if (status.ok()) {
      status = s;
    }
    RecordDroppedKeys(range_del_out_stats, &sub_split->split_job_stats);
  }

  sub_split->split_job_stats.cpu_micros =
      env_->NowCPUNanos() / 1000 - prev_cpu_micros;

  if (measure_io_stats_) {
    sub_split->split_job_stats.file_write_nanos +=
        IOSTATS(write_nanos) - prev_write_nanos;
    sub_split->split_job_stats.file_fsync_nanos +=
        IOSTATS(fsync_nanos) - prev_fsync_nanos;
    sub_split->split_job_stats.file_range_sync_nanos +=
        IOSTATS(range_sync_nanos) - prev_range_sync_nanos;
    sub_split->split_job_stats.file_prepare_write_nanos +=
        IOSTATS(prepare_write_nanos) - prev_prepare_write_nanos;
    sub_split->split_job_stats.cpu_micros -=
        (IOSTATS(cpu_write_nanos) - prev_cpu_write_nanos +
         IOSTATS(cpu_read_nanos) - prev_cpu_read_nanos) /
        1000;
    if (prev_perf_level != PerfLevel::kEnableTimeAndCPUTimeExceptForMutex) {
      SetPerfLevel(prev_perf_level);
    }
  }

  sub_split->c_iter.reset();
  input.reset();
  sub_split->status = status;
}

void SplitJob::RecordDroppedKeys(
    const CompactionIterationStats& c_iter_stats,
    SplitJobStats* split_job_stats) {
  if (c_iter_stats.num_record_drop_user > 0) {
    RecordTick(stats_, COMPACTION_KEY_DROP_USER,
               c_iter_stats.num_record_drop_user);
  }
  if (c_iter_stats.num_record_drop_hidden > 0) {
    RecordTick(stats_, COMPACTION_KEY_DROP_NEWER_ENTRY,
               c_iter_stats.num_record_drop_hidden);
    if (split_job_stats) {
      split_job_stats->num_records_replaced +=
          c_iter_stats.num_record_drop_hidden;
    }
  }
  if (c_iter_stats.num_record_drop_obsolete > 0) {
    RecordTick(stats_, COMPACTION_KEY_DROP_OBSOLETE,
               c_iter_stats.num_record_drop_obsolete);
    if (split_job_stats) {
      split_job_stats->num_expired_deletion_records +=
          c_iter_stats.num_record_drop_obsolete;
    }
  }
  if (c_iter_stats.num_record_drop_range_del > 0) {
    RecordTick(stats_, COMPACTION_KEY_DROP_RANGE_DEL,
               c_iter_stats.num_record_drop_range_del);
  }
  if (c_iter_stats.num_range_del_drop_obsolete > 0) {
    RecordTick(stats_, COMPACTION_RANGE_DEL_DROP_OBSOLETE,
               c_iter_stats.num_range_del_drop_obsolete);
  }
  if (c_iter_stats.num_optimized_del_drop_obsolete > 0) {
    RecordTick(stats_, COMPACTION_OPTIMIZED_DEL_DROP_OBSOLETE,
               c_iter_stats.num_optimized_del_drop_obsolete);
  }
}

Status SplitJob::FinishSplitOutputFile(
    const Status& input_status, SubsplitState* sub_split,
    CompactionRangeDelAggregator* range_del_agg,
    CompactionIterationStats* range_del_out_stats,
    const Slice* next_table_min_key /* = nullptr */,
    bool is_child) {
  AutoThreadOperationStageUpdater stage_updater(
      ThreadStatus::STAGE_COMPACTION_SYNC_FILE);
  assert(sub_split != nullptr);

  uint64_t output_number;
  ColumnFamilyData* cfd;

  if (is_child) {
    assert(sub_split->outfile);
    assert(sub_split->child_builder != nullptr);
    assert(sub_split->child_current_output() != nullptr);
    output_number = sub_split->child_current_output()->meta.fd.GetNumber();
    cfd = sub_split->children_cfds[sub_split->child_idx];
  } else {
    assert(sub_split->outfile);
    assert(sub_split->parent_builder != nullptr);
    assert(sub_split->parent_current_output() != nullptr);
    output_number = sub_split->parent_current_output()->meta.fd.GetNumber();
    cfd = sub_split->compaction->column_family_data();
  }
  assert(output_number != 0);
  const Comparator* ucmp = cfd->user_comparator();
  ROCKS_LOG_INFO(
      db_options_.info_log,
      "SplitJob::FinishSplitOutputFile child_idx : %ld\n",
      sub_split->child_idx
  );


  // Check for iterator errors
  Status s = input_status;
  FileMetaData* meta;
  if (is_child) {
    meta = &sub_split->child_current_output()->meta;
  } else {
    meta = &sub_split->parent_current_output()->meta;
  }
  assert(meta != nullptr && !is_child);
  if (s.ok() && !is_child) { // tombstone always go to parent 
    Slice lower_bound_guard, upper_bound_guard;
    std::string smallest_user_key;
    const Slice *lower_bound, *upper_bound;
    bool lower_bound_from_sub_split = false;
    if (sub_split->parent_outputs.size() == 1) {
      // For the first output table, include range tombstones before the min key
      // but after the subcompaction boundary.
      lower_bound = sub_split->start;
      lower_bound_from_sub_split = true;
    } else if (meta->smallest.size() > 0) {
      // For subsequent output tables, only include range tombstones from min
      // key onwards since the previous file was extended to contain range
      // tombstones falling before min key.
      smallest_user_key = meta->smallest.user_key().ToString(false /*hex*/);
      lower_bound_guard = Slice(smallest_user_key);
      lower_bound = &lower_bound_guard;
    } else {
      lower_bound = nullptr;
    }
    if (next_table_min_key != nullptr) {
      // This may be the last file in the subcompaction in some cases, so we
      // need to compare the end key of subcompaction with the next file start
      // key. When the end key is chosen by the subcompaction, we know that
      // it must be the biggest key in output file. Therefore, it is safe to
      // use the smaller key as the upper bound of the output file, to ensure
      // that there is no overlapping between different output files.
      upper_bound_guard = ExtractUserKey(*next_table_min_key);
      if (sub_split->end != nullptr &&
          ucmp->Compare(upper_bound_guard, *sub_split->end) >= 0) {
        upper_bound = sub_split->end;
      } else {
        upper_bound = &upper_bound_guard;
      }
    } else {
      // This is the last file in the subcompaction, so extend until the
      // subcompaction ends.
      upper_bound = sub_split->end;
    }
    auto earliest_snapshot = kMaxSequenceNumber;
    if (existing_snapshots_.size() > 0) {
      earliest_snapshot = existing_snapshots_[0];
    }
    bool has_overlapping_endpoints;
    if (upper_bound != nullptr && meta->largest.size() > 0) {
      has_overlapping_endpoints =
          ucmp->Compare(meta->largest.user_key(), *upper_bound) == 0;
    } else {
      has_overlapping_endpoints = false;
    }

    // The end key of the subcompaction must be bigger or equal to the upper
    // bound. If the end of subcompaction is null or the upper bound is null,
    // it means that this file is the last file in the compaction. So there
    // will be no overlapping between this file and others.
    assert(sub_split->end == nullptr ||
           upper_bound == nullptr ||
           ucmp->Compare(*upper_bound , *sub_split->end) <= 0);
    auto it = range_del_agg->NewIterator(lower_bound, upper_bound,
                                         has_overlapping_endpoints);
    // Position the range tombstone output iterator. There may be tombstone
    // fragments that are entirely out of range, so make sure that we do not
    // include those.
    if (lower_bound != nullptr) {
      it->Seek(*lower_bound);
    } else {
      it->SeekToFirst();
    }
    for (; it->Valid(); it->Next()) {
      auto tombstone = it->Tombstone();
      if (upper_bound != nullptr) {
        int cmp = ucmp->Compare(*upper_bound, tombstone.start_key_);
        if ((has_overlapping_endpoints && cmp < 0) ||
            (!has_overlapping_endpoints && cmp <= 0)) {
          // Tombstones starting after upper_bound only need to be included in
          // the next table. If the current SST ends before upper_bound, i.e.,
          // `has_overlapping_endpoints == false`, we can also skip over range
          // tombstones that start exactly at upper_bound. Such range tombstones
          // will be included in the next file and are not relevant to the point
          // keys or endpoints of the current file.
          break;
        }
      }

      if (bottommost_level_ && tombstone.seq_ <= earliest_snapshot) {
        // TODO(andrewkr): tombstones that span multiple output files are
        // counted for each compaction output file, so lots of double counting.
        range_del_out_stats->num_range_del_drop_obsolete++;
        range_del_out_stats->num_record_drop_obsolete++;
        continue;
      }

      auto kv = tombstone.Serialize();
      assert(lower_bound == nullptr ||
             ucmp->Compare(*lower_bound, kv.second) < 0);
      sub_split->parent_builder->Add(kv.first.Encode(), kv.second);
      InternalKey smallest_candidate = std::move(kv.first);
      if (lower_bound != nullptr &&
          ucmp->Compare(smallest_candidate.user_key(), *lower_bound) <= 0) {
        // Pretend the smallest key has the same user key as lower_bound
        // (the max key in the previous table or subcompaction) in order for
        // files to appear key-space partitioned.
        //
        // When lower_bound is chosen by a subcompaction, we know that
        // subcompactions over smaller keys cannot contain any keys at
        // lower_bound. We also know that smaller subcompactions exist, because
        // otherwise the subcompaction woud be unbounded on the left. As a
        // result, we know that no other files on the output level will contain
        // actual keys at lower_bound (an output file may have a largest key of
        // lower_bound@kMaxSequenceNumber, but this only indicates a large range
        // tombstone was truncated). Therefore, it is safe to use the
        // tombstone's sequence number, to ensure that keys at lower_bound at
        // lower levels are covered by truncated tombstones.
        //
        // If lower_bound was chosen by the smallest data key in the file,
        // choose lowest seqnum so this file's smallest internal key comes after
        // the previous file's largest. The fake seqnum is OK because the read
        // path's file-picking code only considers user key.
        smallest_candidate = InternalKey(
            *lower_bound, lower_bound_from_sub_split ? tombstone.seq_ : 0,
            kTypeRangeDeletion);
      }
      InternalKey largest_candidate = tombstone.SerializeEndKey();
      if (upper_bound != nullptr &&
          ucmp->Compare(*upper_bound, largest_candidate.user_key()) <= 0) {
        // Pretend the largest key has the same user key as upper_bound (the
        // min key in the following table or subcompaction) in order for files
        // to appear key-space partitioned.
        //
        // Choose highest seqnum so this file's largest internal key comes
        // before the next file's/subcompaction's smallest. The fake seqnum is
        // OK because the read path's file-picking code only considers the user
        // key portion.
        //
        // Note Seek() also creates InternalKey with (user_key,
        // kMaxSequenceNumber), but with kTypeDeletion (0x7) instead of
        // kTypeRangeDeletion (0xF), so the range tombstone comes before the
        // Seek() key in InternalKey's ordering. So Seek() will look in the
        // next file for the user key.
        largest_candidate =
            InternalKey(*upper_bound, kMaxSequenceNumber, kTypeRangeDeletion);
      }
#ifndef NDEBUG
      SequenceNumber smallest_ikey_seqnum = kMaxSequenceNumber;
      if (meta->smallest.size() > 0) {
        smallest_ikey_seqnum = GetInternalKeySeqno(meta->smallest.Encode());
      }
#endif
      meta->UpdateBoundariesForRange(smallest_candidate, largest_candidate,
                                     tombstone.seq_,
                                     cfd->internal_comparator());

      // The smallest key in a file is used for range tombstone truncation, so
      // it cannot have a seqnum of 0 (unless the smallest data key in a file
      // has a seqnum of 0). Otherwise, the truncated tombstone may expose
      // deleted keys at lower levels.
      assert(smallest_ikey_seqnum == 0 ||
             ExtractInternalKeyFooter(meta->smallest.Encode()) !=
                 PackSequenceAndType(0, kTypeRangeDeletion));
    }
    if (is_child) {
      meta->marked_for_compaction = sub_split->child_builder->NeedCompact();
    } else {
      meta->marked_for_compaction = sub_split->parent_builder->NeedCompact();
    }
  }
  
  const uint64_t current_entries = is_child ? sub_split->child_builder->NumEntries() :
                                 sub_split->parent_builder->NumEntries();             
  
  if (s.ok()) {
    if (is_child) {
      s = sub_split->child_builder->Finish();
    } else {
      s = sub_split->parent_builder->Finish();
    }
  } else {
    if (is_child) {
      sub_split->child_builder->Abandon();
    } else {
      sub_split->parent_builder->Abandon();
    }
  }

  const uint64_t current_bytes = is_child ? sub_split->child_builder->FileSize() :
                                            sub_split->parent_builder->FileSize();
  if (s.ok()) {
    meta->fd.file_size = current_bytes;
  }

  if (is_child) {
    sub_split->child_current_output()->finished = true;
  } else {
    sub_split->parent_current_output()->finished = true;
  }
  sub_split->total_bytes += current_bytes;

  // Finish and check for file errors
  if (s.ok()) {
    StopWatch sw(env_, stats_, COMPACTION_OUTFILE_SYNC_MICROS);
    s = sub_split->outfile->Sync(db_options_.use_fsync);
  }
  if (s.ok()) {
    s = sub_split->outfile->Close();
  }

  sub_split->outfile.reset();

  TableProperties tp;
  if (s.ok()) {
    if (is_child) {
      tp = sub_split->child_builder->GetTableProperties();
    } else {
      tp = sub_split->parent_builder->GetTableProperties();
    }
  }

  if (s.ok() && current_entries == 0 && tp.num_range_deletions == 0) {
    // If there is nothing to output, no necessary to generate a sst file.
    // This happens when the output level is bottom level, at the same time
    // the sub_split output nothing.
    std::string fname =
        TableFileName(sub_split->compaction->immutable_cf_options()->cf_paths,
                      meta->fd.GetNumber(), meta->fd.GetPathId());
    env_->DeleteFile(fname);

    // Also need to remove the file from outputs, or it will be added to the
    // VersionEdit.
    if (is_child) {
      assert(!sub_split->child_outputs.empty());
      sub_split->child_outputs.pop_back();
    } else {
      assert(!sub_split->parent_outputs.empty());
      sub_split->parent_outputs.pop_back();
    }
    meta = nullptr;
  }

  if (s.ok() && (current_entries > 0 || tp.num_range_deletions > 0)) {
    // Output to event logger and fire events.
    if (is_child) {
      sub_split->child_current_output()->table_properties =
          std::make_shared<TableProperties>(tp);
    } else {
      sub_split->parent_current_output()->table_properties =
          std::make_shared<TableProperties>(tp);
    }
    
    ROCKS_LOG_INFO(db_options_.info_log,
                   "[%s] [JOB %d] Generated table #%" PRIu64 ": %" PRIu64
                   " keys, %" PRIu64 " bytes%s",
                   cfd->GetName().c_str(), job_id_, output_number,
                   current_entries, current_bytes,
                   meta->marked_for_compaction ? " (need compaction)" : "");
  }
  std::string fname;
  FileDescriptor output_fd;
  if (meta != nullptr) {
    fname =
        TableFileName(sub_split->compaction->immutable_cf_options()->cf_paths,
                      meta->fd.GetNumber(), meta->fd.GetPathId());
    output_fd = meta->fd;
  } else {
    fname = "(nil)";
  }
  EventHelpers::LogAndNotifyTableFileCreationFinished(
      event_logger_, cfd->ioptions()->listeners, dbname_, cfd->GetName(), fname,
      job_id_, output_fd, tp, TableFileCreationReason::kSplit, s);

#ifndef ROCKSDB_LITE
  // Report new file to SstFileManagerImpl
  auto sfm =
      static_cast<SstFileManagerImpl*>(db_options_.sst_file_manager.get());
  if (sfm && meta != nullptr && meta->fd.GetPathId() == 0) {
    sfm->OnAddFile(fname);
    if (sfm->IsMaxAllowedSpaceReached()) {
      // TODO(ajkr): should we return OK() if max space was reached by the final
      // compaction output file (similarly to how flush works when full)?
      s = Status::SpaceLimit("Max allowed space was reached");
      TEST_SYNC_POINT(
          "SplitJob::FinishSplitOutputFile:"
          "MaxAllowedSpaceReached");
      InstrumentedMutexLock l(db_mutex_);
      db_error_handler_->SetBGError(s, BackgroundErrorReason::kSplit);
    }
  }
#endif

  if (is_child) {
    sub_split->child_builder.reset();
    sub_split->child_current_output_file_size = 0;
  } else {
    sub_split->parent_builder.reset();
    sub_split->parent_current_output_file_size = 0;
  }

  return s;
}

Status SplitJob::InstallSplitResults() {
  db_mutex_->AssertHeld();

  auto* compaction = split_->compaction;
  // paranoia: verify that the files that we started with
  // still exist in the current version and in the same original level.
  // This ensures that a concurrent compaction did not erroneously
  // pick the same files to split_.
  if (!versions_->VerifyCompactionFileConsistency(compaction)) {
    Compaction::InputLevelSummaryBuffer inputs_summary;

    ROCKS_LOG_ERROR(db_options_.info_log, "[%s] [JOB %d] Split %s aborted",
                    compaction->column_family_data()->GetName().c_str(),
                    job_id_, compaction->InputLevelSummary(&inputs_summary));
    return Status::Corruption("Split input files inconsistent");
  }

  {
    Compaction::InputLevelSummaryBuffer inputs_summary;
    ROCKS_LOG_INFO(
        db_options_.info_log, "[%s] [JOB %d] Splitted %s => %" PRIu64 " bytes",
        compaction->column_family_data()->GetName().c_str(), job_id_,
        compaction->InputLevelSummary(&inputs_summary), split_->total_bytes);
  }

  autovector<ColumnFamilyData*> column_family_datas;
  autovector<autovector<VersionEdit*>> edit_lists;

  ColumnFamilyData* cfd_in = split_->compaction->column_family_data();
    column_family_datas.push_back(cfd_in);
  for (auto& child_cfd: split_->sub_split_states[0].children_cfds) {
    column_family_datas.push_back(child_cfd);
  }

  autovector<VersionEdit*> edit_in;
  autovector<VersionEdit*> edit_out[children_cnt_];
  VersionEdit e_in;
  VersionEdit e_out[children_cnt_];
  
  e_in.SetColumnFamily(split_->compaction->column_family_data()->GetID());

  for (size_t which = 0; which < split_->compaction->num_input_levels(); which++) {
    for (size_t i = 0; i < split_->compaction->num_input_files(which); i++) {
      e_in.DeleteFile(split_->compaction->level(which), split_->compaction->input(which, i)->fd.GetNumber());
    }
  }

  // split move
  for (const auto& sub_split : split_->sub_split_states) {
    for (const auto& out : sub_split.parent_outputs) {
      e_in.AddFile(compaction->output_level(), out.meta);
    }
    for (const auto& out : sub_split.parent_outputs) {
      int idx = out.child_idx;
      e_out[idx].AddFile(compaction->output_level(), out.meta);
    }
  }
  // trivial move
  std::vector<FileMetaData*> metas = split_->metas;
  assert(metas.size() == children_cnt_);
  for (size_t idx = 0; idx < metas.size(); idx++) {
    const FileMetaData* meta_idx = const_cast<const FileMetaData*>(metas[idx]);
    e_out[idx].AddFile(2, *meta_idx);
  }

  edit_in.push_back(&e_in);
  for(size_t idx = 0; idx < metas.size(); idx++) {
    edit_out[idx].push_back(&e_out[idx]);
  }

  edit_lists.push_back(edit_in);
  for(size_t idx = 0; idx < metas.size(); idx++) {
    edit_lists.push_back(edit_out[idx]);
  }

  autovector<const MutableCFOptions*> mutable_cf_options_list;
  mutable_cf_options_list.push_back(cfd_in->GetLatestMutableCFOptions());
  for (auto& child_cfd: split_->sub_split_states[0].children_cfds) {
    mutable_cf_options_list.push_back(child_cfd->GetLatestMutableCFOptions());
  }

  int num_entries = (int)edit_lists.size();
  for (auto& edits: edit_lists) {
    assert(edits.size() == 1);
    edits[0]->MarkAtomicGroup(--num_entries);
  }

  return versions_->LogAndApply(column_family_datas,
                                mutable_cf_options_list,
                                edit_lists,
                                db_mutex_,
                                db_directory_,
                                false,
                                nullptr);
}

void SplitJob::RecordSplitIOStats() {
  RecordTick(stats_, COMPACT_READ_BYTES, IOSTATS(bytes_read));
  ThreadStatusUtil::IncreaseThreadOperationProperty(
      ThreadStatus::COMPACTION_BYTES_READ, IOSTATS(bytes_read));
  IOSTATS_RESET(bytes_read);
  RecordTick(stats_, COMPACT_WRITE_BYTES, IOSTATS(bytes_written));
  ThreadStatusUtil::IncreaseThreadOperationProperty(
      ThreadStatus::COMPACTION_BYTES_WRITTEN, IOSTATS(bytes_written));
  IOSTATS_RESET(bytes_written);
}

Status SplitJob::OpenSplitOutputFile(
    SubsplitState* sub_split, bool is_child) {
  assert(sub_split != nullptr);
  if (is_child) {
    assert(sub_split->child_builder == nullptr);
  } else {
    assert(sub_split->parent_builder == nullptr);
  }
 
  // no need to lock because VersionSet::next_file_number_ is atomic
  uint64_t file_number = versions_->NewFileNumber();
  std::string fname =
      TableFileName(sub_split->compaction->immutable_cf_options()->cf_paths,
                    file_number, sub_split->compaction->output_path_id());
  // Fire events.
  ColumnFamilyData* cfd_out;
  if (is_child) {
    cfd_out = sub_split->children_cfds[sub_split->child_idx];
  } else {
    cfd_out = sub_split->compaction->column_family_data();
  }
  
  ROCKS_LOG_INFO(
      db_options_.info_log,
      "SplitJob::OpenSplitOutputFile cfd name : %s",
      cfd_out->GetName().c_str()
  );
#ifndef ROCKSDB_LITE
  EventHelpers::NotifyTableFileCreationStarted(
      cfd_out->ioptions()->listeners, dbname_, cfd_out->GetName(), fname, job_id_,
      TableFileCreationReason::kSplit);
#endif  // !ROCKSDB_LITE
  // Make the output file
  std::unique_ptr<WritableFile> writable_file;
#ifndef NDEBUG
  bool syncpoint_arg = env_options_.use_direct_writes;
  TEST_SYNC_POINT_CALLBACK("SplitJob::OpenSplitOutputFile",
                           &syncpoint_arg);
#endif
  Status s = NewWritableFile(env_, fname, &writable_file, env_options_);
  if (!s.ok()) {
    ROCKS_LOG_ERROR(
        db_options_.info_log,
        "[%s] [JOB %d] OpenSplitOutputFiles for table #%" PRIu64
        " fails at NewWritableFile with status %s",
        cfd_out->GetName().c_str(),
        job_id_, file_number, s.ToString().c_str());
    LogFlush(db_options_.info_log);
    EventHelpers::LogAndNotifyTableFileCreationFinished(
        event_logger_, cfd_out->ioptions()->listeners, dbname_, cfd_out->GetName().c_str(),
        fname, job_id_, FileDescriptor(), TableProperties(),
        TableFileCreationReason::kSplit, s);
    return s;
  }

  SubsplitState::Output out;
  out.meta.fd =
      FileDescriptor(file_number, sub_split->compaction->output_path_id(), 0);
  out.finished = false;
  out.child_idx = sub_split->child_idx;
  if (is_child) {
    sub_split->child_outputs.push_back(out);
  } else {
    sub_split->parent_outputs.push_back(out);
  }
  writable_file->SetIOPriority(Env::IO_LOW);
  writable_file->SetWriteLifeTimeHint(write_hint_);
  writable_file->SetPreallocationBlockSize(static_cast<size_t>(
      sub_split->compaction->OutputFilePreallocationSize()));
  const auto& listeners =
      sub_split->compaction->immutable_cf_options()->listeners;
  sub_split->outfile.reset(
      new WritableFileWriter(std::move(writable_file), fname, env_options_,
                             env_, db_options_.statistics.get(), listeners));

  // If the Column family flag is to only optimize filters for hits,
  // we can skip creating filters if this is the bottommost_level where
  // data is going to be found
  bool skip_filters =
      cfd_out->ioptions()->optimize_filters_for_hits && bottommost_level_;

  uint64_t output_file_creation_time =
      sub_split->compaction->MaxInputFileCreationTime();
  if (output_file_creation_time == 0) {
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
    output_file_creation_time = static_cast<uint64_t>(_current_time);
  }

  if (is_child) {
    sub_split->child_builder.reset(NewTableBuilder(
        *cfd_out->ioptions(), *(sub_split->compaction->mutable_cf_options()),
        cfd_out->internal_comparator(), cfd_out->int_tbl_prop_collector_factories(),
        cfd_out->GetID(), cfd_out->GetName(), sub_split->outfile.get(),
        sub_split->compaction->output_compression(),
        0 /*sample_for_compression */,
        sub_split->compaction->output_compression_opts(),
        sub_split->compaction->output_level(), skip_filters,
        output_file_creation_time, 0 /* oldest_key_time */,
        sub_split->compaction->max_output_file_size()));
  } else{
   sub_split->parent_builder.reset(NewTableBuilder(
        *cfd_out->ioptions(), *(sub_split->compaction->mutable_cf_options()),
        cfd_out->internal_comparator(), cfd_out->int_tbl_prop_collector_factories(),
        cfd_out->GetID(), cfd_out->GetName(), sub_split->outfile.get(),
        sub_split->compaction->output_compression(),
        0 /*sample_for_compression */,
        sub_split->compaction->output_compression_opts(),
        sub_split->compaction->output_level(), skip_filters,
        output_file_creation_time, 0 /* oldest_key_time */,
        sub_split->compaction->max_output_file_size()));
  }

  
  LogFlush(db_options_.info_log);
  return s;
}

void SplitJob::CleanupSplit() {
  for (SubsplitState& sub_split : split_->sub_split_states) {
    const auto& sub_status = sub_split.status;

    if (sub_split.parent_builder != nullptr) {
      // May happen if we get a shutdown call in the middle of compaction
      sub_split.parent_builder->Abandon();
      sub_split.parent_builder.reset();
    } else if (sub_split.child_builder != nullptr) {
      // May happen if we get a shutdown call in the middle of compaction
      sub_split.child_builder->Abandon();
      sub_split.child_builder.reset();
    } else {
      assert(!sub_status.ok() || sub_split.outfile == nullptr);
    }
    for (const auto& out : sub_split.parent_outputs) {
      // If this file was inserted into the table cache then remove
      // them here because this compaction was not committed.
      if (!sub_status.ok()) {
        TableCache::Evict(table_cache_.get(), out.meta.fd.GetNumber());
      }
    }
    for (const auto& out : sub_split.child_outputs) {
      // If this file was inserted into the table cache then remove
      // them here because this compaction was not committed.
      if (!sub_status.ok()) {
        TableCache::Evict(table_cache_.get(), out.meta.fd.GetNumber());
      }
    }
  }
  delete split_;
  split_ = nullptr;
}

#ifndef ROCKSDB_LITE
namespace {
void CopyPrefix(const Slice& src, size_t prefix_length, std::string* dst) {
  assert(prefix_length > 0);
  size_t length = src.size() > prefix_length ? prefix_length : src.size();
  dst->assign(src.data(), length);
}
}  // namespace

#endif  // !ROCKSDB_LITE

void SplitJob::UpdateSplitStats() {
  Compaction* compaction = split_->compaction;
  compaction_stats_.num_input_files_in_non_output_levels = 0;
  compaction_stats_.num_input_files_in_output_level = 0;
  for (int input_level = 0;
       input_level < static_cast<int>(compaction->num_input_levels());
       ++input_level) {
    if (compaction->level(input_level) != compaction->output_level()) {
      UpdateSplitInputStatsHelper(
          &compaction_stats_.num_input_files_in_non_output_levels,
          &compaction_stats_.bytes_read_non_output_levels, input_level);
    } else {
      UpdateSplitInputStatsHelper(
          &compaction_stats_.num_input_files_in_output_level,
          &compaction_stats_.bytes_read_output_level, input_level);
    }
  }

  for (const auto& sub_split : split_->sub_split_states) {
    size_t child_num_output_files = sub_split.child_outputs.size();
    if (sub_split.child_builder != nullptr) {
      // An error occurred so ignore the last output.
      assert(child_num_output_files > 0);
      --child_num_output_files;
    }
    size_t parent_num_output_files = sub_split.parent_outputs.size();
    if (sub_split.parent_builder != nullptr) {
      // An error occurred so ignore the last output
      assert(parent_num_output_files > 0);
      --parent_num_output_files;
    }
    size_t num_output_files = child_num_output_files + parent_num_output_files;
    compaction_stats_.num_output_files += static_cast<int>(num_output_files);

    for (const auto& out : sub_split.child_outputs) {
      compaction_stats_.bytes_written += out.meta.fd.file_size;
    }
    for (const auto& out : sub_split.parent_outputs) {
      compaction_stats_.bytes_written += out.meta.fd.file_size;
    }
   
    size_t total_num_output_records = sub_split.parent_num_output_records +
                                      sub_split.child_num_output_records;

    if (sub_split.num_input_records > total_num_output_records) {
      compaction_stats_.num_dropped_records +=
          sub_split.num_input_records - total_num_output_records;
    }
  }
}

void SplitJob::UpdateSplitInputStatsHelper(int* num_files,
                                                     uint64_t* bytes_read,
                                                     int input_level) {
  const Compaction* compaction = split_->compaction;
  auto num_input_files = compaction->num_input_files(input_level);
  *num_files += static_cast<int>(num_input_files);

  for (size_t i = 0; i < num_input_files; ++i) {
    const auto* file_meta = compaction->input(input_level, i);
    *bytes_read += file_meta->fd.GetFileSize();
    compaction_stats_.num_input_records +=
        static_cast<uint64_t>(file_meta->num_entries);
  }
}

void SplitJob::UpdateSplitJobStats(
    const InternalStats::CompactionStats& stats) const {
#ifndef ROCKSDB_LITE
  if (split_job_stats_) {
    split_job_stats_->elapsed_micros = stats.micros;

    // input information
    split_job_stats_->total_input_bytes =
        stats.bytes_read_non_output_levels + stats.bytes_read_output_level;
    split_job_stats_->num_input_records = split_->num_input_records;
    split_job_stats_->num_input_files =
        stats.num_input_files_in_non_output_levels +
        stats.num_input_files_in_output_level;


    // output information
    split_job_stats_->total_output_bytes = stats.bytes_written;
    split_job_stats_->num_output_records = split_->num_output_records;
    split_job_stats_->num_output_files = stats.num_output_files;

    if (split_->NumOutputFiles() > 0U) {
      CopyPrefix(split_->SmallestUserKey(),
                 SplitJobStats::kMaxPrefixLength,
                 &split_job_stats_->smallest_output_key_prefix);
      CopyPrefix(split_->LargestUserKey(),
                 SplitJobStats::kMaxPrefixLength,
                 &split_job_stats_->largest_output_key_prefix);
    }
  }
#else
  (void)stats;
#endif  // !ROCKSDB_LITE
}

void SplitJob::LogSplit() {
  Compaction* compaction = split_->compaction;
  ColumnFamilyData* cfd = compaction->column_family_data();

  // Let's check if anything will get logged. Don't prepare all the info if
  // we're not logging
  if (db_options_.info_log_level <= InfoLogLevel::INFO_LEVEL) {
    Compaction::InputLevelSummaryBuffer inputs_summary;
    ROCKS_LOG_INFO(
        db_options_.info_log, "[%s] [JOB %d] Splitting %s, score %.2f",
        cfd->GetName().c_str(), job_id_,
        compaction->InputLevelSummary(&inputs_summary), compaction->score());
    char scratch[2345];
    compaction->Summary(scratch, sizeof(scratch));
    ROCKS_LOG_INFO(db_options_.info_log, "[%s] Split start summary: %s\n",
                   cfd->GetName().c_str(), scratch);
    // build event logger report
    auto stream = event_logger_->Log();
    stream << "job" << job_id_ << "event"
           << "split_started"
           << "split_reason"
           << GetCompactionReasonString(compaction->compaction_reason());
    for (size_t i = 0; i < compaction->num_input_levels(); ++i) {
      stream << ("files_L" + ToString(compaction->level(i)));
      stream.StartArray();
      for (auto f : *compaction->inputs(i)) {
        stream << f->fd.GetNumber();
      }
      stream.EndArray();
    }
    stream << "score" << compaction->score() << "input_data_size"
           << compaction->CalculateTotalInputSize();
  }
}

}  // namespace rocksdb
