import os
import glob
import os.path
import shutil
import subprocess
import time
import tempfile
import re
import sys

def ldb_scan_test(mangle_scan, orig_scan, out_file):
  w_file = open(out_file, 'a')

  m_file = open(mangle_scan, 'r')
  mlines = m_file.readlines()
  o_file = open(orig_scan, 'r')
  olines = o_file.readlines()

  if (len(mlines) != len(olines)):
    w_file.write("[TEST 3] (FAIL) scanned key count is different - mangle keys : %d, original keys : %d\n" %(len(mlines), len(olines)))
    return

  l_cnt = 0
  pmk = ""
  pok = ""

  for m,o in zip(mlines,olines):
    m = m.strip()
    o = o.strip()

    mkv = m.split(' : ')
    okv = o.split(' : ')

    mk = mkv[0]
    ok = okv[0]
    mv = mkv[1]
    ov = okv[1]

    if (len(mk) != len(ok) or len(mv) != len(ov)) :
      w_file.write("[TEST 3] (FAIL) scanned keys have different sizes at line %d\n" % (l_cnt))
      return

    m_order = True
    o_order = True

    if (l_cnt != 0):
      m_order = (pmk < mk)
      o_order = (pok < ok)
      if (m_order != o_order):
        w_file.write("[TEST 3] (FAIL) scanned keys have different key order at line %d\n" % (l_cnt))
        return
    l_cnt = l_cnt + 1 
    pmk = mk
    pok = ok

  w_file.write("[TEST 3] (PASS) ldb scan")
  w_file.close()

if __name__ == "__main__":
  ldb_scan_test(sys.argv[1], sys.argv[2], sys.argv[3])
