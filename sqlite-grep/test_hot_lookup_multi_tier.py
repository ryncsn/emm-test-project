#!/usr/bin/env python3

from lib import *

conn = connect()

print("Hot usage...  ", end = "")
ms = run_rand_indexed_lookups(conn, "hot_data", HOT_DATA_TABLE_SIZE / 2, HOT_DATA_TABLE_SIZE * 2)
print(f"Done, took: {ms:.2f} ms")

print("Mid lookup... ", end = "")
ms = run_rand_indexed_lookups(conn, "mid_data", MID_DATA_TABLE_SIZE, MID_DATA_TABLE_SIZE / 100)
print(f"Done, took: {ms:.2f} ms")

print("Cold scan...  ", end = "")
ms = run_seq_lookups(conn, "cold_data")
print(f"Done, took: {ms:.2f} ms")
