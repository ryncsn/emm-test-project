/*
 * REPRODUCER STRATEGY
 * ===================
 * Assuming running in a 48G memcg:
 *
 * 1. Read a large file (e.g., 8 GB) into page cache, keeping it hot
 *    by re-reading it periodically from many child processes. Each
 *    child process = a separate mm_struct in the lruvec's mm_list.
 *    This makes aging expensive and repeatedly triggered.
 *
 * 2. A separate thread allocates ~44 GB of anonymous memory (no swap),
 *    forcing reclaim. Since anon pages are unevictable without swap,
 *    the reclaimer MUST evict the file cache to make progress.
 *
 * USAGE
 * =====
 *   # First, create a 8 GB test file (once, assuming /mnt/ramdisk
 *   is a fast ramdisk: mkfs.ext4 /dev/pmem0; mount /dev/pmem0 /mnt/ramdisk):
 *   dd if=/dev/zero of=/mnt/ramdisk/zero.img bs=1M count=8196
 *
 *   # Compile:
 *   gcc -O2 -Wall -pthread -o file_anon_mix_pressure file_anon_mix_pressure.c
 *
 *   # Run (in a 48G memcg):
 *   echo 3 > /proc/sys/vm/drop_caches
 *   sync
 *   swapoff -a
 *   sleep 10
 *   ./file_anon_mix_pressure /mnt/ramdisk/zero.img 44g 32
 *
 *   # The file size is auto-detected from the file.
 *   # The anon pressure size (in GB) is required (second argument).
 *   # The number of file reader processes is optional (third argument, default: 64).
 *   # Swap must be disabled
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <pthread.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <signal.h>
#include <errno.h>
#include <time.h>

/* ── Tunables ─────────────────────────────────────────────────────── */

/*
 * Default number of child processes that mmap the file, overridden by argv[3].
 * Each child = a separate mm_struct in the lruvec mm_list.
 * More processes → aging has to walk more page tables → more expensive
 * aging → more likely to trigger should_run_aging() repeatedly.
 */
#define DEFAULT_NR_READERS	64

/*
 * Number of pressure threads that fault anon pages concurrently.
 * Multiple direct reclaimers all hitting the age-then-break path
 * simultaneously is what exhausts reclaim budget and triggers OOM.
 * A single thread just stalls because kswapd can trickle progress.
 */
#define NR_PRESSURE_THREADS	96

/*
 * Number of alloc/release rounds for the pressure loop.
 * Each round mmaps, faults all pages, then munmaps — forcing the
 * reclaimer to evict file cache again on the next round.
 */
#define NR_PRESSURE_ROUNDS	128

/*
 * Read stride for the file reader children (in bytes).
 * Each child reads one page per stride to keep the PTEs accessed,
 * but spread out so the aging walk has to cover all the page tables.
 */
#define READ_STRIDE		(256UL * 1024)  /* 256 KB */

/*
 * How many seconds the file readers keep re-reading before the
 * anonymous pressure starts. This primes the page cache.
 */
#define WARMUP_SECONDS		12
#define ANON_WAIT_SECONDS	3

/* ── Globals ──────────────────────────────────────────────────────── */

static volatile sig_atomic_t stop_readers = 0;
static pid_t *reader_pids;
static int nr_readers;
static const char *filepath;
static size_t filesize;
static int anon_gb;

/* Per-thread info for the pressure workers */
struct pressure_work {
	int id;
	size_t offset;		/* byte offset into the anon region */
	size_t length;		/* bytes this thread is responsible for */
	char *base;		/* mmap base (shared across threads) */
	int success;
};

static void sigterm_handler(int sig)
{
	(void)sig;
	stop_readers = 1;
}

/*
 * File reader child process.
 *
 * Each child opens and mmaps the same file independently — this gives
 * it its own mm_struct and its own set of PTEs mapping the file pages.
 * It periodically reads through the mapping to keep the accessed bits
 * set, ensuring that:
 *   - The file pages stay "hot" (referenced) from aging's perspective
 *   - should_run_aging() keeps returning true because the pages
 *     appear referenced across many mms
 *   - Aging is expensive (must walk all nr_readers mm_structs)
 */
static void file_reader_child(int id)
{
	int fd;
	char *map;
	volatile char sink;
	size_t off;

	/* Each child opens independently → separate file description */
	fd = open(filepath, O_RDONLY);
	if (fd < 0) {
		perror("open");
		_exit(1);
	}

	map = mmap(NULL, filesize, PROT_READ, MAP_SHARED, fd, 0);
	if (map == MAP_FAILED) {
		perror("mmap");
		close(fd);
		_exit(1);
	}
	close(fd);

	/*
	 * Stagger the starting offset so different children touch
	 * different parts of the file on each pass, spreading the
	 * accessed-bit pattern across all page tables.
	 */
	off = ((size_t)id * 4096) % filesize;

	signal(SIGTERM, sigterm_handler);

	while (!stop_readers) {
		size_t pos;

		for (pos = off; pos < filesize; pos += READ_STRIDE) {
			sink = map[pos];
			(void)sink;
		}
		/* Wrap around for the portion we skipped at the start */
		for (pos = 0; pos < off; pos += READ_STRIDE) {
			sink = map[pos];
			(void)sink;
		}

		/*
		 * Small sleep so we don't completely dominate the CPU.
		 */
		usleep(120000);  /* 120 ms */
	}

	munmap(map, filesize);
	_exit(0);
}

