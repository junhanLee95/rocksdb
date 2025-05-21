import re
import os
import sys

directory = sys.argv[1]
o_dir = sys.argv[2]

for root, dirs, files in os.walk(directory):
  for file in files:
    if file.endswith("dump.txt"):
      f_path = os.path.join(root,file)
      of_path = o_dir + "/" +  file
      print(of_path)
      with open(f_path, 'r') as f:
        with open(of_path, 'w') as of:
          ls = f.readlines()
          for l in ls:
            ss = re.split("=>", l)
            if(len(ss) <2):
              continue
            of.write(ss[0]+"\n")
