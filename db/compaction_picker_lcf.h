//  Copyright (c) 2011-present, Facebook, Inc.  All rights reserved.
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).
//
// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#pragma once
#ifndef ROCKSDB_LITE

#include "db/compaction_picker.h"

namespace rocksdb {
class LCFCompactionPicker : public CompactionPicker {
 public:
  LCFCompactionPicker(const ImmutableCFOptions& ioptions,
                            const InternalKeyComparator* icmp)
      : CompactionPicker(ioptions, icmp) {}
  virtual Compaction* PickCompaction(const std::string& cf_name,
                                     const MutableCFOptions& mutable_cf_options,
                                     VersionStorageInfo* vstorage,
                                     LogBuffer* log_buffer) override;

  virtual InterCFCompaction* PickInterCFCompaction(const std::string& cf_name,
                                     const MutableCFOptions& mutable_cf_options,
                                     VersionStorageInfo* vstorage,
                                     VersionStorageInfo* parent_vstorage,
                                     LogBuffer* log_buffer,
                                     CompactionPicker* parent_picker) override;


  virtual int MaxOutputLevel() const override { return NumberLevels() - 1; }

  virtual bool NeedsCompaction(
      const VersionStorageInfo* vstorage) const override;

 private:
  // Pick a path ID to place a newly generated file, with its estimated file
  // size.
  static uint32_t GetPathId(const ImmutableCFOptions& ioptions,
                            const MutableCFOptions& mutable_cf_options,
                            uint64_t file_size);
};
}  // namespace rocksdb
#endif  // !ROCKSDB_LITE
