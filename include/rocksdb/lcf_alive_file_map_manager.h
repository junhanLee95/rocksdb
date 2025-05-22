//  Copyright (c) 2011-present, Facebook, Inc.  All rights reserved.
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).

#pragma once

#include <memory>
#include <string>
#include <iostream>
#include <unordered_map>
#include <vector>
#include "util/mutexlock.h"
#include "port/port.h"

namespace rocksdb {

// LCF: tracking alive flies across multiple column families
class LCFAliveFileMapManager {
  public:
    LCFAliveFileMapManager() {
      std::cout << "Init LCFAliveFileMapManager\n";
    };

    void PrintAliveFiles(std::string msg) {
      ReadLock rl(&lcf_alive_file_mutex_);
      std::cout <<">>>" << msg << ">>>" <<std::endl;
      for (auto& files: lcf_alive_file_map_) {
        std::cout << "file["<< files.first <<"] : " << files.second << std::endl;
      }
      std::cout <<"<<<" << msg << "<<<"<< std::endl;
    }

    void Increment(uint64_t file_num) {
      WriteLock wl(&lcf_alive_file_mutex_);
      if (lcf_alive_file_map_.find(file_num) == lcf_alive_file_map_.end()) {
        lcf_alive_file_map_.insert(std::make_pair(file_num, 1));
      }
      else {
        int file_cnt = lcf_alive_file_map_[file_num];
        lcf_alive_file_map_[file_num] = file_cnt + 1;
      }
    }

    bool Decrement(uint64_t file_num) {
      WriteLock wl(&lcf_alive_file_mutex_);
      if(lcf_alive_file_map_.find(file_num) == lcf_alive_file_map_.end()){
        //error
        return true;
      }
      int file_cnt = lcf_alive_file_map_[file_num];
      if(file_cnt == 1) {
        lcf_alive_file_map_.erase(file_num);
        return true;
      }
      else {
        lcf_alive_file_map_[file_num] = file_cnt - 1;
        return false;
      }
    }

    std::unordered_map<uint64_t, int> lcf_alive_file_map_;
    port::RWMutex lcf_alive_file_mutex_;
};
}  // namespace rocksdb
