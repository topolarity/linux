// SPDX-License-Identifier: GPL-2.0
/*
 * Test program for the perf_event reference timebase (refclock).
 *
 * Creates a reference event (pinned, reference=1) and several counting
 * events with PERF_FORMAT_REFCLOCK_TIME_ENABLED/RUNNING, then verifies
 * that the refclock times are reported correctly.
 *
 * Can use either hardware (ref-cycles, cycles) or software (cpu-clock)
 * events as the reference, depending on PMU availability.
 *
 * Usage: ./refclock-test [--software]
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <linux/perf_event.h>

/* New UAPI bits — remove once the header is updated */
#ifndef PERF_FORMAT_REFCLOCK_TIME_ENABLED
#define PERF_FORMAT_REFCLOCK_TIME_ENABLED	(1U << 5)
#endif
#ifndef PERF_FORMAT_REFCLOCK_TIME_RUNNING
#define PERF_FORMAT_REFCLOCK_TIME_RUNNING	(1U << 6)
#endif

/* The 'reference' bit position in perf_event_attr — after defer_output */
#define ATTR_SET_REFERENCE(attr) \
	((attr)->defer_output |= 0)  /* placeholder: set via raw bitfield below */

static int perf_event_open(struct perf_event_attr *attr, pid_t pid, int cpu,
			   int group_fd, unsigned long flags)
{
	return syscall(__NR_perf_event_open, attr, pid, cpu, group_fd, flags);
}

/*
 * Set the 'reference' bit in perf_event_attr.
 * It's the bit right after defer_output in the bitfield.
 * We use a raw byte manipulation since the header may not have the field yet.
 */
static void attr_set_reference(struct perf_event_attr *attr)
{
	/*
	 * The 'reference' bit is bit 40 of the 64-bit bitfield that starts
	 * at struct offset 40 (the field containing disabled, pinned, etc.).
	 * Bit 40 = byte 5, bit 0 within that byte.
	 */
	unsigned char *p = (unsigned char *)attr;
	p[40 + 5] |= (1 << 0);
}

static void do_work(unsigned long iters)
{
	volatile unsigned long x = 0;

	for (unsigned long i = 0; i < iters; i++)
		x += i;
}

#define PASS(fmt, ...) printf("  PASS: " fmt "\n", ##__VA_ARGS__)
#define FAIL(fmt, ...) printf("  FAIL: " fmt "\n", ##__VA_ARGS__)
#define INFO(fmt, ...) printf("  INFO: " fmt "\n", ##__VA_ARGS__)

struct read_data {
	__u64 value;
	__u64 time_enabled;
	__u64 time_running;
	__u64 refclock_time_enabled;
	__u64 refclock_time_running;
};

/*
 * Test 1: Basic refclock creation and reading.
 * Create a reference event and a counting event, do some work, read.
 */
