Reproducer for a race issue with Linux Kernel up to 6.8.

- First apply following kernel patch to increase the race window with brd, or it may take days to reproduce:

```
diff --git a/drivers/block/brd.c b/drivers/block/brd.c
index 970bd6ff38c4..258592219d76 100644
--- a/drivers/block/brd.c
+++ b/drivers/block/brd.c
@@ -24,6 +24,7 @@
 #include <linux/slab.h>
 #include <linux/backing-dev.h>
 #include <linux/debugfs.h>
+#include <linux/delay.h>

 #include <linux/uaccess.h>

@@ -228,6 +229,7 @@ static int brd_do_bvec(struct brd_device *brd, struct page *page,

        mem = kmap_atomic(page);
        if (!op_is_write(opf)) {
                copy_from_brd(mem + off, brd, sector, len);
                flush_dcache_page(page);
+               mdelay(1);
        } else {
```

- Setup up a small ramdisk as swap device:

```
modprobe brd rd_nr=1 rd_size=32768
mkswap /dev/ram0
swapon /dev/ram0
```

- Then just run the reproducer:
gcc -g -lpthread test-thread-swap-race.c && ./a.out

If nothing went wrong, it will loop print following log:

```
Polulating 32MB of memory region...·
Keep swapping out...·
Starting round 0...
Spawning 65536 workers...
32746 workers spawned, wait for done...
Round 0 Good!
Starting round 1...
Spawning 65536 workers...
32745 workers spawned, wait for done...
Round 1 Good!
Starting round 2...
Spawning 65536 workers...
32743 workers spawned, wait for done...
Round 2 Good!
Starting round 3...
Spawning 65536 workers...
32741 workers spawned, wait for done...

...snip...
```

If there are any race causing data loss, folloiwing log is printed:

```
Polulating 32MB of memory region...
Keep swapping out...
Starting round 0...
Spawning 65536 workers...
32737 workers spawned, wait for done...
Round 0: Error on 0x23c00, expected 32737, got 32735, 2 data loss!
Round 0 Failed, 2 data loss!
```

This reproducer used following method to increase race rate:
- Spawn massive workers to stress the swapin path.
- Use only 32M of swap so swap entry reuse is more likely to happen.
- Use madvise to swapout more aggressively, LRU based swapout is less
  likely to swapout a just modified page.
- Add a delay to brd read-in path so the race window is increased.
