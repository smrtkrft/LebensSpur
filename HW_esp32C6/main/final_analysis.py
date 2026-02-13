import os
import re

# Summary table data
files = {
    'main.c': (251, 4),
    'web_server.c': (2275, 67),
    'config_manager.c': (632, 32),
    'mail_sender.c': (657, 23),
    'relay_manager.c': (371, 18),
    'session_auth.c': (370, 21),
    'timer_scheduler.c': (362, 19),
    'gui_downloader.c': (454, 14),
    'ota_manager.c': (309, 12),
    'file_manager.c': (348, 17),
    'log_manager.c': (272, 15),
    'time_manager.c': (195, 12),
    'device_id.c': (113, 6),
    'ext_flash.c': (160, 11),
    'wifi_manager.c': (293, 14),
    'button_manager.c': (186, 7),
    'web_assets.c': (34, 2),
}

# Print summary
print("=" * 90)
print("FILE METRICS SUMMARY")
print("=" * 90)
print(f"{'File':<25} {'Lines':>6} {'Est. Functions':>15} {'Complexity':>15}")
print("-" * 90)

total_lines = 0
total_funcs = 0

for file, (lines, funcs) in sorted(files.items(), key=lambda x: x[1][0], reverse=True):
    total_lines += lines
    total_funcs += funcs
    
    complexity = "VERY HIGH" if lines > 1500 else "HIGH" if lines > 700 else "MODERATE" if lines > 400 else "LOW"
    print(f"{file:<25} {lines:>6} {funcs:>15} {complexity:>15}")

print("-" * 90)
print(f"{'TOTAL':<25} {total_lines:>6} {total_funcs:>15}")
print("=" * 90)

# Identify problem files
print("\nOVERLY COMPLEX FILES (>500 LOC):")
print("-" * 90)
problem_files = [(f, l, fn) for f, (l, fn) in files.items() if l > 500]
for file, lines, funcs in sorted(problem_files, key=lambda x: x[1], reverse=True):
    avg_func_size = lines / funcs if funcs > 0 else 0
    print(f"{file:<25} {lines:5} lines, {funcs:3} functions, ~{avg_func_size:.0f} LOC/function")

print("\n" + "=" * 90)
