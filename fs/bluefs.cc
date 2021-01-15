// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#include <boost/program_options/variables_map.hpp>
#include <boost/program_options/parsers.hpp>

#include <stdio.h>
#include <string.h>
#include <iostream>
#include <time.h>
#include <fcntl.h>
#include "common/ceph_argparse.h"
#include "include/stringify.h"
#include "common/errno.h"
#include "common/safe_io.h"

#include "fs/bluefs.h"

static bluefs *globalfsp = nullptr;

void bluefs::validate_path(
  CephContext *cct, 
  bool bluefs)
{
  BlueStore bluestore(cct, path);
  string type;

  int r = bluestore.read_meta("type", &type);
  if (r < 0) {
    cerr << "failed to load os-type: " << cpp_strerror(r) << std::endl;
    exit(EXIT_FAILURE);
  }
  if (type != "bluestore") {
    cerr << "expected bluestore, but type is " << type << std::endl;
    exit(EXIT_FAILURE);
  }
  if (!bluefs) {
    return;
  }

  string kv_backend;
  r = bluestore.read_meta("kv_backend", &kv_backend);
  if (r < 0) {
    cerr << "failed to load kv_backend: " << cpp_strerror(r) << std::endl;
    exit(EXIT_FAILURE);
  }
  if (kv_backend != "rocksdb") {
    cerr << "expect kv_backend to be rocksdb, but is " << kv_backend
         << std::endl;
    exit(EXIT_FAILURE);
  }
  string bluefs_enabled;
  r = bluestore.read_meta("bluefs", &bluefs_enabled);
  if (r < 0) {
    cerr << "failed to load do_bluefs: " << cpp_strerror(r) << std::endl;
    exit(EXIT_FAILURE);
  }
  if (bluefs_enabled != "1") {
    cerr << "bluefs not enabled for rocksdb" << std::endl;
    exit(EXIT_FAILURE);
  }
}

const char *bluefs::find_device_path(
  int id,
  CephContext *cct,
  const vector<string>& devs)
{
  for (auto& i : devs) {
    bluestore_bdev_label_t label;
    int r = BlueStore::_read_bdev_label(cct, i, &label);
    if (r < 0) {
      cerr << "unable to read label for " << i << ": "
	   << cpp_strerror(r) << std::endl;
      exit(EXIT_FAILURE);
    }
    if ((id == BlueFS::BDEV_SLOW && label.description == "main") ||
        (id == BlueFS::BDEV_DB && label.description == "bluefs db") ||
        (id == BlueFS::BDEV_WAL && label.description == "bluefs wal")) {
      return i.c_str();
    }
  }
  return nullptr;
}

void bluefs::parse_devices(
  CephContext *cct,
  const vector<string>& devs,
  map<string, int>* got,
  bool* has_db,
  bool* has_wal)
{
  string main;
  bool was_db = false;
  if (has_wal) {
    *has_wal = false;
  }
  if (has_db) {
    *has_db = false;
  }
  for (auto& d : devs) {
    bluestore_bdev_label_t label;
    int r = BlueStore::_read_bdev_label(cct, d, &label);
    if (r < 0) {
      cerr << "unable to read label for " << d << ": "
	   << cpp_strerror(r) << std::endl;
      exit(EXIT_FAILURE);
    }
    int id = -1;
    if (label.description == "main")
      main = d;
    else if (label.description == "bluefs db") {
      id = BlueFS::BDEV_DB;
      was_db = true;
      if (has_db) {
	*has_db = true;
      }
    }
    else if (label.description == "bluefs wal") {
      id = BlueFS::BDEV_WAL;
      if (has_wal) {
	*has_wal = true;
      }
    }
    if (id >= 0) {
      got->emplace(d, id);
    }
  }
  if (main.length()) {
    int id = was_db ? BlueFS::BDEV_SLOW : BlueFS::BDEV_DB;
    got->emplace(main, id);
  }
}

