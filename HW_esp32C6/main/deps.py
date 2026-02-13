import os
import re

# Get all .h files
headers = set(f.replace('.h', '') for f in os.listdir('.') if f.endswith('.h'))

# For each .c file, extract local includes
c_files = sorted([f for f in os.listdir('.') if f.endswith('.c')])

print("INTERNAL DEPENDENCIES (module includes):")
print("=" * 70)

deps = {}
for c_file in c_files:
    with open(c_file, 'r', encoding='utf-8', errors='ignore') as f:
        includes = re.findall(r'#include "([^"]+)"', f.read())
    
    # Filter to local module headers
    local_includes = [inc.replace('.h', '') for inc in includes if inc.endswith('.h')]
    if local_includes:
        deps[c_file.replace('.c', '')] = local_includes
        print(f"\n{c_file.replace('.c', ''):25} includes:")
        for inc in sorted(local_includes):
            print(f"  -> {inc}")

# Calculate dependency depth
print("\n\nDEPENDENCY COMPLEXITY:")
print("=" * 70)

def get_deps_recursive(module, visited=None, depth=0):
    if visited is None:
        visited = set()
    if module in visited or depth > 5:
        return set()
    visited.add(module)
    
    result = set()
    if module in deps:
        for d in deps[module]:
            result.add(d)
            result.update(get_deps_recursive(d, visited, depth + 1))
    return result

for module in sorted(deps.keys()):
    direct = len(deps[module])
    transitive = len(get_deps_recursive(module)) - direct
    total = direct + transitive
    print(f"{module:25} {direct:2} direct, {transitive:2} transitive (total: {total:2})")
