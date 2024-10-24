//  Copyright (c) 2011-present, Facebook, Inc.  All rights reserved.
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).
//
// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#include "db/builder.h"

#include <algorithm>
#include <deque>
#include <vector>
#include <inttypes.h>

#include "db/compaction_iterator.h"
#include "db/dbformat.h"
#include "db/event_helpers.h"
#include "db/internal_stats.h"
#include "db/merge_helper.h"
#include "db/range_del_aggregator.h"
#include "db/table_cache.h"
#include "db/version_edit.h"
#include "monitoring/iostats_context_imp.h"
#include "monitoring/thread_status_util.h"
#include "rocksdb/db.h"
#include "rocksdb/env.h"
#include "rocksdb/iterator.h"
#include "rocksdb/options.h"
#include "rocksdb/table.h"
#include "table/block_based_table_builder.h"
#include "table/format.h"
#include "table/internal_iterator.h"
#include "util/file_reader_writer.h"
#include "util/filename.h"
#include "util/stop_watch.h"
#include "util/sync_point.h"

namespace rocksdb {

class TableFactory;

TableBuilder* NewTableBuilder(
    const ImmutableCFOptions& ioptions, const MutableCFOptions& moptions,
    const InternalKeyComparator& internal_comparator,
    const std::vector<std::unique_ptr<IntTblPropCollectorFactory>>*
        int_tbl_prop_collector_factories,
    uint32_t column_family_id, const std::string& column_family_name,
    WritableFileWriter* file, const CompressionType compression_type,
    uint64_t sample_for_compression, const CompressionOptions& compression_opts,
    int level, const bool skip_filters, const uint64_t creation_time,
    const uint64_t oldest_key_time, const uint64_t target_file_size) {
  assert((column_family_id ==
          TablePropertiesCollectorFactory::Context::kUnknownColumnFamily) ==
         column_family_name.empty());
  return ioptions.table_factory->NewTableBuilder(
      TableBuilderOptions(ioptions, moptions, internal_comparator,
                          int_tbl_prop_collector_factories, compression_type,
                          sample_for_compression, compression_opts,
                          skip_filters, column_family_name, level,
                          creation_time, oldest_key_time, target_file_size),
      column_family_id, file);
}

Status BuildTable(
    const std::string& dbname, Env* env, const ImmutableCFOptions& ioptions,
    const MutableCFOptions& mutable_cf_options, const EnvOptions& env_options,
    TableCache* table_cache, InternalIterator* iter,
    std::vector<std::unique_ptr<FragmentedRangeTombstoneIterator>>
        range_del_iters,
    FileMetaData* meta, const InternalKeyComparator& internal_comparator,
    const std::vector<std::unique_ptr<IntTblPropCollectorFactory>>*
        int_tbl_prop_collector_factories,
    uint32_t column_family_id, const std::string& column_family_name,
    std::vector<SequenceNumber> snapshots,
    SequenceNumber earliest_write_conflict_snapshot,
    SnapshotChecker* snapshot_checker, const CompressionType compression,
    uint64_t sample_for_compression, const CompressionOptions& compression_opts,
    bool paranoid_file_checks, InternalStats* internal_stats,
    TableFileCreationReason reason, EventLogger* event_logger, int job_id,
    const Env::IOPriority io_priority, TableProperties* table_properties,
    int level, const uint64_t creation_time, const uint64_t oldest_key_time,
    Env::WriteLifeTimeHint write_hint) {
  assert((column_family_id ==
          TablePropertiesCollectorFactory::Context::kUnknownColumnFamily) ==
         column_family_name.empty());
  // Reports the IOStats for flush for every following bytes.
  const size_t kReportFlushIOStatsEvery = 1048576;
  Status s;
  meta->fd.file_size = 0;
  iter->SeekToFirst();
  std::unique_ptr<CompactionRangeDelAggregator> range_del_agg(
      new CompactionRangeDelAggregator(&internal_comparator, snapshots));
  for (auto& range_del_iter : range_del_iters) {
    range_del_agg->AddTombstones(std::move(range_del_iter));
  }

  std::string fname = TableFileName(ioptions.cf_paths, meta->fd.GetNumber(),
                                    meta->fd.GetPathId());
#ifndef ROCKSDB_LITE
  EventHelpers::NotifyTableFileCreationStarted(
      ioptions.listeners, dbname, column_family_name, fname, job_id, reason);
#endif  // !ROCKSDB_LITE
  TableProperties tp;

  if (iter->Valid() || !range_del_agg->IsEmpty()) {
    TableBuilder* builder;
    std::unique_ptr<WritableFileWriter> file_writer;
    // Currently we only enable dictionary compression during compaction to the
    // bottommost level.
    CompressionOptions compression_opts_for_flush(compression_opts);
    compression_opts_for_flush.max_dict_bytes = 0;
    compression_opts_for_flush.zstd_max_train_bytes = 0;
    {
      std::unique_ptr<WritableFile> file;
#ifndef NDEBUG
      bool use_direct_writes = env_options.use_direct_writes;
      TEST_SYNC_POINT_CALLBACK("BuildTable:create_file", &use_direct_writes);
#endif  // !NDEBUG
      s = NewWritableFile(env, fname, &file, env_options);
      if (!s.ok()) {
        EventHelpers::LogAndNotifyTableFileCreationFinished(
            event_logger, ioptions.listeners, dbname, column_family_name, fname,
            job_id, meta->fd, tp, reason, s);
        return s;
      }
      file->SetIOPriority(io_priority);
      file->SetWriteLifeTimeHint(write_hint);

      file_writer.reset(
          new WritableFileWriter(std::move(file), fname, env_options, env,
                                 ioptions.statistics, ioptions.listeners));
      builder = NewTableBuilder(
          ioptions, mutable_cf_options, internal_comparator,
          int_tbl_prop_collector_factories, column_family_id,
          column_family_name, file_writer.get(), compression,
          sample_for_compression, compression_opts_for_flush, level,
          false /* skip_filters */, creation_time, oldest_key_time);
    }

    MergeHelper merge(env, internal_comparator.user_comparator(),
                      ioptions.merge_operator, nullptr, ioptions.info_log,
                      true /* internal key corruption is not ok */,
                      snapshots.empty() ? 0 : snapshots.back(),
                      snapshot_checker);

    CompactionIterator c_iter(
        iter, internal_comparator.user_comparator(), &merge, kMaxSequenceNumber,
        &snapshots, earliest_write_conflict_snapshot, snapshot_checker, env,
        ShouldReportDetailedTime(env, ioptions.statistics),
        true /* internal key corruption is not ok */, range_del_agg.get());
    c_iter.SeekToFirst();

    
    //for (; c_iter.Valid(); c_iter.Next()) {
    for (; ;) {
      

      if (!c_iter.Valid()) {
        break;
      }


      const Slice& key = c_iter.key();
      //const Slice& value = c_iter.value();
      Slice value(c_iter.value());
      SequenceNumber sequence = c_iter.ikey().sequence;
      ParsedInternalKey uikey;
      ParseInternalKey(key, &uikey);
      //std::cout << "[f]add key : " << uikey.DebugString() << std::endl;
      std::string key_str_copy = key.ToString();
      //builder->Add(key, value);
      //meta->UpdateBoundaries(key, c_iter.ikey().sequence);

       
      // TODO(noetzli): Update stats after flush, too.
      //if (io_priority == Env::IO_HIGH &&
      //    IOSTATS(bytes_written) >= kReportFlushIOStatsEvery) {
      //  ThreadStatusUtil::SetThreadOperationProperty(
      //      ThreadStatus::FLUSH_BYTES_WRITTEN, IOSTATS(bytes_written));
      //}

      c_iter.Next();
      //uint64_t extra_key_put_cnt = c_iter.GetExtraKeyPutCnt();
      //std::cout << "[f]cur_key_put_cnt: " << uikey.put_cnt << std::endl;
      //std::cout << "[f]ext_key_put_cnt: " << extra_key_put_cnt << std::endl;
      // uint64_t cur_key_put_cnt = uikey.put_cnt + extra_key_put_cnt;
      uint64_t cur_key_put_cnt = 1;

      //std::cout << "[f]key: " << key.ToString() << std::endl;
      UpdateFlushCount(&key_str_copy, cur_key_put_cnt);
      UpdateCompactionCount(&key_str_copy, cur_key_put_cnt);
      //std::cout << "[f]cur_key_put_cnt(2): " << ExtractPutCount(key_str_copy) << std::endl;
      const Slice updated_key(key_str_copy);

      ParseInternalKey(updated_key, &uikey);
      //std::cout << "[f]add key(2) : " << uikey.DebugString() << std::endl;
      builder->Add(updated_key, value);
      meta->UpdateBoundaries(updated_key, sequence);

       
      // TODO(noetzli): Update stats after flush, too.
      if (io_priority == Env::IO_HIGH &&
          IOSTATS(bytes_written) >= kReportFlushIOStatsEvery) {
        ThreadStatusUtil::SetThreadOperationProperty(
            ThreadStatus::FLUSH_BYTES_WRITTEN, IOSTATS(bytes_written));
      }
    }

    auto range_del_it = range_del_agg->NewIterator();
    for (range_del_it->SeekToFirst(); range_del_it->Valid();
         range_del_it->Next()) {
      auto tombstone = range_del_it->Tombstone();
      auto kv = tombstone.Serialize();
      builder->Add(kv.first.Encode(), kv.second);
      meta->UpdateBoundariesForRange(kv.first, tombstone.SerializeEndKey(),
                                     tombstone.seq_, internal_comparator);
    }

    // Finish and check for builder errors
    tp = builder->GetTableProperties();


    bool empty = builder->NumEntries() == 0 && tp.num_range_deletions == 0;
    s = c_iter.status();
    if (!s.ok() || empty) {
      builder->Abandon();
    } else {
      s = builder->Finish();
    }

    if (s.ok() && !empty) {
      uint64_t file_size = builder->FileSize();
      meta->fd.file_size = file_size;
      meta->marked_for_compaction = builder->NeedCompact();
      assert(meta->fd.GetFileSize() > 0);
      tp = builder->GetTableProperties(); // refresh now that builder is finished
      if (table_properties) {
        *table_properties = tp;
      }
    }
    delete builder;

    // JH: apply smallest/largest key to tp
    //std::cout << "[f]smallest : " << meta->smallest.user_key().ToString() << std::endl;
    //std::cout << "[f]largest  : " << meta->largest.user_key().ToString() << std::endl;
    tp.smallest_user_key = meta->smallest.user_key().ToString();
    tp.largest_user_key = meta->largest.user_key().ToString();

    // Finish and check for file errors
    if (s.ok() && !empty) {
      StopWatch sw(env, ioptions.statistics, TABLE_SYNC_MICROS);
      s = file_writer->Sync(ioptions.use_fsync);
    }
    if (s.ok() && !empty) {
      s = file_writer->Close();
    }

    if (s.ok() && !empty) {
      // Verify that the table is usable
      // We set for_compaction to false and don't OptimizeForCompactionTableRead
      // here because this is a special case after we finish the table building
      // No matter whether use_direct_io_for_flush_and_compaction is true,
      // we will regrad this verification as user reads since the goal is
      // to cache it here for further user reads
      std::unique_ptr<InternalIterator> it(table_cache->NewIterator(
          ReadOptions(), env_options, internal_comparator, *meta,
          nullptr /* range_del_agg */,
          mutable_cf_options.prefix_extractor.get(), nullptr,
          (internal_stats == nullptr) ? nullptr
                                      : internal_stats->GetFileReadHist(0),
          false /* for_compaction */, nullptr /* arena */,
          false /* skip_filter */, level));
      s = it->status();
      if (s.ok() && paranoid_file_checks) {
        for (it->SeekToFirst(); it->Valid(); it->Next()) {
        }
        s = it->status();
      }
    }
  }

  // Check for input iterator errors
  if (!iter->status().ok()) {
    s = iter->status();
  }

  if (!s.ok() || meta->fd.GetFileSize() == 0) {
    env->DeleteFile(fname);
  }

  // Output to event logger and fire events.
  EventHelpers::LogAndNotifyTableFileCreationFinished(
      event_logger, ioptions.listeners, dbname, column_family_name, fname,
      job_id, meta->fd, tp, reason, s);

  return s;
}

Status BuildTables(
    const std::string& dbname, Env* env, const ImmutableCFOptions& ioptions,
    const MutableCFOptions& mutable_cf_options, const EnvOptions& env_options,
    TableCache* table_cache, InternalIterator* iter,
    std::vector<std::unique_ptr<FragmentedRangeTombstoneIterator>>
    range_del_iters,
    FileMetaData* meta, 
    const InternalKeyComparator& internal_comparator,
    const std::vector<std::unique_ptr<IntTblPropCollectorFactory>>*
        int_tbl_prop_collector_factories,
    uint32_t column_family_id, const std::string& column_family_name,
    std::vector<SequenceNumber> snapshots,
    SequenceNumber earliest_write_conflict_snapshot,
    SnapshotChecker* snapshot_checker, const CompressionType compression,
    uint64_t sample_for_compression, const CompressionOptions& compression_opts,
    bool paranoid_file_checks, InternalStats* internal_stats,
    TableFileCreationReason reason,
    std::vector<FileMetaData>& children_metas,
    std::vector<PartitionTreeNode*>& children_nodes,
    std::vector<TableProperties>& children_table_properties,
    EventLogger* event_logger, int job_id,
    const Env::IOPriority io_priority, TableProperties* table_properties,
    int level, const uint64_t creation_time, const uint64_t oldest_key_time,
    Env::WriteLifeTimeHint write_hint) {
  assert((column_family_id ==
          TablePropertiesCollectorFactory::Context::kUnknownColumnFamily) ==
         column_family_name.empty());
  assert(!children_metas.empty());
  assert(!children_nodes.empty());
  assert(children_nodes.size() == children_metas.size());
  size_t children_size = children_nodes.size();
  // Reports the IOStats for flush for every following bytes.
  const size_t kReportFlushIOStatsEvery = 1048576;
  Status s;
  meta->fd.file_size = 0;
  for (auto c_meta: children_metas) {
    c_meta.fd.file_size = 0;
  }
  iter->SeekToFirst();
  std::unique_ptr<CompactionRangeDelAggregator> range_del_agg(
      new CompactionRangeDelAggregator(&internal_comparator, snapshots));
  for (auto& range_del_iter : range_del_iters) {
    range_del_agg->AddTombstones(std::move(range_del_iter));
  }

  std::string fname = TableFileName(ioptions.cf_paths, meta->fd.GetNumber(),
                                    meta->fd.GetPathId());
  std::vector<std::string> children_fnames;
  for (auto c_meta: children_metas) {
    children_fnames.push_back(TableFileName(ioptions.cf_paths, c_meta.fd.GetNumber(),
                              c_meta.fd.GetPathId()));
  }
#ifndef ROCKSDB_LITE
  EventHelpers::NotifyTableFileCreationStarted(
      ioptions.listeners, dbname, column_family_name, fname, job_id, reason);
#endif  // !ROCKSDB_LITE
  TableProperties tp;

  if (iter->Valid() || !range_del_agg->IsEmpty()) {
    TableBuilder* builder;
    std::unique_ptr<WritableFileWriter> file_writer;
    // Currently we only enable dictionary compression during compaction to the
    // bottommost level.
    CompressionOptions compression_opts_for_flush(compression_opts);
    compression_opts_for_flush.max_dict_bytes = 0;
    compression_opts_for_flush.zstd_max_train_bytes = 0;
    {
      std::unique_ptr<WritableFile> file;
#ifndef NDEBUG
      bool use_direct_writes = env_options.use_direct_writes;
      TEST_SYNC_POINT_CALLBACK("BuildTable:create_file", &use_direct_writes);
#endif  // !NDEBUG
      s = NewWritableFile(env, fname, &file, env_options);
      if (!s.ok()) {
        EventHelpers::LogAndNotifyTableFileCreationFinished(
            event_logger, ioptions.listeners, dbname, column_family_name, fname,
            job_id, meta->fd, tp, reason, s);
        return s;
      }
      file->SetIOPriority(io_priority);
      file->SetWriteLifeTimeHint(write_hint);

      file_writer.reset(
          new WritableFileWriter(std::move(file), fname, env_options, env,
                                 ioptions.statistics, ioptions.listeners));
      builder = NewTableBuilder(
          ioptions, mutable_cf_options, internal_comparator,
          int_tbl_prop_collector_factories, column_family_id,
          column_family_name, file_writer.get(), compression,
          sample_for_compression, compression_opts_for_flush, level,
          false /* skip_filters */, creation_time, oldest_key_time);
    }
    // JH: set table builder for children nodes
    TableBuilder* children_builders[children_size];
    std::unique_ptr<WritableFileWriter> children_file_writers[children_size];
    {
      std::unique_ptr<WritableFile> children_files[children_size];
      for(size_t i = 0; i < children_size; i++) {
        s = NewWritableFile(env, children_fnames[i], &children_files[i], env_options);  
        if (!s.ok()) {
          EventHelpers::LogAndNotifyTableFileCreationFinished(
            event_logger, ioptions.listeners, dbname, children_nodes[i]->cfd_->GetName(),
            children_fnames[i],
            job_id, children_metas[i].fd, tp, reason, s);
          return s;
        }
        children_files[i]->SetIOPriority(io_priority);
        children_files[i]->SetWriteLifeTimeHint(write_hint);

        children_file_writers[i].reset(
            new WritableFileWriter(std::move(children_files[i]), children_fnames[i],
                                   env_options, env,
                                   ioptions.statistics, ioptions.listeners));

        children_builders[i] = NewTableBuilder(
            ioptions, mutable_cf_options, internal_comparator,
            int_tbl_prop_collector_factories, children_nodes[i]->cfd_->GetID(),
            children_nodes[i]->cfd_->GetName(), children_file_writers[i].get(), compression,
            sample_for_compression, compression_opts_for_flush, level,
            false /* skip_filters */, creation_time, oldest_key_time);
      }
    }

    MergeHelper merge(env, internal_comparator.user_comparator(),
                      ioptions.merge_operator, nullptr, ioptions.info_log,
                      true /* internal key corruption is not ok */,
                      snapshots.empty() ? 0 : snapshots.back(),
                      snapshot_checker);

    // JH: choose builder to add by corresponding keys
    size_t child_idx = 0; // -1 if parent, child_idx if child

    CompactionIterator c_iter(
        iter, internal_comparator.user_comparator(), &merge, kMaxSequenceNumber,
        &snapshots, earliest_write_conflict_snapshot, snapshot_checker, env,
        ShouldReportDetailedTime(env, ioptions.statistics),
        true /* internal key corruption is not ok */, range_del_agg.get());

    c_iter.SeekToFirst();

    for (; c_iter.Valid(); ) {
      const Slice& key = c_iter.key();
      const Slice& value = c_iter.value();
      bool to_parent = true;
      Slice c_user_key = c_iter.user_key();
      while (child_idx < children_nodes.size()) {
        if (c_user_key.compare(get_lmost_key(children_nodes[child_idx])) >= 0 &&
            c_user_key.compare(get_rmost_key(children_nodes[child_idx])) <= 0)
        {
          to_parent = false;
          break; 
        }
        else if(c_user_key.compare(get_lmost_key(children_nodes[child_idx])) < 0) {
          // to_parent = true
          break;
        }
        else {
          // increment idx
          child_idx ++;
        }
      }
      if (to_parent) { // parent
        builder->Add(key, value);
        meta->UpdateBoundaries(key, c_iter.ikey().sequence);  
      }
      else { // child
        assert(child_idx < children_nodes.size());
        children_builders[child_idx]->Add(key, value);
        children_metas[child_idx].UpdateBoundaries(key, c_iter.ikey().sequence);  
      }
      
      // TODO(noetzli): Update stats after flush, too.
      if (io_priority == Env::IO_HIGH &&
          IOSTATS(bytes_written) >= kReportFlushIOStatsEvery) {
        ThreadStatusUtil::SetThreadOperationProperty(
            ThreadStatus::FLUSH_BYTES_WRITTEN, IOSTATS(bytes_written));
      }

      c_iter.Next();
    }



    // TODO(Junhan): Consider adding rangedel tombstone when split-then-flush
    auto range_del_it = range_del_agg->NewIterator();
    for (range_del_it->SeekToFirst(); range_del_it->Valid();
         range_del_it->Next()) {
      auto tombstone = range_del_it->Tombstone();
      auto kv = tombstone.Serialize();
      builder->Add(kv.first.Encode(), kv.second);
      meta->UpdateBoundariesForRange(kv.first, tombstone.SerializeEndKey(),
                                     tombstone.seq_, internal_comparator);
    }

    // Finish and check for builder(JH: including children builders) errors
    tp = builder->GetTableProperties();
    bool empty = builder->NumEntries() == 0 && tp.num_range_deletions == 0;
    ROCKS_LOG_INFO(ioptions.info_log, "[%s] FlushJob: parent's num_entries : %ld",
                   column_family_name.c_str(), builder->NumEntries());
    bool children_empty[children_size];
    for (size_t i=0; i<children_size; i++) {
      children_empty[i] = children_builders[i]->NumEntries() == 0;// JH: Skip considering rangedel
      ROCKS_LOG_INFO(ioptions.info_log, "[%s] FlushJob: child[%ld]'s num_entries : %ld",
                     children_nodes[i]->cfd_->GetName().c_str(),
                     i, children_builders[i]->NumEntries());
    }
    Status iter_s = c_iter.status();
    s = c_iter.status();
    if (!iter_s.ok() || empty) {
      builder->Abandon();
    } else {
      s = builder->Finish();
    }
    if (s.ok() && !empty) {
      uint64_t file_size = builder->FileSize();
      meta->fd.file_size = file_size;
      meta->marked_for_compaction = builder->NeedCompact();
      assert(meta->fd.GetFileSize() > 0);
      tp = builder->GetTableProperties(); // refresh now that builder is finished
      if (table_properties) {
        *table_properties = tp;
      }
    }
    delete builder;

    for(size_t i=0; i<children_size; i++) {
      s = iter_s;
      if (!iter_s.ok() || children_empty[i]) {
        children_builders[i]->Abandon(); 
      } else {
        s = children_builders[i]->Finish(); 
      }
      if (s.ok() && !children_empty[i]) {
        uint64_t file_size = children_builders[i]->FileSize();
        children_metas[i].fd.file_size = file_size;
        children_metas[i].marked_for_compaction = children_builders[i]->NeedCompact();
        assert(children_metas[i].fd.GetFileSize() > 0);
        children_table_properties[i] = children_builders[i]->GetTableProperties(); // refresh not that builder is finished
      }
      delete children_builders[i];
    }

    // Finish and check for file errors
    if (s.ok() && !empty) {
      StopWatch sw(env, ioptions.statistics, TABLE_SYNC_MICROS);
      s = file_writer->Sync(ioptions.use_fsync);
    }
    if (s.ok() && !empty) {
      s = file_writer->Close();
    }
    // Finish and check for file errors
    for(size_t i=0; i<children_size; i++) {
      if (s.ok() && !children_empty[i]) {
        StopWatch sw(env, ioptions.statistics, TABLE_SYNC_MICROS);
        s = children_file_writers[i]->Sync(ioptions.use_fsync);
      }
      if (s.ok() && !empty) {
        s = children_file_writers[i]->Close();
      }  
    }

    if (s.ok() && !empty) {
      // Verify that the table is usable
      // We set for_compaction to false and don't OptimizeForCompactionTableRead
      // here because this is a special case after we finish the table building
      // No matter whether use_direct_io_for_flush_and_compaction is true,
      // we will regrad this verification as user reads since the goal is
      // to cache it here for further user reads
      std::unique_ptr<InternalIterator> it(table_cache->NewIterator(
          ReadOptions(), env_options, internal_comparator, *meta,
          nullptr /* range_del_agg */,
          mutable_cf_options.prefix_extractor.get(), nullptr,
          (internal_stats == nullptr) ? nullptr
                                      : internal_stats->GetFileReadHist(0),
          false /* for_compaction */, nullptr /* arena */,
          false /* skip_filter */, level));
      s = it->status();
      if (s.ok() && paranoid_file_checks) {
        for (it->SeekToFirst(); it->Valid(); it->Next()) {
        }
        s = it->status();
      }
    }
    for (size_t i=0; i<children_size; i++) {
      if (s.ok() && !children_empty[i]) {
        // Verify that the table is usable
        // We set for_compaction to false and don't OptimizeForCompactionTableRead
        // here because this is a special case after we finish the table building
        // No matter whether use_direct_io_for_flush_and_compaction is true,
        // we will regrad this verification as user reads since the goal is
        // to cache it here for further user reads
        std::unique_ptr<InternalIterator> it(table_cache->NewIterator(
              ReadOptions(), env_options, internal_comparator, children_metas[i],
              nullptr /* range_del_agg */,
              mutable_cf_options.prefix_extractor.get(), nullptr,
              (internal_stats == nullptr) ? nullptr
              : internal_stats->GetFileReadHist(0),
              false /* for_compaction */, nullptr /* arena */,
              false /* skip_filter */, level));
        s = it->status();
        if (s.ok() && paranoid_file_checks) {
          for (it->SeekToFirst(); it->Valid(); it->Next()) {
          }
          s = it->status();
        }
      }  
    }
  }

  // Check for input iterator errors
  if (!iter->status().ok()) {
    s = iter->status();
  }

  if (!s.ok() || meta->fd.GetFileSize() == 0) {
    env->DeleteFile(fname);
  }
  for (size_t i=0; i<children_size; i++) {
    if (!s.ok() || children_metas[i].fd.GetFileSize() == 0) {
      env->DeleteFile(children_fnames[i]); 
    } 
  }

  // Output to event logger and fire events.
  // TODO(JH): TableProperties tp needs to reflect the characteristics of child builders,
  // not only parent builder.
  EventHelpers::LogAndNotifyTableFileCreationFinished(
      event_logger, ioptions.listeners, dbname, column_family_name, fname,
      job_id, meta->fd, *table_properties, reason, s);
  for(size_t i=0; i<children_size; i++) {
    EventHelpers::LogAndNotifyTableFileCreationFinished(
        event_logger, ioptions.listeners, dbname, children_nodes[i]->cfd_->GetName(), children_fnames[i],
        job_id, children_metas[i].fd, children_table_properties[i], reason, s);
  }

  return s;
}

Status BuildParentTable(
    const std::string& dbname, Env* env, const ImmutableCFOptions& ioptions,
    const MutableCFOptions& mutable_cf_options, const EnvOptions& env_options,
    TableCache* table_cache, std::vector<ScopedArenaIterator*> iters,
    std::vector<std::unique_ptr<FragmentedRangeTombstoneIterator>>
    range_del_iters,
    FileMetaData* meta, 
    const InternalKeyComparator& internal_comparator,
    const std::vector<std::unique_ptr<IntTblPropCollectorFactory>>*
        int_tbl_prop_collector_factories,
    uint32_t column_family_id, const std::string& column_family_name,
    std::vector<SequenceNumber> snapshots,
    SequenceNumber earliest_write_conflict_snapshot,
    SnapshotChecker* snapshot_checker, const CompressionType compression,
    uint64_t sample_for_compression, const CompressionOptions& compression_opts,
    bool paranoid_file_checks, InternalStats* internal_stats,
    TableFileCreationReason reason,
    std::vector<PartitionTreeNode*>& children_nodes,
    EventLogger* event_logger, int job_id,
    const Env::IOPriority io_priority, TableProperties* table_properties,
    int level, const uint64_t creation_time, const uint64_t oldest_key_time,
    Env::WriteLifeTimeHint write_hint, 
    Slice start, Slice end,
		std::vector<Slice> sub_starts, std::vector<Slice> sub_ends) {
  assert((column_family_id ==
          TablePropertiesCollectorFactory::Context::kUnknownColumnFamily) ==
         column_family_name.empty());

  size_t children_size = children_nodes.size();

  ROCKS_LOG_INFO(ioptions.info_log, "[JOB %d] [%s] BuildParentTable start [%s, %s]",
      job_id,
      column_family_name.c_str(),
      start.ToString().c_str(),
      end.ToString().c_str());
  // Reports the IOStats for flush for every following bytes.
  const size_t kReportFlushIOStatsEvery = 1048576;
  Status s;
  meta->fd.file_size = 0;


  std::vector <IterKey> start_iter(children_size+1);
  for (size_t i = 0; i < children_size + 1; i++) {
    InternalIterator* iter = iters[i]->get();
    if (i == 0) {
      if (column_family_id == 0) { // root node
        iter->SeekToFirst();
      } else { // internal node
        start_iter[i].SetInternalKey(start, kMaxSequenceNumber, kValueTypeForSeek);
        iter->Seek(start_iter[i].GetInternalKey());
      }
      continue;
    }
    ROCKS_LOG_INFO(ioptions.info_log, "BuildParentTable() end key : %s", sub_ends[i-1].ToString().c_str());
    start_iter[i].SetInternalKey(sub_ends[i-1], kMaxSequenceNumber, kValueTypeForSeek);
    ROCKS_LOG_INFO(ioptions.info_log, "BuildParentTable() start iter key : %s", start_iter[i].GetInternalKey().ToString().c_str());
    iter->Seek(start_iter[i].GetInternalKey());
    ROCKS_LOG_INFO(ioptions.info_log, "BuildParentTable() seek iter key : %s", iter->key().ToString().c_str());
  }
	bool iter_valid = true;
  /*
	for (size_t i = 0; i < children_size + 1; i++) {
		InternalIterator* iter = iters[i]->get();
		if (!iter->Valid()) {
      ROCKS_LOG_INFO(ioptions.info_log, "[JOB %d] BuildParentTable() iters(%ld) is not valid", job_id, i);
			iter_valid = false;
			break;
		}
	}*/

  std::unique_ptr<CompactionRangeDelAggregator> range_del_agg(
      new CompactionRangeDelAggregator(&internal_comparator, snapshots));

	//TODO: Have to change this... koo..
  for (auto& range_del_iter : range_del_iters) {
    range_del_agg->AddTombstones(std::move(range_del_iter));
  }

  std::string fname = TableFileName(ioptions.cf_paths, meta->fd.GetNumber(),
                                    meta->fd.GetPathId());
#ifndef ROCKSDB_LITE
  EventHelpers::NotifyTableFileCreationStarted(
      ioptions.listeners, dbname, column_family_name, fname, job_id, reason);
#endif  // !ROCKSDB_LITE
  TableProperties tp;

  if (iter_valid || !range_del_agg->IsEmpty()) {
    TableBuilder* builder;
    std::unique_ptr<WritableFileWriter> file_writer;
    // Currently we only enable dictionary compression during compaction to the
    // bottommost level.
    CompressionOptions compression_opts_for_flush(compression_opts);
    compression_opts_for_flush.max_dict_bytes = 0;
    compression_opts_for_flush.zstd_max_train_bytes = 0;
    {
      std::unique_ptr<WritableFile> file;
#ifndef NDEBUG
      bool use_direct_writes = env_options.use_direct_writes;
      TEST_SYNC_POINT_CALLBACK("BuildTable:create_file", &use_direct_writes);
#endif  // !NDEBUG
      s = NewWritableFile(env, fname, &file, env_options);
      if (!s.ok()) {
        EventHelpers::LogAndNotifyTableFileCreationFinished(
            event_logger, ioptions.listeners, dbname, column_family_name, fname,
            job_id, meta->fd, tp, reason, s);
        return s;
      }
      file->SetIOPriority(io_priority);
      file->SetWriteLifeTimeHint(write_hint);

      file_writer.reset(
          new WritableFileWriter(std::move(file), fname, env_options, env,
                                 ioptions.statistics, ioptions.listeners));
      builder = NewTableBuilder(
          ioptions, mutable_cf_options, internal_comparator,
          int_tbl_prop_collector_factories, column_family_id,
          column_family_name, file_writer.get(), compression,
          sample_for_compression, compression_opts_for_flush, level,
          false /* skip_filters */, creation_time, oldest_key_time);
    }


    MergeHelper merge(env, internal_comparator.user_comparator(),
                      ioptions.merge_operator, nullptr, ioptions.info_log,
                      true /* internal key corruption is not ok */,
                      snapshots.empty() ? 0 : snapshots.back(),
                      snapshot_checker);

    // JH: choose builder to add by corresponding keys
    //size_t child_idx = 0; // -1 if parent, child_idx if child

		std::vector<CompactionIterator*> c_iters;


		for (size_t i = 0; i < children_size + 1; i++) {
			InternalIterator* iter = iters[i]->get();
			
			CompactionIterator* c_iter = new CompactionIterator(
        iter, internal_comparator.user_comparator(), &merge, kMaxSequenceNumber,
        &snapshots, earliest_write_conflict_snapshot, snapshot_checker, env,
        ShouldReportDetailedTime(env, ioptions.statistics),
        true /* internal key corruption is not ok */, range_del_agg.get());
			c_iter->SeekToFirst();
      // JH : c_iters[i] needs to exclude sub_ends, except for i != 0
      if (i != 0 && c_iter->Valid()) {
        c_iter->Next(); 
      }
      ROCKS_LOG_INFO(ioptions.info_log, "[JOB %d] [%s] BuildParentTable %ld key start : %s",
          job_id,
          column_family_name.c_str(),
          i, c_iter->user_key().ToString().c_str());

		  c_iters.push_back(c_iter);
		}

		for (size_t i = 0; i < children_size + 1; i++) {
      ROCKS_LOG_INFO(ioptions.info_log, "[JOB %d] [%s] BuildParentTable %ld-th over %ld staggered scan",
          job_id,
          column_family_name.c_str(),
          i, children_size+1);

			for (;;) {
				if (!c_iters[i]->Valid()) {
          ROCKS_LOG_WARN(ioptions.info_log, "[JOB %d] BuildParentTable c_iter[%ld] is no longer valid",
                         job_id, i);
					break;
				}
				const Slice& key = c_iters[i]->key();
				const Slice& value = c_iters[i]->value();
				Slice user_key = c_iters[i]->user_key();

        // boundary check
        if (i == children_size && !end.empty() && user_key.compare(end) > 0) {
          break;
        }

				if (i == children_size || 
						user_key.compare(sub_starts[i]) < 0) {
          builder->Add(key, value);
          ROCKS_LOG_WARN(ioptions.info_log, "[JOB %d] BuildParentTable c_iter[%ld] add key : %s",
              job_id, i, user_key.ToString().c_str());

          //std::cout <<  "[" << column_family_name << "] int add : " << user_key_str << std::endl;
					meta->UpdateBoundaries(key, c_iters[i]->ikey().sequence);  
				} else {
					break;
				}
				// TODO(noetzli): Update stats after flush, too.
				if (io_priority == Env::IO_HIGH &&
						IOSTATS(bytes_written) >= kReportFlushIOStatsEvery) {
					ThreadStatusUtil::SetThreadOperationProperty(
							ThreadStatus::FLUSH_BYTES_WRITTEN, IOSTATS(bytes_written));
				}
				c_iters[i]->Next();
			}
		}

    // TODO(Junhan): Consider adding rangedel tombstone when split-then-flush
    auto range_del_it = range_del_agg->NewIterator();
    for (range_del_it->SeekToFirst(); range_del_it->Valid();
         range_del_it->Next()) {
      auto tombstone = range_del_it->Tombstone();
      auto kv = tombstone.Serialize();
      builder->Add(kv.first.Encode(), kv.second);
      meta->UpdateBoundariesForRange(kv.first, tombstone.SerializeEndKey(),
                                     tombstone.seq_, internal_comparator);
    }

    // Finish and check for builder(JH: including children builders) errors
    tp = builder->GetTableProperties();
    bool empty = builder->NumEntries() == 0 && tp.num_range_deletions == 0;
    ROCKS_LOG_INFO(ioptions.info_log, "[%s] FlushJob: parent's num_entries : %ld",
                   column_family_name.c_str(), builder->NumEntries());

		bool finish_builder = true;
		bool refresh_builder = true;
		bool file_sync = true;
		bool file_close = true;
		for (size_t i = 0; i < children_size + 1; i++) {
			Status iter_s = c_iters[i]->status();
			s = c_iters[i]->status();

			if (!(!iter_s.ok() || empty)) {
				finish_builder = false;
			}

			if (!(s.ok() && !empty)) {
				refresh_builder = false;
			}

			// Finish and check for file errors
			if (!(s.ok() && !empty)) {
				file_sync = false;
			}

			if (!(s.ok() && !empty)) {
				file_close = false;
			}
		}

		if (finish_builder) {
			builder->Abandon();
		} else {
			s = builder->Finish();
		}

		if (refresh_builder) {
			uint64_t file_size = builder->FileSize();
			meta->fd.file_size = file_size;
			meta->marked_for_compaction = builder->NeedCompact();
			assert(meta->fd.GetFileSize() > 0);
			tp = builder->GetTableProperties(); // refresh now that builder is finished
			if (table_properties) {
				*table_properties = tp;
			}
		}
		delete builder;

		if (file_sync) {
			StopWatch sw(env, ioptions.statistics, TABLE_SYNC_MICROS);
			s = file_writer->Sync(ioptions.use_fsync);
		}

		if (file_close) {
			s = file_writer->Close();
		}




    if (refresh_builder) {
      // Verify that the table is usable
      // We set for_compaction to false and don't OptimizeForCompactionTableRead
      // here because this is a special case after we finish the table building
      // No matter whether use_direct_io_for_flush_and_compaction is true,
      // we will regrad this verification as user reads since the goal is
      // to cache it here for further user reads
      std::unique_ptr<InternalIterator> it(table_cache->NewIterator(
          ReadOptions(), env_options, internal_comparator, *meta,
          nullptr /* range_del_agg */,
          mutable_cf_options.prefix_extractor.get(), nullptr,
          (internal_stats == nullptr) ? nullptr
                                      : internal_stats->GetFileReadHist(0),
          false /* for_compaction */, nullptr /* arena */,
          false /* skip_filter */, level));
      s = it->status();
      if (s.ok() && paranoid_file_checks) {
        for (it->SeekToFirst(); it->Valid(); it->Next()) {
        }
        s = it->status();
      }
    }
  }

  // Check for input iterator errors
	for (size_t i = 0; i < children_size + 1; i++) {
		InternalIterator* iter = iters[i]->get();
		if (!iter->status().ok()) {
			s = iter->status();
		}
		if (!s.ok() || meta->fd.GetFileSize() == 0) {
			env->DeleteFile(fname);
		}
	}


  // Output to event logger and fire events.
  // TODO(JH): TableProperties tp needs to reflect the characteristics of child builders,
  // not only parent builder.
  EventHelpers::LogAndNotifyTableFileCreationFinished(
      event_logger, ioptions.listeners, dbname, column_family_name, fname,
      job_id, meta->fd, tp, reason, s);

  return s;
}

Status BuildsubTable(
    const std::string& dbname, Env* env, const ImmutableCFOptions& ioptions,
    const MutableCFOptions& mutable_cf_options, const EnvOptions& env_options,
    TableCache* table_cache, InternalIterator* iter,
    std::vector<std::unique_ptr<FragmentedRangeTombstoneIterator>>
        range_del_iters,
    FileMetaData* meta, const InternalKeyComparator& internal_comparator,
    const std::vector<std::unique_ptr<IntTblPropCollectorFactory>>*
        int_tbl_prop_collector_factories,
    uint32_t column_family_id, const std::string& column_family_name,
    std::vector<SequenceNumber> snapshots,
    SequenceNumber earliest_write_conflict_snapshot,
    SnapshotChecker* snapshot_checker, const CompressionType compression,
    uint64_t sample_for_compression, const CompressionOptions& compression_opts,
    bool paranoid_file_checks, InternalStats* internal_stats,
    TableFileCreationReason reason, EventLogger* event_logger, int job_id,
    const Env::IOPriority io_priority, TableProperties* table_properties,
    int level, const uint64_t creation_time, const uint64_t oldest_key_time,
    Env::WriteLifeTimeHint write_hint, Slice sub_flush_start, Slice sub_flush_end, 
    int sub_flush_id) {
  assert((column_family_id ==
        TablePropertiesCollectorFactory::Context::kUnknownColumnFamily) ==
      column_family_name.empty());
  (void)sub_flush_id; /* unused */
  // Reports the IOStats for flush for every following bytes.
  const size_t kReportFlushIOStatsEvery = 1048576;
  Status s;
  meta->fd.file_size = 0;

  // WOW
  if (!sub_flush_start.empty()) {
    iter->SeekToFirst();
  } else {
    IterKey start_iter;
    start_iter.SetInternalKey(sub_flush_start, kMaxSequenceNumber, kValueTypeForSeek);
    iter->Seek(start_iter.GetInternalKey());
  }


  std::unique_ptr<CompactionRangeDelAggregator> range_del_agg(
      new CompactionRangeDelAggregator(&internal_comparator, snapshots));



  for (auto& range_del_iter : range_del_iters) {
    range_del_agg->AddTombstones(std::move(range_del_iter));
  }


  std::string fname = TableFileName(ioptions.cf_paths, meta->fd.GetNumber(),
      meta->fd.GetPathId());
#ifndef ROCKSDB_LITE
  EventHelpers::NotifyTableFileCreationStarted(
      ioptions.listeners, dbname, column_family_name, fname, job_id, reason);
#endif  // !ROCKSDB_LITE
  TableProperties tp;


  //std::cout << "BuildsubTable() " << job_id << " " << sub_flush_id  << " iter->Valid() "
  //	  << iter->Valid() << " range_del_agg->IsEmpty() " << range_del_agg->IsEmpty() << std::endl;

  if (iter->Valid() || !range_del_agg->IsEmpty()) {
    TableBuilder* builder;
    std::unique_ptr<WritableFileWriter> file_writer;
    // Currently we only enable dictionary compression during compaction to the
    // bottommost level.
    CompressionOptions compression_opts_for_flush(compression_opts);
    compression_opts_for_flush.max_dict_bytes = 0;
    compression_opts_for_flush.zstd_max_train_bytes = 0;
    {
      std::unique_ptr<WritableFile> file;
#ifndef NDEBUG
      bool use_direct_writes = env_options.use_direct_writes;
      TEST_SYNC_POINT_CALLBACK("BuildTable:create_file", &use_direct_writes);
#endif  // !NDEBUG
      s = NewWritableFile(env, fname, &file, env_options);
      if (!s.ok()) {
        EventHelpers::LogAndNotifyTableFileCreationFinished(
            event_logger, ioptions.listeners, dbname, column_family_name, fname,
            job_id, meta->fd, tp, reason, s);
        return s;
      }
      file->SetIOPriority(io_priority);
      file->SetWriteLifeTimeHint(write_hint);

      file_writer.reset(
          new WritableFileWriter(std::move(file), fname, env_options, env,
            ioptions.statistics, ioptions.listeners));
      builder = NewTableBuilder(
          ioptions, mutable_cf_options, internal_comparator,
          int_tbl_prop_collector_factories, column_family_id,
          column_family_name, file_writer.get(), compression,
          sample_for_compression, compression_opts_for_flush, level,
          false /* skip_filters */, creation_time, oldest_key_time);
    }

    MergeHelper merge(env, internal_comparator.user_comparator(),
        ioptions.merge_operator, nullptr, ioptions.info_log,
        true /* internal key corruption is not ok */,
        snapshots.empty() ? 0 : snapshots.back(),
        snapshot_checker);

    CompactionIterator c_iter(
        iter, internal_comparator.user_comparator(), &merge, kMaxSequenceNumber,
        &snapshots, earliest_write_conflict_snapshot, snapshot_checker, env,
        ShouldReportDetailedTime(env, ioptions.statistics),
        true /* internal key corruption is not ok */, range_del_agg.get());

    /* Made by Kyoungho Koo
     * : for multi-threaded split-then-flush
     */
    c_iter.SeekToFirst();

    //bool first = true;
    for (;;) {


      if (!c_iter.Valid()) {
        break;
      }


      const Slice& key = c_iter.key();
      const Slice& value = c_iter.value();

      Slice user_key = c_iter.user_key();

      /*
         if (first) {
         std::cout << "BuildsubTable() "<< user_key << std::endl;
         first = false;
         }
       */


      if (!sub_flush_start.empty() && user_key.compare(sub_flush_start) < 0) {
        c_iter.Next();
        continue;
      }

      if (!sub_flush_end.empty() && user_key.compare(sub_flush_end) > 0) {
        /*
           fprintf(stdout, "BuildsubTable() [bigger than end %d] job_id %d sub_flush_id %d sub_flush_start %s sub_flush_end %s user_key %s \n", 
           user_key.compare(sub_flush_end), job_id, sub_flush_id, sub_flush_start.c_str(), sub_flush_end.c_str(), user_key.c_str());
         */
        break;
      }

      //std::cout <<  "[" << column_family_name << "] sub add : " << user_key << std::endl;
      builder->Add(key, value);
      meta->UpdateBoundaries(key, c_iter.ikey().sequence);

      // TODO(noetzli): Update stats after flush, too.
      if (io_priority == Env::IO_HIGH &&
          IOSTATS(bytes_written) >= kReportFlushIOStatsEvery) {
        ThreadStatusUtil::SetThreadOperationProperty(
            ThreadStatus::FLUSH_BYTES_WRITTEN, IOSTATS(bytes_written));
      }
      c_iter.Next();

    }

    auto range_del_it = range_del_agg->NewIterator();
    for (range_del_it->SeekToFirst(); range_del_it->Valid();
        range_del_it->Next()) {
      auto tombstone = range_del_it->Tombstone();
      auto kv = tombstone.Serialize();
      builder->Add(kv.first.Encode(), kv.second);
      meta->UpdateBoundariesForRange(kv.first, tombstone.SerializeEndKey(),
          tombstone.seq_, internal_comparator);
    }


    // Finish and check for builder errors
    tp = builder->GetTableProperties();
    // JH: apply smallest/largest key to tp
    tp.smallest_user_key = meta->smallest.user_key().ToString();
    tp.largest_user_key = meta->largest.user_key().ToString();

    bool empty = builder->NumEntries() == 0 && tp.num_range_deletions == 0;
    s = c_iter.status();
    if (!s.ok() || empty) {
      builder->Abandon();
    } else {
      s = builder->Finish();
    }


    if (s.ok() && !empty) {
      uint64_t file_size = builder->FileSize();
      meta->fd.file_size = file_size;
      meta->marked_for_compaction = builder->NeedCompact();
      assert(meta->fd.GetFileSize() > 0);
      tp = builder->GetTableProperties(); // refresh now that builder is finished
      if (table_properties) {
        *table_properties = tp;
      }
    }
    delete builder;

    // Finish and check for file errors
    if (s.ok() && !empty) {
      StopWatch sw(env, ioptions.statistics, TABLE_SYNC_MICROS);
      s = file_writer->Sync(ioptions.use_fsync);
    }
    if (s.ok() && !empty) {
      s = file_writer->Close();
    }


    if (s.ok() && !empty) {
      // Verify that the table is usable
      // We set for_compaction to false and don't OptimizeForCompactionTableRead
      // here because this is a special case after we finish the table building
      // No matter whether use_direct_io_for_flush_and_compaction is true,
      // we will regrad this verification as user reads since the goal is
      // to cache it here for further user reads
      std::unique_ptr<InternalIterator> it(table_cache->NewIterator(
            ReadOptions(), env_options, internal_comparator, *meta,
            nullptr /* range_del_agg */,
            mutable_cf_options.prefix_extractor.get(), nullptr,
            (internal_stats == nullptr) ? nullptr
            : internal_stats->GetFileReadHist(0),
            false /* for_compaction */, nullptr /* arena */,
            false /* skip_filter */, level));
      s = it->status();
      if (s.ok() && paranoid_file_checks) {
        for (it->SeekToFirst(); it->Valid(); it->Next()) {
        }
        s = it->status();
      }
    }
  }

  //  std::cout << "BuildsubTable() " << job_id << " " << sub_flush_id  << " line 4"<< std::endl;
  // Check for input iterator errors
  if (!iter->status().ok()) {
    s = iter->status();
  }

  if (!s.ok() || meta->fd.GetFileSize() == 0) {
    env->DeleteFile(fname);
  }

  // Output to event logger and fire events.
  EventHelpers::LogAndNotifyTableFileCreationFinished(
      event_logger, ioptions.listeners, dbname, column_family_name, fname,
      job_id, meta->fd, tp, reason, s);

  return s;
}
}  // namespace rocksdb
