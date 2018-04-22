/*
 * Copyright (c) 2016-2018 Wuklab, Purdue University. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include <linux/sched.h>
#include <linux/init.h>
#include <linux/mm.h>
#include <linux/net.h>
#include <linux/kthread.h>
#include <linux/workqueue.h>
#include <linux/list.h>
#include <linux/string.h>
#include <linux/jiffies.h>
#include <linux/pci.h>
#include <linux/delay.h>
#include <linux/slab.h>
#include <linux/time.h>
#include <linux/kernel.h>
#include <rdma/ib_verbs.h>

#include "../../include/uapi/fit.h"

#include "fit_internal.h"

/*
 * NOTE:
 * This array specifies hostname of machines you want to use in Lego cluster.
 * Hostnames are listed by the order of FIT node ID. Any wrong configuration
 * lead to an early panic.
 */
static const char *lego_cluster_hostnames[CONFIG_FIT_NR_NODES] = {
	[0]	= 	"wuklab00",
	[1]	= 	"wuklab02",
	[2]	= 	"wuklab07",
};

/* Built based on node id */
struct fit_machine_info *lego_cluster[CONFIG_FIT_NR_NODES];

static struct fit_machine_info WUKLAB_CLUSTER[] = {
[0]	= {	.hostname =	"wuklab00",	.lid =	2,	.first_qpn =	0,	},
[1]	= {	.hostname =	"wuklab01",	.lid =	6,	.first_qpn =	72,	},
[2]	= {	.hostname =	"wuklab02",	.lid =	8,	.first_qpn =	72,	},
[3]	= {	.hostname =	"wuklab03",	.lid =	9,	.first_qpn =	74,	},
[4]	= {	.hostname =	"wuklab04",	.lid =	7,	.first_qpn =	72,	},
[5]	= {	.hostname =	"wuklab05",	.lid =	3,	.first_qpn =	0,	},
[6]	= {	.hostname =	"wuklab06",	.lid =	5,	.first_qpn =	0,	},
[7]	= {	.hostname =	"wuklab07",	.lid =	4,	.first_qpn =	74,	},
[8]	= {	.hostname =	"wuklab08",	.lid =	10,	.first_qpn =	72,	},
[9]	= {	.hostname =	"wuklab09",	.lid =	12,	.first_qpn =	72,	},
[10]	= {	.hostname =	"wuklab10",	.lid =	14,	.first_qpn =	74,	},
[11]	= {	.hostname =	"wuklab11",	.lid =	11,	.first_qpn =	74,	},
[12]	= {	.hostname =	"wuklab12",	.lid =	13,	.first_qpn =	72,	},
[13]	= {	.hostname =	"wuklab13",	.lid =	15,	.first_qpn =	72,	},
[14]	= {	.hostname =	"wuklab14",	.lid =	16,	.first_qpn =	74,	},
[15]	= {	.hostname =	"wuklab15",	.lid =	17,	.first_qpn =	72,	},
[16]	= {	.hostname =	"wuklab16",	.lid =	20,	.first_qpn =	74,	},
[17]	= {	.hostname =	"wuklab17",	.lid =	21,	.first_qpn =	0,	},
[18]	= {	.hostname =	"wuklab18",	.lid =	19,	.first_qpn =	0,	},
[19]	= {	.hostname =	"wuklab19",	.lid =	18,	.first_qpn =	74,	},
[20]	= {	.hostname =	"wuklab20",	.lid =	27,	.first_qpn =	0,	},
[21]	= {	.hostname =	"wuklab21",	.lid =	28,	.first_qpn =	0,	},
[22]	= {	.hostname =	"wuklab22",	.lid =	29,	.first_qpn =	0,	},
[23]	= {	.hostname =	"wuklab23",	.lid =	30,	.first_qpn =	0,	},
[24]	= {	.hostname =	"wuklab24",	.lid =	31,	.first_qpn =	0,	},
[25]	= {	.hostname =	"wuklab25",	.lid =	26,	.first_qpn =	0,	},
};

/* Indicate machines that are used by lego */
static DECLARE_BITMAP(cluster_used_machines, 32);

/* Exposed array used by FIT code */
unsigned int global_lid[CONFIG_FIT_NR_NODES];
unsigned int first_qpn[CONFIG_FIT_NR_NODES];

