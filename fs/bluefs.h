#include <stdio.h>
#include <stdlib.h>
#include <string>
#include <vector>

#include "rocksdb/db.h"

#include "global/global_init.h"
#include "os/bluestore/BlueFS.h"
#include "os/bluestore/BlueStore.h"
#include "os/bluestore/BlueRocksEnv.h"
#include "common/admin_socket.h"

using namespace std;

class bluefs {
 public:
  string path;
  BlueFS *bluefsp;
  rocksdb::Env *env;

  bluefs(const char *path) : path(path), bluefsp(nullptr), env(nullptr) {}
  ~bluefs() {}

  void mount();
  void umount();
  rocksdb::Env *get_env();
 
 private:
  void validate_path(CephContext *cct, bool bluefs);
  const char *find_device_path(int id, CephContext *cct, const vector<string>& devs);
  void parse_devices(CephContext *cct, const vector<string>& devs, map<string, int>* got, bool *has_db, bool *has_wal);
  void add_devices(BlueFS *fs, CephContext *cct, const vector<string>& devs);
  BlueFS *open_bluefs(CephContext *cct, const vector<string>& devs);
  void log_dump(CephContext *cct, const vector<string>& devs);
  void inferring_bluefs_devices(vector<string>& devs); 
};


void StartBluefs(); 
rocksdb::Env *GetBluefsEnv();
void EndBluefs();