void bluefs::add_devices(
  BlueFS *fs,
  CephContext *cct,
  const vector<string>& devs)
{
  map<string, int> got;
  parse_devices(cct, devs, &got, nullptr, nullptr);
  for(auto e : got) {
    char target_path[PATH_MAX] = "";
    if(!e.first.empty()) {
      if (realpath(e.first.c_str(), target_path) == nullptr) {
	cerr << "failed to retrieve absolute path for " << e.first
	      << ": " << cpp_strerror(errno)
	      << std::endl;
      }
    }

    cout << " slot " << e.second << " " << e.first;
    if (target_path[0]) {
      cout << " -> " << target_path;
    }
    cout << std::endl;
    int r = fs->add_block_device(e.second, e.first, false);
    if (r < 0) {
      cerr << "unable to open " << e.first << ": " << cpp_strerror(r) << std::endl;
      exit(EXIT_FAILURE);
    }
  }
}

BlueFS *bluefs::open_bluefs(
  CephContext *cct,
  const vector<string>& devs)
{
  //validate_path(cct, true);
  fprintf(stdout,"[DHINFO] HIHIHIHIHIH1-5\n"); fflush(stdout);
  BlueFS *fs = new BlueFS(cct);
  fprintf(stdout,"[DHINFO] HIHIHIHIHIH1-6\n"); fflush(stdout);

  add_devices(fs, cct, devs);
  fprintf(stdout,"[DHINFO] HIHIHIHIHIH1-7\n"); fflush(stdout);

  int r = fs->mount();
  if (r < 0) {
    cerr << "unable to mount bluefs: " << cpp_strerror(r)
	 << std::endl;
    exit(EXIT_FAILURE);
  }
  fprintf(stdout,"[DHINFO] HIHIHIHIHIH1-8\n");
  return fs;
}

void bluefs::log_dump(
  CephContext *cct,
  const vector<string>& devs)
{
  BlueFS* fs = open_bluefs(cct, devs);
  int r = fs->log_dump();
  if (r < 0) {
    cerr << "log_dump failed" << ": "
         << cpp_strerror(r) << std::endl;
    exit(EXIT_FAILURE);
  }

  delete fs;
}

void bluefs::inferring_bluefs_devices(
  vector<string>& devs)
{
  cout << "inferring bluefs devices from bluestore path" << std::endl;
  for (auto fn : {"block", "block.wal", "block.db"}) {
    string p = path + "/" + fn;
    struct stat st;
    if (::stat(p.c_str(), &st) == 0) {
      devs.push_back(p);
    }
  }
}

void bluefs::mount(
  void)
{
  vector<string> devs;
  //string action;

  //action="bluefs-db-bench";
  fprintf(stdout,"[DHINFO] HIHIHIHIHIH1-1\n");
  vector<const char*> args;
  args.push_back("--no-log-to-stderr");
  args.push_back("--err-to-stderr");
  auto cct = global_init(NULL, args, CEPH_ENTITY_TYPE_CLIENT,
			 CODE_ENVIRONMENT_UTILITY,
			 CINIT_FLAG_NO_DEFAULT_CONFIG_FILE);
  fprintf(stdout,"[DHINFO] HIHIHIHIHIH1-2\n");

  common_init_finish(cct.get());
  fprintf(stdout,"[DHINFO] HIHIHIHIHIH1-3\n");

  inferring_bluefs_devices(devs);
  fprintf(stdout,"[DHINFO] HIHIHIHIHIH1-4\n");

  //log_dump(cct.get(), devs);
  bluefsp = open_bluefs(cct.get(), devs); 
  fprintf(stdout,"[DHINFO] HIHIHIHIHIH1-8\n");
  env = new BlueRocksEnv(bluefsp);
}

void bluefs::umount(
  void)
{
  bluefsp->umount();
  delete bluefsp;
  bluefsp = nullptr;  
}

rocksdb::Env *bluefs::get_env() {
  return env;
}

void StartBluefs() {
  if (!globalfsp) {
    fprintf(stdout,"[DHINFO] Create & Mount Bluefs\n");
    globalfsp = new bluefs("/var/lib/ceph/osd/ceph-4");
    globalfsp->mount();
  }
}

rocksdb::Env *GetBluefsEnv() {
  if (!globalfsp) 
    fprintf(stdout,"[DHERROR] NONONNONON\n");

  fprintf(stdout,"[DHINFO] HIHIHIHIHI2\n");
  return globalfsp->get_env();
}

void EndBluefs() {
  return;
  if (globalfsp) {
    fprintf(stdout,"[DHINFO] Umount & Delete Bluefs\n");
    globalfsp->umount();
    globalfsp = nullptr;
  }
}

