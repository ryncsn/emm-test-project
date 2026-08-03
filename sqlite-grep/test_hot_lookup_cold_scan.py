#!/usr/bin/env python3

from time import sleep
from lib import *

conn = connect()

print("Hot lookup... ", end = "")
ms = run_rand_indexed_lookups(conn, "hot_data", HOT_DATA_TABLE_SIZE / 2, HOT_DATA_TABLE_SIZE)
print(f"Done, took: {ms:.2f} ms")

sleep(3)

print("Cold scan...  ", end = "")
ms = run_seq_lookups(conn, "cold_data")
print(f"Done, took: {ms:.2f} ms")

sleep(3)
