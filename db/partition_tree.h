// Author: Dohyun Kim (ehgus421210@kaist.ac.kr)
// Note: Partition tree for logical column family.
// Modified by : Junhan (junhanlee2020@gmail.com)
// Note: add rw mutex to partition tree for the synchronization.

#pragma once 

#include <string>
#include <vector>
#include <atomic>
#include <iostream>

#include "db/column_family.h"
#include "rocksdb/slice.h"
#include "port/port.h"
#include "util/mutexlock.h"

#define get_lmost_key(n) ((n)->cfd_->GetSmallestKey())
#define get_rmost_key(n) ((n)->cfd_->GetLargestKey())

namespace rocksdb {

class ColumnFamilyData;

class PartitionTreeNode {
 public:
  
  ColumnFamilyData *cfd_ = nullptr;
  int depth_=0;
  int hdepth_=0;

  std::vector<PartitionTreeNode*> lower_level_nodes_;
  PartitionTreeNode *parent_node_ = nullptr;
  mutable port::RWMutex rwlock_;
  
  PartitionTreeNode() {};
  PartitionTreeNode(ColumnFamilyData*); 
  ~PartitionTreeNode();

  void SetColumnFamily(ColumnFamilyData*);
  PartitionTreeNode *SearchNextNode(const Slice &key);

  PartitionTreeNode *GetParentNode(void);
  std::vector<PartitionTreeNode *> GetChildrenNodes(void);

  void Print(std::string TreeID, bool recursive);
  int GetDepth(void);
  int GetHDepth(void);
};

class PartitionTree {
 public:
  int height_ = 0;
  PartitionTreeNode* root_ = nullptr;
  const ImmutableCFOptions* ioptions_;
  
  PartitionTree() {};
  PartitionTree(ColumnFamilyData *cfd);
  ~PartitionTree();

  Status InsertSplittedColumnFamily (ColumnFamilyData *base_cfd, const std::vector<ColumnFamilyData*> &new_cfds);
  ColumnFamilyData *SearchColumnFamily (const Slice &key); 
  std::vector<ColumnFamilyData*> SearchAllColumnFamilies (const Slice &key); 
  std::vector<PartitionTreeNode*> GetChildrenNodes(PartitionTreeNode* node);
  
  void PrintAll();
};

}