/*
 * Anonymous memory pressure worker thread.
 *
 * Each thread touches pages in its assigned portion of a large shared
 * anonymous mapping. Multiple threads faulting simultaneously means
 * multiple direct reclaimers all entering try_to_shrink_lruvec() at
 * once. On the buggy kernel, they ALL hit the age-then-break path
 * and collectively waste all reclaim cycles, triggering OOM.
 *
 * A single thread just stalls (kswapd trickles enough progress), but
 * 8 threads overwhelm the system's reclaim capacity.
 */
static void *pressure_worker(void *arg)
{
	struct pressure_work *work = arg;
	size_t j;

	for (j = 0; j < work->length; j += 4096)
		work->base[work->offset + j] = (char)(j & 0xff);

	work->success = 1;
	return NULL;
}

static void kill_readers(void)
{
	int i;

	for (i = 0; i < nr_readers; i++) {
		if (reader_pids[i] > 0) {
			kill(reader_pids[i], SIGTERM);
		}
	}
	for (i = 0; i < nr_readers; i++) {
		if (reader_pids[i] > 0) {
			waitpid(reader_pids[i], NULL, 0);
			reader_pids[i] = 0;
		}
	}
}

int main(int argc, char **argv)
{
	struct stat st;
	int i;
	struct timespec ts_start, ts_end;
	double elapsed;

	if (argc < 3 || argc > 4) {
		fprintf(stderr,
			"Usage: %s <path-to-large-file> <anon_gb> [nr_readers]\n"
			"\n"
			"  path-to-large-file  Path to a large file (size auto-detected)\n"
			"  anon_gb             Anonymous pressure size in GB\n"
			"  nr_readers          Number of file reader processes (default: %d)\n"
			"\n"
			"Create the file first:\n"
			"  dd if=/dev/urandom of=/tmp/testfile bs=1M count=32768\n"
			"\n"
			"Then run:\n"
			"  sudo swapoff -a\n"
			"  %s /tmp/testfile 108\n",
			argv[0], DEFAULT_NR_READERS, argv[0]);
		return 1;
	}

	filepath = argv[1];
	anon_gb = atoi(argv[2]);
	nr_readers = (argc >= 4) ? atoi(argv[3]) : DEFAULT_NR_READERS;

	if (anon_gb <= 0) {
		fprintf(stderr, "Error: anon_gb must be positive\n");
		return 1;
	}
	if (nr_readers <= 0) {
		fprintf(stderr, "Error: nr_readers must be positive\n");
		return 1;
	}

	reader_pids = calloc(nr_readers, sizeof(pid_t));
	if (!reader_pids) {
		perror("calloc");
		return 1;
	}

	if (stat(filepath, &st) < 0) {
		perror("stat");
		return 1;
	}
	filesize = st.st_size;

	fprintf(stderr, "=== File / Anon pressure reproducer ===\n");
	fprintf(stderr, "  File:         %s (%zu MB)\n",
		filepath, filesize / (1024*1024));
	fprintf(stderr, "  File readers: %d processes (separate mm_structs)\n",
		nr_readers);
	fprintf(stderr, "  Anon pressure: %d GB (%d threads)\n",
		anon_gb, NR_PRESSURE_THREADS);
	fprintf(stderr, "  Read stride:  %lu KB\n", READ_STRIDE / 1024);
	fprintf(stderr, "\n");

	if (filesize < 1UL * 1024 * 1024 * 1024) {
		fprintf(stderr, "WARNING: File is smaller than 1 GB. "
			"For best results, use a ~32 GB file.\n");
	}

	/* Check MGLRU since this reproducer is meant to stress test MGLRU */
	{
		FILE *f = fopen("/sys/kernel/mm/lru_gen/enabled", "r");
		if (f) {
			int val;
			if (fscanf(f, "%i", &val) == 1) {
				if (!(val & 1))
					fprintf(stderr, "WARNING: MGLRU not enabled! "
						"echo 7 > /sys/kernel/mm/lru_gen/enabled\n");
				else
					fprintf(stderr, "  MGLRU:        enabled (0x%x)\n", val);
			}
			fclose(f);
		} else {
			fprintf(stderr, "WARNING: Cannot read /sys/kernel/mm/lru_gen/enabled\n");
		}
	}

	/* Check swap */
	{
		FILE *f = fopen("/proc/swaps", "r");
		if (f) {
			char buf[256];
			int lines = 0;
			while (fgets(buf, sizeof(buf), f))
				lines++;
			fclose(f);
			if (lines > 1)
				fprintf(stderr, "WARNING: Swap is active! "
					"Run 'swapoff -a' first.\n");
			else
				fprintf(stderr, "  Swap:         disabled (good)\n");
		}
	}

	fprintf(stderr, "\n");

	/* ── Phase 1: Fork file reader children ───────────────────── */
	fprintf(stderr, "[phase1] Forking %d file reader processes...\n",
		nr_readers);

	for (i = 0; i < nr_readers; i++) {
		pid_t pid = fork();
		if (pid < 0) {
			perror("fork");
			kill_readers();
			return 1;
		}
		if (pid == 0) {
			/* Child */
			file_reader_child(i);
			/* not reached */
		}
		reader_pids[i] = pid;
	}

	fprintf(stderr, "[phase1] All %d readers forked.\n", nr_readers);

	/* ── Phase 2: Warm up — let readers populate page cache ─── */
	fprintf(stderr, "[phase2] Warming up for %d seconds "
		"(populating page cache)...\n", WARMUP_SECONDS);
	sleep(WARMUP_SECONDS);

	/* Show memory state */
	{
		FILE *f = fopen("/proc/meminfo", "r");
		if (f) {
			char buf[256];
			while (fgets(buf, sizeof(buf), f)) {
				if (strncmp(buf, "MemAvailable:", 13) == 0 ||
				    strncmp(buf, "Cached:", 7) == 0 ||
				    strncmp(buf, "Active(file):", 13) == 0 ||
				    strncmp(buf, "Inactive(file):", 15) == 0)
					fprintf(stderr, "  %s", buf);
			}
			fclose(f);
		}
	}

	/* ── Phase 3: Start anon memory pressure ──────────────────── */
	fprintf(stderr, "\n[phase3] Starting %d anonymous pressure threads "
		"(%d GB x %d rounds)...\n",
		NR_PRESSURE_THREADS, anon_gb, NR_PRESSURE_ROUNDS);

	clock_gettime(CLOCK_MONOTONIC, &ts_start);

	{
		size_t total = (size_t)anon_gb * 1024UL * 1024UL * 1024UL;
		size_t per_thread = (total / NR_PRESSURE_THREADS) & ~4095UL;
		int round;

		for (round = 0; round < NR_PRESSURE_ROUNDS; round++) {
			char *anon_base;
			pthread_t tids[NR_PRESSURE_THREADS];
			struct pressure_work works[NR_PRESSURE_THREADS];
			int all_ok = 1;

			anon_base = mmap(NULL, total, PROT_READ | PROT_WRITE,
					 MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE,
					 -1, 0);
			if (anon_base == MAP_FAILED) {
				perror("[pressure] mmap");
				fprintf(stderr, "[pressure] *** mmap failed at round "
					"%d — likely OOM ***\n", round + 1);
				break;
			}

			fprintf(stderr, "[pressure] Round %d/%d: faulting %d GB "
				"across %d threads...\n",
				round + 1, NR_PRESSURE_ROUNDS,
				anon_gb, NR_PRESSURE_THREADS);

			for (i = 0; i < NR_PRESSURE_THREADS; i++) {
				works[i].id = i;
				works[i].base = anon_base;
				works[i].offset = (size_t)i * per_thread;
				works[i].length = per_thread;
				works[i].success = 0;
				if (pthread_create(&tids[i], NULL, pressure_worker, &works[i])) {
					perror("pthread_create");
					works[i].success = -1;
				}
			}

			for (i = 0; i < NR_PRESSURE_THREADS; i++) {
				if (works[i].success == -1)
					continue;
				pthread_join(tids[i], NULL);
				if (!works[i].success)
					all_ok = 0;
			}

			munmap(anon_base, total);

			if (!all_ok) {
				fprintf(stderr, "[pressure] *** Round %d: some "
					"threads failed — likely OOM ***\n",
					round + 1);
				break;
			}

			fprintf(stderr, "[pressure] Round %d/%d complete.\n",
				round + 1, NR_PRESSURE_ROUNDS);

			sleep(ANON_WAIT_SECONDS);
		}

		if (round == NR_PRESSURE_ROUNDS) {
			fprintf(stderr, "[pressure] All %d rounds of %d GB "
				"completed! No OOM — reclaim worked.\n",
				NR_PRESSURE_ROUNDS, anon_gb);
		}
	}

	clock_gettime(CLOCK_MONOTONIC, &ts_end);
	elapsed = (ts_end.tv_sec - ts_start.tv_sec) +
		  (ts_end.tv_nsec - ts_start.tv_nsec) / 1e9;

	fprintf(stderr, "\n[done] Pressure phase took %.1f seconds.\n", elapsed);

	/* ── Cleanup ─────────────────────────────────────────────── */
	fprintf(stderr, "[cleanup] Terminating file readers...\n");
	kill_readers();

	fprintf(stderr, "\n=== TEST COMPLETE ===\n");
	fprintf(stderr, "If you got here without OOM kills, the fix is working.\n");
	fprintf(stderr, "Check dmesg for OOM messages: dmesg | grep -i oom\n");

	free(reader_pids);
	return 0;
}
