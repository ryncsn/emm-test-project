#!/usr/bin/env python3

import sqlite3, os, shutil

conn = sqlite3.connect('app.db')
cursor = conn.cursor()

# Build the DB only if it hasn't been created yet, so this script is safe to
# re-run (e.g. just to regenerate the file tree) without bloating app.db.
cursor.execute("SELECT COUNT(*) FROM sqlite_master WHERE type='table' AND name='hot_data';")
if cursor.fetchone()[0] == 0:
    # 1. Hot Table: 100,000 indexed records, roughly 60M
    cursor.execute("CREATE TABLE hot_data (id INTEGER PRIMARY KEY, name TEXT, data TEXT);")
    cursor.executemany(
        "INSERT INTO hot_data VALUES (?, ?, ?);",
        [(i, f"data_{i}", "x" * 500) for i in range(120000)]
    )

    # 2. Mid Table: 600,000 indexed records, roughly 300M
    cursor.execute("CREATE TABLE mid_data (id INTEGER PRIMARY KEY, name TEXT, data TEXT);")
    cursor.executemany(
        "INSERT INTO mid_data VALUES (?, ?, ?);",
        [(i, f"data_{i}", "x" * 500) for i in range(600000)]
    )

    # 3. Cold Table: 500,000 unindexed log entries, roughly 1GB
    cursor.execute("CREATE TABLE cold_data (id INTEGER, data TEXT);")
    cursor.executemany(
        "INSERT INTO cold_data VALUES (?, ?);",
        [(i, "x" * 2000) for i in range(500000)]
    )

    conn.commit()
conn.close()

# File tree: 300 folders x 300 files => ~350 MiB of 4 KiB blocks, will
# be much more bloated considering the metadata.
TREE_ROOT = "files"
N_DIRS = 300
N_FILES = 300

shutil.rmtree(TREE_ROOT, ignore_errors=True)
os.makedirs(TREE_ROOT)
for d in range(N_DIRS):
    dd = os.path.join(TREE_ROOT, f"{d:03d}")
    os.makedirs(dd)
    for f in range(N_FILES):
        with open(os.path.join(dd, f"{f:03d}"), "w") as fh:
            fh.write("boo\n" * 512) # Create a small file

print(f"Done: {N_DIRS * N_FILES} files under {TREE_ROOT}/.")