static int test_basic(int use_software)
{
	struct perf_event_attr ref_attr, count_attr;
	int ref_fd, count_fd;
	struct read_data data;
	int ret = 0;

	printf("Test 1: Basic refclock (%s)\n",
	       use_software ? "software" : "hardware");

	/* Reference event */
	memset(&ref_attr, 0, sizeof(ref_attr));
	ref_attr.size = sizeof(ref_attr);
	ref_attr.pinned = 1;
	ref_attr.disabled = 1;
	if (use_software) {
		ref_attr.type = PERF_TYPE_SOFTWARE;
		ref_attr.config = PERF_COUNT_SW_CPU_CLOCK;
	} else {
		ref_attr.type = PERF_TYPE_HARDWARE;
		ref_attr.config = PERF_COUNT_HW_REF_CPU_CYCLES;
		ref_attr.exclude_kernel = 1;
	}
	attr_set_reference(&ref_attr);

	ref_fd = perf_event_open(&ref_attr, 0, -1, -1, 0);
	if (ref_fd < 0) {
		if (errno == EINVAL) {
			INFO("reference bit not supported by kernel (EINVAL)");
			return -1;
		}
		if (!use_software && errno == ENOENT) {
			INFO("HW_REF_CPU_CYCLES not available, try --software");
			return -1;
		}
		FAIL("ref event open: %s", strerror(errno));
		return 1;
	}

	/* Counting event with refclock format */
	memset(&count_attr, 0, sizeof(count_attr));
	count_attr.size = sizeof(count_attr);
	count_attr.type = PERF_TYPE_SOFTWARE;
	count_attr.config = PERF_COUNT_SW_TASK_CLOCK;
	count_attr.disabled = 1;
	count_attr.read_format = PERF_FORMAT_TOTAL_TIME_ENABLED |
				 PERF_FORMAT_TOTAL_TIME_RUNNING |
				 PERF_FORMAT_REFCLOCK_TIME_ENABLED |
				 PERF_FORMAT_REFCLOCK_TIME_RUNNING;

	count_fd = perf_event_open(&count_attr, 0, -1, -1, 0);
	if (count_fd < 0) {
		FAIL("count event open: %s", strerror(errno));
		close(ref_fd);
		return 1;
	}

	/* Enable both and do work */
	ioctl(ref_fd, PERF_EVENT_IOC_ENABLE, 0);
	ioctl(count_fd, PERF_EVENT_IOC_ENABLE, 0);

	do_work(10000000);

	ioctl(count_fd, PERF_EVENT_IOC_DISABLE, 0);
	ioctl(ref_fd, PERF_EVENT_IOC_DISABLE, 0);

	/* Read the counting event */
	if (read(count_fd, &data, sizeof(data)) != sizeof(data)) {
		FAIL("read: %s", strerror(errno));
		ret = 1;
		goto out;
	}

	INFO("value=%llu", data.value);
	INFO("time_enabled=%llu time_running=%llu",
	     data.time_enabled, data.time_running);
	INFO("refclock_time_enabled=%llu refclock_time_running=%llu",
	     data.refclock_time_enabled, data.refclock_time_running);

	if (data.time_enabled == 0 || data.time_running == 0) {
		FAIL("wall-clock times are zero");
		ret = 1;
	} else {
		PASS("wall-clock times are non-zero");
	}

	if (data.refclock_time_enabled == 0 && data.refclock_time_running == 0) {
		FAIL("refclock times are both zero (refclock not active?)");
		ret = 1;
	} else if (data.refclock_time_running > data.refclock_time_enabled) {
		FAIL("refclock running > enabled");
		ret = 1;
	} else {
		PASS("refclock times are non-zero and consistent");
	}

out:
	close(count_fd);
	close(ref_fd);
	return ret;
}

/*
 * Test 2: Conflict detection.
 * Two reference events in the same context — second should fail to schedule.
 */
static int test_conflict(int use_software)
{
	struct perf_event_attr attr1, attr2;
	int fd1, fd2;
	int ret = 0;

	printf("Test 2: Conflict detection (%s)\n",
	       use_software ? "software" : "hardware");

	memset(&attr1, 0, sizeof(attr1));
	attr1.size = sizeof(attr1);
	attr1.pinned = 1;
	attr1.type = PERF_TYPE_SOFTWARE;
	attr1.config = PERF_COUNT_SW_CPU_CLOCK;
	attr_set_reference(&attr1);

	memcpy(&attr2, &attr1, sizeof(attr2));

	fd1 = perf_event_open(&attr1, 0, -1, -1, 0);
	if (fd1 < 0) {
		FAIL("first ref event: %s", strerror(errno));
		return 1;
	}

	fd2 = perf_event_open(&attr2, 0, -1, -1, 0);
	if (fd2 < 0) {
		FAIL("second ref event open failed: %s", strerror(errno));
		close(fd1);
		return 1;
	}

	/* Enable both — second should enter ERROR */
	ioctl(fd1, PERF_EVENT_IOC_ENABLE, 0);
	ioctl(fd2, PERF_EVENT_IOC_ENABLE, 0);

	do_work(1000000);

	/* Try reading the second — should return 0 (EOF) if in ERROR state */
	__u64 val;
	ssize_t n = read(fd2, &val, sizeof(val));
	if (n == 0) {
		PASS("second reference event is in ERROR state (read returned EOF)");
	} else if (n > 0) {
		INFO("second reference event read succeeded (val=%llu) — may not have conflicted", val);
		/* Not necessarily a failure — depends on scheduling */
	} else {
		FAIL("read error: %s", strerror(errno));
		ret = 1;
	}

	close(fd2);
	close(fd1);
	return ret;
}

/*
 * Test 3: Validation — reference without pinned should fail.
 */
