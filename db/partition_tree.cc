// Author: Dohyun Kim (ehgus421210@kaist.ac.kr)
// Note: Partition tree for logical column family.
//

#include "db/partition_tree.h"

#define get_lmost_key(n) ((n)->cfd_->GetSmallestKey())
#define get_rmost_key(n) ((n)->cfd_->GetLargestKey())

namespace rocksdb {

PartitionTreeNode::PartitionTreeNode (
    ColumnFamilyData *column_family_data) 
    : cfd_(column_family_data) {

  if (cfd_ == nullptr) { 
    // TODO: error raise should be added. 
    return; 
  }
} 

void PartitionTreeNode::SetColumnFamily (
    ColumnFamilyData* column_family_data) {
  cfd_ = column_family_data;
}

PartitionTreeNode *PartitionTreeNode::SearchNextNode (
    const Slice &key) {
    
  if (lower_level_nodes_.empty()) 
    return nullptr;

  // Linear Search
  for (auto nodes: lower_level_nodes) {
    Slice right_most_key(get_rmost_key(nodes));

    if (key.compare(right_most_key) =< 0) { 

      Slice left_most_key(get_lmost_key(nodes));

      if (key.compare(left_most_key) >= 0)
        return nodes;
      else 
        return nullptr;
    }
  }

  return nullptr;
}

// PartitionTree function.

void PartitionTree::SetColumnFamily (
    ColumnFamilyData* column_family_data) {
  root_.SetColumnFamily(column_family_data);
}

void PartitionTree::InsertSplittedColumnFamily (
    ColumnFamilyData *base_cfd, 
    const std::vector<ColumnFamilyData*> &new_cfds) {

  auto base_node = partition_nodes_.find(base_cfd->GetID());
  bool push_back = base_node->lower_level_cfds_.empty();

  // TODO: check violation 1. 
  // Violation 1. The key range of splitted nodes under the base should be 
  // included in the key range of base column family. 

  // Note: We assume that the vector new_cfds already sorted 
  // with respect to its key range. 
  for (auto new_cfd: new_cfds) {
    auto node = new PartitionTreeNode(new_cfd);

    if (push_back) {
      base_node->lower_level_cfds_.push_back(node);
      continue;
    } 

    //...
  }
}

ColumnFamilyData *SearchColumnFamily (const Slice &key) {
  PartitionTreeNode *cnode = &root_; // current node
  PartitionTreeNode *nnode = nullptr; // next node

  while (true) {
    nnode = cnode->SearchNextNode(key);

    if (nnode == nullptr) 
      break; 

    cnode = nnode; 
  }

  return cnode->cfd_;
}
  
 
}




