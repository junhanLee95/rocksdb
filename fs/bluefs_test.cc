#include "fs/bluefs.h"

int main(int argc, char **argv) {
  const char *path = "/var/lib/ceph/osd/ceph-4";

  bluefs *fsp = new bluefs(path);

  fsp->mount();
  fsp->umount();

  return 0;
}
