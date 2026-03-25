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
 * 2. A separate thread allocates ~44 GB of anonymous memory,
 *    forcing reclaim.
 *
 * USAGE
 * =====
 *   # Compile:
 *   gcc -O2 -Wall -pthread -o file_anon_mix_pressure file_anon_mix_pressure.c
 *
 *   # Run (in a 48G memcg):
 *   echo 3 > /proc/sys/vm/drop_caches
 *   sync
 *   sleep 10
 *   ./file_anon_mix_pressure /mnt/ramdisk/zero.img 44g 8g
 *
 *   # The test file at the given path is auto-created if it doesn't exist
 *   # or extended if smaller than file_size.
 *   # The anon pressure size and file cache size accept suffixes: g/G, m/M, k/K.
 *   # Optional arguments:
 *   #   anon_workers   - number of pressure threads (default: 96)
 *   #   file_workers   - number of file reader processes (default: 64)
 *   #   reader_sleep   - file reader sleep between passes in microseconds (default: 120000)
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
 * Default number of child processes that mmap the file, overridden by argv[5].
 * Each child = a separate mm_struct in the lruvec mm_list.
 * More processes → aging has to walk more page tables → more expensive
 * aging → more likely to trigger should_run_aging() repeatedly.
 */
#define DEFAULT_NR_READERS	64

/*
 * Default number of pressure threads that fault anon pages concurrently,
 * overridden by argv[4].
 * Multiple direct reclaimers all hitting the age-then-break path
 * simultaneously is what exhausts reclaim budget and triggers OOM.
 * A single thread just stalls because kswapd can trickle progress.
 */
#define DEFAULT_NR_PRESSURE_THREADS	96

/*
 * Default sleep time for file readers in microseconds, overridden by argv[6].
 * Small sleep so readers don't completely dominate the CPU.
 */
#define DEFAULT_READER_SLEEP_US		120000  /* 120 ms */

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
static int nr_pressure_threads;
static useconds_t reader_sleep_us;
static const char *filepath;
static size_t filesize;
static size_t anon_size;
static size_t file_size;

/*
 * Parse a size string with optional suffix: g/G (GiB), m/M (MiB), k/K (KiB).
 * Returns the size in bytes, or 0 on parse error.
 */