unsigned int get_node_global_lid(unsigned int nid)
{
	BUG_ON(nid >= CONFIG_FIT_NR_NODES);
	return global_lid[nid];
}

unsigned int get_node_first_qpn(unsigned int nid)
{
	BUG_ON(nid >= CONFIG_FIT_NR_NODES);
	return first_qpn[nid];
}

/*
 * This come after arrays are initialized
 * We check if this runtime's QPN matches our wuklab_cluster table
 */
void check_current_first_qpn(unsigned int qpn)
{
	struct fit_machine_info *self = lego_cluster[CONFIG_FIT_LOCAL_ID];

	if (self->first_qpn == qpn)
		return;

	pr_err("******\n");
	pr_err("******\n");
	pr_err("******  ERROR: QPN Changed!\n");
	pr_err("******  Other Lego machines will fail to connect.\n");
	pr_err("******  (Previous: %d New: %d)\n", self->first_qpn, qpn);
	pr_err("******\n");
	pr_err("******\n");
}

/*
 * Fill the lego_cluster and global_lid array based on nid.
 * Return 0 on success, return 1 if duplicates
 */
static int assign_fit_machine(unsigned int nid, struct fit_machine_info *machine)
{
	unsigned int machine_index;

	machine_index = machine - WUKLAB_CLUSTER;
	if (test_and_set_bit(machine_index, cluster_used_machines))
		return 1;

	/* Sanity Check */
	if (machine->first_qpn == 0) {
		pr_info("******\n");
		pr_info("******      WARNING: %s first_qpn not finalized, "
			"default to use 72\n", machine->hostname);
		pr_info("******\n");
		machine->first_qpn = 72;
	}

	lego_cluster[nid] = machine;
	global_lid[nid] = lego_cluster[nid]->lid;
	first_qpn[nid] = lego_cluster[nid]->first_qpn;

	return 0;
}

static struct fit_machine_info *find_fit_machine(const char *hostname)
{
	struct fit_machine_info *machine;
	int i;

	/* Linear search for a small cluster */
	for (i = 0; i < ARRAY_SIZE(WUKLAB_CLUSTER); i++) {
		machine = &WUKLAB_CLUSTER[i];
		if (!strncmp(hostname, machine->hostname, FIT_HOSTNAME_MAX))
			return machine;
	}
	return NULL;
}

/*
 * Statically setting LIDs and QPNs now
 * since we don't have socket working
 */
void init_global_lid_qpn(void)
{
	int nid;
	bool bug = false;

	BUILD_BUG_ON(CONFIG_FIT_LOCAL_ID >= CONFIG_FIT_NR_NODES);

	/*
	 * Build the machine list based on user provided
	 * hostnames, including global_lid array and first_qpn.
	 */
	for (nid = 0; nid < CONFIG_FIT_NR_NODES; nid++) {
		struct fit_machine_info *machine;
		const char *hostname = lego_cluster_hostnames[nid];

		if (!hostname) {
			pr_info("    Empty hostname on node %d\n", nid);
			bug = true;
			continue;
		}

		machine = find_fit_machine(hostname);
		if (!machine) {
			pr_info("    Wrong hostname %s on node %d\n",
				hostname, nid);
			bug = true;
			continue;
		}

		if (assign_fit_machine(nid, machine)) {
			pr_info("    Duplicated hostname %s on node %d\n",
				hostname, nid);
			bug = true;
		}
	}
	if (bug)
		panic("Please check your network config!");

	/* FIT module can get the first_qpn from linux */
}

void print_gloabl_lid(void)
{
	int nid;

	pr_info("***  FIT_local_id:            %d\n", CONFIG_FIT_LOCAL_ID);
	pr_info("***\n");
	pr_info("***    NodeID    Hostname    LID    QPN\n");
	for (nid = 0; nid < CONFIG_FIT_NR_NODES; nid++) {
		pr_info("***    %6d    %s    %3d    %3d",
			nid, lego_cluster[nid]->hostname,
			get_node_global_lid(nid),
			get_node_first_qpn(nid));

		if (nid == CONFIG_FIT_LOCAL_ID)
			pr_cont(" <---\n");
		else
			pr_cont("\n");
	}
	pr_info("***\n");
}
