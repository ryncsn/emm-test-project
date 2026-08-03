import sqlite3, random, time, subprocess

HOT_DATA_TABLE_SIZE=120000
MID_DATA_TABLE_SIZE=600000
COLD_DATA_TABLE_SIZE=500000

def connect(db_path='app.db'):
    # Force SQLite to rely on OS page cache, that's very close to the default.
    conn = sqlite3.connect(db_path)
    conn.execute("PRAGMA cache_size = -1000;")
    return conn

def run_seq_lookups(conn, table):
    cursor = conn.cursor()
    start = time.perf_counter()
    cursor.execute(f"SELECT COUNT(*) FROM {table} WHERE data LIKE '%missing_key%';")
    cursor.fetchone()
    return (time.perf_counter() - start) * 1000

def run_rand_indexed_lookups(conn, table, table_range, iter):
    cursor = conn.cursor()
    start = time.perf_counter()
    for _ in range(int(iter)):
        id = random.randint(0, table_range)
        cursor.execute(f"SELECT data FROM {table} WHERE id = ?;", (id,))
        cursor.fetchone()
    return (time.perf_counter() - start) * 1000

def run_grep(root, pattern, iterations):
    start = time.perf_counter()
    for _ in range(iterations):
        subprocess.run(
            ["grep", "-r", "-F", pattern, root],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            check=False,
        )
    return (time.perf_counter() - start) * 1000

def run_rg(root, pattern, iterations):
    start = time.perf_counter()
    for _ in range(iterations):
        subprocess.run(
            ["rg", "-ucF", pattern, root],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            check=False,
        )
    return (time.perf_counter() - start) * 1000