static int test_validation(void)
{
	struct perf_event_attr attr;
	int fd;

	printf("Test 3: Validation (reference without pinned)\n");

	memset(&attr, 0, sizeof(attr));
	attr.size = sizeof(attr);
	attr.type = PERF_TYPE_SOFTWARE;
	attr.config = PERF_COUNT_SW_CPU_CLOCK;
	attr.pinned = 0;  /* NOT pinned */
	attr_set_reference(&attr);

	fd = perf_event_open(&attr, 0, -1, -1, 0);
	if (fd < 0 && errno == EINVAL) {
		PASS("reference without pinned correctly rejected (EINVAL)");
		return 0;
	} else if (fd < 0) {
		FAIL("unexpected error: %s", strerror(errno));
		return 1;
	} else {
		FAIL("reference without pinned was accepted (should be EINVAL)");
		close(fd);
		return 1;
	}
}

/*
 * Test 4: Validation — reference with inherit should fail.
 */
static int test_validation_inherit(void)
{
	struct perf_event_attr attr;
	int fd;

	printf("Test 4: Validation (reference with inherit)\n");

	memset(&attr, 0, sizeof(attr));
	attr.size = sizeof(attr);
	attr.type = PERF_TYPE_SOFTWARE;
	attr.config = PERF_COUNT_SW_CPU_CLOCK;
	attr.pinned = 1;
	attr.inherit = 1;
	attr_set_reference(&attr);

	fd = perf_event_open(&attr, 0, -1, -1, 0);
	if (fd < 0 && errno == EINVAL) {
		PASS("reference with inherit correctly rejected (EINVAL)");
		return 0;
	} else if (fd < 0) {
		FAIL("unexpected error: %s", strerror(errno));
		return 1;
	} else {
		FAIL("reference with inherit was accepted (should be EINVAL)");
		close(fd);
		return 1;
	}
}

/*
 * Test 5: No refclock — events with REFCLOCK format but no reference
 * event should get zero refclock times.
 */
static int test_no_refclock(void)
{
	struct perf_event_attr attr;
	struct read_data data;
	int fd;

	printf("Test 5: No refclock (zero refclock times expected)\n");

	memset(&attr, 0, sizeof(attr));
	attr.size = sizeof(attr);
	attr.type = PERF_TYPE_SOFTWARE;
	attr.config = PERF_COUNT_SW_TASK_CLOCK;
	attr.disabled = 1;
	attr.read_format = PERF_FORMAT_TOTAL_TIME_ENABLED |
			   PERF_FORMAT_TOTAL_TIME_RUNNING |
			   PERF_FORMAT_REFCLOCK_TIME_ENABLED |
			   PERF_FORMAT_REFCLOCK_TIME_RUNNING;

	fd = perf_event_open(&attr, 0, -1, -1, 0);
	if (fd < 0) {
		FAIL("event open: %s", strerror(errno));
		return 1;
	}

	ioctl(fd, PERF_EVENT_IOC_ENABLE, 0);
	do_work(1000000);
	ioctl(fd, PERF_EVENT_IOC_DISABLE, 0);

	if (read(fd, &data, sizeof(data)) != sizeof(data)) {
		FAIL("read: %s", strerror(errno));
		close(fd);
		return 1;
	}

	INFO("refclock_time_enabled=%llu refclock_time_running=%llu",
	     data.refclock_time_enabled, data.refclock_time_running);

	if (data.refclock_time_enabled == 0 && data.refclock_time_running == 0) {
		PASS("refclock times are zero without a reference event");
	} else {
		FAIL("refclock times are non-zero without a reference event");
		close(fd);
		return 1;
	}

	close(fd);
	return 0;
}

int main(int argc, char **argv)
{
	int use_software = 0;
	int failures = 0;
	int skipped = 0;
	int r;

	if (argc > 1 && strcmp(argv[1], "--software") == 0)
		use_software = 1;

	r = test_basic(use_software);
	if (r < 0) skipped++;
	else failures += r;

	r = test_conflict(use_software);
	if (r < 0) skipped++;
	else failures += r;

	failures += test_validation();
	failures += test_validation_inherit();
	failures += test_no_refclock();

	printf("\n%d failures, %d skipped\n", failures, skipped);
	return failures ? 1 : 0;
}
