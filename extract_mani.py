import re
from collections import defaultdict

def extract_sstable_numbers(mani_file):
    level_sstables = defaultdict(list)
    current_level = None

    with open(mani_file, 'r') as f:
        for line in f:
            level_match = re.match(r"--- level (\d+) ---", line)
            sstable_match = re.match(r"^\s*(\d+):", line)

            if level_match:
                current_level = int(level_match.group(1))
            elif sstable_match and current_level is not None:
                sstable_number = int(sstable_match.group(1))
                level_sstables[current_level].append(sstable_number)

    return level_sstables

# 사용 예시
mani_file_path = "mani"  # mani 파일 경로 입력
sstable_numbers = extract_sstable_numbers(mani_file_path)

# 결과 출력
for level, sstables in sorted(sstable_numbers.items()):
    print(f"Level {level}: {sstables}")

