import os
import re

def count_functions(filename):
    """Count public and static functions"""
    try:
        with open(filename, 'r', encoding='utf-8', errors='ignore') as f:
            content = f.read()
        
        # Match function definitions: return_type name(...) {
        # This is simplified - counts potential function starts
        pattern = r'^(?:static\s+)?[a-zA-Z_][\w\s*\[\],]*\([^)]*\)\s*\{' 
        matches = re.findall(pattern, content, re.MULTILINE)
        return len(matches)
    except:
        return 0

files = sorted([f for f in os.listdir('.') if f.endswith('.c')])
for f in files:
    lines = sum(1 for _ in open(f, encoding='utf-8', errors='ignore'))
    funcs = count_functions(f)
    print(f"{f:25} {lines:5} lines, ~{funcs} functions")
