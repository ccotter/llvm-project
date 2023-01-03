#!/usr/bin/env python3

import sys
from collections import defaultdict

def is_fix_at_line(output, line_number, fix):
    #print(f"fix={fix}")
    def check_line(line, fix_line):
        val = fix_line in line
        #print(f"check_line --{line}-- --{fix_line}-- = {val}")
        return val

    lines = output[line_number:line_number+len(fix)]
    #print("--DONE")
    #print(lines)
    #print("--DONE")
    if len(lines) != len(fix):
        return False
    for line, fix_line in zip(lines, fix):
        if not check_line(line, fix_line):
            return False
    return True

class Fix:
    def __init__(self, fix_lines):
        self.fix_lines = fix_lines
    def __hash__(self):
        return hash(str(self.fix_lines))
    def __eq__(self, other):
        return self.fix_lines == other.fix_lines
    def __repr__(self):
        return f"Fix({repr(self.fix_lines)})"

def main():
    output_file = sys.argv[1]
    orig_file = sys.argv[2]

    with open(output_file) as f:
        output = [l[0:-1] for l in f.readlines()]
    with open(orig_file) as f:
        orig = [l[0:-1] for l in f.readlines()]

    def make_fix(lines):
        assert len(lines) > 0
        fix_lines = [lines[0].split("CHECK-FIXES: ")[1]]
        for l in lines[1:]:
            fix_lines.append(l.split("CHECK-FIXES-NEXT: ")[1])
        return Fix(fix_lines)

    expected_fixes = defaultdict(int)
    current_fix = None
    for l in orig:
        if "CHECK-FIXES: " in l:
            assert current_fix is None, sys.argv[2]
            current_fix = [l]
        elif "CHECK-FIXES-NEXT: " in l:
            assert current_fix is not None, sys.argv[2]
            current_fix.append(l)
        else:
            if current_fix is not None:
                expected_fixes[make_fix(current_fix)] += 1
                current_fix = None

    #is_fix_at_line(output, 24, expected_fixes[0])
    #return

    for fix, count in expected_fixes.items():
        found = 0
        for i in range(0, len(output)):
            if is_fix_at_line(output, i, fix.fix_lines):
                #print(f"Found fix {fix.fix_lines} at {i}")
                found += 1
        if found == 0:
            print(f"Did not find {fix.fix_lines}")
        elif count != found:
            too_many = found > count
            print(f"Did not find {fix.fix_lines} the correct number of times count={count} found={found} too_many={too_many}")
        else:
            pass
            #print(f"Found {fix.fix_lines} the correct number of times")

if __name__ == "__main__":
    cmd = " ".join(sys.argv)
    print(f"Running: {cmd}")
    main()
