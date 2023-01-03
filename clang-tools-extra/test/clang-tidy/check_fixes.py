#!/usr/bin/env python3

import sys

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

def main():
    output_file = sys.argv[1]
    orig_file = sys.argv[2]

    with open(output_file) as f:
        output = [l[0:-1] for l in f.readlines()]
    with open(orig_file) as f:
        orig = [l[0:-1] for l in f.readlines()]

    def make_fix(lines):
        assert len(lines) > 0
        fix = [lines[0].split("CHECK-FIXES: ")[1]]
        for l in lines[1:]:
            fix.append(l.split("CHECK-FIXES-NEXT: ")[1])
        return fix

    expected_fixes = []
    current_fix = None
    for l in orig:
        if "CHECK-FIXES: " in l:
            assert current_fix is None, l
            current_fix = [l]
        elif "CHECK-FIXES-NEXT: " in l:
            assert current_fix is not None
            current_fix.append(l)
        else:
            if current_fix is not None:
                expected_fixes.append(make_fix(current_fix))
                current_fix = None

    #is_fix_at_line(output, 24, expected_fixes[0])
    #return
    for fix in expected_fixes:
        found = False
        for i in range(0, len(output)):
            if is_fix_at_line(output, i, fix):
                print(f"Found fix {fix} at {i}")
                found = True
                break
        if not found:
            print(f"Did not find {fix}")

if __name__ == "__main__":
    main()
