#!/usr/bin/env python3

from time import sleep
from lib import *

conn = connect()

print("Hot scan & lookup...   ", end = "")
ms = run_seq_lookups(conn, "hot_data")
ms = run_rand_indexed_lookups(conn, "hot_data", HOT_DATA_TABLE_SIZE, HOT_DATA_TABLE_SIZE)
print(f"Done, took: {ms:.2f} ms")
sleep(3)

print("File grep...  ", end = "")
ms = run_rg("files", ".", 1)
print(f"Done, took: {ms:.2f} ms")
sleep(3)

print("Hot lookup... ", end = "")
ms = run_rand_indexed_lookups(conn, "hot_data", HOT_DATA_TABLE_SIZE, 1000)
print(f"Done, took: {ms:.2f} ms")
sleep(3)
