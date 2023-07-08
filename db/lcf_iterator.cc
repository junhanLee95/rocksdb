	//  Copyright (c) 2011-present, Facebook, Inc.  All rights reserved.
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).

#ifndef ROCKSDB_LITE
#include "db/lcf_iterator.h"

#include <limits>
#include <string>
#include <utility>
#include <algorithm>
#include <set>

#include "db/column_family.h"
#include "db/db_impl.h"
#include "db/db_iter.h"
#include "db/dbformat.h"
#include "db/job_context.h"
#include "db/range_del_aggregator.h"
#include "db/range_tombstone_fragmenter.h"
#include "rocksdb/env.h"
#include "rocksdb/slice.h"
#include "rocksdb/slice_transform.h"
#include "table/merging_iterator.h"
#include "util/string_util.h"
#include "util/sync_point.h"

#define get_lmost_key(n) ((n)->cfd_->GetSmallestKey())
#define get_rmost_key(n) ((n)->cfd_->GetLargestKey())

namespace rocksdb {

// Usage:
//     LCFLevelIterator iter;
//     iter.SetFileIndex(file_index);
//     iter.Seek(target); // or iter.SeekToFirst();
//     iter.Next()
    // (which discards pre-existing error status), and SetFileIndex() may set
    // an error status, which we shouldn't discard.

LCFIterator::LCFIterator(DBImpl* db, const ReadOptions& read_options,
                                 ColumnFamilyData* root, std::vector<InternalIterator*> iterator,
								 std::vector<PartitionTreeNode*> nodes)
    : db_(db),
      read_options_(read_options),
	  root_(root),
	  iterators_(iterator),
	  nodes_(nodes),
	  merge_iter_builder_(MergeIteratorBuilder(&root_->internal_comparator(),&arena_))
      {
	tree_nodes_=root_->GetPartitionTreeNode()->Traversal();
	for(auto iter: iterators_)
	  merge_iter_builder_.AddIterator(iter);
	merge_iter_=merge_iter_builder_.GetMergeIter();
  /*if (sv_) {
	  RebuildIterators(false);
  }*/
}

LCFIterator::~LCFIterator() {

  //start to record time
  uint64_t start_time = db_->env_->NowMicros();
  //fprintf(stdout,"Now time is %ld",start_time);
  for(auto cnodes : tree_nodes_){
	  //fprintf(stdout,"CF %d is Hot? : %d\n",cnodes->cfd_->GetID(),cnodes->cfd_->IsHot());
	  cnodes->cfd_->SetHot(false);
	  //fprintf(stdout,"CF %d is Hot? : %d\n",cnodes->cfd_->GetID(),cnodes->cfd_->IsHot());
  }
  if(kvpair_.size() <= 1){
    merge_iter_ = merge_iter_builder_.Finish();
    this->Cleanup(true);
    merge_iter_=nullptr;
	return;
  }
  //sort(kvpair_.begin(),kvpair_.end());
  /*
  if(kvpair_.front().first.compare(kvpair_.back().first)>0){
	std::cout << "fail\n";
	return;
  }*/

  //checkpoint1
  //uint64_t time_1 = db_->env_->NowMicros();
  std::pair<std::string,std::string> range = std::make_pair(kvpair_.front().first, kvpair_.back().first);
  //fprintf(stdout,"(%s %s)\n", range.first.c_str(), range.second.c_str());
  std::vector<std::string> keys;
  //std::vector<std::string> test;
  for(auto pair : kvpair_){
    keys.push_back(pair.first);
    //test.push_back(pair.first);
  }

//  test.erase(unique(test.begin(), test.end()),test.end());
//  if(keys.size()!=test.size()){
//	  std::cout << "failed\n";
//	  return;
//  }

  std::pair<std::vector<int>,std::pair<std::string,std::string>> jobs;
  root_->GetPartitionTreeNode()->Traversal(range,keys,&jobs);
  //for(auto id:jobs.first)
    //fprintf(stdout,"ID is %d\n",id);

  //std::cout << jobs.second.first << " " << jobs.second.second << std::endl;
  /*
  for(auto pair : jobs){
    fprintf(stdout,"(%d -> %s,%s)\n",pair.first,pair.second.first.c_str(),pair.second.second.c_str());
  }*/

  //checkpoint2
  uint64_t time_2 = db_->env_->NowMicros();

  merge_iter_ = merge_iter_builder_.Finish();
  this->Cleanup(true);
  merge_iter_=nullptr;
  std::vector<std::string>().swap(keys);
/*
  for(auto i : jobs.first)
    std::cout << i << " ";
  std::cout << std::endl;*/
  if(jobs.second.first.empty() || jobs.second.second.empty()){
    //std::cout << range.first << " " << range.second << std::endl;
	//std::cout << "success" << std::endl;
	return;
  }

  if(int(jobs.first.size())==0){
	return;
  }
  if(jobs.second.first.compare(jobs.second.second)==0)
	return;

  int source_cfd = jobs.first[0];
  std::vector<int> del_cfd;
  for(int i=1;i<int(jobs.first.size());i++)
	del_cfd.push_back(jobs.first[i]);

  std::vector<ColumnFamilyData*> new_cfds; 
  //if(source_cfds.size()!=0){

	//for(i=0;i<int(source_cfds.size());i++){
  //std::cout << source_cfd << std::endl;

  auto cfd = db_->MergeColumnFamily(source_cfd,del_cfd,root_,jobs.second.first,jobs.second.second);
  //checkpoint3
  uint64_t time_3 = db_->env_->NowMicros();

  if(cfd!=nullptr){
    cfd->SetMade(true);
    new_cfds.push_back(cfd);
  }

	//}
  //}


  //for(auto cf:new_cfds)
    //fprintf(stdout,"ID is %d\n",cf->GetID());

  if(int(new_cfds.size()) > 0 ){
    Status s = db_->IterToMem(kvpair_, new_cfds,type_);
    assert(s.ok());
  }

  std::vector<std::pair<std::string,std::string>>().swap(kvpair_);
  //checkpoint4
  uint64_t time_4 = db_->env_->NowMicros();

 // uint64_t hot_time = time_1-start_time;
  uint64_t range_time = time_2-start_time;
  uint64_t cf_time = time_3-time_2;
  uint64_t mem_time = time_4-time_3;
  db_->range_time_.push_back(range_time);
  db_->cf_time_.push_back(cf_time);
  db_->mem_time_.push_back(mem_time);
  //fprintf(stdout,"Elapsed time is %ld, %ld, %ld, %ld\n",hot_time,range_time,cf_time,mem_time);


  //fprintf(stdout,"Success to delete LCFIterator\n");
  /*
  for(auto pair : jobs)
	  fprintf(stdout,"(%d -> %d)\n",pair.first,pair.second);
  for(auto pair : sibilng_jobs)
	  fprintf(stdout,"(%d -> %d)\n",pair.first,pair.second);
  */
  /*std::vector<int> target_cfd;
  for(auto pair : jobs)
    target_cfd.push_back(pair.second);
  sort(target_cfd.begin(),target_cfd.end());
  target_cfd.erase(unique(target_cfd.begin(),target_cfd.end()),target_cfd.end());

  std::vector<HotRange(target_cfd);
  for(auto pair : sibling_jobs)
    target_cfd.push_back(pair.second);
  sort(target_cfd.begin(),target_cfd.end());
  target_cfd.erase(unique(target_cfd.begin(),target_cfd.end()),target_cfd.end());*/

  //for(auto a : target_cfd)
  //  fprintf(stdout,"element is %d\n",a);
  
  //MakeNewPartition(jobs, sibiling_jobs);
}

void LCFIterator::Addkey(){
  Slice key = merge_iter_->key();
  Slice value = merge_iter_->value();
  ParsedInternalKey ikey;

  ParseInternalKey(key, &ikey);
  kvpair_.push_back(std::make_pair(ikey.user_key.ToString(),value.ToString()));
  type_.push_back(ikey.type);

}

void LCFIterator::SVCleanup(DBImpl* db, SuperVersion* sv,
                                bool background_purge_on_iterator_cleanup) {
  if (sv->Unref()) {
    // Job id == 0 means that this is not our background process, but rather
    // user thread
    JobContext job_context(0);
    db->mutex_.Lock();
    sv->Cleanup();
    db->FindObsoleteFiles(&job_context, false, true);
    if (background_purge_on_iterator_cleanup) {
      db->ScheduleBgLogWriterClose(&job_context);
    }
    db->mutex_.Unlock();
    delete sv;
    if (job_context.HaveSomethingToDelete()) {
      db->PurgeObsoleteFiles(job_context, background_purge_on_iterator_cleanup);
    }
    job_context.Clean();
  }
}

namespace {
struct SVCleanupParams {
  DBImpl* db;
  SuperVersion* sv;
  bool background_purge_on_iterator_cleanup;
};
}

// Used in PinnedIteratorsManager to release pinned SuperVersion
void LCFIterator::DeferredSVCleanup(void* arg) {
  auto d = reinterpret_cast<SVCleanupParams*>(arg);
  LCFIterator::SVCleanup(
    d->db, d->sv, d->background_purge_on_iterator_cleanup);
  delete d;
}

void LCFIterator::SVCleanup() {
  if (sv_ == nullptr) {
    return;
  }
  bool background_purge =
      read_options_.background_purge_on_iterator_cleanup ||
      db_->immutable_db_options().avoid_unnecessary_blocking_io;
  if(pinned_iters_mgr_->PinningEnabled())
    fprintf(stdout,"YesYes\n");
  if (pinned_iters_mgr_ && pinned_iters_mgr_->PinningEnabled()) {
    // pinned_iters_mgr_ tells us to make sure that all visited key-value slices
    // are alive until pinned_iters_mgr_->ReleasePinnedData() is called.
    // The slices may point into some memtables owned by sv_, so we need to keep
    // sv_ referenced until pinned_iters_mgr_ unpins everything.
    auto p = new SVCleanupParams{db_, sv_, background_purge};
    pinned_iters_mgr_->PinPtr(p, &LCFIterator::DeferredSVCleanup);
  } else {
    SVCleanup(db_, sv_, background_purge);
  }
}

void LCFIterator::Cleanup(bool release_sv) {
  DeleteIterator(merge_iter_,true);
  if (release_sv) {
    SVCleanup();
  }
  //for(auto* m : iterators_)
//	 DeleteIterator(m,true);
  //iterators_.clear();
}

bool LCFIterator::Valid() const {
  // See UpdateCurrent().
  return merge_iter_->Valid();
}

void LCFIterator::SeekToFirst() {
  /*if (sv_ == nullptr) {
    RebuildIterators(true);
  } else if (sv_->version_number != cfd_->GetSuperVersionNumber()) {
    RenewIterators();
  } else if (immutable_status_.IsIncomplete()) {
    ResetIncompleteIterators();
  }*/
  SeekInternal(Slice(), true);
}
/*
bool LCFIterator::IsOverUpperBound(const Slice& internal_key) const {
  return !(read_options_.iterate_upper_bound == nullptr ||
           cfd_->internal_comparator().user_comparator()->Compare(
               ExtractUserKey(internal_key),
               *read_options_.iterate_upper_bound) < 0);
}*/

void LCFIterator::Seek(const Slice& internal_key) {
  SeekInternal(internal_key, false);
}

void LCFIterator::SeekInternal(const Slice& internal_key,
                                   bool seek_to_first) {
  if(seek_to_first){
	//Traverse partition tree only left direction and add it to merge_iter_.
	std::vector<std::string> small_keys;
    for(auto cnodes: nodes_)
		small_keys.push_back(get_lmost_key(cnodes));

    sort(small_keys.begin(), small_keys.end());
	std::string smallest_key=small_keys.front();
    

    //fprintf(stdout,"Initial node num is %d\n",int(nodes_.size()));
    //Find nodes which added to merge_iter_.
	std::vector<PartitionTreeNode*> addnode;
    for(auto cnodes: nodes_){
	  if (get_rmost_key(cnodes).empty() || smallest_key.compare(get_rmost_key(cnodes)) <= 0) {
	    if (get_lmost_key(cnodes).empty() || smallest_key.compare(get_lmost_key(cnodes)) >= 0){
	      addnode.push_back(cnodes);
		  nodes_.erase(remove(nodes_.begin(), nodes_.end(), cnodes),nodes_.end());
		  auto childnodes=cnodes->Traversal();
		  for(auto dnodes: childnodes){
			if(find(addnode.begin(), addnode.end(), dnodes)==addnode.end()){
              addnode.push_back(dnodes);
              nodes_.erase(remove(nodes_.begin(), nodes_.end(), dnodes),nodes_.end());
			}
		  }
		}
	  }
	}
	//fprintf(stdout,"left node's num is %d\n",int(nodes_.size()));

    //If some nodes iterator need to be added, expand merge_iter_.
    if(addnode.size() >= 1){
	  //fprintf(stdout,"%d iterators is need to be added!!!\n",int(addnode.size()));
      SuperVersion* sv=nullptr;

	  // Use MergingIterator::AddIterator
      for(auto cnodes: addnode){
		UpdateMaxKey(get_rmost_key(cnodes));
		ColumnFamilyData* node_cfd = cnodes->cfd_;
		sv = node_cfd->GetReferencedSuperVersion(&(db_->mutex_));
		InternalIterator* node_iter = new ForwardIterator(db_,read_options_,node_cfd,sv);
	    merge_iter_builder_.AddIterator(node_iter);
	  }
	  merge_iter_=merge_iter_builder_.GetMergeIter();
    }

    merge_iter_->SeekToFirst();
  }
  else{
    //Find nodes which added to merge_iter_.
    std::vector<PartitionTreeNode*> addnode;
	//fprintf(stdout,"left node's num is %d\n",int(nodes_.size()));
	std::string key_str = ExtractUserKey(internal_key).ToString();
    for(auto cnodes: nodes_){
	  if (get_rmost_key(cnodes).empty() || key_str.compare(get_rmost_key(cnodes)) <= 0) {
	    if (get_lmost_key(cnodes).empty() || key_str.compare(get_lmost_key(cnodes)) >= 0){
	      addnode.push_back(cnodes);
		  nodes_.erase(remove(nodes_.begin(), nodes_.end(), cnodes),nodes_.end());
		  auto childnodes=cnodes->Traversal();
		  for(auto dnodes: childnodes){
			if(find(addnode.begin(), addnode.end(), dnodes)==addnode.end()){
              addnode.push_back(dnodes);
              nodes_.erase(remove(nodes_.begin(), nodes_.end(), dnodes),nodes_.end());
			}
		  }
	    }
	  } 
    }

	//if seek target was root theres isn't any addnode. But add one node which have smallest key larger than internal_key.
	if(addnode.size() == 0){
	  
      std::vector<std::string> small_keys;
      for(auto cnodes: nodes_)
	    small_keys.push_back(get_lmost_key(cnodes));
      std::string smallest_key;
      sort(small_keys.begin(), small_keys.end());
 
	  for(int i =0; i< int(small_keys.size());i++){
        if(small_keys[i].compare(ExtractUserKey(internal_key).ToString())>=0){
          smallest_key=small_keys[i];
		  break;
	    }
	  }
      for(auto cnodes: nodes_){
        if (get_rmost_key(cnodes).empty() || smallest_key.compare(get_rmost_key(cnodes)) <= 0) {
          if (get_lmost_key(cnodes).empty() || smallest_key.compare(get_lmost_key(cnodes)) >= 0){
            addnode.push_back(cnodes);
            nodes_.erase(remove(nodes_.begin(), nodes_.end(), cnodes),nodes_.end());
		    auto childnodes=cnodes->Traversal();
		    for(auto dnodes: childnodes){
			  if(find(addnode.begin(), addnode.end(), dnodes)==addnode.end()){
                addnode.push_back(dnodes);
                nodes_.erase(remove(nodes_.begin(), nodes_.end(), dnodes),nodes_.end());
			  }
		    }
	      }
	    }
      }
	}

    //If some nodes iterator need to be added, expand merge_iter_.
  
    if(addnode.size() >= 1){
	  //fprintf(stdout,"%d iterators is need to be added!!!\n",int(addnode.size()));
      SuperVersion* sv=nullptr;


	  // Use MergingIterator::AddIterator
      for(auto cnodes: addnode){
		UpdateMaxKey(get_rmost_key(cnodes));
		ColumnFamilyData* node_cfd = cnodes->cfd_;
		sv = node_cfd->GetReferencedSuperVersion(&(db_->mutex_));
		InternalIterator* node_iter = new ForwardIterator(db_,read_options_,node_cfd,sv);
	    merge_iter_builder_.AddIterator(node_iter);
	  }
	  merge_iter_=merge_iter_builder_.GetMergeIter();
    }
  	merge_iter_->Seek(internal_key);
  }
  /*
  if(max_key_.empty()){
	UpdateMaxKey(ExtractUserKey(merge_iter_->key()).ToString());
  }*/
  //max_key_=ExtractUserKey(merge_iter_->key()).ToString();
  //update query nums
  if(!merge_iter_->Valid())
	  return;
  std::vector<ColumnFamilyData *> target_CF = CFIncludingKey(tree_nodes_,ExtractUserKey(merge_iter_->key()).ToString());
  //db_->mutex_.Lock();
  for (auto cnodes : tree_nodes_){
    cnodes->cfd_->Increase_Num_Query(find(target_CF.begin(),target_CF.end(),cnodes->cfd_) != target_CF.end());
  }
  //db_->mutex_.Unlock();
  Addkey();
  TEST_SYNC_POINT_CALLBACK("LCFIterator::SeekInternal:Return", this);
}

void LCFIterator::Next() {
  //TEST_SYNC_POINT_CALLBACK("LCFIterator::Next:Return", this);

  //Move codes related with small_keys to Constructor to save sort cost.
  std::vector<std::string> small_keys;
  //add condition when nodes_ is empty
  //fprintf(stdout,"max key is %s\n",max_key_.c_str());

  if(nodes_.size()>=1 && (ExtractUserKey(merge_iter_->key()).ToString().compare(max_key_)==0 || max_key_.empty())){
    for(auto cnodes: nodes_)
	  small_keys.push_back(get_lmost_key(cnodes));

    std::string smallest_key;
    sort(small_keys.begin(), small_keys.end());
    //std::string smallest_key=small_keys.front();
 
	for(int i =0; i< int(small_keys.size());i++){
      if(small_keys[i].compare(max_key_)>=0){
        smallest_key=small_keys[i];
		break;
	  }
	}

    //Distinguish Time of adding Column Family
    //When CF is added( Constructor, SeekInternal(), Next() ), update some variable ( ex) current_max), then compare that variable and key.
    //It can easliy update CF adding timing.

    //Find nodes which added to merge_iter_.
    std::vector<PartitionTreeNode*> addnode;
    for(auto cnodes: nodes_){
      //fprintf(stdout,"key is %s %s\n",get_lmost_key(cnodes).c_str(),get_rmost_key(cnodes).c_str());
      if (get_rmost_key(cnodes).empty() || smallest_key.compare(get_rmost_key(cnodes)) <= 0) {
        if (get_lmost_key(cnodes).empty() || smallest_key.compare(get_lmost_key(cnodes)) >= 0){
          addnode.push_back(cnodes);
          nodes_.erase(remove(nodes_.begin(), nodes_.end(), cnodes),nodes_.end());
		  auto childnodes=cnodes->Traversal();
		  for(auto dnodes: childnodes){
			if(find(addnode.begin(), addnode.end(), dnodes)==addnode.end()){
              addnode.push_back(dnodes);
              nodes_.erase(remove(nodes_.begin(), nodes_.end(), dnodes),nodes_.end());
			}
		  }
	    }
	  }
    }

    //If some nodes iterator need to be added, expand merge_iter_.
    if(addnode.size() >= 1){
      //fprintf(stdout,"%d iterators is need to be added!!!\n",int(addnode.size()));
      SuperVersion* sv=nullptr;

	  // Use MergingIterator::AddIterator
      for(auto cnodes: addnode){
		UpdateMaxKey(get_rmost_key(cnodes));
	    ColumnFamilyData* node_cfd = cnodes->cfd_;
	    sv = node_cfd->GetReferencedSuperVersion(&(db_->mutex_));
	    InternalIterator* node_iter = new ForwardIterator(db_,read_options_,node_cfd,sv);
		node_iter->SeekToFirst();
	    merge_iter_builder_.AddIterator(node_iter);
	  }
	  //It is need to reinitialize Heap when after call AddIterator().
      merge_iter_builder_.InitForNext();
      merge_iter_=merge_iter_builder_.GetMergeIter();
    }
  }
  merge_iter_->Next();
  
  //update query nums
  if(!merge_iter_->Valid())
	  return;
  std::vector<ColumnFamilyData *> target_CF = CFIncludingKey(tree_nodes_,ExtractUserKey(merge_iter_->key()).ToString());
  //db_->mutex_.Lock();
  for (auto cnodes : tree_nodes_){
    cnodes->cfd_->Increase_Num_Query(find(target_CF.begin(),target_CF.end(),cnodes->cfd_) != target_CF.end());
  //db_->mutex_.Unlock();
  }
  Addkey();
}

Slice LCFIterator::key() const {
  assert(Valid());
  return merge_iter_->key();
}

Slice LCFIterator::value() const {
  assert(Valid());
  return merge_iter_->value();
}

Status LCFIterator::status() const {
  return merge_iter_->status();
}

Status LCFIterator::GetProperty(std::string prop_name, std::string* prop) {
  assert(prop != nullptr);
  if (prop_name == "rocksdb.iterator.super-version-number") {
    *prop = ToString(sv_->version_number);
    return Status::OK();
  }
  return Status::InvalidArgument();
}

std::vector<ColumnFamilyData *> LCFIterator::CFIncludingKey(std::vector<PartitionTreeNode*> nodes,std::string key){
	std::vector<ColumnFamilyData *> cfds;
  for(auto cnodes: nodes){
    if (get_rmost_key(cnodes).empty() || key.compare(get_rmost_key(cnodes)) <= 0) {
	  if (get_lmost_key(cnodes).empty() || key.compare(get_lmost_key(cnodes)) >= 0){
  	    cfds.push_back(cnodes->cfd_);
	  }
	}
  }
  return cfds;
}

void LCFIterator::UpdateMaxKey(std::string user_key){
  if(max_key_.empty())
    max_key_=user_key;
  else
	max_key_ = (max_key_.compare(user_key) < 0 ) ? user_key : max_key_;
}

void LCFIterator::SetPinnedItersMgr(
	PinnedIteratorsManager* pinned_iters_mgr) {
  pinned_iters_mgr_ = pinned_iters_mgr;
  //UpdateChildrenPinnedItersMgr();
}
/*
void LCFIterator::UpdateChildrenPinnedItersMgr() {
  // Set PinnedIteratorsManager for mutable memtable iterator.
  if (mutable_iter_) {
    mutable_iter_->SetPinnedItersMgr(pinned_iters_mgr_);
  }

  // Set PinnedIteratorsManager for immutable memtable iterators.
  for (InternalIterator* child_iter : imm_iters_) {
    if (child_iter) {
      child_iter->SetPinnedItersMgr(pinned_iters_mgr_);
    }
  }

  // Set PinnedIteratorsManager for L0 files iterators.
  for (InternalIterator* child_iter : l0_iters_) {
    if (child_iter) {
      child_iter->SetPinnedItersMgr(pinned_iters_mgr_);
    }
  }

  // Set PinnedIteratorsManager for L1+ levels iterators.
  for (LCFLevelIterator* child_iter : level_iters_) {
    if (child_iter) {
      child_iter->SetPinnedItersMgr(pinned_iters_mgr_);
    }
  }
}
*/
bool LCFIterator::IsKeyPinned() const {
 // return pinned_iters_mgr_ && pinned_iters_mgr_->PinningEnabled() &&
   //      current_->IsKeyPinned();
	return true;
}

bool LCFIterator::IsValuePinned() const {
//  return pinned_iters_mgr_ && pinned_iters_mgr_->PinningEnabled() &&
  //       current_->IsValuePinned();
	return true;
}


/*
void LCFIterator::RebuildIterators(bool refresh_sv) {
  // Clean up
  Cleanup(refresh_sv);
  if (refresh_sv) {
    // New
    sv_ = cfd_->GetReferencedSuperVersion(&(db_->mutex_));
  }
  ReadRangeDelAggregator range_del_agg(&cfd_->internal_comparator(),
                                       kMaxSequenceNumber *//* upper_bound */ //);
/*  mutable_iter_ = sv_->mem->NewIterator(read_options_, &arena_);
  sv_->imm->AddIterators(read_options_, &imm_iters_, &arena_);
  if (!read_options_.ignore_range_deletions) {
    std::unique_ptr<FragmentedRangeTombstoneIterator> range_del_iter(
        sv_->mem->NewRangeTombstoneIterator(
            read_options_, sv_->current->version_set()->LastSequence()));
    range_del_agg.AddTombstones(std::move(range_del_iter));
    sv_->imm->AddRangeTombstoneIterators(read_options_, &arena_,
                                         &range_del_agg);
  }
  has_iter_trimmed_for_upper_bound_ = false;

  const auto* vstorage = sv_->current->storage_info();
  const auto& l0_files = vstorage->LevelFiles(0);
  l0_iters_.reserve(l0_files.size());
  for (const auto* l0 : l0_files) {
    if ((read_options_.iterate_upper_bound != nullptr) &&
        cfd_->internal_comparator().user_comparator()->Compare(
            l0->smallest.user_key(), *read_options_.iterate_upper_bound) > 0) {
      // No need to set has_iter_trimmed_for_upper_bound_: this LCFIterator
      // will never be interested in files with smallest key above
      // iterate_upper_bound, since iterate_upper_bound can't be changed.
      l0_iters_.push_back(nullptr);
      continue;
    }
    l0_iters_.push_back(cfd_->table_cache()->NewIterator(
        read_options_, *cfd_->soptions(), cfd_->internal_comparator(), *l0,
        read_options_.ignore_range_deletions ? nullptr : &range_del_agg,
        sv_->mutable_cf_options.prefix_extractor.get()));
  }
  BuildLevelIterators(vstorage);
  current_ = nullptr;
  is_prev_set_ = false;

  UpdateChildrenPinnedItersMgr();
  if (!range_del_agg.IsEmpty()) {
    status_ = Status::NotSupported(
        "Range tombstones unsupported with LCFIterator");
    valid_ = false;
  }
}

void LCFIterator::RenewIterators() {
  SuperVersion* svnew;
  assert(sv_);
  svnew = cfd_->GetReferencedSuperVersion(&(db_->mutex_));

  if (mutable_iter_ != nullptr) {
    DeleteIterator(mutable_iter_, true*/ /* is_arena */ //);
/*  }
  for (auto* m : imm_iters_) {
    DeleteIterator(m, true*/ /* is_arena */ //);
/*  }
  imm_iters_.clear();

  mutable_iter_ = svnew->mem->NewIterator(read_options_, &arena_);
  svnew->imm->AddIterators(read_options_, &imm_iters_, &arena_);
  ReadRangeDelAggregator range_del_agg(&cfd_->internal_comparator(),
                                       kMaxSequenceNumber*/ /* upper_bound */ //);
/*  if (!read_options_.ignore_range_deletions) {
    std::unique_ptr<FragmentedRangeTombstoneIterator> range_del_iter(
        svnew->mem->NewRangeTombstoneIterator(
            read_options_, sv_->current->version_set()->LastSequence()));
    range_del_agg.AddTombstones(std::move(range_del_iter));
    svnew->imm->AddRangeTombstoneIterators(read_options_, &arena_,
                                           &range_del_agg);
  }

  const auto* vstorage = sv_->current->storage_info();
  const auto& l0_files = vstorage->LevelFiles(0);
  const auto* vstorage_new = svnew->current->storage_info();
  const auto& l0_files_new = vstorage_new->LevelFiles(0);
  size_t iold, inew;
  bool found;
  std::vector<InternalIterator*> l0_iters_new;
  l0_iters_new.reserve(l0_files_new.size());

  for (inew = 0; inew < l0_files_new.size(); inew++) {
    found = false;
    for (iold = 0; iold < l0_files.size(); iold++) {
      if (l0_files[iold] == l0_files_new[inew]) {
        found = true;
        break;
      }
    }
    if (found) {
      if (l0_iters_[iold] == nullptr) {
        l0_iters_new.push_back(nullptr);
        TEST_SYNC_POINT_CALLBACK("LCFIterator::RenewIterators:Null", this);
      } else {
        l0_iters_new.push_back(l0_iters_[iold]);
        l0_iters_[iold] = nullptr;
        TEST_SYNC_POINT_CALLBACK("LCFIterator::RenewIterators:Copy", this);
      }
      continue;
    }
    l0_iters_new.push_back(cfd_->table_cache()->NewIterator(
        read_options_, *cfd_->soptions(), cfd_->internal_comparator(),
        *l0_files_new[inew],
        read_options_.ignore_range_deletions ? nullptr : &range_del_agg,
        svnew->mutable_cf_options.prefix_extractor.get()));
  }

  for (auto* f : l0_iters_) {
    DeleteIterator(f);
  }
  l0_iters_.clear();
  l0_iters_ = l0_iters_new;

  for (auto* l : level_iters_) {
    DeleteIterator(l);
  }
  level_iters_.clear();
  BuildLevelIterators(vstorage_new);
  current_ = nullptr;
  is_prev_set_ = false;
  SVCleanup();
  sv_ = svnew;

  UpdateChildrenPinnedItersMgr();
  if (!range_del_agg.IsEmpty()) {
    status_ = Status::NotSupported(
        "Range tombstones unsupported with LCFIterator");
    valid_ = false;
  }
}

void LCFIterator::BuildLevelIterators(const VersionStorageInfo* vstorage) {
  level_iters_.reserve(vstorage->num_levels() - 1);
  for (int32_t level = 1; level < vstorage->num_levels(); ++level) {
    const auto& level_files = vstorage->LevelFiles(level);
    if ((level_files.empty()) ||
        ((read_options_.iterate_upper_bound != nullptr) &&
         (user_comparator_->Compare(*read_options_.iterate_upper_bound,
                                    level_files[0]->smallest.user_key()) <
          0))) {
      level_iters_.push_back(nullptr);
      if (!level_files.empty()) {
        has_iter_trimmed_for_upper_bound_ = true;
      }
    } else {
      level_iters_.push_back(new LCFLevelIterator(
          cfd_, read_options_, level_files,
          sv_->mutable_cf_options.prefix_extractor.get()));
    }
  }
}

void LCFIterator::ResetIncompleteIterators() {
  const auto& l0_files = sv_->current->storage_info()->LevelFiles(0);
  for (size_t i = 0; i < l0_iters_.size(); ++i) {
    assert(i < l0_files.size());
    if (!l0_iters_[i] || !l0_iters_[i]->status().IsIncomplete()) {
      continue;
    }
    DeleteIterator(l0_iters_[i]);
    l0_iters_[i] = cfd_->table_cache()->NewIterator(
        read_options_, *cfd_->soptions(), cfd_->internal_comparator(),
        *l0_files[i], nullptr */ /* range_del_agg */ /*,
        sv_->mutable_cf_options.prefix_extractor.get());
    l0_iters_[i]->SetPinnedItersMgr(pinned_iters_mgr_);
  }

  for (auto* level_iter : level_iters_) {
    if (level_iter && level_iter->status().IsIncomplete()) {
      level_iter->Reset();
    }
  }

  current_ = nullptr;
  is_prev_set_ = false;
}

void LCFIterator::UpdateCurrent() {
  if (immutable_min_heap_.empty() && !mutable_iter_->Valid()) {
    current_ = nullptr;
  } else if (immutable_min_heap_.empty()) {
    current_ = mutable_iter_;
  } else if (!mutable_iter_->Valid()) {
    current_ = immutable_min_heap_.top();
    immutable_min_heap_.pop();
  } else {
    current_ = immutable_min_heap_.top();
    assert(current_ != nullptr);
    assert(current_->Valid());
    int cmp = cfd_->internal_comparator().InternalKeyComparator::Compare(
        mutable_iter_->key(), current_->key());
    assert(cmp != 0);
    if (cmp > 0) {
      immutable_min_heap_.pop();
    } else {
      current_ = mutable_iter_;
    }
  }
  valid_ = current_ != nullptr && immutable_status_.ok();
  if (!status_.ok()) {
    status_ = Status::OK();
  }

  // Upper bound doesn't apply to the memtable iterator. We want Valid() to
  // return false when all iterators are over iterate_upper_bound, but can't
  // just set valid_ to false, as that would effectively disable the tailing
  // optimization (Seek() would be called on all immutable iterators regardless
  // of whether the target key is greater than prev_key_).
  current_over_upper_bound_ = valid_ && IsOverUpperBound(current_->key());
}

bool LCFIterator::NeedToSeekImmutable(const Slice& target) {
  // We maintain the interval (prev_key_, immutable_min_heap_.top()->key())
  // such that there are no records with keys within that range in
  // immutable_min_heap_. Since immutable structures (SST files and immutable
  // memtables) can't change in this version, we don't need to do a seek if
  // 'target' belongs to that interval (immutable_min_heap_.top() is already
  // at the correct position).

  if (!valid_ || !current_ || !is_prev_set_ || !immutable_status_.ok()) {
    return true;
  }
  Slice prev_key = prev_key_.GetInternalKey();
  if (prefix_extractor_ && prefix_extractor_->Transform(target).compare(
    prefix_extractor_->Transform(prev_key)) != 0) {
    return true;
  }
  if (cfd_->internal_comparator().InternalKeyComparator::Compare(
        prev_key, target) >= (is_prev_inclusive_ ? 1 : 0)) {
    return true;
  }

  if (immutable_min_heap_.empty() && current_ == mutable_iter_) {
    // Nothing to seek on.
    return false;
  }
  if (cfd_->internal_comparator().InternalKeyComparator::Compare(
        target, current_ == mutable_iter_ ? immutable_min_heap_.top()->key()
                                          : current_->key()) > 0) {
    return true;
  }
  return false;
}

void LCFIterator::DeleteCurrentIter() {
  const VersionStorageInfo* vstorage = sv_->current->storage_info();
  const std::vector<FileMetaData*>& l0 = vstorage->LevelFiles(0);
  for (size_t i = 0; i < l0.size(); ++i) {
    if (!l0_iters_[i]) {
      continue;
    }
    if (l0_iters_[i] == current_) {
      has_iter_trimmed_for_upper_bound_ = true;
      DeleteIterator(l0_iters_[i]);
      l0_iters_[i] = nullptr;
      return;
    }
  }

  for (int32_t level = 1; level < vstorage->num_levels(); ++level) {
    if (level_iters_[level - 1] == nullptr) {
      continue;
    }
    if (level_iters_[level - 1] == current_) {
      has_iter_trimmed_for_upper_bound_ = true;
      DeleteIterator(level_iters_[level - 1]);
      level_iters_[level - 1] = nullptr;
    }
  }
}

bool LCFIterator::TEST_CheckDeletedIters(int* pdeleted_iters,
                                             int* pnum_iters) {
uint32_t LCFIterator::FindFileInRange(
    const std::vector<FileMetaData*>& files, const Slice& internal_key,
    uint32_t left, uint32_t right) {
  auto cmp = [&](const FileMetaData* f, const Slice& key) -> bool {
    return cfd_->internal_comparator().InternalKeyComparator::Compare(
            f->largest.Encode(), key) < 0;
  };
  const auto &b = files.begin();
  return static_cast<uint32_t>(std::lower_bound(b + left,
                                 b + right, internal_key, cmp) - b);
}
*/
void LCFIterator::DeleteIterator(InternalIterator* iter, bool is_arena) {
  if (iter == nullptr) {
    return;
  }

  if (pinned_iters_mgr_ && pinned_iters_mgr_->PinningEnabled()) {
    pinned_iters_mgr_->PinIterator(iter, is_arena);
  } else {
    if (is_arena) {
      iter->~InternalIterator();
    } else {
      delete iter;
    }
  }
}

}  // namespace rocksdb

#endif  // ROCKSDB_LITE
