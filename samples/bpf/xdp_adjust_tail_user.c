/* SPDX-License-Identifier: GPL-2.0
 * Copyright (c) 2018 Facebook
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of version 2 of the GNU General Public
 * License as published by the Free Software Foundation.
 */
#include <linux/bpf.h>
#include <linux/if_link.h>
#include <assert.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <arpa/inet.h>
#include <netinet/ether.h>
#include <unistd.h>
#include <time.h>
#include "bpf_load.h"
#include "libbpf.h"
#include "bpf_util.h"

#define STATS_INTERVAL_S 2U

static int ifindex = -1;
static __u32 xdp_flags;

static void int_exit(int sig)
{
	if (ifindex > -1)
		bpf_set_link_xdp_fd(ifindex, -1, xdp_flags);
	exit(0);
}

/* simple "icmp packet too big sent" counter
 */
static void poll_stats(unsigned int kill_after_s)
{
	time_t started_at = time(NULL);
	__u64 value = 0;
	int key = 0;


	while (!kill_after_s || time(NULL) - started_at <= kill_after_s) {
		sleep(STATS_INTERVAL_S);

		assert(bpf_map_lookup_elem(map_fd[0], &key, &value) == 0);

		printf("icmp \"packet too big\" sent: %10llu pkts\n", value);
	}
}

static void usage(const char *cmd)
{
	printf("Start a XDP prog which send ICMP \"packet too big\" \n"
		"messages if ingress packet is bigger then MAX_SIZE bytes\n");
	printf("Usage: %s [...]\n", cmd);
	printf("    -i <ifindex> Interface Index\n");
	printf("    -T <stop-after-X-seconds> Default: 0 (forever)\n");
	printf("    -S use skb-mode\n");
	printf("    -N enforce native mode\n");
	printf("    -h Display this help\n");
}

int main(int argc, char **argv)
{
	unsigned char opt_flags[256] = {};
	unsigned int kill_after_s = 0;
	const char *optstr = "i:T:SNh";
	struct rlimit r = {RLIM_INFINITY, RLIM_INFINITY};
	char filename[256];
	int opt;
	int i;


	for (i = 0; i < strlen(optstr); i++)
		if (optstr[i] != 'h' && 'a' <= optstr[i] && optstr[i] <= 'z')
			opt_flags[(unsigned char)optstr[i]] = 1;

	while ((opt = getopt(argc, argv, optstr)) != -1) {

		switch (opt) {
		case 'i':
			ifindex = atoi(optarg);
			break;
		case 'T':
			kill_after_s = atoi(optarg);
			break;
		case 'S':
			xdp_flags |= XDP_FLAGS_SKB_MODE;
			break;
		case 'N':
			xdp_flags |= XDP_FLAGS_DRV_MODE;
			break;
		default:
			usage(argv[0]);
			return 1;
		}
		opt_flags[opt] = 0;
	}

	for (i = 0; i < strlen(optstr); i++) {
		if (opt_flags[(unsigned int)optstr[i]]) {
			fprintf(stderr, "Missing argument -%c\n", optstr[i]);
			usage(argv[0]);
			return 1;
		}
	}

	if (setrlimit(RLIMIT_MEMLOCK, &r)) {
		perror("setrlimit(RLIMIT_MEMLOCK, RLIM_INFINITY)");
		return 1;
	}

	snprintf(filename, sizeof(filename), "%s_kern.o", argv[0]);

	if (load_bpf_file(filename)) {
		printf("%s", bpf_log_buf);
		return 1;
	}

	if (!prog_fd[0]) {
		printf("load_bpf_file: %s\n", strerror(errno));
		return 1;
	}

	signal(SIGINT, int_exit);
	signal(SIGTERM, int_exit);

	if (bpf_set_link_xdp_fd(ifindex, prog_fd[0], xdp_flags) < 0) {
		printf("link set xdp fd failed\n");
		return 1;
	}

	poll_stats(kill_after_s);

	bpf_set_link_xdp_fd(ifindex, -1, xdp_flags);

	return 0;
}