static size_t parse_size(const char *str)
{
	char *end;
	double val;

	val = strtod(str, &end);
	if (end == str || val < 0)
		return 0;

	switch (*end) {
	case 'g': case 'G':
		return (size_t)(val * 1024 * 1024 * 1024);
	case 'm': case 'M':
		return (size_t)(val * 1024 * 1024);
	case 'k': case 'K':
		return (size_t)(val * 1024);
	case '\0':
		/* No suffix: assume bytes */
		return (size_t)val;
	default:
		return 0;
	}
}

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
		usleep(reader_sleep_us);
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

	if (argc < 4 || argc > 7) {
		fprintf(stderr,
			"Usage: %s <path-to-large-file> <anon_size> <file_size>"
			" [anon_workers] [file_workers] [reader_sleep_us]\n"
			"\n"
			"  path-to-large-file  Path to a large file (auto-created if missing or too small)\n"
			"  anon_size           Anonymous pressure size (e.g., 44g, 512m)\n"
			"  file_size           File cache working set size (e.g., 8g, 4096m)\n"
			"  anon_workers        Number of anon pressure threads (default: %d)\n"
			"  file_workers        Number of file reader processes (default: %d)\n"
			"  reader_sleep_us     File reader sleep in microseconds (default: %d)\n"
			"\n"
			"The test file is auto-created/extended if it doesn't exist or is too small.\n"
			"\n"
			"Example:\n"
			"  %s /tmp/testfile 44g 8g\n",
			argv[0], DEFAULT_NR_PRESSURE_THREADS,
			DEFAULT_NR_READERS, DEFAULT_READER_SLEEP_US, argv[0]);
		return 1;
	}

	filepath = argv[1];

	anon_size = parse_size(argv[2]);
	if (anon_size == 0) {
		fprintf(stderr, "Error: invalid anon_size '%s'\n", argv[2]);
		return 1;
	}

	file_size = parse_size(argv[3]);
	if (file_size == 0) {
		fprintf(stderr, "Error: invalid file_size '%s'\n", argv[3]);
		return 1;
	}

	nr_pressure_threads = (argc >= 5) ? atoi(argv[4]) : DEFAULT_NR_PRESSURE_THREADS;
	nr_readers = (argc >= 6) ? atoi(argv[5]) : DEFAULT_NR_READERS;
	reader_sleep_us = (argc >= 7) ? (useconds_t)atoi(argv[6]) : DEFAULT_READER_SLEEP_US;

	if (nr_pressure_threads <= 0) {
		fprintf(stderr, "Error: anon_workers must be positive\n");
		return 1;
	}
	if (nr_readers <= 0) {
		fprintf(stderr, "Error: file_workers must be positive\n");
		return 1;
	}

	reader_pids = calloc(nr_readers, sizeof(pid_t));
	if (!reader_pids) {
		perror("calloc");
		return 1;
	}

	if (stat(filepath, &st) < 0 || (size_t)st.st_size < file_size) {
		int need_create = (errno == ENOENT || stat(filepath, &st) < 0);

		fprintf(stderr, "[setup] %s '%s' (%zu MB)...\n",
			need_create ? "Creating test file" : "Extending test file",
			filepath, file_size / (1024 * 1024));

		{
			int fd;
			size_t remaining, current_size = 0;
			char buf[1024 * 1024]; /* 1 MB write buffer */

			fd = open(filepath, O_WRONLY | O_CREAT, 0644);
			if (fd < 0) {
				perror("open (create)");
				free(reader_pids);
				return 1;
			}

			if (!need_create) {
				current_size = st.st_size;
				if (lseek(fd, 0, SEEK_END) < 0) {
					perror("lseek");
					close(fd);
					free(reader_pids);
					return 1;
				}
			}

			memset(buf, 0, sizeof(buf));
			remaining = file_size - current_size;
			while (remaining > 0) {
				size_t chunk = remaining < sizeof(buf) ?
					       remaining : sizeof(buf);
				ssize_t written = write(fd, buf, chunk);

				if (written < 0) {
					perror("write");
					close(fd);
					free(reader_pids);
					return 1;
				}
				remaining -= written;
			}

			if (fdatasync(fd) < 0)
				perror("fdatasync");
			close(fd);
		}

		if (stat(filepath, &st) < 0) {
			perror("stat (after create)");
			free(reader_pids);
			return 1;
		}
		fprintf(stderr, "[setup] File ready: %zu MB\n",
			(size_t)st.st_size / (1024 * 1024));
	}
	filesize = file_size;

	fprintf(stderr, "=== File / Anon pressure reproducer ===\n");
	fprintf(stderr, "  File:           %s (%zu MB mapped)\n",
		filepath, filesize / (1024*1024));
	fprintf(stderr, "  File workers:   %d processes (separate mm_structs)\n",
		nr_readers);
	fprintf(stderr, "  Reader sleep:   %u us\n", (unsigned)reader_sleep_us);
	fprintf(stderr, "  Anon pressure:  %zu MB (%d threads)\n",
		anon_size / (1024*1024), nr_pressure_threads);
	fprintf(stderr, "  Read stride:  %lu KB\n", READ_STRIDE / 1024);
	fprintf(stderr, "\n");

	if (filesize < 1UL * 1024 * 1024 * 1024) {
		fprintf(stderr, "WARNING: File working set is smaller than 1 GB. "
			"For best results, use a larger file_size.\n");
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
		"(%zu MB x %d rounds)...\n",
		nr_pressure_threads, anon_size / (1024*1024), NR_PRESSURE_ROUNDS);

	clock_gettime(CLOCK_MONOTONIC, &ts_start);

	{
		size_t total = anon_size;
		size_t per_thread = (total / nr_pressure_threads) & ~4095UL;
		int round;
		pthread_t *tids;
		struct pressure_work *works;

		tids = calloc(nr_pressure_threads, sizeof(pthread_t));
		works = calloc(nr_pressure_threads, sizeof(struct pressure_work));
		if (!tids || !works) {
			perror("calloc");
			kill_readers();
			free(tids);
			free(works);
			free(reader_pids);
			return 1;
		}

		for (round = 0; round < NR_PRESSURE_ROUNDS; round++) {
			char *anon_base;
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

			fprintf(stderr, "[pressure] Round %d/%d: faulting %zu MB "
				"across %d threads...\n",
				round + 1, NR_PRESSURE_ROUNDS,
				anon_size / (1024*1024), nr_pressure_threads);

			for (i = 0; i < nr_pressure_threads; i++) {
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

			for (i = 0; i < nr_pressure_threads; i++) {
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
			fprintf(stderr, "[pressure] All %d rounds of %zu MB "
				"completed! No OOM — reclaim worked.\n",
				NR_PRESSURE_ROUNDS, anon_size / (1024*1024));
		}

		free(tids);
		free(works);
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
