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
      std::cout <<"[pointer]" << this << std::endl;
      std::cout <<">>>" << msg << ">>>" <<std::endl;
      for (auto& files: lcf_alive_file_map_) {
        std::cout << "file["<< files.first <<"] : " << files.second << std::endl;
      }
      std::cout <<"<<<" << msg << "<<<"<< std::endl;
    }

    void Increment(uint64_t file_num) {
      //std::cout <<"[Increment]" << file_num << std::endl;
      WriteLock wl(&lcf_alive_file_mutex_);
      ++lcf_alive_file_map_[file_num];
    }

    bool Decrement(uint64_t file_num) {
      //std::cout <<"[Decrement]" << file_num << std::endl;
      WriteLock wl(&lcf_alive_file_mutex_);
      auto it = lcf_alive_file_map_.find(file_num);
      if (it == lcf_alive_file_map_.end()) {
        return true;
      }
      if (it->second == 1) {
        lcf_alive_file_map_.erase(it);
        return true;
      }
      else {
        it->second --;
        return false;
      }
    }

    int Get(uint64_t file_num) {
      ReadLock wl(&lcf_alive_file_mutex_);
      auto it = lcf_alive_file_map_.find(file_num);
      return (it != lcf_alive_file_map_.end()) ? it->second : 0;
    }

    std::unordered_map<uint64_t, int> lcf_alive_file_map_;
    port::RWMutex lcf_alive_file_mutex_;
};
}  // namespace rocksdb
