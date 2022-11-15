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

  lower_level_nodes_ = {};
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
  for (auto nodes: lower_level_nodes_) {
    Slice right_most_key(get_rmost_key(nodes));

    // Right most key is "" or key is less than right most key. 
    if (right_most_key.empty() || key.compare(right_most_key) < 0) { 

      Slice left_most_key(get_lmost_key(nodes));

    // Left most key is "" or key is greater than or equal to left most key. 
      if (left_most_key.empty() || key.compare(left_most_key) >= 0)
        return nodes;
      else 
        return nullptr;
    }
  }

  return nullptr;
}

PartitionTreeNode* PartitionTreeNode::GetParentNode(void) {
  return parent_node_;
}

void PartitionTreeNode::Print(
    std::string TreeID, 
    bool recursive) {

  fprintf(stdout, "%-6s LCF[%d] %s => [%s, %s)\n", 
    TreeID.c_str(), cfd_->GetID(), cfd_->GetName().c_str(), 
    cfd_->GetSmallestKey().c_str(), cfd_->GetLargestKey().c_str());

  if (!recursive) 
    return;

  uint32_t i = 0; 
  for (auto nodes: lower_level_nodes_) 
    nodes->Print(TreeID + std::to_string(i++), true);
}


// PartitionTree function.

PartitionTree::PartitionTree( 
    ColumnFamilyData* column_family_data) : root_(column_family_data) {

  fprintf(stdout, "[PartitionTree] Insert New CFD %d\n", column_family_data->GetID());

  partition_nodes_.insert({column_family_data->GetID(), &root_}); 
}

void PartitionTree::SetRootColumnFamily (
    ColumnFamilyData* column_family_data) {
  root_.SetColumnFamily(column_family_data);

  fprintf(stdout, "[PartitionTree] Insert New CFD %d\n", column_family_data->GetID());

  partition_nodes_.insert(
    std::make_pair<uint32_t, PartitionTreeNode*>(column_family_data->GetID(), &root_));
}

void PartitionTree::InsertSplittedColumnFamily (
    ColumnFamilyData *base_cfd, 
    const std::vector<ColumnFamilyData*> &new_cfds) {

  auto base_node_iter = partition_nodes_.find(base_cfd->GetID());
  auto base_node = base_node_iter->second;
  bool push_back = base_node->lower_level_nodes_.empty();

  // TODO: check violation 1. 
  // Violation 1. The key range of splitted nodes under the base should be 
  // included in the key range of base column family. 

  // Note: We assume that the vector new_cfds already sorted 
  // with respect to its key range. 

  for (auto new_cfd: new_cfds) {
    auto node = new PartitionTreeNode(new_cfd);
    if (new_cfd != nullptr) {
      new_cfd->SetPartitionTreeNode(node);  
    }

    fprintf(stdout, "[PartitionTree] Insert New CFD[%d] %s\n", new_cfd->GetID(), new_cfd->GetName().c_str());

    if (push_back) {
      base_node->lower_level_nodes_.push_back(node);
      //JH: Set parent node
      node->parent_node_ = base_node;
      continue;
    } 

    //...
  }
}

ColumnFamilyData* PartitionTree::SearchColumnFamily (const Slice &key) {
  PartitionTreeNode *cnode = &root_; // current node
  PartitionTreeNode *nnode = nullptr; // next node

  while (true) {
    nnode = cnode->SearchNextNode(key);

    if (nnode == nullptr) 
      break; 

    cnode = nnode; 
  }

  
  fprintf(stdout, "CFD[%s] Search... %s < [%s] < %s\n", 
    cnode->cfd_->GetName().c_str(), get_lmost_key(cnode).c_str(), key.data(), get_rmost_key(cnode).c_str());
  
  return cnode->cfd_;
}

std::vector<ColumnFamilyData*> PartitionTree::SearchAllColumnFamilies (const Slice &key) {
  std::vector<ColumnFamilyData*> search_cfds; // cfds to return
  PartitionTreeNode *cnode = &root_; // current node
  PartitionTreeNode *nnode = nullptr; // next node

  search_cfds.push_back(cnode->cfd_);
  while (true) {
    nnode = cnode->SearchNextNode(key);

    if (nnode == nullptr) 
      break; 

    cnode = nnode; 
    search_cfds.push_back(cnode->cfd_);
  }

  
  fprintf(stdout, "CFD[%s] Search... %s < [%s] < %s\n", 
    cnode->cfd_->GetName().c_str(), get_lmost_key(cnode).c_str(), key.data(), get_rmost_key(cnode).c_str());
  
  return search_cfds;
}
  
void PartitionTree::PrintAll() {
  root_.Print("0", true);
}
 
}




