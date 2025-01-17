/*
 * Completely Fair Scheduling (CFS) Class (SCHED_NORMAL/SCHED_BATCH)
 *
 *  Copyright (C) 2007 Red Hat, Inc., Ingo Molnar <mingo@redhat.com>
 *
 *  Interactivity improvements by Mike Galbraith
 *  (C) 2007 Mike Galbraith <efault@gmx.de>
 *
 *  Various enhancements by Dmitry Adamushko.
 *  (C) 2007 Dmitry Adamushko <dmitry.adamushko@gmail.com>
 *
 *  Group scheduling enhancements by Srivatsa Vaddagiri
 *  Copyright IBM Corporation, 2007
 *  Author: Srivatsa Vaddagiri <vatsa@linux.vnet.ibm.com>
 *
 *  Scaled math optimizations by Thomas Gleixner
 *  Copyright (C) 2007, Thomas Gleixner <tglx@linutronix.de>
 *
 *  Adaptive scheduling granularity, math enhancements by Peter Zijlstra
 *  Copyright (C) 2007 Red Hat, Inc., Peter Zijlstra <pzijlstr@redhat.com>
 */

#include <linux/latencytop.h>
#include <linux/sched.h>
#include <linux/cpumask.h>
#include <linux/cpuidle.h>
#include <linux/slab.h>
#include <linux/profile.h>
#include <linux/interrupt.h>
#include <linux/mempolicy.h>
#include <linux/migrate.h>
#include <linux/task_work.h>
#include <linux/ratelimit.h>

#include <trace/events/sched.h>

#include "qhmp_sched.h"

/*
 * Targeted preemption latency for CPU-bound tasks:
 * (default: 6ms * (1 + ilog(ncpus)), units: nanoseconds)
 *
 * NOTE: this latency value is not the same as the concept of
 * 'timeslice length' - timeslices in CFS are of variable length
 * and have no persistent notion like in traditional, time-slice
 * based scheduling concepts.
 *
 * (to see the precise effective timeslice length of your workload,
 *  run vmstat and monitor the context-switches (cs) field)
 */
unsigned int sysctl_sched_latency = 6000000ULL;
unsigned int normalized_sysctl_sched_latency = 6000000ULL;

/*
 * The initial- and re-scaling of tunables is configurable
 * (default SCHED_TUNABLESCALING_LOG = *(1+ilog(ncpus))
 *
 * Options are:
 * SCHED_TUNABLESCALING_NONE - unscaled, always *1
 * SCHED_TUNABLESCALING_LOG - scaled logarithmical, *1+ilog(ncpus)
 * SCHED_TUNABLESCALING_LINEAR - scaled linear, *ncpus
 */
enum sched_tunable_scaling sysctl_sched_tunable_scaling
	= SCHED_TUNABLESCALING_LOG;

/*
 * Minimal preemption granularity for CPU-bound tasks:
 * (default: 0.75 msec * (1 + ilog(ncpus)), units: nanoseconds)
 */
unsigned int sysctl_sched_min_granularity = 750000ULL;
unsigned int normalized_sysctl_sched_min_granularity = 750000ULL;

/*
 * is kept at sysctl_sched_latency / sysctl_sched_min_granularity
 */
static unsigned int sched_nr_latency = 8;

/*
 * After fork, child runs first. If set to 0 (default) then
 * parent will (try to) run first.
 */
unsigned int sysctl_sched_child_runs_first __read_mostly;

/*
 * Controls whether, when SD_SHARE_PKG_RESOURCES is on, if all
 * tasks go to idle CPUs when woken. If this is off, note that the
 * per-task flag PF_WAKE_UP_IDLE can still cause a task to go to an
 * idle CPU upon being woken.
 */
unsigned int __read_mostly sysctl_sched_wake_to_idle;

/*
 * SCHED_OTHER wake-up granularity.
 * (default: 1 msec * (1 + ilog(ncpus)), units: nanoseconds)
 *
 * This option delays the preemption effects of decoupled workloads
 * and reduces their over-scheduling. Synchronous workloads will still
 * have immediate wakeup/sleep latencies.
 */
unsigned int sysctl_sched_wakeup_granularity = 1000000UL;
unsigned int normalized_sysctl_sched_wakeup_granularity = 1000000UL;

const_debug unsigned int sysctl_sched_migration_cost = 500000UL;

/*
 * The exponential sliding  window over which load is averaged for shares
 * distribution.
 * (default: 10msec)
 */
unsigned int __read_mostly sysctl_sched_shares_window = 10000000UL;

#ifdef CONFIG_CFS_BANDWIDTH
/*
 * Amount of runtime to allocate from global (tg) to local (per-cfs_rq) pool
 * each time a cfs_rq requests quota.
 *
 * Note: in the case that the slice exceeds the runtime remaining (either due
 * to consumption or the quota being specified to be smaller than the slice)
 * we will always only issue the remaining available time.
 *
 * default: 5 msec, units: microseconds
  */
unsigned int sysctl_sched_cfs_bandwidth_slice = 5000UL;
#endif

#ifdef CONFIG_SCHEDSTATS
unsigned int sysctl_sched_latency_panic_threshold;
unsigned int sysctl_sched_latency_warn_threshold;

struct sched_max_latency {
	unsigned int latency_us;
	char comm[TASK_COMM_LEN];
	pid_t pid;
};

static DEFINE_PER_CPU(struct sched_max_latency, sched_max_latency);
#endif /* CONFIG_SCHEDSTATS */

static inline void update_load_add(struct load_weight *lw, unsigned long inc)
{
	lw->weight += inc;
	lw->inv_weight = 0;
}

static inline void update_load_sub(struct load_weight *lw, unsigned long dec)
{
	lw->weight -= dec;
	lw->inv_weight = 0;
}

static inline void update_load_set(struct load_weight *lw, unsigned long w)
{
	lw->weight = w;
	lw->inv_weight = 0;
}

/*
 * Increase the granularity value when there are more CPUs,
 * because with more CPUs the 'effective latency' as visible
 * to users decreases. But the relationship is not linear,
 * so pick a second-best guess by going with the log2 of the
 * number of CPUs.
 *
 * This idea comes from the SD scheduler of Con Kolivas:
 */
static int get_update_sysctl_factor(void)
{
	unsigned int cpus = min_t(int, num_online_cpus(), 8);
	unsigned int factor;

	switch (sysctl_sched_tunable_scaling) {
	case SCHED_TUNABLESCALING_NONE:
		factor = 1;
		break;
	case SCHED_TUNABLESCALING_LINEAR:
		factor = cpus;
		break;
	case SCHED_TUNABLESCALING_LOG:
	default:
		factor = 1 + ilog2(cpus);
		break;
	}

	return factor;
}

static void update_sysctl(void)
{
	unsigned int factor = get_update_sysctl_factor();

#define SET_SYSCTL(name) \
	(sysctl_##name = (factor) * normalized_sysctl_##name)
	SET_SYSCTL(sched_min_granularity);
	SET_SYSCTL(sched_latency);
	SET_SYSCTL(sched_wakeup_granularity);
#undef SET_SYSCTL
}

void sched_init_granularity(void)
{
	update_sysctl();
}

#define WMULT_CONST	(~0U)
#define WMULT_SHIFT	32

static void __update_inv_weight(struct load_weight *lw)
{
	unsigned long w;

	if (likely(lw->inv_weight))
		return;

	w = scale_load_down(lw->weight);

	if (BITS_PER_LONG > 32 && unlikely(w >= WMULT_CONST))
		lw->inv_weight = 1;
	else if (unlikely(!w))
		lw->inv_weight = WMULT_CONST;
	else
		lw->inv_weight = WMULT_CONST / w;
}

/*
 * delta_exec * weight / lw.weight
 *   OR
 * (delta_exec * (weight * lw->inv_weight)) >> WMULT_SHIFT
 *
 * Either weight := NICE_0_LOAD and lw \e prio_to_wmult[], in which case
 * we're guaranteed shift stays positive because inv_weight is guaranteed to
 * fit 32 bits, and NICE_0_LOAD gives another 10 bits; therefore shift >= 22.
 *
 * Or, weight =< lw.weight (because lw.weight is the runqueue weight), thus
 * weight/lw.weight <= 1, and therefore our shift will also be positive.
 */
static u64 __calc_delta(u64 delta_exec, unsigned long weight, struct load_weight *lw)
{
	u64 fact = scale_load_down(weight);
	int shift = WMULT_SHIFT;

	__update_inv_weight(lw);

	if (unlikely(fact >> 32)) {
		while (fact >> 32) {
			fact >>= 1;
			shift--;
		}
	}

	/* hint to use a 32x32->64 mul */
	fact = (u64)(u32)fact * lw->inv_weight;

	while (fact >> 32) {
		fact >>= 1;
		shift--;
	}

	return mul_u64_u32_shr(delta_exec, fact, shift);
}

#ifdef CONFIG_SMP
static int active_load_balance_cpu_stop(void *data);
#endif

const struct sched_class fair_sched_class;

/**************************************************************
 * CFS operations on generic schedulable entities:
 */

#ifdef CONFIG_FAIR_GROUP_SCHED

/* cpu runqueue to which this cfs_rq is attached */
static inline struct rq *rq_of(struct cfs_rq *cfs_rq)
{
	return cfs_rq->rq;
}

/* An entity is a task if it doesn't "own" a runqueue */
#define entity_is_task(se)	(!se->my_q)

static inline struct task_struct *task_of(struct sched_entity *se)
{
#ifdef CONFIG_SCHED_DEBUG
	WARN_ON_ONCE(!entity_is_task(se));
#endif
	return container_of(se, struct task_struct, se);
}

/* Walk up scheduling entities hierarchy */
#define for_each_sched_entity(se) \
		for (; se; se = se->parent)

static inline struct cfs_rq *task_cfs_rq(struct task_struct *p)
{
	return p->se.cfs_rq;
}

/* runqueue on which this entity is (to be) queued */
static inline struct cfs_rq *cfs_rq_of(struct sched_entity *se)
{
	return se->cfs_rq;
}

/* runqueue "owned" by this group */
static inline struct cfs_rq *group_cfs_rq(struct sched_entity *grp)
{
	return grp->my_q;
}

static inline void list_add_leaf_cfs_rq(struct cfs_rq *cfs_rq)
{
	if (!cfs_rq->on_list) {
		/*
		 * Ensure we either appear before our parent (if already
		 * enqueued) or force our parent to appear after us when it is
		 * enqueued.  The fact that we always enqueue bottom-up
		 * reduces this to two cases.
		 */
		if (cfs_rq->tg->parent &&
		    cfs_rq->tg->parent->cfs_rq[cpu_of(rq_of(cfs_rq))]->on_list) {
			list_add_rcu(&cfs_rq->leaf_cfs_rq_list,
				&rq_of(cfs_rq)->leaf_cfs_rq_list);
		} else {
			list_add_tail_rcu(&cfs_rq->leaf_cfs_rq_list,
				&rq_of(cfs_rq)->leaf_cfs_rq_list);
		}

		cfs_rq->on_list = 1;
	}
}

static inline void list_del_leaf_cfs_rq(struct cfs_rq *cfs_rq)
{
	if (cfs_rq->on_list) {
		list_del_rcu(&cfs_rq->leaf_cfs_rq_list);
		cfs_rq->on_list = 0;
	}
}

/* Iterate thr' all leaf cfs_rq's on a runqueue */
#define for_each_leaf_cfs_rq(rq, cfs_rq) \
	list_for_each_entry_rcu(cfs_rq, &rq->leaf_cfs_rq_list, leaf_cfs_rq_list)

/* Do the two (enqueued) entities belong to the same group ? */
static inline struct cfs_rq *
is_same_group(struct sched_entity *se, struct sched_entity *pse)
{
	if (se->cfs_rq == pse->cfs_rq)
		return se->cfs_rq;

	return NULL;
}

static inline struct sched_entity *parent_entity(struct sched_entity *se)
{
	return se->parent;
}

static void
find_matching_se(struct sched_entity **se, struct sched_entity **pse)
{
	int se_depth, pse_depth;

	/*
	 * preemption test can be made between sibling entities who are in the
	 * same cfs_rq i.e who have a common parent. Walk up the hierarchy of
	 * both tasks until we find their ancestors who are siblings of common
	 * parent.
	 */

	/* First walk up until both entities are at same depth */
	se_depth = (*se)->depth;
	pse_depth = (*pse)->depth;

	while (se_depth > pse_depth) {
		se_depth--;
		*se = parent_entity(*se);
	}

	while (pse_depth > se_depth) {
		pse_depth--;
		*pse = parent_entity(*pse);
	}

	while (!is_same_group(*se, *pse)) {
		*se = parent_entity(*se);
		*pse = parent_entity(*pse);
	}
}

#else	/* !CONFIG_FAIR_GROUP_SCHED */

static inline struct task_struct *task_of(struct sched_entity *se)
{
	return container_of(se, struct task_struct, se);
}

static inline struct rq *rq_of(struct cfs_rq *cfs_rq)
{
	return container_of(cfs_rq, struct rq, cfs);
}

#define entity_is_task(se)	1

#define for_each_sched_entity(se) \
		for (; se; se = NULL)

static inline struct cfs_rq *task_cfs_rq(struct task_struct *p)
{
	return &task_rq(p)->cfs;
}

static inline struct cfs_rq *cfs_rq_of(struct sched_entity *se)
{
	struct task_struct *p = task_of(se);
	struct rq *rq = task_rq(p);

	return &rq->cfs;
}

/* runqueue "owned" by this group */
static inline struct cfs_rq *group_cfs_rq(struct sched_entity *grp)
{
	return NULL;
}

static inline void list_add_leaf_cfs_rq(struct cfs_rq *cfs_rq)
{
}

static inline void list_del_leaf_cfs_rq(struct cfs_rq *cfs_rq)
{
}

#define for_each_leaf_cfs_rq(rq, cfs_rq) \
		for (cfs_rq = &rq->cfs; cfs_rq; cfs_rq = NULL)

static inline struct sched_entity *parent_entity(struct sched_entity *se)
{
	return NULL;
}

static inline void
find_matching_se(struct sched_entity **se, struct sched_entity **pse)
{
}

#endif	/* CONFIG_FAIR_GROUP_SCHED */

static __always_inline
void account_cfs_rq_runtime(struct cfs_rq *cfs_rq, u64 delta_exec);

/**************************************************************
 * Scheduling class tree data structure manipulation methods:
 */

static inline u64 max_vruntime(u64 max_vruntime, u64 vruntime)
{
	s64 delta = (s64)(vruntime - max_vruntime);
	if (delta > 0)
		max_vruntime = vruntime;

	return max_vruntime;
}

static inline u64 min_vruntime(u64 min_vruntime, u64 vruntime)
{
	s64 delta = (s64)(vruntime - min_vruntime);
	if (delta < 0)
		min_vruntime = vruntime;

	return min_vruntime;
}

static inline int entity_before(struct sched_entity *a,
				struct sched_entity *b)
{
	return (s64)(a->vruntime - b->vruntime) < 0;
}

static void update_min_vruntime(struct cfs_rq *cfs_rq)
{
	u64 vruntime = cfs_rq->min_vruntime;

	if (cfs_rq->curr)
		vruntime = cfs_rq->curr->vruntime;

	if (cfs_rq->rb_leftmost) {
		struct sched_entity *se = rb_entry(cfs_rq->rb_leftmost,
						   struct sched_entity,
						   run_node);

		if (!cfs_rq->curr)
			vruntime = se->vruntime;
		else
			vruntime = min_vruntime(vruntime, se->vruntime);
	}

	/* ensure we never gain time by being placed backwards. */
	cfs_rq->min_vruntime = max_vruntime(cfs_rq->min_vruntime, vruntime);
#ifndef CONFIG_64BIT
	smp_wmb();
	cfs_rq->min_vruntime_copy = cfs_rq->min_vruntime;
#endif
}

/*
 * Enqueue an entity into the rb-tree:
 */
static void __enqueue_entity(struct cfs_rq *cfs_rq, struct sched_entity *se)
{
	struct rb_node **link = &cfs_rq->tasks_timeline.rb_node;
	struct rb_node *parent = NULL;
	struct sched_entity *entry;
	int leftmost = 1;

	/*
	 * Find the right place in the rbtree:
	 */
	while (*link) {
		parent = *link;
		entry = rb_entry(parent, struct sched_entity, run_node);
		/*
		 * We dont care about collisions. Nodes with
		 * the same key stay together.
		 */
		if (entity_before(se, entry)) {
			link = &parent->rb_left;
		} else {
			link = &parent->rb_right;
			leftmost = 0;
		}
	}

	/*
	 * Maintain a cache of leftmost tree entries (it is frequently
	 * used):
	 */
	if (leftmost)
		cfs_rq->rb_leftmost = &se->run_node;

	rb_link_node(&se->run_node, parent, link);
	rb_insert_color(&se->run_node, &cfs_rq->tasks_timeline);
}

static void __dequeue_entity(struct cfs_rq *cfs_rq, struct sched_entity *se)
{
	if (cfs_rq->rb_leftmost == &se->run_node) {
		struct rb_node *next_node;

		next_node = rb_next(&se->run_node);
		cfs_rq->rb_leftmost = next_node;
	}

	rb_erase(&se->run_node, &cfs_rq->tasks_timeline);
}

struct sched_entity *__pick_first_entity(struct cfs_rq *cfs_rq)
{
	struct rb_node *left = cfs_rq->rb_leftmost;

	if (!left)
		return NULL;

	return rb_entry(left, struct sched_entity, run_node);
}

static struct sched_entity *__pick_next_entity(struct sched_entity *se)
{
	struct rb_node *next = rb_next(&se->run_node);

	if (!next)
		return NULL;

	return rb_entry(next, struct sched_entity, run_node);
}

#ifdef CONFIG_SCHED_DEBUG
struct sched_entity *__pick_last_entity(struct cfs_rq *cfs_rq)
{
	struct rb_node *last = rb_last(&cfs_rq->tasks_timeline);

	if (!last)
		return NULL;

	return rb_entry(last, struct sched_entity, run_node);
}

/**************************************************************
 * Scheduling class statistics methods:
 */

int sched_proc_update_handler(struct ctl_table *table, int write,
		void __user *buffer, size_t *lenp,
		loff_t *ppos)
{
	int ret = proc_dointvec_minmax(table, write, buffer, lenp, ppos);
	int factor = get_update_sysctl_factor();

	if (ret || !write)
		return ret;

	sched_nr_latency = DIV_ROUND_UP(sysctl_sched_latency,
					sysctl_sched_min_granularity);

#define WRT_SYSCTL(name) \
	(normalized_sysctl_##name = sysctl_##name / (factor))
	WRT_SYSCTL(sched_min_granularity);
	WRT_SYSCTL(sched_latency);
	WRT_SYSCTL(sched_wakeup_granularity);
#undef WRT_SYSCTL

	return 0;
}
#endif

/*
 * delta /= w
 */
static inline u64 calc_delta_fair(u64 delta, struct sched_entity *se)
{
	if (unlikely(se->load.weight != NICE_0_LOAD))
		delta = __calc_delta(delta, NICE_0_LOAD, &se->load);

	return delta;
}

/*
 * The idea is to set a period in which each task runs once.
 *
 * When there are too many tasks (sched_nr_latency) we have to stretch
 * this period because otherwise the slices get too small.
 *
 * p = (nr <= nl) ? l : l*nr/nl
 */
static u64 __sched_period(unsigned long nr_running)
{
	if (unlikely(nr_running > sched_nr_latency))
		return nr_running * sysctl_sched_min_granularity;
	else
		return sysctl_sched_latency;
}

/*
 * We calculate the wall-time slice from the period by taking a part
 * proportional to the weight.
 *
 * s = p*P[w/rw]
 */
static u64 sched_slice(struct cfs_rq *cfs_rq, struct sched_entity *se)
{
	u64 slice = __sched_period(cfs_rq->nr_running + !se->on_rq);

	for_each_sched_entity(se) {
		struct load_weight *load;
		struct load_weight lw;

		cfs_rq = cfs_rq_of(se);
		load = &cfs_rq->load;

		if (unlikely(!se->on_rq)) {
			lw = cfs_rq->load;

			update_load_add(&lw, se->load.weight);
			load = &lw;
		}
		slice = __calc_delta(slice, se->load.weight, load);
	}
	return slice;
}

/*
 * We calculate the vruntime slice of a to-be-inserted task.
 *
 * vs = s/w
 */
static u64 sched_vslice(struct cfs_rq *cfs_rq, struct sched_entity *se)
{
	return calc_delta_fair(sched_slice(cfs_rq, se), se);
}

#ifdef CONFIG_SMP
static int select_idle_sibling(struct task_struct *p, int cpu);
static unsigned long task_h_load(struct task_struct *p);

/*
 * We choose a half-life close to 1 scheduling period.
 * Note: The tables below are dependent on this value.
 */
#define LOAD_AVG_PERIOD 32
#define LOAD_AVG_MAX 47742 /* maximum possible load avg */
#define LOAD_AVG_MAX_N 345 /* number of full periods to produce LOAD_MAX_AVG */

/* Give new sched_entity start runnable values to heavy its load in infant time */
void init_entity_runnable_average(struct sched_entity *se)
{
	struct sched_avg *sa = &se->avg;

	sa->last_update_time = 0;
	/*
	 * sched_avg's period_contrib should be strictly less then 1024, so
	 * we give it 1023 to make sure it is almost a period (1024us), and
	 * will definitely be update (after enqueue).
	 */
	sa->period_contrib = 1023;
	sa->load_avg = scale_load_down(se->load.weight);
	sa->load_sum = sa->load_avg * LOAD_AVG_MAX;
	sa->util_avg = scale_load_down(SCHED_LOAD_SCALE);
	sa->util_sum = sa->util_avg * LOAD_AVG_MAX;
	/* when this task enqueue'ed, it will contribute to its cfs_rq's load_avg */
}

#else
void init_entity_runnable_average(struct sched_entity *se)
{
}
#endif

/*
 * Update the current task's runtime statistics.
 */
static void update_curr(struct cfs_rq *cfs_rq)
{
	struct sched_entity *curr = cfs_rq->curr;
	u64 now = rq_clock_task(rq_of(cfs_rq));
	u64 delta_exec;

	if (unlikely(!curr))
		return;

	delta_exec = now - curr->exec_start;
	if (unlikely((s64)delta_exec <= 0))
		return;

	curr->exec_start = now;

	schedstat_set(curr->statistics.exec_max,
		      max(delta_exec, curr->statistics.exec_max));

	curr->sum_exec_runtime += delta_exec;
	schedstat_add(cfs_rq, exec_clock, delta_exec);

	curr->vruntime += calc_delta_fair(delta_exec, curr);
	update_min_vruntime(cfs_rq);

	if (entity_is_task(curr)) {
		struct task_struct *curtask = task_of(curr);

		trace_sched_stat_runtime(curtask, delta_exec, curr->vruntime);
		cpuacct_charge(curtask, delta_exec);
		account_group_exec_runtime(curtask, delta_exec);
	}

	account_cfs_rq_runtime(cfs_rq, delta_exec);
}

static void update_curr_fair(struct rq *rq)
{
	update_curr(cfs_rq_of(&rq->curr->se));
}

static inline void
update_stats_wait_start(struct cfs_rq *cfs_rq, struct sched_entity *se,
			bool migrating)
{
	schedstat_set(se->statistics.wait_start,
		migrating &&
		likely(rq_clock(rq_of(cfs_rq)) > se->statistics.wait_start) ?
		rq_clock(rq_of(cfs_rq)) - se->statistics.wait_start :
		rq_clock(rq_of(cfs_rq)));
}

/*
 * Task is being enqueued - update stats:
 */
static void update_stats_enqueue(struct cfs_rq *cfs_rq, struct sched_entity *se,
				 bool migrating)
{
	/*
	 * Are we enqueueing a waiting task? (for current tasks
	 * a dequeue/enqueue event is a NOP)
	 */
	if (se != cfs_rq->curr)
		update_stats_wait_start(cfs_rq, se, migrating);
}

#ifdef CONFIG_SCHEDSTATS
int sched_max_latency_sysctl(struct ctl_table *table, int write,
			     void __user *buffer, size_t *lenp, loff_t *ppos)
{
	int ret = 0;
	int i, cpu = nr_cpu_ids;
	char msg[256];
	unsigned long flags;
	struct rq *rq;
	struct sched_max_latency max, *lat;

	if (!write) {
		max.latency_us = 0;
		for_each_possible_cpu(i) {
			rq = cpu_rq(i);
			raw_spin_lock_irqsave(&rq->lock, flags);

			lat = &per_cpu(sched_max_latency, i);
			if (max.latency_us < lat->latency_us) {
				max = *lat;
				cpu = i;
			}

			raw_spin_unlock_irqrestore(&rq->lock, flags);
		}

		if (cpu != nr_cpu_ids) {
			table->maxlen =
			    snprintf(msg, sizeof(msg),
				     "cpu%d comm=%s pid=%u latency=%u(us)",
				     cpu, max.comm, max.pid, max.latency_us);
			table->data = msg;
			ret = proc_dostring(table, write, buffer, lenp, ppos);
		}
	} else {
		for_each_possible_cpu(i) {
			rq = cpu_rq(i);
			raw_spin_lock_irqsave(&rq->lock, flags);

			memset(&per_cpu(sched_max_latency, i), 0,
			       sizeof(struct sched_max_latency));

			raw_spin_unlock_irqrestore(&rq->lock, flags);
		}
	}

	return ret;
}

static inline void check_for_high_latency(struct task_struct *p, u64 latency_us)
{
	int do_warn, do_panic;
	const char *fmt = "excessive latency comm=%s pid=%d latency=%llu(us)\n";
	static DEFINE_RATELIMIT_STATE(rs, DEFAULT_RATELIMIT_INTERVAL,
				      DEFAULT_RATELIMIT_BURST);

	do_warn = (sysctl_sched_latency_warn_threshold &&
		   latency_us > sysctl_sched_latency_warn_threshold);
	do_panic = (sysctl_sched_latency_panic_threshold &&
		    latency_us > sysctl_sched_latency_panic_threshold);
	if (unlikely(do_panic || (do_warn && __ratelimit(&rs)))) {
		if (do_panic)
			panic(fmt, p->comm, p->pid, latency_us);
		else
			printk_deferred(fmt, p->comm, p->pid, latency_us);
	}
}
#else
static inline void check_for_high_latency(struct task_struct *p, u64 latency)
{
}
#endif

static void
update_stats_wait_end(struct cfs_rq *cfs_rq, struct sched_entity *se,
		      bool migrating)
{
	if (migrating) {
		schedstat_set(se->statistics.wait_start,
			      rq_clock(rq_of(cfs_rq)) -
			      se->statistics.wait_start);
		return;
	}

	schedstat_set(se->statistics.wait_max, max(se->statistics.wait_max,
			rq_clock(rq_of(cfs_rq)) - se->statistics.wait_start));
	schedstat_set(se->statistics.wait_count, se->statistics.wait_count + 1);
	schedstat_set(se->statistics.wait_sum, se->statistics.wait_sum +
			rq_clock(rq_of(cfs_rq)) - se->statistics.wait_start);
#ifdef CONFIG_SCHEDSTATS
	if (entity_is_task(se)) {
		u64 delta;
		struct sched_max_latency *max;

		delta = rq_clock(rq_of(cfs_rq)) - se->statistics.wait_start;
		trace_sched_stat_wait(task_of(se), delta);

		delta = delta >> 10;
		max = this_cpu_ptr(&sched_max_latency);
		if (max->latency_us < delta) {
			max->latency_us = delta;
			max->pid = task_of(se)->pid;
			memcpy(max->comm, task_of(se)->comm, TASK_COMM_LEN);
		}

		check_for_high_latency(task_of(se), delta);
	}
#endif
	schedstat_set(se->statistics.wait_start, 0);
}

static inline void
update_stats_dequeue(struct cfs_rq *cfs_rq, struct sched_entity *se,
		     bool migrating)
{
	/*
	 * Mark the end of the wait period if dequeueing a
	 * waiting task:
	 */
	if (se != cfs_rq->curr)
		update_stats_wait_end(cfs_rq, se, migrating);
}

/*
 * We are picking a new current task - update its stats:
 */
static inline void
update_stats_curr_start(struct cfs_rq *cfs_rq, struct sched_entity *se)
{
	/*
	 * We are starting a new run period:
	 */
	se->exec_start = rq_clock_task(rq_of(cfs_rq));
}

/**************************************************
 * Scheduling class queueing methods:
 */

#ifdef CONFIG_NUMA_BALANCING
/*
 * Approximate time to scan a full NUMA task in ms. The task scan period is
 * calculated based on the tasks virtual memory size and
 * numa_balancing_scan_size.
 */
unsigned int sysctl_numa_balancing_scan_period_min = 1000;
unsigned int sysctl_numa_balancing_scan_period_max = 60000;

/* Portion of address space to scan in MB */
unsigned int sysctl_numa_balancing_scan_size = 256;

/* Scan @scan_size MB every @scan_period after an initial @scan_delay in ms */
unsigned int sysctl_numa_balancing_scan_delay = 1000;

static unsigned int task_nr_scan_windows(struct task_struct *p)
{
	unsigned long rss = 0;
	unsigned long nr_scan_pages;

	/*
	 * Calculations based on RSS as non-present and empty pages are skipped
	 * by the PTE scanner and NUMA hinting faults should be trapped based
	 * on resident pages
	 */
	nr_scan_pages = sysctl_numa_balancing_scan_size << (20 - PAGE_SHIFT);
	rss = get_mm_rss(p->mm);
	if (!rss)
		rss = nr_scan_pages;

	rss = round_up(rss, nr_scan_pages);
	return rss / nr_scan_pages;
}

/* For sanitys sake, never scan more PTEs than MAX_SCAN_WINDOW MB/sec. */
#define MAX_SCAN_WINDOW 2560

static unsigned int task_scan_min(struct task_struct *p)
{
	unsigned int scan_size = READ_ONCE(sysctl_numa_balancing_scan_size);
	unsigned int scan, floor;
	unsigned int windows = 1;

	if (scan_size < MAX_SCAN_WINDOW)
		windows = MAX_SCAN_WINDOW / scan_size;
	floor = 1000 / windows;

	scan = sysctl_numa_balancing_scan_period_min / task_nr_scan_windows(p);
	return max_t(unsigned int, floor, scan);
}

static unsigned int task_scan_max(struct task_struct *p)
{
	unsigned int smin = task_scan_min(p);
	unsigned int smax;

	/* Watch for min being lower than max due to floor calculations */
	smax = sysctl_numa_balancing_scan_period_max / task_nr_scan_windows(p);
	return max(smin, smax);
}

static void account_numa_enqueue(struct rq *rq, struct task_struct *p)
{
	rq->nr_numa_running += (p->numa_preferred_nid != -1);
	rq->nr_preferred_running += (p->numa_preferred_nid == task_node(p));
}

static void account_numa_dequeue(struct rq *rq, struct task_struct *p)
{
	rq->nr_numa_running -= (p->numa_preferred_nid != -1);
	rq->nr_preferred_running -= (p->numa_preferred_nid == task_node(p));
}

struct numa_group {
	atomic_t refcount;

	spinlock_t lock; /* nr_tasks, tasks */
	int nr_tasks;
	pid_t gid;
	struct list_head task_list;

	struct rcu_head rcu;
	nodemask_t active_nodes;
	unsigned long total_faults;
	/*
	 * Faults_cpu is used to decide whether memory should move
	 * towards the CPU. As a consequence, these stats are weighted
	 * more by CPU use than by memory faults.
	 */
	unsigned long *faults_cpu;
	unsigned long faults[0];
};

/* Shared or private faults. */
#define NR_NUMA_HINT_FAULT_TYPES 2

/* Memory and CPU locality */
#define NR_NUMA_HINT_FAULT_STATS (NR_NUMA_HINT_FAULT_TYPES * 2)

/* Averaged statistics, and temporary buffers. */
#define NR_NUMA_HINT_FAULT_BUCKETS (NR_NUMA_HINT_FAULT_STATS * 2)

pid_t task_numa_group_id(struct task_struct *p)
{
	return p->numa_group ? p->numa_group->gid : 0;
}

static inline int task_faults_idx(int nid, int priv)
{
	return NR_NUMA_HINT_FAULT_TYPES * nid + priv;
}

static inline unsigned long task_faults(struct task_struct *p, int nid)
{
	if (!p->numa_faults_memory)
		return 0;

	return p->numa_faults_memory[task_faults_idx(nid, 0)] +
		p->numa_faults_memory[task_faults_idx(nid, 1)];
}

static inline unsigned long group_faults(struct task_struct *p, int nid)
{
	if (!p->numa_group)
		return 0;

	return p->numa_group->faults[task_faults_idx(nid, 0)] +
		p->numa_group->faults[task_faults_idx(nid, 1)];
}

static inline unsigned long group_faults_cpu(struct numa_group *group, int nid)
{
	return group->faults_cpu[task_faults_idx(nid, 0)] +
		group->faults_cpu[task_faults_idx(nid, 1)];
}

/*
 * These return the fraction of accesses done by a particular task, or
 * task group, on a particular numa node.  The group weight is given a
 * larger multiplier, in order to group tasks together that are almost
 * evenly spread out between numa nodes.
 */
static inline unsigned long task_weight(struct task_struct *p, int nid)
{
	unsigned long total_faults;

	if (!p->numa_faults_memory)
		return 0;

	total_faults = p->total_numa_faults;

	if (!total_faults)
		return 0;

	return 1000 * task_faults(p, nid) / total_faults;
}

static inline unsigned long group_weight(struct task_struct *p, int nid)
{
	if (!p->numa_group || !p->numa_group->total_faults)
		return 0;

	return 1000 * group_faults(p, nid) / p->numa_group->total_faults;
}

bool should_numa_migrate_memory(struct task_struct *p, struct page * page,
				int src_nid, int dst_cpu)
{
	struct numa_group *ng = p->numa_group;
	int dst_nid = cpu_to_node(dst_cpu);
	int last_cpupid, this_cpupid;

	this_cpupid = cpu_pid_to_cpupid(dst_cpu, current->pid);

	/*
	 * Multi-stage node selection is used in conjunction with a periodic
	 * migration fault to build a temporal task<->page relation. By using
	 * a two-stage filter we remove short/unlikely relations.
	 *
	 * Using P(p) ~ n_p / n_t as per frequentist probability, we can equate
	 * a task's usage of a particular page (n_p) per total usage of this
	 * page (n_t) (in a given time-span) to a probability.
	 *
	 * Our periodic faults will sample this probability and getting the
	 * same result twice in a row, given these samples are fully
	 * independent, is then given by P(n)^2, provided our sample period
	 * is sufficiently short compared to the usage pattern.
	 *
	 * This quadric squishes small probabilities, making it less likely we
	 * act on an unlikely task<->page relation.
	 */
	last_cpupid = page_cpupid_xchg_last(page, this_cpupid);
	if (!cpupid_pid_unset(last_cpupid) &&
				cpupid_to_nid(last_cpupid) != dst_nid)
		return false;

	/* Always allow migrate on private faults */
	if (cpupid_match_pid(p, last_cpupid))
		return true;

	/* A shared fault, but p->numa_group has not been set up yet. */
	if (!ng)
		return true;

	/*
	 * Do not migrate if the destination is not a node that
	 * is actively used by this numa group.
	 */
	if (!node_isset(dst_nid, ng->active_nodes))
		return false;

	/*
	 * Source is a node that is not actively used by this
	 * numa group, while the destination is. Migrate.
	 */
	if (!node_isset(src_nid, ng->active_nodes))
		return true;

	/*
	 * Both source and destination are nodes in active
	 * use by this numa group. Maximize memory bandwidth
	 * by migrating from more heavily used groups, to less
	 * heavily used ones, spreading the load around.
	 * Use a 1/4 hysteresis to avoid spurious page movement.
	 */
	return group_faults(p, dst_nid) < (group_faults(p, src_nid) * 3 / 4);
}

static unsigned long weighted_cpuload(const int cpu);
static unsigned long source_load(int cpu, int type);
static unsigned long target_load(int cpu, int type);
static unsigned long capacity_of(int cpu);
static long effective_load(struct task_group *tg, int cpu, long wl, long wg);

/* Cached statistics for all CPUs within a node */
struct numa_stats {
	unsigned long nr_running;
	unsigned long load;

	/* Total compute capacity of CPUs on a node */
	unsigned long compute_capacity;

	/* Approximate capacity in terms of runnable tasks on a node */
	unsigned long task_capacity;
	int has_free_capacity;
};

/*
 * XXX borrowed from update_sg_lb_stats
 */
static void update_numa_stats(struct numa_stats *ns, int nid)
{
	int smt, cpu, cpus = 0;
	unsigned long capacity;

	memset(ns, 0, sizeof(*ns));
	for_each_cpu(cpu, cpumask_of_node(nid)) {
		struct rq *rq = cpu_rq(cpu);

		ns->nr_running += rq->nr_running;
		ns->load += weighted_cpuload(cpu);
		ns->compute_capacity += capacity_of(cpu);

		cpus++;
	}

	/*
	 * If we raced with hotplug and there are no CPUs left in our mask
	 * the @ns structure is NULL'ed and task_numa_compare() will
	 * not find this node attractive.
	 *
	 * We'll either bail at !has_free_capacity, or we'll detect a huge
	 * imbalance and bail there.
	 */
	if (!cpus)
		return;

	/* smt := ceil(cpus / capacity), assumes: 1 < smt_power < 2 */
	smt = DIV_ROUND_UP(SCHED_CAPACITY_SCALE * cpus, ns->compute_capacity);
	capacity = cpus / smt; /* cores */

	ns->task_capacity = min_t(unsigned, capacity,
		DIV_ROUND_CLOSEST(ns->compute_capacity, SCHED_CAPACITY_SCALE));
	ns->has_free_capacity = (ns->nr_running < ns->task_capacity);
}

struct task_numa_env {
	struct task_struct *p;

	int src_cpu, src_nid;
	int dst_cpu, dst_nid;

	struct numa_stats src_stats, dst_stats;

	int imbalance_pct;

	struct task_struct *best_task;
	long best_imp;
	int best_cpu;
};

static void task_numa_assign(struct task_numa_env *env,
			     struct task_struct *p, long imp)
{
	if (env->best_task)
		put_task_struct(env->best_task);

	env->best_task = p;
	env->best_imp = imp;
	env->best_cpu = env->dst_cpu;
}

static bool load_too_imbalanced(long src_load, long dst_load,
				struct task_numa_env *env)
{
	long imb, old_imb;
	long orig_src_load, orig_dst_load;
	long src_capacity, dst_capacity;

	/*
	 * The load is corrected for the CPU capacity available on each node.
	 *
	 * src_load        dst_load
	 * ------------ vs ---------
	 * src_capacity    dst_capacity
	 */
	src_capacity = env->src_stats.compute_capacity;
	dst_capacity = env->dst_stats.compute_capacity;

	/* We care about the slope of the imbalance, not the direction. */
	if (dst_load < src_load)
		swap(dst_load, src_load);

	/* Is the difference below the threshold? */
	imb = dst_load * src_capacity * 100 -
	      src_load * dst_capacity * env->imbalance_pct;
	if (imb <= 0)
		return false;

	/*
	 * The imbalance is above the allowed threshold.
	 * Compare it with the old imbalance.
	 */
	orig_src_load = env->src_stats.load;
	orig_dst_load = env->dst_stats.load;

	if (orig_dst_load < orig_src_load)
		swap(orig_dst_load, orig_src_load);

	old_imb = orig_dst_load * src_capacity * 100 -
		  orig_src_load * dst_capacity * env->imbalance_pct;

	/* Would this change make things worse? */
	return (imb > old_imb);
}

/*
 * This checks if the overall compute and NUMA accesses of the system would
 * be improved if the source tasks was migrated to the target dst_cpu taking
 * into account that it might be best if task running on the dst_cpu should
 * be exchanged with the source task
 */
static void task_numa_compare(struct task_numa_env *env,
			      long taskimp, long groupimp)
{
	struct rq *src_rq = cpu_rq(env->src_cpu);
	struct rq *dst_rq = cpu_rq(env->dst_cpu);
	struct task_struct *cur;
	long src_load, dst_load;
	long load;
	long imp = env->p->numa_group ? groupimp : taskimp;
	long moveimp = imp;
	bool assigned = false;

	rcu_read_lock();

	raw_spin_lock_irq(&dst_rq->lock);
	cur = dst_rq->curr;
	/*
	 * No need to move the exiting task or idle task.
	 */
	if ((cur->flags & PF_EXITING) || is_idle_task(cur))
		cur = NULL;
	else {
		/*
		 * The task_struct must be protected here to protect the
		 * p->numa_faults access in the task_weight since the
		 * numa_faults could already be freed in the following path:
		 * finish_task_switch()
		 *     --> put_task_struct()
		 *         --> __put_task_struct()
		 *             --> task_numa_free()
		 */
		get_task_struct(cur);
	}

	raw_spin_unlock_irq(&dst_rq->lock);

	/*
	 * Because we have preemption enabled we can get migrated around and
	 * end try selecting ourselves (current == env->p) as a swap candidate.
	 */
	if (cur == env->p)
		goto unlock;

	/*
	 * "imp" is the fault differential for the source task between the
	 * source and destination node. Calculate the total differential for
	 * the source task and potential destination task. The more negative
	 * the value is, the more rmeote accesses that would be expected to
	 * be incurred if the tasks were swapped.
	 */
	if (cur) {
		/* Skip this swap candidate if cannot move to the source cpu */
		if (!cpumask_test_cpu(env->src_cpu, tsk_cpus_allowed(cur)))
			goto unlock;

		/*
		 * If dst and source tasks are in the same NUMA group, or not
		 * in any group then look only at task weights.
		 */
		if (cur->numa_group == env->p->numa_group) {
			imp = taskimp + task_weight(cur, env->src_nid) -
			      task_weight(cur, env->dst_nid);
			/*
			 * Add some hysteresis to prevent swapping the
			 * tasks within a group over tiny differences.
			 */
			if (cur->numa_group)
				imp -= imp/16;
		} else {
			/*
			 * Compare the group weights. If a task is all by
			 * itself (not part of a group), use the task weight
			 * instead.
			 */
			if (cur->numa_group)
				imp += group_weight(cur, env->src_nid) -
				       group_weight(cur, env->dst_nid);
			else
				imp += task_weight(cur, env->src_nid) -
				       task_weight(cur, env->dst_nid);
		}
	}

	if (imp <= env->best_imp && moveimp <= env->best_imp)
		goto unlock;

	if (!cur) {
		/* Is there capacity at our destination? */
		if (env->src_stats.nr_running <= env->src_stats.task_capacity &&
		    !env->dst_stats.has_free_capacity)
			goto unlock;

		goto balance;
	}

	/* Balance doesn't matter much if we're running a task per cpu */
	if (imp > env->best_imp && src_rq->nr_running == 1 &&
			dst_rq->nr_running == 1)
		goto assign;

	/*
	 * In the overloaded case, try and keep the load balanced.
	 */
balance:
	load = task_h_load(env->p);
	dst_load = env->dst_stats.load + load;
	src_load = env->src_stats.load - load;

	if (moveimp > imp && moveimp > env->best_imp) {
		/*
		 * If the improvement from just moving env->p direction is
		 * better than swapping tasks around, check if a move is
		 * possible. Store a slightly smaller score than moveimp,
		 * so an actually idle CPU will win.
		 */
		if (!load_too_imbalanced(src_load, dst_load, env)) {
			imp = moveimp - 1;
			put_task_struct(cur);
			cur = NULL;
			goto assign;
		}
	}

	if (imp <= env->best_imp)
		goto unlock;

	if (cur) {
		load = task_h_load(cur);
		dst_load -= load;
		src_load += load;
	}

	if (load_too_imbalanced(src_load, dst_load, env))
		goto unlock;

	/*
	 * One idle CPU per node is evaluated for a task numa move.
	 * Call select_idle_sibling to maybe find a better one.
	 */
	if (!cur)
		env->dst_cpu = select_idle_sibling(env->p, env->dst_cpu);

assign:
	assigned = true;
	task_numa_assign(env, cur, imp);
unlock:
	rcu_read_unlock();
	/*
	 * The dst_rq->curr isn't assigned. The protection for task_struct is
	 * finished.
	 */
	if (cur && !assigned)
		put_task_struct(cur);
}

static void task_numa_find_cpu(struct task_numa_env *env,
				long taskimp, long groupimp)
{
	int cpu;

	for_each_cpu(cpu, cpumask_of_node(env->dst_nid)) {
		/* Skip this CPU if the source task cannot migrate */
		if (!cpumask_test_cpu(cpu, tsk_cpus_allowed(env->p)))
			continue;

		env->dst_cpu = cpu;
		task_numa_compare(env, taskimp, groupimp);
	}
}

static int task_numa_migrate(struct task_struct *p)
{
	struct task_numa_env env = {
		.p = p,

		.src_cpu = task_cpu(p),
		.src_nid = task_node(p),

		.imbalance_pct = 112,

		.best_task = NULL,
		.best_imp = 0,
		.best_cpu = -1
	};
	struct sched_domain *sd;
	unsigned long taskweight, groupweight;
	int nid, ret;
	long taskimp, groupimp;

	/*
	 * Pick the lowest SD_NUMA domain, as that would have the smallest
	 * imbalance and would be the first to start moving tasks about.
	 *
	 * And we want to avoid any moving of tasks about, as that would create
	 * random movement of tasks -- counter the numa conditions we're trying
	 * to satisfy here.
	 */
	rcu_read_lock();
	sd = rcu_dereference(per_cpu(sd_numa, env.src_cpu));
	if (sd)
		env.imbalance_pct = 100 + (sd->imbalance_pct - 100) / 2;
	rcu_read_unlock();

	/*
	 * Cpusets can break the scheduler domain tree into smaller
	 * balance domains, some of which do not cross NUMA boundaries.
	 * Tasks that are "trapped" in such domains cannot be migrated
	 * elsewhere, so there is no point in (re)trying.
	 */
	if (unlikely(!sd)) {
		p->numa_preferred_nid = task_node(p);
		return -EINVAL;
	}

	taskweight = task_weight(p, env.src_nid);
	groupweight = group_weight(p, env.src_nid);
	update_numa_stats(&env.src_stats, env.src_nid);
	env.dst_nid = p->numa_preferred_nid;
	taskimp = task_weight(p, env.dst_nid) - taskweight;
	groupimp = group_weight(p, env.dst_nid) - groupweight;
	update_numa_stats(&env.dst_stats, env.dst_nid);

	/* Try to find a spot on the preferred nid. */
	task_numa_find_cpu(&env, taskimp, groupimp);

	/* No space available on the preferred nid. Look elsewhere. */
	if (env.best_cpu == -1) {
		for_each_online_node(nid) {
			if (nid == env.src_nid || nid == p->numa_preferred_nid)
				continue;

			/* Only consider nodes where both task and groups benefit */
			taskimp = task_weight(p, nid) - taskweight;
			groupimp = group_weight(p, nid) - groupweight;
			if (taskimp < 0 && groupimp < 0)
				continue;

			env.dst_nid = nid;
			update_numa_stats(&env.dst_stats, env.dst_nid);
			task_numa_find_cpu(&env, taskimp, groupimp);
		}
	}

	/*
	 * If the task is part of a workload that spans multiple NUMA nodes,
	 * and is migrating into one of the workload's active nodes, remember
	 * this node as the task's preferred numa node, so the workload can
	 * settle down.
	 * A task that migrated to a second choice node will be better off
	 * trying for a better one later. Do not set the preferred node here.
	 */
	if (p->numa_group) {
		if (env.best_cpu == -1)
			nid = env.src_nid;
		else
			nid = env.dst_nid;

		if (node_isset(nid, p->numa_group->active_nodes))
			sched_setnuma(p, env.dst_nid);
	}

	/* No better CPU than the current one was found. */
	if (env.best_cpu == -1)
		return -EAGAIN;

	/*
	 * Reset the scan period if the task is being rescheduled on an
	 * alternative node to recheck if the tasks is now properly placed.
	 */
	p->numa_scan_period = task_scan_min(p);

	if (env.best_task == NULL) {
		ret = migrate_task_to(p, env.best_cpu);
		if (ret != 0)
			trace_sched_stick_numa(p, env.src_cpu, env.best_cpu);
		return ret;
	}

	ret = migrate_swap(p, env.best_task);
	if (ret != 0)
		trace_sched_stick_numa(p, env.src_cpu, task_cpu(env.best_task));
	put_task_struct(env.best_task);
	return ret;
}

/* Attempt to migrate a task to a CPU on the preferred node. */
static void numa_migrate_preferred(struct task_struct *p)
{
	unsigned long interval = HZ;

	/* This task has no NUMA fault statistics yet */
	if (unlikely(p->numa_preferred_nid == -1 || !p->numa_faults_memory))
		return;

	/* Periodically retry migrating the task to the preferred node */
	interval = min(interval, msecs_to_jiffies(p->numa_scan_period) / 16);
	p->numa_migrate_retry = jiffies + interval;

	/* Success if task is already running on preferred CPU */
	if (task_node(p) == p->numa_preferred_nid)
		return;

	/* Otherwise, try migrate to a CPU on the preferred node */
	task_numa_migrate(p);
}

/*
 * Find the nodes on which the workload is actively running. We do this by
 * tracking the nodes from which NUMA hinting faults are triggered. This can
 * be different from the set of nodes where the workload's memory is currently
 * located.
 *
 * The bitmask is used to make smarter decisions on when to do NUMA page
 * migrations, To prevent flip-flopping, and excessive page migrations, nodes
 * are added when they cause over 6/16 of the maximum number of faults, but
 * only removed when they drop below 3/16.
 */
static void update_numa_active_node_mask(struct numa_group *numa_group)
{
	unsigned long faults, max_faults = 0;
	int nid;

	for_each_online_node(nid) {
		faults = group_faults_cpu(numa_group, nid);
		if (faults > max_faults)
			max_faults = faults;
	}

	for_each_online_node(nid) {
		faults = group_faults_cpu(numa_group, nid);
		if (!node_isset(nid, numa_group->active_nodes)) {
			if (faults > max_faults * 6 / 16)
				node_set(nid, numa_group->active_nodes);
		} else if (faults < max_faults * 3 / 16)
			node_clear(nid, numa_group->active_nodes);
	}
}

/*
 * When adapting the scan rate, the period is divided into NUMA_PERIOD_SLOTS
 * increments. The more local the fault statistics are, the higher the scan
 * period will be for the next scan window. If local/(local+remote) ratio is
 * below NUMA_PERIOD_THRESHOLD (where range of ratio is 1..NUMA_PERIOD_SLOTS)
 * the scan period will decrease. Aim for 70% local accesses.
 */
#define NUMA_PERIOD_SLOTS 10
#define NUMA_PERIOD_THRESHOLD 7

/*
 * Increase the scan period (slow down scanning) if the majority of
 * our memory is already on our local node, or if the majority of
 * the page accesses are shared with other processes.
 * Otherwise, decrease the scan period.
 */
static void update_task_scan_period(struct task_struct *p,
			unsigned long shared, unsigned long private)
{
	unsigned int period_slot;
	int ratio;
	int diff;

	unsigned long remote = p->numa_faults_locality[0];
	unsigned long local = p->numa_faults_locality[1];

	/*
	 * If there were no record hinting faults then either the task is
	 * completely idle or all activity is areas that are not of interest
	 * to automatic numa balancing. Scan slower
	 */
	if (local + shared == 0) {
		p->numa_scan_period = min(p->numa_scan_period_max,
			p->numa_scan_period << 1);

		p->mm->numa_next_scan = jiffies +
			msecs_to_jiffies(p->numa_scan_period);

		return;
	}

	/*
	 * Prepare to scale scan period relative to the current period.
	 *	 == NUMA_PERIOD_THRESHOLD scan period stays the same
	 *       <  NUMA_PERIOD_THRESHOLD scan period decreases (scan faster)
	 *	 >= NUMA_PERIOD_THRESHOLD scan period increases (scan slower)
	 */
	period_slot = DIV_ROUND_UP(p->numa_scan_period, NUMA_PERIOD_SLOTS);
	ratio = (local * NUMA_PERIOD_SLOTS) / (local + remote);
	if (ratio >= NUMA_PERIOD_THRESHOLD) {
		int slot = ratio - NUMA_PERIOD_THRESHOLD;
		if (!slot)
			slot = 1;
		diff = slot * period_slot;
	} else {
		diff = -(NUMA_PERIOD_THRESHOLD - ratio) * period_slot;

		/*
		 * Scale scan rate increases based on sharing. There is an
		 * inverse relationship between the degree of sharing and
		 * the adjustment made to the scanning period. Broadly
		 * speaking the intent is that there is little point
		 * scanning faster if shared accesses dominate as it may
		 * simply bounce migrations uselessly
		 */
		ratio = DIV_ROUND_UP(private * NUMA_PERIOD_SLOTS, (private + shared + 1));
		diff = (diff * ratio) / NUMA_PERIOD_SLOTS;
	}

	p->numa_scan_period = clamp(p->numa_scan_period + diff,
			task_scan_min(p), task_scan_max(p));
	memset(p->numa_faults_locality, 0, sizeof(p->numa_faults_locality));
}

/*
 * Get the fraction of time the task has been running since the last
 * NUMA placement cycle. The scheduler keeps similar statistics, but
 * decays those on a 32ms period, which is orders of magnitude off
 * from the dozens-of-seconds NUMA balancing period. Use the scheduler
 * stats only if the task is so new there are no NUMA statistics yet.
 */
static u64 numa_get_avg_runtime(struct task_struct *p, u64 *period)
{
	u64 runtime, delta, now;
	/* Use the start of this time slice to avoid calculations. */
	now = p->se.exec_start;
	runtime = p->se.sum_exec_runtime;

	if (p->last_task_numa_placement) {
		delta = runtime - p->last_sum_exec_runtime;
		*period = now - p->last_task_numa_placement;

		/* Avoid time going backwards, prevent potential divide error: */
		if (unlikely((s64)*period < 0))
			*period = 0;
	} else {
		delta = p->se.avg.load_sum / p->se.load.weight;
		*period = LOAD_AVG_MAX;
	}

	p->last_sum_exec_runtime = runtime;
	p->last_task_numa_placement = now;

	return delta;
}

static void task_numa_placement(struct task_struct *p)
{
	int seq, nid, max_nid = -1, max_group_nid = -1;
	unsigned long max_faults = 0, max_group_faults = 0;
	unsigned long fault_types[2] = { 0, 0 };
	unsigned long total_faults;
	u64 runtime, period;
	spinlock_t *group_lock = NULL;

	seq = READ_ONCE(p->mm->numa_scan_seq);
	if (p->numa_scan_seq == seq)
		return;
	p->numa_scan_seq = seq;
	p->numa_scan_period_max = task_scan_max(p);

	total_faults = p->numa_faults_locality[0] +
		       p->numa_faults_locality[1];
	runtime = numa_get_avg_runtime(p, &period);

	/* If the task is part of a group prevent parallel updates to group stats */
	if (p->numa_group) {
		group_lock = &p->numa_group->lock;
		spin_lock_irq(group_lock);
	}

	/* Find the node with the highest number of faults */
	for_each_online_node(nid) {
		unsigned long faults = 0, group_faults = 0;
		int priv, i;

		for (priv = 0; priv < NR_NUMA_HINT_FAULT_TYPES; priv++) {
			long diff, f_diff, f_weight;

			i = task_faults_idx(nid, priv);

			/* Decay existing window, copy faults since last scan */
			diff = p->numa_faults_buffer_memory[i] - p->numa_faults_memory[i] / 2;
			fault_types[priv] += p->numa_faults_buffer_memory[i];
			p->numa_faults_buffer_memory[i] = 0;

			/*
			 * Normalize the faults_from, so all tasks in a group
			 * count according to CPU use, instead of by the raw
			 * number of faults. Tasks with little runtime have
			 * little over-all impact on throughput, and thus their
			 * faults are less important.
			 */
			f_weight = div64_u64(runtime << 16, period + 1);
			f_weight = (f_weight * p->numa_faults_buffer_cpu[i]) /
				   (total_faults + 1);
			f_diff = f_weight - p->numa_faults_cpu[i] / 2;
			p->numa_faults_buffer_cpu[i] = 0;

			p->numa_faults_memory[i] += diff;
			p->numa_faults_cpu[i] += f_diff;
			faults += p->numa_faults_memory[i];
			p->total_numa_faults += diff;
			if (p->numa_group) {
				/* safe because we can only change our own group */
				p->numa_group->faults[i] += diff;
				p->numa_group->faults_cpu[i] += f_diff;
				p->numa_group->total_faults += diff;
				group_faults += p->numa_group->faults[i];
			}
		}

		if (faults > max_faults) {
			max_faults = faults;
			max_nid = nid;
		}

		if (group_faults > max_group_faults) {
			max_group_faults = group_faults;
			max_group_nid = nid;
		}
	}

	update_task_scan_period(p, fault_types[0], fault_types[1]);

	if (p->numa_group) {
		update_numa_active_node_mask(p->numa_group);
		spin_unlock_irq(group_lock);
		max_nid = max_group_nid;
	}

	if (max_faults) {
		/* Set the new preferred node */
		if (max_nid != p->numa_preferred_nid)
			sched_setnuma(p, max_nid);

		if (task_node(p) != p->numa_preferred_nid)
			numa_migrate_preferred(p);
	}
}

static inline int get_numa_group(struct numa_group *grp)
{
	return atomic_inc_not_zero(&grp->refcount);
}

static inline void put_numa_group(struct numa_group *grp)
{
	if (atomic_dec_and_test(&grp->refcount))
		kfree_rcu(grp, rcu);
}

static void task_numa_group(struct task_struct *p, int cpupid, int flags,
			int *priv)
{
	struct numa_group *grp, *my_grp;
	struct task_struct *tsk;
	bool join = false;
	int cpu = cpupid_to_cpu(cpupid);
	int i;

	if (unlikely(!p->numa_group)) {
		unsigned int size = sizeof(struct numa_group) +
				    4*nr_node_ids*sizeof(unsigned long);

		grp = kzalloc(size, GFP_KERNEL | __GFP_NOWARN);
		if (!grp)
			return;

		atomic_set(&grp->refcount, 1);
		spin_lock_init(&grp->lock);
		INIT_LIST_HEAD(&grp->task_list);
		grp->gid = p->pid;
		/* Second half of the array tracks nids where faults happen */
		grp->faults_cpu = grp->faults + NR_NUMA_HINT_FAULT_TYPES *
						nr_node_ids;

		node_set(task_node(current), grp->active_nodes);

		for (i = 0; i < NR_NUMA_HINT_FAULT_STATS * nr_node_ids; i++)
			grp->faults[i] = p->numa_faults_memory[i];

		grp->total_faults = p->total_numa_faults;

		list_add(&p->numa_entry, &grp->task_list);
		grp->nr_tasks++;
		rcu_assign_pointer(p->numa_group, grp);
	}

	rcu_read_lock();
	tsk = READ_ONCE(cpu_rq(cpu)->curr);

	if (!cpupid_match_pid(tsk, cpupid))
		goto no_join;

	grp = rcu_dereference(tsk->numa_group);
	if (!grp)
		goto no_join;

	my_grp = p->numa_group;
	if (grp == my_grp)
		goto no_join;

	/*
	 * Only join the other group if its bigger; if we're the bigger group,
	 * the other task will join us.
	 */
	if (my_grp->nr_tasks > grp->nr_tasks)
		goto no_join;

	/*
	 * Tie-break on the grp address.
	 */
	if (my_grp->nr_tasks == grp->nr_tasks && my_grp > grp)
		goto no_join;

	/* Always join threads in the same process. */
	if (tsk->mm == current->mm)
		join = true;

	/* Simple filter to avoid false positives due to PID collisions */
	if (flags & TNF_SHARED)
		join = true;

	/* Update priv based on whether false sharing was detected */
	*priv = !join;

	if (join && !get_numa_group(grp))
		goto no_join;

	rcu_read_unlock();

	if (!join)
		return;

	BUG_ON(irqs_disabled());
	double_lock_irq(&my_grp->lock, &grp->lock);

	for (i = 0; i < NR_NUMA_HINT_FAULT_STATS * nr_node_ids; i++) {
		my_grp->faults[i] -= p->numa_faults_memory[i];
		grp->faults[i] += p->numa_faults_memory[i];
	}
	my_grp->total_faults -= p->total_numa_faults;
	grp->total_faults += p->total_numa_faults;

	list_move(&p->numa_entry, &grp->task_list);
	my_grp->nr_tasks--;
	grp->nr_tasks++;

	spin_unlock(&my_grp->lock);
	spin_unlock_irq(&grp->lock);

	rcu_assign_pointer(p->numa_group, grp);

	put_numa_group(my_grp);
	return;

no_join:
	rcu_read_unlock();
	return;
}

void task_numa_free(struct task_struct *p)
{
	struct numa_group *grp = p->numa_group;
	void *numa_faults = p->numa_faults_memory;
	unsigned long flags;
	int i;

	if (grp) {
		spin_lock_irqsave(&grp->lock, flags);
		for (i = 0; i < NR_NUMA_HINT_FAULT_STATS * nr_node_ids; i++)
			grp->faults[i] -= p->numa_faults_memory[i];
		grp->total_faults -= p->total_numa_faults;

		list_del(&p->numa_entry);
		grp->nr_tasks--;
		spin_unlock_irqrestore(&grp->lock, flags);
		RCU_INIT_POINTER(p->numa_group, NULL);
		put_numa_group(grp);
	}

	p->numa_faults_memory = NULL;
	p->numa_faults_buffer_memory = NULL;
	p->numa_faults_cpu= NULL;
	p->numa_faults_buffer_cpu = NULL;
	kfree(numa_faults);
}

/*
 * Got a PROT_NONE fault for a page on @node.
 */
void task_numa_fault(int last_cpupid, int mem_node, int pages, int flags)
{
	struct task_struct *p = current;
	bool migrated = flags & TNF_MIGRATED;
	int cpu_node = task_node(current);
	int local = !!(flags & TNF_FAULT_LOCAL);
	int priv;

	if (!numabalancing_enabled)
		return;

	/* for example, ksmd faulting in a user's mm */
	if (!p->mm)
		return;

	/* Allocate buffer to track faults on a per-node basis */
	if (unlikely(!p->numa_faults_memory)) {
		int size = sizeof(*p->numa_faults_memory) *
			   NR_NUMA_HINT_FAULT_BUCKETS * nr_node_ids;

		p->numa_faults_memory = kzalloc(size, GFP_KERNEL|__GFP_NOWARN);
		if (!p->numa_faults_memory)
			return;

		BUG_ON(p->numa_faults_buffer_memory);
		/*
		 * The averaged statistics, shared & private, memory & cpu,
		 * occupy the first half of the array. The second half of the
		 * array is for current counters, which are averaged into the
		 * first set by task_numa_placement.
		 */
		p->numa_faults_cpu = p->numa_faults_memory + (2 * nr_node_ids);
		p->numa_faults_buffer_memory = p->numa_faults_memory + (4 * nr_node_ids);
		p->numa_faults_buffer_cpu = p->numa_faults_memory + (6 * nr_node_ids);
		p->total_numa_faults = 0;
		memset(p->numa_faults_locality, 0, sizeof(p->numa_faults_locality));
	}

	/*
	 * First accesses are treated as private, otherwise consider accesses
	 * to be private if the accessing pid has not changed
	 */
	if (unlikely(last_cpupid == (-1 & LAST_CPUPID_MASK))) {
		priv = 1;
	} else {
		priv = cpupid_match_pid(p, last_cpupid);
		if (!priv && !(flags & TNF_NO_GROUP))
			task_numa_group(p, last_cpupid, flags, &priv);
	}

	/*
	 * If a workload spans multiple NUMA nodes, a shared fault that
	 * occurs wholly within the set of nodes that the workload is
	 * actively using should be counted as local. This allows the
	 * scan rate to slow down when a workload has settled down.
	 */
	if (!priv && !local && p->numa_group &&
			node_isset(cpu_node, p->numa_group->active_nodes) &&
			node_isset(mem_node, p->numa_group->active_nodes))
		local = 1;

	task_numa_placement(p);

	/*
	 * Retry task to preferred node migration periodically, in case it
	 * case it previously failed, or the scheduler moved us.
	 */
	if (time_after(jiffies, p->numa_migrate_retry))
		numa_migrate_preferred(p);

	if (migrated)
		p->numa_pages_migrated += pages;

	p->numa_faults_buffer_memory[task_faults_idx(mem_node, priv)] += pages;
	p->numa_faults_buffer_cpu[task_faults_idx(cpu_node, priv)] += pages;
	p->numa_faults_locality[local] += pages;
}

static void reset_ptenuma_scan(struct task_struct *p)
{
	WRITE_ONCE(p->mm->numa_scan_seq, READ_ONCE(p->mm->numa_scan_seq) + 1);
	p->mm->numa_scan_offset = 0;
}

/*
 * The expensive part of numa migration is done from task_work context.
 * Triggered from task_tick_numa().
 */
void task_numa_work(struct callback_head *work)
{
	unsigned long migrate, next_scan, now = jiffies;
	struct task_struct *p = current;
	struct mm_struct *mm = p->mm;
	struct vm_area_struct *vma;
	unsigned long start, end;
	unsigned long nr_pte_updates = 0;
	long pages;

	WARN_ON_ONCE(p != container_of(work, struct task_struct, numa_work));

	work->next = work; /* protect against double add */
	/*
	 * Who cares about NUMA placement when they're dying.
	 *
	 * NOTE: make sure not to dereference p->mm before this check,
	 * exit_task_work() happens _after_ exit_mm() so we could be called
	 * without p->mm even though we still had it when we enqueued this
	 * work.
	 */
	if (p->flags & PF_EXITING)
		return;

	if (!mm->numa_next_scan) {
		mm->numa_next_scan = now +
			msecs_to_jiffies(sysctl_numa_balancing_scan_delay);
	}

	/*
	 * Enforce maximal scan/migration frequency..
	 */
	migrate = mm->numa_next_scan;
	if (time_before(now, migrate))
		return;

	if (p->numa_scan_period == 0) {
		p->numa_scan_period_max = task_scan_max(p);
		p->numa_scan_period = task_scan_min(p);
	}

	next_scan = now + msecs_to_jiffies(p->numa_scan_period);
	if (cmpxchg(&mm->numa_next_scan, migrate, next_scan) != migrate)
		return;

	/*
	 * Delay this task enough that another task of this mm will likely win
	 * the next time around.
	 */
	p->node_stamp += 2 * TICK_NSEC;

	start = mm->numa_scan_offset;
	pages = sysctl_numa_balancing_scan_size;
	pages <<= 20 - PAGE_SHIFT; /* MB in pages */
	if (!pages)
		return;

	if (!down_read_trylock(&mm->mmap_sem))
		return;
	vma = find_vma(mm, start);
	if (!vma) {
		reset_ptenuma_scan(p);
		start = 0;
		vma = mm->mmap;
	}
	for (; vma; vma = vma->vm_next) {
		if (!vma_migratable(vma) || !vma_policy_mof(vma) ||
		    is_vm_hugetlb_page(vma) || (vma->vm_flags & VM_MIXEDMAP)) {
			continue;
		}

		/*
		 * Shared library pages mapped by multiple processes are not
		 * migrated as it is expected they are cache replicated. Avoid
		 * hinting faults in read-only file-backed mappings or the vdso
		 * as migrating the pages will be of marginal benefit.
		 */
		if (!vma->vm_mm ||
		    (vma->vm_file && (vma->vm_flags & (VM_READ|VM_WRITE)) == (VM_READ)))
			continue;

		/*
		 * Skip inaccessible VMAs to avoid any confusion between
		 * PROT_NONE and NUMA hinting ptes
		 */
		if (!(vma->vm_flags & (VM_READ | VM_EXEC | VM_WRITE)))
			continue;

		do {
			start = max(start, vma->vm_start);
			end = ALIGN(start + (pages << PAGE_SHIFT), HPAGE_SIZE);
			end = min(end, vma->vm_end);
			nr_pte_updates += change_prot_numa(vma, start, end);

			/*
			 * Scan sysctl_numa_balancing_scan_size but ensure that
			 * at least one PTE is updated so that unused virtual
			 * address space is quickly skipped.
			 */
			if (nr_pte_updates)
				pages -= (end - start) >> PAGE_SHIFT;

			start = end;
			if (pages <= 0)
				goto out;

			cond_resched();
		} while (end != vma->vm_end);
	}

out:
	/*
	 * It is possible to reach the end of the VMA list but the last few
	 * VMAs are not guaranteed to the vma_migratable. If they are not, we
	 * would find the !migratable VMA on the next scan but not reset the
	 * scanner to the start so check it now.
	 */
	if (vma)
		mm->numa_scan_offset = start;
	else
		reset_ptenuma_scan(p);
	up_read(&mm->mmap_sem);
}

/*
 * Drive the periodic memory faults..
 */
void task_tick_numa(struct rq *rq, struct task_struct *curr)
{
	struct callback_head *work = &curr->numa_work;
	u64 period, now;

	/*
	 * We don't care about NUMA placement if we don't have memory.
	 */
	if ((curr->flags & (PF_EXITING | PF_KTHREAD)) || work->next != work)
		return;

	/*
	 * Using runtime rather than walltime has the dual advantage that
	 * we (mostly) drive the selection from busy threads and that the
	 * task needs to have done some actual work before we bother with
	 * NUMA placement.
	 */
	now = curr->se.sum_exec_runtime;
	period = (u64)curr->numa_scan_period * NSEC_PER_MSEC;

	if (now - curr->node_stamp > period) {
		if (!curr->node_stamp)
			curr->numa_scan_period = task_scan_min(curr);
		curr->node_stamp += period;

		if (!time_before(jiffies, curr->mm->numa_next_scan)) {
			init_task_work(work, task_numa_work); /* TODO: move this into sched_fork() */
			task_work_add(curr, work, true);
		}
	}
}
#else
static void task_tick_numa(struct rq *rq, struct task_struct *curr)
{
}

static inline void account_numa_enqueue(struct rq *rq, struct task_struct *p)
{
}

static inline void account_numa_dequeue(struct rq *rq, struct task_struct *p)
{
}
#endif /* CONFIG_NUMA_BALANCING */

static void
account_entity_enqueue(struct cfs_rq *cfs_rq, struct sched_entity *se)
{
	update_load_add(&cfs_rq->load, se->load.weight);
	if (!parent_entity(se))
		update_load_add(&rq_of(cfs_rq)->load, se->load.weight);
#ifdef CONFIG_SMP
	if (entity_is_task(se)) {
		struct rq *rq = rq_of(cfs_rq);

		account_numa_enqueue(rq, task_of(se));
		list_add(&se->group_node, &rq->cfs_tasks);
	}
#endif
	cfs_rq->nr_running++;
}

static void
account_entity_dequeue(struct cfs_rq *cfs_rq, struct sched_entity *se)
{
	update_load_sub(&cfs_rq->load, se->load.weight);
	if (!parent_entity(se))
		update_load_sub(&rq_of(cfs_rq)->load, se->load.weight);
	if (entity_is_task(se)) {
		account_numa_dequeue(rq_of(cfs_rq), task_of(se));
		list_del_init(&se->group_node);
	}
	cfs_rq->nr_running--;
}

#ifdef CONFIG_FAIR_GROUP_SCHED
# ifdef CONFIG_SMP
static long calc_cfs_shares(struct cfs_rq *cfs_rq, struct task_group *tg)
{
	long tg_weight, load, shares;

	/*
	 * This really should be: cfs_rq->avg.load_avg, but instead we use
	 * cfs_rq->load.weight, which is its upper bound. This helps ramp up
	 * the shares for small weight interactive tasks.
	 */
	load = scale_load_down(cfs_rq->load.weight);

	tg_weight = atomic_long_read(&tg->load_avg);

	/* Ensure tg_weight >= load */
	tg_weight -= cfs_rq->tg_load_avg_contrib;
	tg_weight += load;

	shares = (tg->shares * load);
	if (tg_weight)
		shares /= tg_weight;

	if (shares < MIN_SHARES)
		shares = MIN_SHARES;
	if (shares > tg->shares)
		shares = tg->shares;

	return shares;
}
# else /* CONFIG_SMP */
static inline long calc_cfs_shares(struct cfs_rq *cfs_rq, struct task_group *tg)
{
	return tg->shares;
}
# endif /* CONFIG_SMP */

static void reweight_entity(struct cfs_rq *cfs_rq, struct sched_entity *se,
			    unsigned long weight)
{
	if (se->on_rq) {
		/* commit outstanding execution time */
		if (cfs_rq->curr == se)
			update_curr(cfs_rq);
		account_entity_dequeue(cfs_rq, se);
	}

	update_load_set(&se->load, weight);

	if (se->on_rq)
		account_entity_enqueue(cfs_rq, se);
}

static inline int throttled_hierarchy(struct cfs_rq *cfs_rq);

static void update_cfs_shares(struct cfs_rq *cfs_rq)
{
	struct task_group *tg;
	struct sched_entity *se;
	long shares;

	tg = cfs_rq->tg;
	se = tg->se[cpu_of(rq_of(cfs_rq))];
	if (!se || throttled_hierarchy(cfs_rq))
		return;
#ifndef CONFIG_SMP
	if (likely(se->load.weight == tg->shares))
		return;
#endif
	shares = calc_cfs_shares(cfs_rq, tg);

	reweight_entity(cfs_rq_of(se), se, shares);
}
#else /* CONFIG_FAIR_GROUP_SCHED */
static inline void update_cfs_shares(struct cfs_rq *cfs_rq)
{
}
#endif /* CONFIG_FAIR_GROUP_SCHED */

#ifdef CONFIG_SMP
u32 sched_get_wake_up_idle(struct task_struct *p)
{
	u32 enabled = p->flags & PF_WAKE_UP_IDLE;

	return !!enabled;
}

int sched_set_wake_up_idle(struct task_struct *p, int wake_up_idle)
{
	int enable = !!wake_up_idle;

	if (enable)
		p->flags |= PF_WAKE_UP_IDLE;
	else
		p->flags &= ~PF_WAKE_UP_IDLE;

	return 0;
}

/* Precomputed fixed inverse multiplies for multiplication by y^n */
static const u32 runnable_avg_yN_inv[] = {
	0xffffffff, 0xfa83b2da, 0xf5257d14, 0xefe4b99a, 0xeac0c6e6, 0xe5b906e6,
	0xe0ccdeeb, 0xdbfbb796, 0xd744fcc9, 0xd2a81d91, 0xce248c14, 0xc9b9bd85,
	0xc5672a10, 0xc12c4cc9, 0xbd08a39e, 0xb8fbaf46, 0xb504f333, 0xb123f581,
	0xad583ee9, 0xa9a15ab4, 0xa5fed6a9, 0xa2704302, 0x9ef5325f, 0x9b8d39b9,
	0x9837f050, 0x94f4efa8, 0x91c3d373, 0x8ea4398a, 0x8b95c1e3, 0x88980e80,
	0x85aac367, 0x82cd8698,
};

/*
 * Precomputed \Sum y^k { 1<=k<=n }.  These are floor(true_value) to prevent
 * over-estimates when re-combining.
 */
static const u32 runnable_avg_yN_sum[] = {
	    0, 1002, 1982, 2941, 3880, 4798, 5697, 6576, 7437, 8279, 9103,
	 9909,10698,11470,12226,12966,13690,14398,15091,15769,16433,17082,
	17718,18340,18949,19545,20128,20698,21256,21802,22336,22859,23371,
};

/*
 * Approximate:
 *   val * y^n,    where y^32 ~= 0.5 (~1 scheduling period)
 */
static __always_inline u64 decay_load(u64 val, u64 n)
{
	unsigned int local_n;

	if (!n)
		return val;
	else if (unlikely(n > LOAD_AVG_PERIOD * 63))
		return 0;

	/* after bounds checking we can collapse to 32-bit */
	local_n = n;

	/*
	 * As y^PERIOD = 1/2, we can combine
	 *    y^n = 1/2^(n/PERIOD) * y^(n%PERIOD)
	 * With a look-up table which covers y^n (n<PERIOD)
	 *
	 * To achieve constant time decay_load.
	 */
	if (unlikely(local_n >= LOAD_AVG_PERIOD)) {
		val >>= local_n / LOAD_AVG_PERIOD;
		local_n %= LOAD_AVG_PERIOD;
	}

	val = mul_u64_u32_shr(val, runnable_avg_yN_inv[local_n], 32);
	return val;
}

/*
 * For updates fully spanning n periods, the contribution to runnable
 * average will be: \Sum 1024*y^n
 *
 * We can compute this reasonably efficiently by combining:
 *   y^PERIOD = 1/2 with precomputed \Sum 1024*y^n {for  n <PERIOD}
 */
static u32 __compute_runnable_contrib(u64 n)
{
	u32 contrib = 0;

	if (likely(n <= LOAD_AVG_PERIOD))
		return runnable_avg_yN_sum[n];
	else if (unlikely(n >= LOAD_AVG_MAX_N))
		return LOAD_AVG_MAX;

	/* Compute \Sum k^n combining precomputed values for k^i, \Sum k^j */
	do {
		contrib /= 2; /* y^LOAD_AVG_PERIOD = 1/2 */
		contrib += runnable_avg_yN_sum[LOAD_AVG_PERIOD];

		n -= LOAD_AVG_PERIOD;
	} while (n > LOAD_AVG_PERIOD);

	contrib = decay_load(contrib, n);
	return contrib + runnable_avg_yN_sum[n];
}

static void add_to_scaled_stat(int cpu, struct sched_avg *sa, u64 delta);
static inline void decay_scaled_stat(struct sched_avg *sa, u64 periods);

#ifdef CONFIG_SCHED_HMP

/* Initial task load. Newly created tasks are assigned this load. */
unsigned int __read_mostly sched_init_task_load_pelt;
unsigned int __read_mostly sched_init_task_load_windows;
unsigned int __read_mostly sysctl_sched_init_task_load_pct = 15;

/*
 * Keep these two below in sync. One is in unit of ns and the
 * other in unit of us.
 */
unsigned int __read_mostly sysctl_sched_min_runtime = 0; /* 0 ms */
u64 __read_mostly sched_min_runtime = 0; /* 0 ms */

unsigned int max_task_load(void)
{
	if (sched_use_pelt)
		return LOAD_AVG_MAX;

	return sched_ravg_window;
}

/* Use this knob to turn on or off HMP-aware task placement logic */
unsigned int __read_mostly sched_enable_hmp = 0;

/* A cpu can no longer accomodate more tasks if:
 *
 *	rq->nr_running > sysctl_sched_spill_nr_run ||
 *	rq->hmp_stats.cumulative_runnable_avg > sched_spill_load
 */
unsigned int __read_mostly sysctl_sched_spill_nr_run = 10;

/*
 * Control whether or not individual CPU power consumption is used to
 * guide task placement.
 * This sysctl can be set to a default value using boot command line arguments.
 */
unsigned int __read_mostly sysctl_sched_enable_power_aware = 0;

/*
 * This specifies the maximum percent power difference between 2
 * CPUs for them to be considered identical in terms of their
 * power characteristics (i.e. they are in the same power band).
 */
unsigned int __read_mostly sysctl_sched_powerband_limit_pct = 20;

/*
 * CPUs with load greater than the sched_spill_load_threshold are not
 * eligible for task placement. When all CPUs in a cluster achieve a
 * load higher than this level, tasks becomes eligible for inter
 * cluster migration.
 */
unsigned int __read_mostly sched_spill_load;
unsigned int __read_mostly sysctl_sched_spill_load_pct = 100;

/*
 * Tasks whose bandwidth consumption on a cpu is less than
 * sched_small_task are considered as small tasks.
 */
unsigned int __read_mostly sched_small_task;
unsigned int __read_mostly sysctl_sched_small_task_pct = 10;

/*
 * Tasks with demand >= sched_heavy_task will have their
 * window-based demand added to the previous window's CPU
 * time when they wake up, if they have slept for at least
 * one full window. This feature is disabled when the tunable
 * is set to 0 (the default).
 */
#ifdef CONFIG_SCHED_FREQ_INPUT
unsigned int __read_mostly sysctl_sched_heavy_task_pct;
unsigned int __read_mostly sched_heavy_task;
#endif

/*
 * Tasks whose bandwidth consumption on a cpu is more than
 * sched_upmigrate are considered "big" tasks. Big tasks will be
 * considered for "up" migration, i.e migrating to a cpu with better
 * capacity.
 */
unsigned int __read_mostly sched_upmigrate;
unsigned int __read_mostly sysctl_sched_upmigrate_pct = 80;

/*
 * Big tasks, once migrated, will need to drop their bandwidth
 * consumption to less than sched_downmigrate before they are "down"
 * migrated.
 */
unsigned int __read_mostly sched_downmigrate;
unsigned int __read_mostly sysctl_sched_downmigrate_pct = 60;

/*
 * Tasks whose nice value is > sysctl_sched_upmigrate_min_nice are never
 * considered as "big" tasks.
 */
static int __read_mostly sched_upmigrate_min_nice = 15;
int __read_mostly sysctl_sched_upmigrate_min_nice = 15;

/* grp upmigrate/downmigrate */
unsigned int __read_mostly sched_grp_upmigrate;
unsigned int __read_mostly sysctl_sched_grp_upmigrate_pct = 120;

unsigned int __read_mostly sched_grp_downmigrate;
unsigned int __read_mostly sysctl_sched_grp_downmigrate_pct = 100;

/*
 * The load scale factor of a CPU gets boosted when its max frequency
 * is restricted due to which the tasks are migrating to higher capacity
 * CPUs early. The sched_upmigrate threshold is auto-upgraded by
 * rq->max_possible_freq/rq->max_freq of a lower capacity CPU.
 */
unsigned int up_down_migrate_scale_factor = 1024;

/*
 * Scheduler boost is a mechanism to temporarily place tasks on CPUs
 * with higher capacity than those where a task would have normally
 * ended up with their load characteristics. Any entity enabling
 * boost is responsible for disabling it as well.
 */
unsigned int sysctl_sched_boost;

/*
 * When sched_restrict_tasks_spread is enabled, small tasks are packed
 * up to spill thresholds, which otherwise are packed up to mostly_idle
 * thresholds. The RT tasks are also placed on the fist available lowest
 * power CPU which otherwise placed on least loaded CPU including idle
 * CPUs.
 */
unsigned int __read_mostly sysctl_sched_restrict_tasks_spread;

void update_up_down_migrate(void)
{
	unsigned int up_migrate = pct_to_real(sysctl_sched_upmigrate_pct);
	unsigned int down_migrate = pct_to_real(sysctl_sched_downmigrate_pct);
	unsigned int delta;

	if (up_down_migrate_scale_factor == 1024)
		goto done;

	delta = up_migrate - down_migrate;

	up_migrate /= NSEC_PER_USEC;
	up_migrate *= up_down_migrate_scale_factor;
	up_migrate >>= 10;
	up_migrate *= NSEC_PER_USEC;

	up_migrate = min(up_migrate, sched_ravg_window);

	down_migrate /= NSEC_PER_USEC;
	down_migrate *= up_down_migrate_scale_factor;
	down_migrate >>= 10;
	down_migrate *= NSEC_PER_USEC;

	down_migrate = min(down_migrate, up_migrate - delta);
done:
	sched_upmigrate = up_migrate;
	sched_downmigrate = down_migrate;
}

void set_hmp_defaults(void)
{
	sched_spill_load =
		pct_to_real(sysctl_sched_spill_load_pct);

	sched_small_task =
		pct_to_real(sysctl_sched_small_task_pct);

	update_up_down_migrate();

#ifdef CONFIG_SCHED_FREQ_INPUT
	sched_heavy_task =
		pct_to_real(sysctl_sched_heavy_task_pct);
#endif

	sched_init_task_load_pelt =
		div64_u64((u64)sysctl_sched_init_task_load_pct *
			  (u64)LOAD_AVG_MAX, 100);

	sched_init_task_load_windows =
		div64_u64((u64)sysctl_sched_init_task_load_pct *
			  (u64)sched_ravg_window, 100);

	sched_upmigrate_min_nice = sysctl_sched_upmigrate_min_nice;

	sched_grp_upmigrate =
		pct_to_real(sysctl_sched_grp_upmigrate_pct);
	sched_grp_downmigrate =
		pct_to_real(sysctl_sched_grp_downmigrate_pct);

	sched_grp_task_active_period = sched_ravg_window *
				sysctl_sched_grp_task_active_windows;
	sched_grp_min_task_load_delta = sched_ravg_window / 4;
	sched_grp_min_cluster_update_delta = sched_ravg_window / 10;
}

u32 sched_get_init_task_load(struct task_struct *p)
{
	return p->init_load_pct;
}

int sched_set_init_task_load(struct task_struct *p, int init_load_pct)
{
	if (init_load_pct < 0 || init_load_pct > 100)
		return -EINVAL;

	p->init_load_pct = init_load_pct;

	return 0;
}

int sched_set_cpu_prefer_idle(int cpu, int prefer_idle)
{
	struct rq *rq = cpu_rq(cpu);

	rq->prefer_idle = !!prefer_idle;

	return 0;
}

int sched_get_cpu_prefer_idle(int cpu)
{
	struct rq *rq = cpu_rq(cpu);

	return rq->prefer_idle;
}

int sched_set_cpu_mostly_idle_load(int cpu, int mostly_idle_pct)
{
	struct rq *rq = cpu_rq(cpu);

	if (mostly_idle_pct < 0 || mostly_idle_pct > 100)
		return -EINVAL;

	rq->mostly_idle_load = pct_to_real(mostly_idle_pct);

	return 0;
}

int sched_set_cpu_mostly_idle_freq(int cpu, unsigned int mostly_idle_freq)
{
	struct rq *rq = cpu_rq(cpu);

	if (mostly_idle_freq > cpu_max_possible_freq(cpu))
		return -EINVAL;

	rq->cluster->mostly_idle_freq = mostly_idle_freq;

	return 0;
}

unsigned int sched_get_cpu_mostly_idle_freq(int cpu)
{
	struct rq *rq = cpu_rq(cpu);

	return rq->cluster->mostly_idle_freq;
}

int sched_get_cpu_mostly_idle_load(int cpu)
{
	struct rq *rq = cpu_rq(cpu);
	int mostly_idle_pct;

	mostly_idle_pct = real_to_pct(rq->mostly_idle_load);

	return mostly_idle_pct;
}

int sched_set_cpu_mostly_idle_nr_run(int cpu, int nr_run)
{
	struct rq *rq = cpu_rq(cpu);

	rq->mostly_idle_nr_run = nr_run;

	return 0;
}

int sched_get_cpu_mostly_idle_nr_run(int cpu)
{
	struct rq *rq = cpu_rq(cpu);

	return rq->mostly_idle_nr_run;
}

#ifdef CONFIG_CGROUP_SCHED

static inline int upmigrate_discouraged(struct task_struct *p)
{
	return task_group(p)->upmigrate_discouraged;
}

#else

static inline int upmigrate_discouraged(struct task_struct *p)
{
	return 0;
}

#endif

/* Is a task "big" on its current cpu */
static inline int is_big_task(struct task_struct *p)
{
	u64 load = task_load(p);
	int nice = task_nice(p);

	if (nice > sched_upmigrate_min_nice || upmigrate_discouraged(p))
		return 0;

	load = scale_load_to_cpu(load, task_cpu(p));

	return load > sched_upmigrate;
}

/* Is a task "small" on the minimum capacity CPU */
static inline int is_small_task(struct task_struct *p)
{
	u64 load = task_load(p);
	load *= (u64)max_load_scale_factor;
	load /= 1024;
	return load < sched_small_task;
}

static inline u64 cpu_load(int cpu)
{
	struct rq *rq = cpu_rq(cpu);

	return scale_load_to_cpu(rq->hmp_stats.cumulative_runnable_avg, cpu);
}

static inline u64 cpu_load_sync(int cpu, int sync)
{
	struct rq *rq = cpu_rq(cpu);
	u64 load;

	load = rq->hmp_stats.cumulative_runnable_avg;

	/*
	 * If load is being checked in a sync wakeup environment,
	 * we may want to discount the load of the currently running
	 * task.
	 */
	if (sync && cpu == smp_processor_id()) {
		if (load > rq->curr->ravg.demand)
			load -= rq->curr->ravg.demand;
		else
			load = 0;
	}

	return scale_load_to_cpu(load, cpu);
}

static int
spill_threshold_crossed(u64 task_load, u64 cpu_load, struct rq *rq)
{
	u64 total_load = task_load + cpu_load;

	if (total_load > sched_spill_load ||
	    (rq->nr_running + 1) > sysctl_sched_spill_nr_run)
		return 1;

	return 0;
}

int mostly_idle_cpu(int cpu)
{
	struct rq *rq = cpu_rq(cpu);

	return cpu_load(cpu) <= rq->mostly_idle_load
		&& rq->nr_running <= rq->mostly_idle_nr_run
		&& !sched_cpu_high_irqload(cpu);
}

static int mostly_idle_cpu_sync(int cpu, u64 load, int sync)
{
	struct rq *rq = cpu_rq(cpu);
	int nr_running;

	nr_running = rq->nr_running;

	/*
	 * Sync wakeups mean that the waker task will go to sleep
	 * soon so we should discount its load from this test.
	 */
	if (sync && cpu == smp_processor_id())
		nr_running--;

	return load <= rq->mostly_idle_load &&
		nr_running <= rq->mostly_idle_nr_run;
}

static int boost_refcount;
static DEFINE_SPINLOCK(boost_lock);
static DEFINE_MUTEX(boost_mutex);

static void boost_kick_cpus(void)
{
	int i;
	u32 nr_running;

	for_each_online_cpu(i) {
		/*
		 * kick only "small" cluster
		 */
		if (cpu_capacity(i) != max_capacity) {
			nr_running = ACCESS_ONCE(cpu_rq(i)->nr_running);

			/*
			 * make sense to interrupt CPU if its run-queue
			 * has something running in order to check for
			 * migration afterwards, otherwise skip it.
			 */
			if (nr_running)
				boost_kick(i);
		}
	}
}

int sched_boost(void)
{
	return boost_refcount > 0;
}

int sched_set_boost(int enable)
{
	unsigned long flags;
	int ret = 0;
	int old_refcount;

	if (!sched_enable_hmp)
		return -EINVAL;

	spin_lock_irqsave(&boost_lock, flags);

	old_refcount = boost_refcount;

	if (enable == 1) {
		boost_refcount++;
	} else if (!enable) {
		if (boost_refcount >= 1)
			boost_refcount--;
		else
			ret = -EINVAL;
	} else {
		ret = -EINVAL;
	}

	if (!old_refcount && boost_refcount)
		boost_kick_cpus();

	trace_sched_set_boost(boost_refcount);
	spin_unlock_irqrestore(&boost_lock, flags);

	return ret;
}

int sched_boost_handler(struct ctl_table *table, int write,
		void __user *buffer, size_t *lenp,
		loff_t *ppos)
{
	int ret;

	mutex_lock(&boost_mutex);
	if (!write)
		sysctl_sched_boost = sched_boost();

	ret = proc_dointvec(table, write, buffer, lenp, ppos);
	if (ret || !write)
		goto done;

	ret = (sysctl_sched_boost <= 1) ?
		sched_set_boost(sysctl_sched_boost) : -EINVAL;

done:
	mutex_unlock(&boost_mutex);
	return ret;
}

/*
 * Task will fit on a cpu if it's bandwidth consumption on that cpu
 * will be less than sched_upmigrate. A big task that was previously
 * "up" migrated will be considered fitting on "little" cpu if its
 * bandwidth consumption on "little" cpu will be less than
 * sched_downmigrate. This will help avoid frequenty migrations for
 * tasks with load close to the upmigrate threshold
 */

static int task_load_will_fit(struct task_struct *p, u64 task_load, int cpu)
{
	int prev_cpu = task_cpu(p);
	int upmigrate, nice;

	if (cpu_capacity(cpu) == max_capacity)
		return 1;

	if (sched_boost()) {
		if (cpu_capacity(cpu) > cpu_capacity(prev_cpu))
			return 1;
	} else {
		nice = task_nice(p);
		if (nice > sched_upmigrate_min_nice || upmigrate_discouraged(p))
			return 1;

		upmigrate = sched_upmigrate;
		if (cpu_capacity(prev_cpu) > cpu_capacity(cpu))
			upmigrate = sched_downmigrate;

		if (task_load < upmigrate)
			return 1;
	}

	return 0;
}

static int task_will_fit(struct task_struct *p, int cpu)
{
	u64 tload = scale_load_to_cpu(task_load(p), cpu);

	return task_load_will_fit(p, tload, cpu);
}

int group_will_fit(struct sched_cluster *cluster,
		 struct related_thread_group *grp, u64 demand)
{
	int cpu = cluster_first_cpu(cluster);
	int prev_capacity = 0;
	unsigned int threshold = sched_grp_upmigrate;
	u64 load;

	if (cluster->capacity == max_capacity)
		return 1;

	if (grp->preferred_cluster)
		prev_capacity = grp->preferred_cluster->capacity;

	if (cluster->capacity < prev_capacity)
		threshold = sched_grp_downmigrate;

	load = scale_load_to_cpu(demand, cpu);
	if (load < threshold)
		return 1;

	return 0;
}

static int eligible_cpu(u64 task_load, u64 cpu_load, int cpu, int sync)
{
	struct rq *rq = cpu_rq(cpu);

	if (sched_cpu_high_irqload(cpu))
		return 0;

	if (mostly_idle_cpu_sync(cpu, cpu_load, sync))
		return 1;

	if (cpu_max_possible_capacity(cpu) != max_possible_capacity)
		return !spill_threshold_crossed(task_load, cpu_load, rq);

	return 0;
}

struct cpu_pwr_stats __weak *get_cpu_pwr_stats(void)
{
	return NULL;
}

int power_delta_exceeded(unsigned int cpu_cost, unsigned int base_cost)
{
	int delta, cost_limit;

	if (!base_cost || cpu_cost == base_cost)
		return 0;

	if (!sysctl_sched_enable_power_aware)
		return 1;

	delta = cpu_cost - base_cost;
	cost_limit = div64_u64((u64)sysctl_sched_powerband_limit_pct *
						(u64)base_cost, 100);
	return abs(delta) > cost_limit;
}

static unsigned int power_cost_at_freq(int cpu, unsigned int freq)
{
	int i = 0;
	struct cpu_pwr_stats *per_cpu_info = get_cpu_pwr_stats();
	struct cpu_pstate_pwr *costs;

	if (!per_cpu_info || !per_cpu_info[cpu].ptable ||
	    !sysctl_sched_enable_power_aware)
		/* When power aware scheduling is not in use, or CPU
		 * power data is not available, just use the CPU
		 * capacity as a rough stand-in for real CPU power
		 * numbers, assuming bigger CPUs are more power
		 * hungry. */
		return cpu_max_possible_capacity(cpu);

	if (!freq)
		freq = min_max_freq;

	costs = per_cpu_info[cpu].ptable;

	while (costs[i].freq != 0) {
		if (costs[i].freq >= freq ||
		    costs[i+1].freq == 0)
			return costs[i].power;
		i++;
	}
	BUG();
}

/* Return the cost of running task p on CPU cpu. This function
 * currently assumes that task p is the only task which will run on
 * the CPU. */
unsigned int power_cost(u64 task_load, int cpu)
{
	unsigned int task_freq, cur_freq;
	struct rq *rq = cpu_rq(cpu);
	u64 demand;
	int total_static_pwr_cost = 0;

	if (!sysctl_sched_enable_power_aware)
		return cpu_max_possible_capacity(cpu);

	/* calculate % of max freq needed */
	demand = task_load * 100;
	demand = div64_u64(demand, max_task_load());

	task_freq = demand * cpu_max_possible_freq(cpu);
	task_freq /= 100; /* khz needed */

	cur_freq = cpu_cur_freq(cpu);
	task_freq = max(cur_freq, task_freq);

	if (idle_cpu(cpu) && rq->cstate) {
		total_static_pwr_cost += rq->static_cpu_pwr_cost;
		if (rq->dstate)
			total_static_pwr_cost += rq->static_cluster_pwr_cost;
	}

	return power_cost_at_freq(cpu, task_freq) + total_static_pwr_cost;
}

static int best_small_task_cpu(struct task_struct *p, int sync)
{
	int best_busy_cpu = -1, fallback_cpu = -1;
	int min_cstate_cpu = -1;
	int min_cstate = INT_MAX;
	int cpu_cost, min_cost = INT_MAX;
	int i = task_cpu(p), prev_cpu;
	int hmp_capable;
	u64 tload, cpu_load, min_load = ULLONG_MAX;
	cpumask_t search_cpu;
	cpumask_t fb_search_cpu = CPU_MASK_NONE;
	struct rq *rq;

	hmp_capable = !cpumask_equal(&mpc_mask, cpu_possible_mask);

	cpumask_and(&search_cpu, tsk_cpus_allowed(p), cpu_active_mask);
	if (unlikely(!cpumask_test_cpu(i, &search_cpu))) {
		if (cpumask_empty(&search_cpu))
			return fallback_cpu;

		i = cpumask_first(&search_cpu);
		if (i >= nr_cpu_ids)
			return fallback_cpu;
	}

	do {
		rq = cpu_rq(i);

		trace_sched_cpu_load(rq, idle_cpu(i),
				     mostly_idle_cpu_sync(i,
						  cpu_load_sync(i, sync), sync),
				     sched_irqload(i),
				     power_cost(scale_load_to_cpu(task_load(p),
						i), i),
				     cpu_temp(i));

		if (cpu_max_possible_capacity(i) == max_possible_capacity &&
		    hmp_capable) {
			cpumask_and(&fb_search_cpu, &search_cpu,
				    &rq->freq_domain_cpumask);
			cpumask_andnot(&search_cpu, &search_cpu,
				       &rq->freq_domain_cpumask);
			continue;
		}

		cpumask_clear_cpu(i, &search_cpu);

		if (sched_cpu_high_irqload(i))
			continue;

		if (idle_cpu(i) && rq->cstate) {
			if (rq->cstate < min_cstate) {
				min_cstate_cpu = i;
				min_cstate = rq->cstate;
			}
			continue;
		}

		cpu_load = cpu_load_sync(i, sync);

		if (sysctl_sched_restrict_tasks_spread) {
			tload = scale_load_to_cpu(task_load(p), i);
			if (!spill_threshold_crossed(tload, cpu_load, rq)) {
				if (cpu_load < min_load) {
					min_load = cpu_load;
					best_busy_cpu = i;
				}
			}
			continue;
		}

		if (mostly_idle_cpu_sync(i, cpu_load, sync))
			return i;

	} while ((i = cpumask_first(&search_cpu)) < nr_cpu_ids);

	if (best_busy_cpu != -1)
		return best_busy_cpu;

	if (min_cstate_cpu != -1)
		return min_cstate_cpu;

	if (!sysctl_sched_restrict_tasks_spread) {
		cpumask_and(&search_cpu, tsk_cpus_allowed(p), cpu_active_mask);
		cpumask_andnot(&search_cpu, &search_cpu, &fb_search_cpu);
		for_each_cpu(i, &search_cpu) {
			rq = cpu_rq(i);
			prev_cpu = (i == task_cpu(p));

			if (sched_cpu_high_irqload(i))
				continue;

			tload = scale_load_to_cpu(task_load(p), i);
			cpu_load = cpu_load_sync(i, sync);
			if (!spill_threshold_crossed(tload, cpu_load, rq)) {
				if (cpu_load < min_load ||
						(prev_cpu &&
						 cpu_load == min_load)) {
					min_load = cpu_load;
					best_busy_cpu = i;
				}
			}
		}

		if (best_busy_cpu != -1)
			return best_busy_cpu;

	}

	for_each_cpu(i, &fb_search_cpu) {
		rq = cpu_rq(i);
		prev_cpu = (i == task_cpu(p));

		tload = scale_load_to_cpu(task_load(p), i);
		cpu_cost = power_cost(tload, i);
		if (cpu_cost < min_cost ||
		   (prev_cpu && cpu_cost == min_cost)) {
			fallback_cpu = i;
			min_cost = cpu_cost;
		}
	}

	return fallback_cpu;
}

#define UP_MIGRATION		1
#define DOWN_MIGRATION		2
#define EA_MIGRATION		3
#define IRQLOAD_MIGRATION	4
#define PREFERRED_CLUSTER_MIGRATION	5

/*
 * preferred_cluster() is called from load balance and tick paths without
 * the task pi_lock is held. Access p->grp under rcu_read_lock()
 */
static inline int
preferred_cluster(struct sched_cluster *cluster, struct task_struct *p)
{
	struct related_thread_group *grp;
	int rc = 0;

	rcu_read_lock();

	grp = p->grp;
	/*
	 * If the preferred cluster is the minimum cluster in the system,
	 * there is no need to tie the tasks to their peferred cluster.
	 */
	if (!grp || !sysctl_sched_enable_colocation ||
			grp->preferred_cluster->capacity == min_capacity) {
		rc = 1;
		goto done;
	}

	rc = (grp->preferred_cluster == cluster);
done:
	rcu_read_unlock();

	return rc;
}

static int skip_freq_domain(int tcpu, int cpu, int reason,
			struct sched_cluster *pref_cluster)
{
	int skip;

	if (!reason)
		return 0;

	switch (reason) {
	case UP_MIGRATION:
		skip = cpu_capacity(cpu) <= cpu_capacity(tcpu);
		break;

	case DOWN_MIGRATION:
		skip = cpu_capacity(cpu) >= cpu_capacity(tcpu);
		break;

	case EA_MIGRATION:
		skip = cpu_capacity(cpu) != cpu_capacity(tcpu);
		break;

	case PREFERRED_CLUSTER_MIGRATION:
		skip = cpu_rq(cpu)->cluster != pref_cluster;
		break;

	case IRQLOAD_MIGRATION:
		/* Purposely fall through */

	default:
		return 0;
	}

	return skip;
}

static int skip_cpu(struct rq *task_rq, struct rq *rq, int cpu,
		    u64 task_load, int reason)
{
	int skip;

	if (!reason)
		return 0;

	if (is_reserved(cpu))
		return 1;

	switch (reason) {
	case EA_MIGRATION:
		skip = power_cost(task_load, cpu) >
		       power_cost(task_load, cpu_of(task_rq));
		break;

	case IRQLOAD_MIGRATION:
		/* Purposely fall through */

	default:
		skip = (rq == task_rq);
	}

	return skip;
}

/*
 * Select a single cpu in cluster as target for packing, iff cluster frequency
 * is less than a threshold level
 */
static int select_packing_target(struct task_struct *p, int best_cpu)
{
	struct rq *rq = cpu_rq(best_cpu);
	struct cpumask search_cpus;
	int i;
	int min_cost = INT_MAX;
	int target = best_cpu;

	if (cpu_cur_freq(best_cpu) >= cpu_mostly_idle_freq(best_cpu))
		return best_cpu;

	/* Don't pack if current freq is low because of throttling */
	if (cpu_max_freq(best_cpu) <= cpu_mostly_idle_freq(best_cpu))
		return best_cpu;

	cpumask_and(&search_cpus, tsk_cpus_allowed(p), cpu_active_mask);
	cpumask_and(&search_cpus, &search_cpus, &rq->freq_domain_cpumask);

	/* Pick the first lowest power cpu as target */
	for_each_cpu(i, &search_cpus) {
		int cost = power_cost(scale_load_to_cpu(task_load(p), i), i);

		if (cost < min_cost && !sched_cpu_high_irqload(i)) {
			target = i;
			min_cost = cost;
		}
	}

	return target;
}

/*
 * Should task be woken to any available idle cpu?
 *
 * Waking tasks to idle cpu has mixed implications on both performance and
 * power. In many cases, scheduler can't estimate correctly impact of using idle
 * cpus on either performance or power. PF_WAKE_UP_IDLE allows external kernel
 * module to pass a strong hint to scheduler that the task in question should be
 * woken to idle cpu, generally to improve performance.
 */
static inline int wake_to_idle(struct task_struct *p)
{
	return (current->flags & PF_WAKE_UP_IDLE) ||
			 (p->flags & PF_WAKE_UP_IDLE);
}

/* return cheapest cpu that can fit this task */
static int select_best_cpu(struct task_struct *p, int target, int reason,
			   int sync)
{
	int i, j, prev_cpu, best_cpu = -1;
	int fallback_idle_cpu = -1, min_cstate_cpu = -1;
	int cpu_cost, min_cost = INT_MAX;
	int min_idle_cost = INT_MAX, min_busy_cost = INT_MAX;
	u64 tload, cpu_load;
	u64 min_load = ULLONG_MAX, min_fallback_load = ULLONG_MAX;
	int small_task = is_small_task(p);
	int boost = sched_boost();
	int cstate, min_cstate = INT_MAX;
	int prefer_idle = -1;
	int prefer_idle_override = 0;
	cpumask_t search_cpus;
	struct rq *trq;
	struct related_thread_group *grp;
	struct sched_cluster *pref_cluster = NULL;

	rcu_read_lock();	/* Protected access to p->grp */

	grp = p->grp;

	/*
	 * If the preferred cluster is the minimum cluster in the system,
	 * select the CPU based on the individual task requirements.
	 */
	if (sysctl_sched_enable_colocation && grp && grp->preferred_cluster &&
			grp->preferred_cluster->capacity > min_capacity) {
		pref_cluster = grp->preferred_cluster;
		small_task = 0;
	}

	if (reason) {
		prefer_idle = 1;
		prefer_idle_override = 1;
	}

	if (wake_to_idle(p)) {
		prefer_idle = 1;
		prefer_idle_override = 1;
		small_task = 0;
		/*
		 * If wake to idle and sync are both set prefer wake to idle
		 * since sync is a weak hint that might not always be correct.
		 */
		sync = 0;
	}

	if (small_task && !boost && !sync) {
		best_cpu = best_small_task_cpu(p, sync);
		prefer_idle = 0;	/* For sched_task_load tracepoint */
		goto done;
	}

	trq = task_rq(p);
	cpumask_and(&search_cpus, tsk_cpus_allowed(p), cpu_active_mask);
	if (sync) {
		unsigned int cpuid = smp_processor_id();
		if (cpumask_test_cpu(cpuid, &search_cpus)) {
			best_cpu = cpuid;
			goto done;
		}
	}

	for_each_cpu(i, &search_cpus) {
		struct rq *rq = cpu_rq(i);

		trace_sched_cpu_load(cpu_rq(i), idle_cpu(i),
				     mostly_idle_cpu_sync(i,
						  cpu_load_sync(i, sync), sync),
				     sched_irqload(i),
				     power_cost(scale_load_to_cpu(task_load(p),
						i), i),
				     cpu_temp(i));

		if (skip_freq_domain(task_cpu(p), i, reason, pref_cluster)) {
			cpumask_andnot(&search_cpus, &search_cpus,
						&rq->freq_domain_cpumask);
			continue;
		}

		tload =  scale_load_to_cpu(task_load(p), i);
		if (skip_cpu(trq, rq, i, tload, reason))
			continue;

		prev_cpu = (i == task_cpu(p));

		/*
		 * The least-loaded mostly-idle CPU where the task
		 * won't fit is our fallback if we can't find a CPU
		 * where the task will fit.
		 */
		if ((pref_cluster && rq->cluster != pref_cluster) ||
					!task_load_will_fit(p, tload, i)) {
			for_each_cpu_and(j, &search_cpus,
						&rq->freq_domain_cpumask) {
				cpu_load = cpu_load_sync(j, sync);
				if (mostly_idle_cpu_sync(j, cpu_load, sync) &&
						!sched_cpu_high_irqload(j)) {
					if (cpu_load < min_fallback_load ||
					    (cpu_load == min_fallback_load &&
							 j == task_cpu(p))) {
						min_fallback_load = cpu_load;
						fallback_idle_cpu = j;
					}
				}
			}
			cpumask_andnot(&search_cpus, &search_cpus,
						&rq->freq_domain_cpumask);
			continue;
		}

		/* Set prefer_idle based on the cpu where task will first fit */
		if (prefer_idle == -1)
			prefer_idle = cpu_rq(i)->prefer_idle;

		cpu_load = cpu_load_sync(i, sync);
		if (!eligible_cpu(tload, cpu_load, i, sync))
			continue;

		/*
		 * The task will fit on this CPU, and the CPU is either
		 * mostly_idle or not max capacity and can fit it under
		 * spill.
		 */

		cpu_cost = power_cost(tload, i);

		/*
		 * If the task fits in a CPU in a lower power band, that
		 * overrides load and C-state.
		 */
		if (power_delta_exceeded(cpu_cost, min_cost)) {
			if (cpu_cost > min_cost)
				continue;

			min_cost = cpu_cost;
			min_load = ULLONG_MAX;
			min_cstate = INT_MAX;
			min_cstate_cpu = -1;
			best_cpu = -1;
			if (!prefer_idle_override)
				prefer_idle = cpu_rq(i)->prefer_idle;
		}

		/*
		 * Partition CPUs based on whether they are completely idle
		 * or not. For completely idle CPUs we choose the one in
		 * the lowest C-state and then break ties with power cost.
		 *
		 * For sync wakeups we only consider the waker CPU as idle if
		 * prefer_idle is set. Otherwise if prefer_idle is unset sync
		 * wakeups will get biased away from the waker CPU.
		 */
		if (idle_cpu(i) || (sync && i == smp_processor_id()
			&& prefer_idle && cpu_rq(i)->nr_running == 1)) {
			cstate = cpu_rq(i)->cstate;

			if (cstate > min_cstate)
				continue;

			if (cstate < min_cstate) {
				min_idle_cost = cpu_cost;
				min_cstate = cstate;
				min_cstate_cpu = i;
				continue;
			}

			if (cpu_cost < min_idle_cost ||
			    (prev_cpu && cpu_cost == min_idle_cost)) {
				min_idle_cost = cpu_cost;
				min_cstate_cpu = i;
			}

			continue;
		}

		/*
		 * For CPUs that are not completely idle, pick one with the
		 * lowest load and break ties with power cost
		 */
		if (cpu_load > min_load)
			continue;

		if (cpu_load < min_load) {
			min_load = cpu_load;
			min_busy_cost = cpu_cost;
			best_cpu = i;
			continue;
		}

		/*
		 * The load is equal to the previous selected CPU.
		 * This is rare but when it does happen opt for the
		 * more power efficient CPU option.
		 */
		if (cpu_cost < min_busy_cost ||
		    (prev_cpu && cpu_cost == min_busy_cost)) {
			min_busy_cost = cpu_cost;
			best_cpu = i;
		}
	}

	/*
	 * Don't need to check !sched_cpu_high_irqload(best_cpu) because
	 * best_cpu cannot have high irq load.
	 */
	if (min_cstate_cpu >= 0 && (prefer_idle > 0 || best_cpu < 0 ||
			!mostly_idle_cpu_sync(best_cpu, min_load, sync)))
		best_cpu = min_cstate_cpu;
done:
	if (best_cpu < 0) {
		if (unlikely(fallback_idle_cpu < 0))
			/*
			 * For the lack of a better choice just use
			 * prev_cpu. We may just benefit from having
			 * a hot cache.
			 */
			best_cpu = task_cpu(p);
		else
			best_cpu = fallback_idle_cpu;
	}

	if (cpu_mostly_idle_freq(best_cpu) && !prefer_idle_override)
		best_cpu = select_packing_target(p, best_cpu);

	rcu_read_unlock();

	/*
	 * prefer_idle is initialized towards middle of function. Leave this
	 * tracepoint towards end to capture prefer_idle flag used for this
	 * instance of wakeup.
	 */
	trace_sched_task_load(p, small_task, boost, reason, sync, prefer_idle);

	return best_cpu;
}

static void
inc_nr_big_small_task(struct hmp_sched_stats *stats, struct task_struct *p)
{
	if (!sched_enable_hmp || sched_disable_window_stats)
		return;

	if (is_big_task(p))
		stats->nr_big_tasks++;
	else if (is_small_task(p))
		stats->nr_small_tasks++;
}

static void
dec_nr_big_small_task(struct hmp_sched_stats *stats, struct task_struct *p)
{
	if (!sched_enable_hmp || sched_disable_window_stats)
		return;

	if (is_big_task(p))
		stats->nr_big_tasks--;
	else if (is_small_task(p))
		stats->nr_small_tasks--;

	BUG_ON(stats->nr_big_tasks < 0 || stats->nr_small_tasks < 0);
}

static void
inc_rq_hmp_stats(struct rq *rq, struct task_struct *p, int change_cra)
{
	inc_nr_big_small_task(&rq->hmp_stats, p);
	if (change_cra)
		inc_cumulative_runnable_avg(&rq->hmp_stats, p);
}

static void
dec_rq_hmp_stats(struct rq *rq, struct task_struct *p, int change_cra)
{
	dec_nr_big_small_task(&rq->hmp_stats, p);
	if (change_cra)
		dec_cumulative_runnable_avg(&rq->hmp_stats, p);
}

static void reset_hmp_stats(struct hmp_sched_stats *stats, int reset_cra)
{
	stats->nr_big_tasks = stats->nr_small_tasks = 0;
	if (reset_cra)
		stats->cumulative_runnable_avg = 0;
}


#ifdef CONFIG_CFS_BANDWIDTH

static inline struct task_group *next_task_group(struct task_group *tg)
{
	tg = list_entry_rcu(tg->list.next, typeof(struct task_group), list);

	return (&tg->list == &task_groups) ? NULL : tg;
}

/* Iterate over all cfs_rq in a cpu */
#define for_each_cfs_rq(cfs_rq, tg, cpu)	\
	for (tg = container_of(&task_groups, struct task_group, list);	\
		((tg = next_task_group(tg)) && (cfs_rq = tg->cfs_rq[cpu]));)

static void reset_cfs_rq_hmp_stats(int cpu, int reset_cra)
{
	struct task_group *tg;
	struct cfs_rq *cfs_rq;

	rcu_read_lock();

	for_each_cfs_rq(cfs_rq, tg, cpu)
		reset_hmp_stats(&cfs_rq->hmp_stats, reset_cra);

	rcu_read_unlock();
}

#else	/* CONFIG_CFS_BANDWIDTH */

static inline void reset_cfs_rq_hmp_stats(int cpu, int reset_cra) { }

#endif	/* CONFIG_CFS_BANDWIDTH */

/*
 * Return total number of tasks "eligible" to run on highest capacity cpu
 *
 * This is simply nr_big_tasks for cpus which are not of max_capacity and
 * (nr_running - nr_small_tasks) for cpus of max_capacity
 */
unsigned int nr_eligible_big_tasks(int cpu)
{
	struct rq *rq = cpu_rq(cpu);
	int nr_big = rq->hmp_stats.nr_big_tasks;
	int nr = rq->nr_running;
	int nr_small = rq->hmp_stats.nr_small_tasks;

	if (cpu_max_possible_capacity(cpu) != max_possible_capacity)
		return nr_big;

	/* Consider all (except small) tasks on max_capacity cpu as big tasks */
	nr_big = nr - nr_small;
	if (nr_big < 0)
		nr_big = 0;

	return nr_big;
}

/*
 * reset_cpu_hmp_stats - reset HMP stats for a cpu
 *	nr_big_tasks, nr_small_tasks
 *	cumulative_runnable_avg (iff reset_cra is true)
 */
void reset_cpu_hmp_stats(int cpu, int reset_cra)
{
	reset_cfs_rq_hmp_stats(cpu, reset_cra);
	reset_hmp_stats(&cpu_rq(cpu)->hmp_stats, reset_cra);
}

#ifdef CONFIG_CFS_BANDWIDTH

static inline int cfs_rq_throttled(struct cfs_rq *cfs_rq);

static void inc_cfs_rq_hmp_stats(struct cfs_rq *cfs_rq,
	 struct task_struct *p, int change_cra);
static void dec_cfs_rq_hmp_stats(struct cfs_rq *cfs_rq,
	 struct task_struct *p, int change_cra);

/* Add task's contribution to a cpu' HMP statistics */
static void
_inc_hmp_sched_stats_fair(struct rq *rq, struct task_struct *p, int change_cra)
{
	struct cfs_rq *cfs_rq;
	struct sched_entity *se = &p->se;

	/*
	 * Although below check is not strictly required  (as
	 * inc/dec_nr_big_small_task and inc/dec_cumulative_runnable_avg called
	 * from inc_cfs_rq_hmp_stats() have similar checks), we gain a bit on
	 * efficiency by short-circuiting for_each_sched_entity() loop when
	 * !sched_enable_hmp || sched_disable_window_stats
	 */
	if (!sched_enable_hmp || sched_disable_window_stats)
		return;

	for_each_sched_entity(se) {
		cfs_rq = cfs_rq_of(se);
		inc_cfs_rq_hmp_stats(cfs_rq, p, change_cra);
		if (cfs_rq_throttled(cfs_rq))
			break;
	}

	/* Update rq->hmp_stats only if we didn't find any throttled cfs_rq */
	if (!se)
		inc_rq_hmp_stats(rq, p, change_cra);
}

/* Remove task's contribution from a cpu' HMP statistics */
static void
_dec_hmp_sched_stats_fair(struct rq *rq, struct task_struct *p, int change_cra)
{
	struct cfs_rq *cfs_rq;
	struct sched_entity *se = &p->se;

	/* See comment on efficiency in _inc_hmp_sched_stats_fair */
	if (!sched_enable_hmp || sched_disable_window_stats)
		return;

	for_each_sched_entity(se) {
		cfs_rq = cfs_rq_of(se);
		dec_cfs_rq_hmp_stats(cfs_rq, p, change_cra);
		if (cfs_rq_throttled(cfs_rq))
			break;
	}

	/* Update rq->hmp_stats only if we didn't find any throttled cfs_rq */
	if (!se)
		dec_rq_hmp_stats(rq, p, change_cra);
}

static void inc_hmp_sched_stats_fair(struct rq *rq, struct task_struct *p)
{
	_inc_hmp_sched_stats_fair(rq, p, 1);
}

static void dec_hmp_sched_stats_fair(struct rq *rq, struct task_struct *p)
{
	_dec_hmp_sched_stats_fair(rq, p, 1);
}

static void fixup_hmp_sched_stats_fair(struct rq *rq, struct task_struct *p,
				       u32 new_task_load)
{
	struct cfs_rq *cfs_rq;
	struct sched_entity *se = &p->se;
	u32 old_task_load = p->ravg.demand;

	for_each_sched_entity(se) {
		cfs_rq = cfs_rq_of(se);

		dec_nr_big_small_task(&cfs_rq->hmp_stats, p);
		fixup_cumulative_runnable_avg(&cfs_rq->hmp_stats, p,
					      new_task_load);
		inc_nr_big_small_task(&cfs_rq->hmp_stats, p);
		if (cfs_rq_throttled(cfs_rq))
			break;
		/*
		 * fixup_cumulative_runnable_avg() sets p->ravg.demand to
		 * new_task_load.
		 */
		p->ravg.demand = old_task_load;
	}

	/* Fix up rq->hmp_stats only if we didn't find any throttled cfs_rq */
	if (!se) {
		dec_nr_big_small_task(&rq->hmp_stats, p);
		fixup_cumulative_runnable_avg(&rq->hmp_stats, p, new_task_load);
		inc_nr_big_small_task(&rq->hmp_stats, p);
	}
}

static int task_will_be_throttled(struct task_struct *p);

#else	/* CONFIG_CFS_BANDWIDTH */

static void
inc_hmp_sched_stats_fair(struct rq *rq, struct task_struct *p)
{
	inc_nr_big_small_task(&rq->hmp_stats, p);
	inc_cumulative_runnable_avg(&rq->hmp_stats, p);
}

static void
dec_hmp_sched_stats_fair(struct rq *rq, struct task_struct *p)
{
	dec_nr_big_small_task(&rq->hmp_stats, p);
	dec_cumulative_runnable_avg(&rq->hmp_stats, p);
}

static void
fixup_hmp_sched_stats_fair(struct rq *rq, struct task_struct *p,
			   u32 new_task_load)
{
	dec_nr_big_small_task(&rq->hmp_stats, p);
	fixup_cumulative_runnable_avg(&rq->hmp_stats, p, new_task_load);
	inc_nr_big_small_task(&rq->hmp_stats, p);
}

static inline int task_will_be_throttled(struct task_struct *p)
{
	return 0;
}

static void
_inc_hmp_sched_stats_fair(struct rq *rq, struct task_struct *p, int change_cra)
{
	inc_nr_big_small_task(&rq->hmp_stats, p);
}

#endif	/* CONFIG_CFS_BANDWIDTH */

/*
 * Walk runqueue of cpu and re-initialize 'nr_big_tasks' and 'nr_small_tasks'
 * counters.
 */
void fixup_nr_big_small_task(int cpu, int reset_stats)
{
	struct rq *rq = cpu_rq(cpu);
	struct task_struct *p;

	/* fixup_nr_big_small_task() is called from two functions. In one of
	 * them stats are already reset, don't waste time resetting them again
	 */
	if (reset_stats) {
		/* Do not reset cumulative_runnable_avg */
		reset_cpu_hmp_stats(cpu, 0);
	}

	list_for_each_entry(p, &rq->cfs_tasks, se.group_node)
		_inc_hmp_sched_stats_fair(rq, p, 0);
}

/* Disable interrupts and grab runqueue lock of all cpus listed in @cpus */
void pre_big_small_task_count_change(const struct cpumask *cpus)
{
	int i;

	local_irq_disable();

	for_each_cpu(i, cpus)
		raw_spin_lock(&cpu_rq(i)->lock);
}

/*
 * Reinitialize 'nr_big_tasks' and 'nr_small_tasks' counters on all affected
 * cpus
 */
void post_big_small_task_count_change(const struct cpumask *cpus)
{
	int i;

	/* Assumes local_irq_disable() keeps online cpumap stable */
	for_each_cpu(i, cpus)
		fixup_nr_big_small_task(i, 1);

	for_each_cpu(i, cpus)
		raw_spin_unlock(&cpu_rq(i)->lock);

	local_irq_enable();
}

DEFINE_MUTEX(policy_mutex);

#ifdef CONFIG_SCHED_FREQ_INPUT
static inline int invalid_value_freq_input(unsigned int *data)
{
	if (data == &sysctl_sched_migration_fixup)
		return !(*data == 0 || *data == 1);

	if (data == &sysctl_sched_freq_account_wait_time)
		return !(*data == 0 || *data == 1);

	return 0;
}
#else
static inline int invalid_value_freq_input(unsigned int *data)
{
	return 0;
}
#endif

static inline int invalid_value(unsigned int *data)
{
	unsigned int val = *data;

	if (data == &sysctl_sched_ravg_hist_size)
		return (val < 2 || val > RAVG_HIST_SIZE_MAX);

	if (data == &sysctl_sched_window_stats_policy)
		return val >= WINDOW_STATS_INVALID_POLICY;

	if (data == &sysctl_sched_account_wait_time)
		return !(val == 0 || val == 1);

	return invalid_value_freq_input(data);
}

/*
 * Handle "atomic" update of sysctl_sched_window_stats_policy,
 * sysctl_sched_ravg_hist_size, sysctl_sched_account_wait_time and
 * sched_freq_legacy_mode variables.
 */
int sched_window_update_handler(struct ctl_table *table, int write,
		void __user *buffer, size_t *lenp,
		loff_t *ppos)
{
	int ret;
	unsigned int *data = (unsigned int *)table->data;
	unsigned int old_val;

	if (!sched_enable_hmp)
		return -EINVAL;

	mutex_lock(&policy_mutex);

	old_val = *data;

	ret = proc_dointvec(table, write, buffer, lenp, ppos);
	if (ret || !write || (write && (old_val == *data)))
		goto done;

	if (invalid_value(data)) {
		*data = old_val;
		ret = -EINVAL;
		goto done;
	}

	reset_all_window_stats(0, 0);

done:
	mutex_unlock(&policy_mutex);

	return ret;
}

/*
 * Convert percentage value into absolute form. This will avoid div() operation
 * in fast path, to convert task load in percentage scale.
 */
int sched_hmp_proc_update_handler(struct ctl_table *table, int write,
		void __user *buffer, size_t *lenp,
		loff_t *ppos)
{
	int ret;
	unsigned int old_val;
	unsigned int *data = (unsigned int *)table->data;
	int update_task_count = 0;

	if (!sched_enable_hmp)
		return 0;

	/*
	 * The policy mutex is acquired with cpu_hotplug.lock
	 * held from cpu_up()->cpufreq_governor_interactive()->
	 * sched_set_window(). So enforce the same order here.
	 */
	if (write && (data == &sysctl_sched_upmigrate_pct ||
	    data == &sysctl_sched_small_task_pct ||
	    data == (unsigned int *)&sysctl_sched_upmigrate_min_nice)) {
		update_task_count = 1;
		get_online_cpus();
	}

	mutex_lock(&policy_mutex);

	old_val = *data;

	ret = proc_dointvec_minmax(table, write, buffer, lenp, ppos);

	if (ret || !write || !sched_enable_hmp)
		goto done;

	if (write && (old_val == *data))
		goto done;

	if (data == &sysctl_sched_min_runtime) {
		sched_min_runtime = ((u64) sysctl_sched_min_runtime) * 1000;
		goto done;
	}

	if (data == &sysctl_sched_grp_task_active_windows) {
		sched_grp_task_active_period = sched_ravg_window *
					sysctl_sched_grp_task_active_windows;
		goto done;
	}

	if ((data == &sysctl_sched_grp_upmigrate_pct ||
	     data == &sysctl_sched_grp_downmigrate_pct)) {
		if (sysctl_sched_grp_downmigrate_pct >
				sysctl_sched_grp_upmigrate_pct) {
			*data = old_val;
			ret = -EINVAL;
			goto done;
		} else {
			set_hmp_defaults();
			goto done;
		}
	}

	if (data == (unsigned int *)&sysctl_sched_upmigrate_min_nice) {
		if ((*(int *)data) < -20 || (*(int *)data) > 19) {
			*data = old_val;
			ret = -EINVAL;
			goto done;
		}
	} else {
		/* all tunables other than min_nice are in percentage */
		if (sysctl_sched_downmigrate_pct >
		    sysctl_sched_upmigrate_pct || *data > 100) {
			*data = old_val;
			ret = -EINVAL;
			goto done;
		}
	}

	/*
	 * Big/Small task tunable change will need to re-classify tasks on
	 * runqueue as big and small and set their counters appropriately.
	 * sysctl interface affects secondary variables (*_pct), which is then
	 * "atomically" carried over to the primary variables. Atomic change
	 * includes taking runqueue lock of all online cpus and re-initiatizing
	 * their big/small counter values based on changed criteria.
	 */
	if (update_task_count)
		pre_big_small_task_count_change(cpu_online_mask);

	set_hmp_defaults();

	if (update_task_count)
		post_big_small_task_count_change(cpu_online_mask);

done:
	if (update_task_count)
		put_online_cpus();
	mutex_unlock(&policy_mutex);
	return ret;
}

/*
 * Reset balance_interval at all sched_domain levels of given cpu, so that it
 * honors kick.
 */
static inline void reset_balance_interval(int cpu)
{
	struct sched_domain *sd;

	if (cpu >= nr_cpu_ids)
		return;

	rcu_read_lock();
	for_each_domain(cpu, sd)
		sd->balance_interval = 0;
	rcu_read_unlock();
}

static inline int find_new_hmp_ilb(int type)
{
	int i;
	int call_cpu = raw_smp_processor_id();
	int best_cpu = nr_cpu_ids;
	struct sched_domain *sd;
	int min_cost = INT_MAX, cost;
	struct rq *dst_rq;

	rcu_read_lock();

	/* Pick an idle cpu "closest" to call_cpu */
	for_each_domain(call_cpu, sd) {
		for_each_cpu(i, sched_domain_span(sd)) {
			dst_rq = cpu_rq(i);
			if (!idle_cpu(i) || (type == NOHZ_KICK_RESTRICT
				  && cpu_capacity(i) > cpu_capacity(call_cpu)))
				continue;

			cost = power_cost_at_freq(i, min_max_freq);
			if (cost < min_cost) {
				best_cpu = i;
				min_cost = cost;
			}
		}

		if (best_cpu < nr_cpu_ids)
			break;
	}

	rcu_read_unlock();

	reset_balance_interval(best_cpu);

	return best_cpu;
}

/*
 * For the current task's CPU, we don't check whether there are
 * multiple tasks. Just see if running the task on another CPU is
 * lower power than running only this task on the current CPU. This is
 * not the most accurate model, but we should be load balanced most of
 * the time anyway. */
static int lower_power_cpu_available(struct task_struct *p, int cpu)
{
	int i;
	int lowest_power_cpu = task_cpu(p);
	int lowest_power = power_cost(scale_load_to_cpu(task_load(p),
					lowest_power_cpu), lowest_power_cpu);
	struct cpumask search_cpus;
	struct rq *rq = cpu_rq(cpu);

	/*
	 * This function should be called only when task 'p' fits in the current
	 * CPU which can be ensured by task_will_fit() prior to this.
	 */
	cpumask_and(&search_cpus, tsk_cpus_allowed(p), cpu_active_mask);
	cpumask_and(&search_cpus, &search_cpus, &rq->freq_domain_cpumask);
	cpumask_clear_cpu(lowest_power_cpu, &search_cpus);

	/* Is a lower-powered idle CPU available which will fit this task? */
	for_each_cpu(i, &search_cpus) {
		if (idle_cpu(i)) {
			int cost =
			 power_cost(scale_load_to_cpu(task_load(p), i), i);
			if (cost < lowest_power) {
				lowest_power_cpu = i;
				lowest_power = cost;
			}
		}
	}

	return (lowest_power_cpu != task_cpu(p));
}

static inline int is_cpu_throttling_imminent(int cpu);
static inline int is_task_migration_throttled(struct task_struct *p);

/*
 * Check if a task is on the "wrong" cpu (i.e its current cpu is not the ideal
 * cpu as per its demand or priority)
 *
 * Returns reason why task needs to be migrated
 */
static inline int migration_needed(struct rq *rq, struct task_struct *p)
{
	int nice = task_nice(p);
	int cpu = cpu_of(rq);
	int rc = 0;
	struct related_thread_group *grp;

	if (!sched_enable_hmp || p->state != TASK_RUNNING ||
	    p->nr_cpus_allowed == 1)
		return rc;

	/* No need to migrate task that is about to be throttled */
	if (task_will_be_throttled(p))
		return rc;

	rcu_read_lock(); /* Protected access to p->grp */
	grp = task_related_thread_group(p);
	if (sched_boost()) {
		if (nice > sched_upmigrate_min_nice) {
			if (sysctl_sched_enable_colocation &&
					grp &&
					grp->preferred_cluster->capacity == max_capacity &&
					cpu_capacity(cpu) != max_capacity) {
				rc = UP_MIGRATION;
			}
			goto done;
		}

		if (cpu_capacity(cpu) != max_capacity) {
			rc = UP_MIGRATION;
		}

		goto done;
	}

	if (!preferred_cluster(rq->cluster, p)) {
		rc = PREFERRED_CLUSTER_MIGRATION;
		goto done;
	}

	if (is_small_task(p)) {
		goto done;
	}

	if (sched_cpu_high_irqload(cpu)) {
		rc = IRQLOAD_MIGRATION;
		goto done;
	}

	if ((!sysctl_sched_enable_colocation ||
			!grp ||
			grp->preferred_cluster->capacity == min_capacity) &&
			(nice > sched_upmigrate_min_nice ||
			upmigrate_discouraged(p)) &&
			cpu_capacity(cpu_of(rq)) > min_capacity) {
		rc = DOWN_MIGRATION;
		goto done;
	}

	if (!task_will_fit(p, cpu)) {
		rc = UP_MIGRATION;
		goto done;
	}

	if (sysctl_sched_enable_power_aware &&
	    !is_task_migration_throttled(p) &&
	    is_cpu_throttling_imminent(cpu) &&
	    lower_power_cpu_available(p, cpu)) {
		rc = EA_MIGRATION;
		goto done;
	}

done:
	rcu_read_unlock();
	return rc;
}

static DEFINE_RAW_SPINLOCK(migration_lock);

static inline int
kick_active_balance(struct rq *rq, struct task_struct *p, int new_cpu)
{
	unsigned long flags;
	int rc = 0;

	/* Invoke active balance to force migrate currently running task */
	raw_spin_lock_irqsave(&rq->lock, flags);
	if (!rq->active_balance) {
		rq->active_balance = 1;
		rq->push_cpu = new_cpu;
		get_task_struct(p);
		rq->push_task = p;
		rc = 1;
	}
	raw_spin_unlock_irqrestore(&rq->lock, flags);

	return rc;
}

static bool do_migration(int reason, int new_cpu, int cpu)
{
	if ((reason == UP_MIGRATION || reason == DOWN_MIGRATION)
				&& same_cluster(new_cpu, cpu))
		return false;

	/* Inter cluster high irqload migrations are OK */
	return new_cpu != cpu;
}

/*
 * Check if currently running task should be migrated to a better cpu.
 *
 * Todo: Effect this via changes to nohz_balancer_kick() and load balance?
 */
void check_for_migration(struct rq *rq, struct task_struct *p)
{
	int cpu = cpu_of(rq), new_cpu;
	int active_balance = 0, reason;

	reason = migration_needed(rq, p);
	if (!reason)
		return;

	raw_spin_lock(&migration_lock);
	new_cpu = select_best_cpu(p, cpu, reason, 0);

	if (do_migration(reason, new_cpu, cpu)) {
		active_balance = kick_active_balance(rq, p, new_cpu);
		if (active_balance)
			mark_reserved(new_cpu);
	}

	raw_spin_unlock(&migration_lock);

	if (active_balance)
		stop_one_cpu_nowait(cpu, active_load_balance_cpu_stop, rq,
					&rq->active_balance_work);
}

static inline int nr_big_tasks(struct rq *rq)
{
	return rq->hmp_stats.nr_big_tasks;
}

static inline int is_cpu_throttling_imminent(int cpu)
{
	int throttling = 0;
	struct cpu_pwr_stats *per_cpu_info;

	if (sched_feat(FORCE_CPU_THROTTLING_IMMINENT))
		return 1;

	per_cpu_info = get_cpu_pwr_stats();
	if (per_cpu_info)
		throttling = per_cpu_info[cpu].throttling;
	return throttling;
}

static inline int is_task_migration_throttled(struct task_struct *p)
{
	u64 delta = sched_ktime_clock() - p->run_start;

	return delta < sched_min_runtime;
}

unsigned int cpu_temp(int cpu)
{
	struct cpu_pwr_stats *per_cpu_info = get_cpu_pwr_stats();
	if (per_cpu_info)
		return per_cpu_info[cpu].temp;
	else
		return 0;
}

#else	/* CONFIG_SCHED_HMP */

#define sysctl_sched_enable_power_aware 0

static inline int task_will_fit(struct task_struct *p, int cpu)
{
	return 1;
}

static inline int select_best_cpu(struct task_struct *p, int target,
				  int reason, int sync)
{
	return 0;
}

static inline int find_new_hmp_ilb(int type)
{
	return 0;
}

static inline int
spill_threshold_crossed(u64 task_load, u64 cpu_load, struct rq *rq)
{
	return 0;
}

static inline int mostly_idle_cpu(int cpu)
{
	return 0;
}

static inline int sched_boost(void)
{
	return 0;
}

static inline int is_small_task(struct task_struct *p)
{
	return 0;
}

static inline int is_big_task(struct task_struct *p)
{
	return 0;
}

static inline int nr_big_tasks(struct rq *rq)
{
	return 0;
}

static inline int is_cpu_throttling_imminent(int cpu)
{
	return 0;
}

static inline int is_task_migration_throttled(struct task_struct *p)
{
	return 0;
}

unsigned int cpu_temp(int cpu)
{
	return 0;
}

static inline void
inc_rq_hmp_stats(struct rq *rq, struct task_struct *p, int change_cra) { }
static inline void
dec_rq_hmp_stats(struct rq *rq, struct task_struct *p, int change_cra) { }

static inline void
inc_hmp_sched_stats_fair(struct rq *rq, struct task_struct *p) { }

static inline void
dec_hmp_sched_stats_fair(struct rq *rq, struct task_struct *p) { }

#define preferred_cluster(...) 1

#endif	/* CONFIG_SCHED_HMP */

#ifdef CONFIG_SCHED_HMP

void init_new_task_load(struct task_struct *p)
{
	int i;
	u32 init_load_windows = sched_init_task_load_windows;
	u32 init_load_pelt = sched_init_task_load_pelt;
	u32 init_load_pct = current->init_load_pct;

	p->init_load_pct = 0;
	memset(&p->ravg, 0, sizeof(struct ravg));
	p->grp = NULL;
	INIT_LIST_HEAD(&p->grp_list);

	if (init_load_pct) {
		init_load_pelt = div64_u64((u64)init_load_pct *
			  (u64)LOAD_AVG_MAX, 100);
		init_load_windows = div64_u64((u64)init_load_pct *
			  (u64)sched_ravg_window, 100);
	}

	p->ravg.demand = init_load_windows;
	for (i = 0; i < RAVG_HIST_SIZE_MAX; ++i)
		p->ravg.sum_history[i] = init_load_windows;

	p->se.avg.runnable_avg_sum_scaled = init_load_pelt;
}

#else /* CONFIG_SCHED_HMP */

void init_new_task_load(struct task_struct *p)
{
}

#endif /* CONFIG_SCHED_HMP */

#if (SCHED_LOAD_SHIFT - SCHED_LOAD_RESOLUTION) != 10 || SCHED_CAPACITY_SHIFT != 10
#error "load tracking assumes 2^10 as unit"
#endif

#define cap_scale(v, s) ((v)*(s) >> SCHED_CAPACITY_SHIFT)

/*
 * We can represent the historical contribution to runnable average as the
 * coefficients of a geometric series.  To do this we sub-divide our runnable
 * history into segments of approximately 1ms (1024us); label the segment that
 * occurred N-ms ago p_N, with p_0 corresponding to the current period, e.g.
 *
 * [<- 1024us ->|<- 1024us ->|<- 1024us ->| ...
 *      p0            p1           p2
 *     (now)       (~1ms ago)  (~2ms ago)
 *
 * Let u_i denote the fraction of p_i that the entity was runnable.
 *
 * We then designate the fractions u_i as our co-efficients, yielding the
 * following representation of historical load:
 *   u_0 + u_1*y + u_2*y^2 + u_3*y^3 + ...
 *
 * We choose y based on the with of a reasonably scheduling period, fixing:
 *   y^32 = 0.5
 *
 * This means that the contribution to load ~32ms ago (u_32) will be weighted
 * approximately half as much as the contribution to load within the last ms
 * (u_0).
 *
 * When a period "rolls over" and we have new u_0`, multiplying the previous
 * sum again by y is sufficient to update:
 *   load_avg = u_0` + y*(u_0 + u_1*y + u_2*y^2 + ... )
 *            = u_0 + u_1*y + u_2*y^2 + ... [re-labeling u_i --> u_{i+1}]
 */
static __always_inline int
__update_load_avg(u64 now, int cpu, struct sched_avg *sa,
		  unsigned long weight, int running, struct cfs_rq *cfs_rq)
{
	u64 delta, scaled_delta, periods;
	u32 contrib;
	unsigned int delta_w, scaled_delta_w, decayed = 0;
	unsigned long scale_freq, scale_cpu;
	struct sched_entity *se = NULL;

	delta = now - sa->last_update_time;
	/*
	 * This should only happen when time goes backwards, which it
	 * unfortunately does during sched clock init when we swap over to TSC.
	 */
	if ((s64)delta < 0) {
		sa->last_update_time = now;
		return 0;
	}

	/*
	 * Use 1024ns as the unit of measurement since it's a reasonable
	 * approximation of 1us and fast to compute.
	 */
	delta >>= 10;
	if (!delta)
		return 0;
	sa->last_update_time = now;

	if (sched_use_pelt && !cfs_rq && weight) {
		se = container_of(sa, struct sched_entity, avg);
		if (entity_is_task(se) && se->on_rq)
			dec_hmp_sched_stats_fair(rq_of(cfs_rq), task_of(se));
	}

	scale_freq = arch_scale_freq_capacity(NULL, cpu);
	scale_cpu = arch_scale_cpu_capacity(NULL, cpu);

	/* delta_w is the amount already accumulated against our next period */
	delta_w = sa->period_contrib;
	if (delta + delta_w >= 1024) {
		decayed = 1;

		/* how much left for next period will start over, we don't know yet */
		sa->period_contrib = 0;
        
		/*
		 * Now that we know we're crossing a period boundary, figure
		 * out how much from delta we need to complete the current
		 * period and accrue it.
		 */
		delta_w = 1024 - delta_w;
		scaled_delta_w = cap_scale(delta_w, scale_freq);
		if (weight) {
			sa->load_sum += weight * scaled_delta_w;
			add_to_scaled_stat(cpu, sa, delta_w);
			if (cfs_rq) {
				cfs_rq->runnable_load_sum +=
						weight * scaled_delta_w;
			}
		}

		if (running)
			sa->util_sum += scaled_delta_w * scale_cpu;

		delta -= delta_w;

		/* Figure out how many additional periods this update spans */
		periods = delta / 1024;
		delta %= 1024;

		sa->load_sum = decay_load(sa->load_sum, periods + 1);
		if (cfs_rq) {
			cfs_rq->runnable_load_sum =
				decay_load(cfs_rq->runnable_load_sum, periods + 1);
		}
		sa->util_sum = decay_load((u64)(sa->util_sum), periods + 1);
		decay_scaled_stat(sa, periods + 1);

		/* Efficiently calculate \sum (1..n_period) 1024*y^i */
		contrib = __compute_runnable_contrib(periods);
		contrib = cap_scale(contrib, scale_freq);
		if (weight) {
			sa->load_sum += weight * contrib;
			add_to_scaled_stat(cpu, sa, contrib);
			if (cfs_rq) {
				cfs_rq->runnable_load_sum += weight * contrib;
			}
		}
		if (running)
			sa->util_sum += contrib * scale_cpu;
	}

	/* Remainder of delta accrued against u_0` */
	scaled_delta = cap_scale(delta, scale_freq);
	if (weight) {
		sa->load_sum += weight * scaled_delta;
		add_to_scaled_stat(cpu, sa, delta);
		if (cfs_rq) {
			cfs_rq->runnable_load_sum += weight * scaled_delta;
		}
	}

	if (se && entity_is_task(se) && se->on_rq)
		inc_hmp_sched_stats_fair(rq_of(cfs_rq), task_of(se));

	if (running)
		sa->util_sum += scaled_delta * scale_cpu;

	sa->period_contrib += delta;

	if (decayed) {
		sa->load_avg = div_u64(sa->load_sum, LOAD_AVG_MAX);
		if (cfs_rq) {
			cfs_rq->runnable_load_avg =
				div_u64(cfs_rq->runnable_load_sum, LOAD_AVG_MAX);
		}
		sa->util_avg = sa->util_sum / LOAD_AVG_MAX;
	}

	return decayed;
}

#ifdef CONFIG_FAIR_GROUP_SCHED
/*
 * Updating tg's load_avg is necessary before update_cfs_share (which is done)
 * and effective_load (which is not done because it is too costly).
 */
static inline void update_tg_load_avg(struct cfs_rq *cfs_rq, int force)
{
	long delta = cfs_rq->avg.load_avg - cfs_rq->tg_load_avg_contrib;

	/*
	 * No need to update load_avg for root_task_group as it is not used.
	 */
	if (cfs_rq->tg == &root_task_group)
		return;

	if (force || abs(delta) > cfs_rq->tg_load_avg_contrib / 64) {
		atomic_long_add(delta, &cfs_rq->tg->load_avg);
		cfs_rq->tg_load_avg_contrib = cfs_rq->avg.load_avg;
	}
}

/*
 * Called within set_task_rq() right before setting a task's cpu. The
 * caller only guarantees p->pi_lock is held; no other assumptions,
 * including the state of rq->lock, should be made.
 */
void set_task_rq_fair(struct sched_entity *se,
		      struct cfs_rq *prev, struct cfs_rq *next)
{
	if (!sched_feat(ATTACH_AGE_LOAD))
		return;

	/*
	 * We are supposed to update the task to "current" time, then its up to
	 * date and ready to go to new CPU/cfs_rq. But we have difficulty in
	 * getting what current time is, so simply throw away the out-of-date
	 * time. This will result in the wakee task is less decayed, but giving
	 * the wakee more load sounds not bad.
	 */
	if (se->avg.last_update_time && prev) {
		u64 p_last_update_time;
		u64 n_last_update_time;

#ifndef CONFIG_64BIT
		u64 p_last_update_time_copy;
		u64 n_last_update_time_copy;

		do {
			p_last_update_time_copy = prev->load_last_update_time_copy;
			n_last_update_time_copy = next->load_last_update_time_copy;

			smp_rmb();

			p_last_update_time = prev->avg.last_update_time;
			n_last_update_time = next->avg.last_update_time;

		} while (p_last_update_time != p_last_update_time_copy ||
			 n_last_update_time != n_last_update_time_copy);
#else
		p_last_update_time = prev->avg.last_update_time;
		n_last_update_time = next->avg.last_update_time;
#endif
		__update_load_avg(p_last_update_time, cpu_of(rq_of(prev)),
				  &se->avg, 0, 0, NULL);
		se->avg.last_update_time = n_last_update_time;
	}
}
#else /* CONFIG_FAIR_GROUP_SCHED */
static inline void update_tg_load_avg(struct cfs_rq *cfs_rq, int force) {}
#endif /* CONFIG_FAIR_GROUP_SCHED */

static inline u64 cfs_rq_clock_task(struct cfs_rq *cfs_rq);

/*
 * Unsigned subtract and clamp on underflow.
 *
 * Explicitly do a load-store to ensure the intermediate value never hits
 * memory. This allows lockless observations without ever seeing the negative
 * values.
 */
#define sub_positive(_ptr, _val) do {				\
	typeof(_ptr) ptr = (_ptr);				\
	typeof(*ptr) val = (_val);				\
	typeof(*ptr) res, var = READ_ONCE(*ptr);		\
	res = var - val;					\
	if (res > var)						\
		res = 0;					\
	WRITE_ONCE(*ptr, res);					\
} while (0)

/* Group cfs_rq's load_avg is used for task_h_load and update_cfs_share */
static inline int update_cfs_rq_load_avg(u64 now, struct cfs_rq *cfs_rq)
{
	struct sched_avg *sa = &cfs_rq->avg;
	int decayed, removed = 0;

	if (atomic_long_read(&cfs_rq->removed_load_avg)) {
		long r = atomic_long_xchg(&cfs_rq->removed_load_avg, 0);
		sub_positive(&sa->load_avg, r);
		sub_positive(&sa->load_sum, r * LOAD_AVG_MAX);
		removed = 1;
	}

	if (atomic_long_read(&cfs_rq->removed_util_avg)) {
		long r = atomic_long_xchg(&cfs_rq->removed_util_avg, 0);
		sub_positive(&sa->util_avg, r);
		sub_positive(&sa->util_sum, r * LOAD_AVG_MAX);
	}

	decayed = __update_load_avg(now, cpu_of(rq_of(cfs_rq)), sa,
		scale_load_down(cfs_rq->load.weight), cfs_rq->curr != NULL, cfs_rq);

#ifndef CONFIG_64BIT
	smp_wmb();
	cfs_rq->load_last_update_time_copy = sa->last_update_time;
#endif

	return decayed || removed;
}

/* Update task and its cfs_rq load average */
static inline void update_load_avg(struct sched_entity *se, int update_tg)
{
	struct cfs_rq *cfs_rq = cfs_rq_of(se);
	u64 now = cfs_rq_clock_task(cfs_rq);
	int cpu = cpu_of(rq_of(cfs_rq));

	/*
	 * Track task load average for carrying it to new CPU after migrated, and
	 * track group sched_entity load average for task_h_load calc in migration
	 */
	__update_load_avg(now, cpu, &se->avg,
			  se->on_rq * scale_load_down(se->load.weight),
			  cfs_rq->curr == se, NULL);

	if (update_cfs_rq_load_avg(now, cfs_rq) && update_tg)
		update_tg_load_avg(cfs_rq, 0);
}

static void attach_entity_load_avg(struct cfs_rq *cfs_rq, struct sched_entity *se)
{
	if (!sched_feat(ATTACH_AGE_LOAD))
		goto skip_aging;

	/*
	 * If we got migrated (either between CPUs or between cgroups) we'll
	 * have aged the average right before clearing @last_update_time.
	 */
	if (se->avg.last_update_time) {
		__update_load_avg(cfs_rq->avg.last_update_time, cpu_of(rq_of(cfs_rq)),
				  &se->avg, 0, 0, NULL);

		/*
		 * XXX: we could have just aged the entire load away if we've been
		 * absent from the fair class for too long.
		 */
	}

skip_aging:
	se->avg.last_update_time = cfs_rq->avg.last_update_time;
	cfs_rq->avg.load_avg += se->avg.load_avg;
	cfs_rq->avg.load_sum += se->avg.load_sum;
	cfs_rq->avg.util_avg += se->avg.util_avg;
	cfs_rq->avg.util_sum += se->avg.util_sum;
}

static void detach_entity_load_avg(struct cfs_rq *cfs_rq, struct sched_entity *se)
{
	__update_load_avg(cfs_rq->avg.last_update_time, cpu_of(rq_of(cfs_rq)),
			  &se->avg, se->on_rq * scale_load_down(se->load.weight),
			  cfs_rq->curr == se, NULL);

	sub_positive(&cfs_rq->avg.load_avg, se->avg.load_avg);
	sub_positive(&cfs_rq->avg.load_sum, se->avg.load_sum);
	sub_positive(&cfs_rq->avg.util_avg, se->avg.util_avg);
	sub_positive(&cfs_rq->avg.util_sum, se->avg.util_sum);
}

/* Add the load generated by se into cfs_rq's load average */
static inline void
enqueue_entity_load_avg(struct cfs_rq *cfs_rq, struct sched_entity *se)
{
	struct sched_avg *sa = &se->avg;
	u64 now = cfs_rq_clock_task(cfs_rq);
	int migrated, decayed;

	migrated = !sa->last_update_time;
	if (!migrated) {
		__update_load_avg(now, cpu_of(rq_of(cfs_rq)), sa,
			se->on_rq * scale_load_down(se->load.weight),
			cfs_rq->curr == se, NULL);
	}

	decayed = update_cfs_rq_load_avg(now, cfs_rq);

	cfs_rq->runnable_load_avg += sa->load_avg;
	cfs_rq->runnable_load_sum += sa->load_sum;

	if (migrated)
		attach_entity_load_avg(cfs_rq, se);

	if (decayed || migrated)
		update_tg_load_avg(cfs_rq, 0);
}

/* Remove the runnable load generated by se from cfs_rq's runnable load average */
static inline void
dequeue_entity_load_avg(struct cfs_rq *cfs_rq, struct sched_entity *se)
{
	update_load_avg(se, 1);

	cfs_rq->runnable_load_avg =
		max_t(long, cfs_rq->runnable_load_avg - se->avg.load_avg, 0);
	cfs_rq->runnable_load_sum =
		max_t(s64,  cfs_rq->runnable_load_sum - se->avg.load_sum, 0);
}

#ifndef CONFIG_64BIT
static inline u64 cfs_rq_last_update_time(struct cfs_rq *cfs_rq)
{
	u64 last_update_time_copy;
	u64 last_update_time;

	do {
		last_update_time_copy = cfs_rq->load_last_update_time_copy;
		smp_rmb();
		last_update_time = cfs_rq->avg.last_update_time;
	} while (last_update_time != last_update_time_copy);

	return last_update_time;
}
#else
static inline u64 cfs_rq_last_update_time(struct cfs_rq *cfs_rq)
{
	return cfs_rq->avg.last_update_time;
}
#endif

/*
 * Task first catches up with cfs_rq, and then subtract
 * itself from the cfs_rq (task must be off the queue now).
 */
void remove_entity_load_avg(struct sched_entity *se)
{
	struct cfs_rq *cfs_rq = cfs_rq_of(se);
	u64 last_update_time;

	/*
	 * Newly created task or never used group entity should not be removed
	 * from its (source) cfs_rq
	 */
	if (se->avg.last_update_time == 0)
		return;

	last_update_time = cfs_rq_last_update_time(cfs_rq);

	__update_load_avg(last_update_time, cpu_of(rq_of(cfs_rq)), &se->avg, 0, 0, NULL);
	atomic_long_add(se->avg.load_avg, &cfs_rq->removed_load_avg);
	atomic_long_add(se->avg.util_avg, &cfs_rq->removed_util_avg);
}

/*
 * Update the rq's load with the elapsed running time before entering
 * idle. if the last scheduled task is not a CFS task, idle_enter will
 * be the only way to update the runnable statistic.
 */
void idle_enter_fair(struct rq *this_rq)
{
}

/*
 * Update the rq's load with the elapsed idle time before a task is
 * scheduled. if the newly scheduled task is not a CFS task, idle_exit will
 * be the only way to update the runnable statistic.
 */
void idle_exit_fair(struct rq *this_rq)
{
}

static inline unsigned long cfs_rq_runnable_load_avg(struct cfs_rq *cfs_rq)
{
	return cfs_rq->runnable_load_avg;
}

static inline unsigned long cfs_rq_load_avg(struct cfs_rq *cfs_rq)
{
	return cfs_rq->avg.load_avg;
}

static int idle_balance(struct rq *this_rq);

#else /* CONFIG_SMP */

static inline void update_load_avg(struct sched_entity *se, int update_tg) {}
static inline void
enqueue_entity_load_avg(struct cfs_rq *cfs_rq, struct sched_entity *se) {}
static inline void
dequeue_entity_load_avg(struct cfs_rq *cfs_rq, struct sched_entity *se) {}
static inline void remove_entity_load_avg(struct sched_entity *se) {}

static inline void
attach_entity_load_avg(struct cfs_rq *cfs_rq, struct sched_entity *se) {}
static inline void
detach_entity_load_avg(struct cfs_rq *cfs_rq, struct sched_entity *se) {}

static inline int idle_balance(struct rq *rq)
{
	return 0;
}

static inline void
inc_rq_hmp_stats(struct rq *rq, struct task_struct *p, int change_cra) { }
static inline void
dec_rq_hmp_stats(struct rq *rq, struct task_struct *p, int change_cra) { }

void init_new_task_load(struct task_struct *p)
{
}

#endif /* CONFIG_SMP */

#ifdef CONFIG_SCHED_HMP

/* Return task demand in percentage scale */
unsigned int pct_task_load(struct task_struct *p)
{
	unsigned int load;

	load = div64_u64((u64)task_load(p) * 100, (u64)max_task_load());

	return load;
}

/*
 * Add scaled version of 'delta' to runnable_avg_sum_scaled
 * 'delta' is scaled in reference to "best" cpu
 */
static inline void
add_to_scaled_stat(int cpu, struct sched_avg *sa, u64 delta)
{
	int cur_freq = cpu_cur_freq(cpu);
	u64 scaled_delta;
	int sf;

	if (!sched_enable_hmp)
		return;

	if (unlikely(cur_freq > max_possible_freq))
		cur_freq = max_possible_freq;

	scaled_delta = div64_u64(delta * cur_freq, max_possible_freq);
	sf = (cpu_efficiency(cpu) * 1024) / max_possible_efficiency;
	scaled_delta *= sf;
	scaled_delta >>= 10;
	sa->runnable_avg_sum_scaled += scaled_delta;
}

static inline void decay_scaled_stat(struct sched_avg *sa, u64 periods)
{
	if (!sched_enable_hmp)
		return;

	sa->runnable_avg_sum_scaled =
		decay_load(sa->runnable_avg_sum_scaled,
			   periods);
}

#ifdef CONFIG_CFS_BANDWIDTH

static void init_cfs_rq_hmp_stats(struct cfs_rq *cfs_rq)
{
	cfs_rq->hmp_stats.nr_big_tasks = 0;
	cfs_rq->hmp_stats.nr_small_tasks = 0;
	cfs_rq->hmp_stats.cumulative_runnable_avg = 0;
}

static void inc_cfs_rq_hmp_stats(struct cfs_rq *cfs_rq,
		 struct task_struct *p, int change_cra)
{
	inc_nr_big_small_task(&cfs_rq->hmp_stats, p);
	if (change_cra)
		inc_cumulative_runnable_avg(&cfs_rq->hmp_stats, p);
}

static void dec_cfs_rq_hmp_stats(struct cfs_rq *cfs_rq,
		 struct task_struct *p, int change_cra)
{
	dec_nr_big_small_task(&cfs_rq->hmp_stats, p);
	if (change_cra)
		dec_cumulative_runnable_avg(&cfs_rq->hmp_stats, p);
}

static void inc_throttled_cfs_rq_hmp_stats(struct hmp_sched_stats *stats,
			 struct cfs_rq *cfs_rq)
{
	stats->nr_big_tasks += cfs_rq->hmp_stats.nr_big_tasks;
	stats->nr_small_tasks += cfs_rq->hmp_stats.nr_small_tasks;
	stats->cumulative_runnable_avg +=
				cfs_rq->hmp_stats.cumulative_runnable_avg;
}

static void dec_throttled_cfs_rq_hmp_stats(struct hmp_sched_stats *stats,
				 struct cfs_rq *cfs_rq)
{
	stats->nr_big_tasks -= cfs_rq->hmp_stats.nr_big_tasks;
	stats->nr_small_tasks -= cfs_rq->hmp_stats.nr_small_tasks;
	stats->cumulative_runnable_avg -=
				cfs_rq->hmp_stats.cumulative_runnable_avg;

	BUG_ON(stats->nr_big_tasks < 0 || stats->nr_small_tasks < 0 ||
		(s64)stats->cumulative_runnable_avg < 0);
}

#else	/* CONFIG_CFS_BANDWIDTH */

static inline void inc_cfs_rq_hmp_stats(struct cfs_rq *cfs_rq,
	 struct task_struct *p, int change_cra) { }

static inline void dec_cfs_rq_hmp_stats(struct cfs_rq *cfs_rq,
	 struct task_struct *p, int change_cra) { }

#endif	/* CONFIG_CFS_BANDWIDTH */

#else  /* CONFIG_SCHED_HMP */

static inline void
add_to_scaled_stat(int cpu, struct sched_avg *sa, u64 delta)
{
}

static inline void decay_scaled_stat(struct sched_avg *sa, u64 periods)
{
}

static inline void init_cfs_rq_hmp_stats(struct cfs_rq *cfs_rq) { }

static inline void inc_cfs_rq_hmp_stats(struct cfs_rq *cfs_rq,
	 struct task_struct *p, int change_cra) { }

static inline void dec_cfs_rq_hmp_stats(struct cfs_rq *cfs_rq,
	 struct task_struct *p, int change_cra) { }

static inline void inc_throttled_cfs_rq_hmp_stats(struct hmp_sched_stats *stats,
			 struct cfs_rq *cfs_rq)
{
}

static inline void dec_throttled_cfs_rq_hmp_stats(struct hmp_sched_stats *stats,
			 struct cfs_rq *cfs_rq)
{
}

#endif /* CONFIG_SCHED_HMP */

static void enqueue_sleeper(struct cfs_rq *cfs_rq, struct sched_entity *se)
{
#ifdef CONFIG_SCHEDSTATS
	struct task_struct *tsk = NULL;

	if (entity_is_task(se))
		tsk = task_of(se);

	if (se->statistics.sleep_start) {
		u64 delta = rq_clock(rq_of(cfs_rq)) - se->statistics.sleep_start;

		if ((s64)delta < 0)
			delta = 0;

		if (unlikely(delta > se->statistics.sleep_max))
			se->statistics.sleep_max = delta;

		se->statistics.sleep_start = 0;
		se->statistics.sum_sleep_runtime += delta;

		if (tsk) {
			account_scheduler_latency(tsk, delta >> 10, 1);
			trace_sched_stat_sleep(tsk, delta);
		}
	}
	if (se->statistics.block_start) {
		u64 delta = rq_clock(rq_of(cfs_rq)) - se->statistics.block_start;

		if ((s64)delta < 0)
			delta = 0;

		if (unlikely(delta > se->statistics.block_max))
			se->statistics.block_max = delta;

		se->statistics.block_start = 0;
		se->statistics.sum_sleep_runtime += delta;

		if (tsk) {
			if (tsk->in_iowait) {
				se->statistics.iowait_sum += delta;
				se->statistics.iowait_count++;
				trace_sched_stat_iowait(tsk, delta);
			}

			trace_sched_stat_blocked(tsk, delta);
			trace_sched_blocked_reason(tsk);       

			/*
			 * Blocking time is in units of nanosecs, so shift by
			 * 20 to get a milliseconds-range estimation of the
			 * amount of time that the task spent sleeping:
			 */
			if (unlikely(prof_on == SLEEP_PROFILING)) {
				profile_hits(SLEEP_PROFILING,
						(void *)get_wchan(tsk),
						delta >> 20);
			}
			account_scheduler_latency(tsk, delta >> 10, 0);
		}
	}
#endif
}

static void check_spread(struct cfs_rq *cfs_rq, struct sched_entity *se)
{
#ifdef CONFIG_SCHED_DEBUG
	s64 d = se->vruntime - cfs_rq->min_vruntime;

	if (d < 0)
		d = -d;

	if (d > 3*sysctl_sched_latency)
		schedstat_inc(cfs_rq, nr_spread_over);
#endif
}

static void
place_entity(struct cfs_rq *cfs_rq, struct sched_entity *se, int initial)
{
	u64 vruntime = cfs_rq->min_vruntime;

	/*
	 * The 'current' period is already promised to the current tasks,
	 * however the extra weight of the new task will slow them down a
	 * little, place the new task so that it fits in the slot that
	 * stays open at the end.
	 */
	if (initial && sched_feat(START_DEBIT))
		vruntime += sched_vslice(cfs_rq, se);

	/* sleeps up to a single latency don't count. */
	if (!initial) {
		unsigned long thresh = sysctl_sched_latency;

		/*
		 * Halve their sleep time's effect, to allow
		 * for a gentler effect of sleepers:
		 */
		if (sched_feat(GENTLE_FAIR_SLEEPERS))
			thresh >>= 1;

		vruntime -= thresh;
	}

	/* ensure we never gain time by being placed backwards. */
	se->vruntime = max_vruntime(se->vruntime, vruntime);
}

static void check_enqueue_throttle(struct cfs_rq *cfs_rq);

static void
enqueue_entity(struct cfs_rq *cfs_rq, struct sched_entity *se, int flags)
{
	/*
	 * Update the normalized vruntime before updating min_vruntime
	 * through calling update_curr().
	 */
	if (!(flags & ENQUEUE_WAKEUP) || (flags & ENQUEUE_WAKING))
		se->vruntime += cfs_rq->min_vruntime;

	/*
	 * Update run-time statistics of the 'current'.
	 */
	update_curr(cfs_rq);
	enqueue_entity_load_avg(cfs_rq, se);
	account_entity_enqueue(cfs_rq, se);
	update_cfs_shares(cfs_rq);

	if (flags & ENQUEUE_WAKEUP) {
		place_entity(cfs_rq, se, 0);
		enqueue_sleeper(cfs_rq, se);
	}

	update_stats_enqueue(cfs_rq, se, !!(flags & ENQUEUE_MIGRATING));
	check_spread(cfs_rq, se);

#ifdef CONFIG_FAIR_GROUP_SCHED
	/*
	 * Update depth before it can be picked as next sched entity.
	 */
	se->depth = se->parent ? se->parent->depth + 1 : 0;
#endif

	if (se != cfs_rq->curr)
		__enqueue_entity(cfs_rq, se);
	se->on_rq = 1;

	if (cfs_rq->nr_running == 1) {
		list_add_leaf_cfs_rq(cfs_rq);
		check_enqueue_throttle(cfs_rq);
	}
}

static void __clear_buddies_last(struct sched_entity *se)
{
	for_each_sched_entity(se) {
		struct cfs_rq *cfs_rq = cfs_rq_of(se);
		if (cfs_rq->last != se)
			break;

		cfs_rq->last = NULL;
	}
}

static void __clear_buddies_next(struct sched_entity *se)
{
	for_each_sched_entity(se) {
		struct cfs_rq *cfs_rq = cfs_rq_of(se);
		if (cfs_rq->next != se)
			break;

		cfs_rq->next = NULL;
	}
}

static void __clear_buddies_skip(struct sched_entity *se)
{
	for_each_sched_entity(se) {
		struct cfs_rq *cfs_rq = cfs_rq_of(se);
		if (cfs_rq->skip != se)
			break;

		cfs_rq->skip = NULL;
	}
}

static void clear_buddies(struct cfs_rq *cfs_rq, struct sched_entity *se)
{
	if (cfs_rq->last == se)
		__clear_buddies_last(se);

	if (cfs_rq->next == se)
		__clear_buddies_next(se);

	if (cfs_rq->skip == se)
		__clear_buddies_skip(se);
}

static __always_inline void return_cfs_rq_runtime(struct cfs_rq *cfs_rq);

static void
dequeue_entity(struct cfs_rq *cfs_rq, struct sched_entity *se, int flags)
{
	/*
	 * Update run-time statistics of the 'current'.
	 */
	update_curr(cfs_rq);
	dequeue_entity_load_avg(cfs_rq, se);

	update_stats_dequeue(cfs_rq, se, !!(flags & DEQUEUE_MIGRATING));
	if (flags & DEQUEUE_SLEEP) {
#ifdef CONFIG_SCHEDSTATS
		if (entity_is_task(se)) {
			struct task_struct *tsk = task_of(se);

			if (tsk->state & TASK_INTERRUPTIBLE)
				se->statistics.sleep_start = rq_clock(rq_of(cfs_rq));
			if (tsk->state & TASK_UNINTERRUPTIBLE)
				se->statistics.block_start = rq_clock(rq_of(cfs_rq));
		}
#endif
	}

	clear_buddies(cfs_rq, se);

	if (se != cfs_rq->curr)
		__dequeue_entity(cfs_rq, se);
	se->on_rq = 0;
	account_entity_dequeue(cfs_rq, se);

	/*
	 * Normalize the entity after updating the min_vruntime because the
	 * update can refer to the ->curr item and we need to reflect this
	 * movement in our normalized position.
	 */
	if (!(flags & DEQUEUE_SLEEP))
		se->vruntime -= cfs_rq->min_vruntime;

	/* return excess runtime on last dequeue */
	return_cfs_rq_runtime(cfs_rq);

	update_min_vruntime(cfs_rq);
	update_cfs_shares(cfs_rq);
}

/*
 * Preempt the current task with a newly woken task if needed:
 */
static void
check_preempt_tick(struct cfs_rq *cfs_rq, struct sched_entity *curr)
{
	unsigned long ideal_runtime, delta_exec;
	struct sched_entity *se;
	s64 delta;

	ideal_runtime = sched_slice(cfs_rq, curr);
	delta_exec = curr->sum_exec_runtime - curr->prev_sum_exec_runtime;
	if (delta_exec > ideal_runtime) {
		resched_curr(rq_of(cfs_rq));
		/*
		 * The current task ran long enough, ensure it doesn't get
		 * re-elected due to buddy favours.
		 */
		clear_buddies(cfs_rq, curr);
		return;
	}

	/*
	 * Ensure that a task that missed wakeup preemption by a
	 * narrow margin doesn't have to wait for a full slice.
	 * This also mitigates buddy induced latencies under load.
	 */
	if (delta_exec < sysctl_sched_min_granularity)
		return;

	se = __pick_first_entity(cfs_rq);
	delta = curr->vruntime - se->vruntime;

	if (delta < 0)
		return;

	if (delta > ideal_runtime)
		resched_curr(rq_of(cfs_rq));
}

static void
set_next_entity(struct cfs_rq *cfs_rq, struct sched_entity *se)
{
	/* 'current' is not kept within the tree. */
	if (se->on_rq) {
		/*
		 * Any task has to be enqueued before it get to execute on
		 * a CPU. So account for the time it spent waiting on the
		 * runqueue.
		 */
		update_stats_wait_end(cfs_rq, se, false);
		__dequeue_entity(cfs_rq, se);
		update_load_avg(se, 1);
	}

	update_stats_curr_start(cfs_rq, se);
	cfs_rq->curr = se;
#ifdef CONFIG_SCHEDSTATS
	/*
	 * Track our maximum slice length, if the CPU's load is at
	 * least twice that of our own weight (i.e. dont track it
	 * when there are only lesser-weight tasks around):
	 */
	if (rq_of(cfs_rq)->load.weight >= 2*se->load.weight) {
		se->statistics.slice_max = max(se->statistics.slice_max,
			se->sum_exec_runtime - se->prev_sum_exec_runtime);
	}
#endif
	se->prev_sum_exec_runtime = se->sum_exec_runtime;
}

static int
wakeup_preempt_entity(struct sched_entity *curr, struct sched_entity *se);

/*
 * Pick the next process, keeping these things in mind, in this order:
 * 1) keep things fair between processes/task groups
 * 2) pick the "next" process, since someone really wants that to run
 * 3) pick the "last" process, for cache locality
 * 4) do not run the "skip" process, if something else is available
 */
static struct sched_entity *
pick_next_entity(struct cfs_rq *cfs_rq, struct sched_entity *curr)
{
	struct sched_entity *left = __pick_first_entity(cfs_rq);
	struct sched_entity *se;

	/*
	 * If curr is set we have to see if its left of the leftmost entity
	 * still in the tree, provided there was anything in the tree at all.
	 */
	if (!left || (curr && entity_before(curr, left)))
		left = curr;

	se = left; /* ideally we run the leftmost entity */

	/*
	 * Avoid running the skip buddy, if running something else can
	 * be done without getting too unfair.
	 */
	if (cfs_rq->skip == se) {
		struct sched_entity *second;

		if (se == curr) {
			second = __pick_first_entity(cfs_rq);
		} else {
			second = __pick_next_entity(se);
			if (!second || (curr && entity_before(curr, second)))
				second = curr;
		}

		if (second && (sched_feat(STRICT_SKIP_BUDDY) ||
		    wakeup_preempt_entity(second, left) < 1))
			se = second;
	}

	/*
	 * Prefer last buddy, try to return the CPU to a preempted task.
	 */
	if (cfs_rq->last && wakeup_preempt_entity(cfs_rq->last, left) < 1)
		se = cfs_rq->last;

	/*
	 * Someone really wants this to run. If it's not unfair, run it.
	 */
	if (cfs_rq->next && wakeup_preempt_entity(cfs_rq->next, left) < 1)
		se = cfs_rq->next;

	clear_buddies(cfs_rq, se);

	return se;
}

static bool check_cfs_rq_runtime(struct cfs_rq *cfs_rq);

static void put_prev_entity(struct cfs_rq *cfs_rq, struct sched_entity *prev)
{
	/*
	 * If still on the runqueue then deactivate_task()
	 * was not called and update_curr() has to be done:
	 */
	if (prev->on_rq)
		update_curr(cfs_rq);

	/* throttle cfs_rqs exceeding runtime */
	check_cfs_rq_runtime(cfs_rq);

	check_spread(cfs_rq, prev);
	if (prev->on_rq) {
		update_stats_wait_start(cfs_rq, prev, false);
		/* Put 'current' back into the tree. */
		__enqueue_entity(cfs_rq, prev);
		/* in !on_rq case, update occurred at dequeue */
		update_load_avg(prev, 0);
	}
	cfs_rq->curr = NULL;
}

static void
entity_tick(struct cfs_rq *cfs_rq, struct sched_entity *curr, int queued)
{
	/*
	 * Update run-time statistics of the 'current'.
	 */
	update_curr(cfs_rq);

	/*
	 * Ensure that runnable average is periodically updated.
	 */
	update_load_avg(curr, 1);
	update_cfs_shares(cfs_rq);

#ifdef CONFIG_SCHED_HRTICK
	/*
	 * queued ticks are scheduled to match the slice, so don't bother
	 * validating it and just reschedule.
	 */
	if (queued) {
		resched_curr(rq_of(cfs_rq));
		return;
	}
	/*
	 * don't let the period tick interfere with the hrtick preemption
	 */
	if (!sched_feat(DOUBLE_TICK) &&
			hrtimer_active(&rq_of(cfs_rq)->hrtick_timer))
		return;
#endif

	if (cfs_rq->nr_running > 1)
		check_preempt_tick(cfs_rq, curr);
}


/**************************************************
 * CFS bandwidth control machinery
 */

#ifdef CONFIG_CFS_BANDWIDTH

#ifdef HAVE_JUMP_LABEL
static struct static_key __cfs_bandwidth_used;

static inline bool cfs_bandwidth_used(void)
{
	return static_key_false(&__cfs_bandwidth_used);
}

void cfs_bandwidth_usage_inc(void)
{
	static_key_slow_inc(&__cfs_bandwidth_used);
}

void cfs_bandwidth_usage_dec(void)
{
	static_key_slow_dec(&__cfs_bandwidth_used);
}
#else /* HAVE_JUMP_LABEL */
static bool cfs_bandwidth_used(void)
{
	return true;
}

void cfs_bandwidth_usage_inc(void) {}
void cfs_bandwidth_usage_dec(void) {}
#endif /* HAVE_JUMP_LABEL */

/*
 * default period for cfs group bandwidth.
 * default: 0.1s, units: nanoseconds
 */
static inline u64 default_cfs_period(void)
{
	return 100000000ULL;
}

static inline u64 sched_cfs_bandwidth_slice(void)
{
	return (u64)sysctl_sched_cfs_bandwidth_slice * NSEC_PER_USEC;
}

/*
 * Replenish runtime according to assigned quota and update expiration time.
 * We use sched_clock_cpu directly instead of rq->clock to avoid adding
 * additional synchronization around rq->lock.
 *
 * requires cfs_b->lock
 */
void __refill_cfs_bandwidth_runtime(struct cfs_bandwidth *cfs_b)
{
	u64 now;

	if (cfs_b->quota == RUNTIME_INF)
		return;

	now = sched_clock_cpu(smp_processor_id());
	cfs_b->runtime = cfs_b->quota;
	cfs_b->runtime_expires = now + ktime_to_ns(cfs_b->period);
}

static inline struct cfs_bandwidth *tg_cfs_bandwidth(struct task_group *tg)
{
	return &tg->cfs_bandwidth;
}

/* rq->task_clock normalized against any time this cfs_rq has spent throttled */
static inline u64 cfs_rq_clock_task(struct cfs_rq *cfs_rq)
{
	if (unlikely(cfs_rq->throttle_count))
		return cfs_rq->throttled_clock_task;

	return rq_clock_task(rq_of(cfs_rq)) - cfs_rq->throttled_clock_task_time;
}

/* returns 0 on failure to allocate runtime */
static int assign_cfs_rq_runtime(struct cfs_rq *cfs_rq)
{
	struct task_group *tg = cfs_rq->tg;
	struct cfs_bandwidth *cfs_b = tg_cfs_bandwidth(tg);
	u64 amount = 0, min_amount, expires;

	/* note: this is a positive sum as runtime_remaining <= 0 */
	min_amount = sched_cfs_bandwidth_slice() - cfs_rq->runtime_remaining;

	raw_spin_lock(&cfs_b->lock);
	if (cfs_b->quota == RUNTIME_INF)
		amount = min_amount;
	else {
		/*
		 * If the bandwidth pool has become inactive, then at least one
		 * period must have elapsed since the last consumption.
		 * Refresh the global state and ensure bandwidth timer becomes
		 * active.
		 */
		if (!cfs_b->timer_active) {
			__refill_cfs_bandwidth_runtime(cfs_b);
			__start_cfs_bandwidth(cfs_b, false);
		}

		if (cfs_b->runtime > 0) {
			amount = min(cfs_b->runtime, min_amount);
			cfs_b->runtime -= amount;
			cfs_b->idle = 0;
		}
	}
	expires = cfs_b->runtime_expires;
	raw_spin_unlock(&cfs_b->lock);

	cfs_rq->runtime_remaining += amount;
	/*
	 * we may have advanced our local expiration to account for allowed
	 * spread between our sched_clock and the one on which runtime was
	 * issued.
	 */
	if ((s64)(expires - cfs_rq->runtime_expires) > 0)
		cfs_rq->runtime_expires = expires;

	return cfs_rq->runtime_remaining > 0;
}

/*
 * Note: This depends on the synchronization provided by sched_clock and the
 * fact that rq->clock snapshots this value.
 */
static void expire_cfs_rq_runtime(struct cfs_rq *cfs_rq)
{
	struct cfs_bandwidth *cfs_b = tg_cfs_bandwidth(cfs_rq->tg);

	/* if the deadline is ahead of our clock, nothing to do */
	if (likely((s64)(rq_clock(rq_of(cfs_rq)) - cfs_rq->runtime_expires) < 0))
		return;

	if (cfs_rq->runtime_remaining < 0)
		return;

	/*
	 * If the local deadline has passed we have to consider the
	 * possibility that our sched_clock is 'fast' and the global deadline
	 * has not truly expired.
	 *
	 * Fortunately we can check determine whether this the case by checking
	 * whether the global deadline has advanced. It is valid to compare
	 * cfs_b->runtime_expires without any locks since we only care about
	 * exact equality, so a partial write will still work.
	 */

	if (cfs_rq->runtime_expires != cfs_b->runtime_expires) {
		/* extend local deadline, drift is bounded above by 2 ticks */
		cfs_rq->runtime_expires += TICK_NSEC;
	} else {
		/* global deadline is ahead, expiration has passed */
		cfs_rq->runtime_remaining = 0;
	}
}

static void __account_cfs_rq_runtime(struct cfs_rq *cfs_rq, u64 delta_exec)
{
	/* dock delta_exec before expiring quota (as it could span periods) */
	cfs_rq->runtime_remaining -= delta_exec;
	expire_cfs_rq_runtime(cfs_rq);

	if (likely(cfs_rq->runtime_remaining > 0))
		return;

	/*
	 * if we're unable to extend our runtime we resched so that the active
	 * hierarchy can be throttled
	 */
	if (!assign_cfs_rq_runtime(cfs_rq) && likely(cfs_rq->curr))
		resched_curr(rq_of(cfs_rq));
}

static __always_inline
void account_cfs_rq_runtime(struct cfs_rq *cfs_rq, u64 delta_exec)
{
	if (!cfs_bandwidth_used() || !cfs_rq->runtime_enabled)
		return;

	__account_cfs_rq_runtime(cfs_rq, delta_exec);
}

static inline int cfs_rq_throttled(struct cfs_rq *cfs_rq)
{
	return cfs_bandwidth_used() && cfs_rq->throttled;
}

/*
 * Check if task is part of a hierarchy where some cfs_rq does not have any
 * runtime left.
 *
 * We can't rely on throttled_hierarchy() to do this test, as
 * cfs_rq->throttle_count will not be updated yet when this function is called
 * from scheduler_tick()
 */
static int task_will_be_throttled(struct task_struct *p)
{
	struct sched_entity *se = &p->se;
	struct cfs_rq *cfs_rq;

	if (!cfs_bandwidth_used())
		return 0;

	for_each_sched_entity(se) {
		cfs_rq = cfs_rq_of(se);
		if (!cfs_rq->runtime_enabled)
			continue;
		if (cfs_rq->runtime_remaining <= 0)
			return 1;
	}

	return 0;
}

/* check whether cfs_rq, or any parent, is throttled */
static inline int throttled_hierarchy(struct cfs_rq *cfs_rq)
{
	return cfs_bandwidth_used() && cfs_rq->throttle_count;
}

/*
 * Ensure that neither of the group entities corresponding to src_cpu or
 * dest_cpu are members of a throttled hierarchy when performing group
 * load-balance operations.
 */
static inline int throttled_lb_pair(struct task_group *tg,
				    int src_cpu, int dest_cpu)
{
	struct cfs_rq *src_cfs_rq, *dest_cfs_rq;

	src_cfs_rq = tg->cfs_rq[src_cpu];
	dest_cfs_rq = tg->cfs_rq[dest_cpu];

	return throttled_hierarchy(src_cfs_rq) ||
	       throttled_hierarchy(dest_cfs_rq);
}

/* updated child weight may affect parent so we have to do this bottom up */
static int tg_unthrottle_up(struct task_group *tg, void *data)
{
	struct rq *rq = data;
	struct cfs_rq *cfs_rq = tg->cfs_rq[cpu_of(rq)];

	cfs_rq->throttle_count--;
#ifdef CONFIG_SMP
	if (!cfs_rq->throttle_count) {
		/* adjust cfs_rq_clock_task() */
		cfs_rq->throttled_clock_task_time += rq_clock_task(rq) -
					     cfs_rq->throttled_clock_task;
	}
#endif

	return 0;
}

static int tg_throttle_down(struct task_group *tg, void *data)
{
	struct rq *rq = data;
	struct cfs_rq *cfs_rq = tg->cfs_rq[cpu_of(rq)];

	/* group is entering throttled state, stop time */
	if (!cfs_rq->throttle_count)
		cfs_rq->throttled_clock_task = rq_clock_task(rq);
	cfs_rq->throttle_count++;

	return 0;
}

static void throttle_cfs_rq(struct cfs_rq *cfs_rq)
{
	struct rq *rq = rq_of(cfs_rq);
	struct cfs_bandwidth *cfs_b = tg_cfs_bandwidth(cfs_rq->tg);
	struct sched_entity *se;
	long task_delta, dequeue = 1;

	se = cfs_rq->tg->se[cpu_of(rq_of(cfs_rq))];

	/* freeze hierarchy runnable averages while throttled */
	rcu_read_lock();
	walk_tg_tree_from(cfs_rq->tg, tg_throttle_down, tg_nop, (void *)rq);
	rcu_read_unlock();

	task_delta = cfs_rq->h_nr_running;
	for_each_sched_entity(se) {
		struct cfs_rq *qcfs_rq = cfs_rq_of(se);
		/* throttled entity or throttle-on-deactivate */
		if (!se->on_rq)
			break;

		if (dequeue)
			dequeue_entity(qcfs_rq, se, DEQUEUE_SLEEP);
		qcfs_rq->h_nr_running -= task_delta;
		dec_throttled_cfs_rq_hmp_stats(&qcfs_rq->hmp_stats, cfs_rq);

		if (qcfs_rq->load.weight)
			dequeue = 0;
	}

	if (!se) {
		sub_nr_running(rq, task_delta);
		dec_throttled_cfs_rq_hmp_stats(&rq->hmp_stats, cfs_rq);
	}

	cfs_rq->throttled = 1;
	cfs_rq->throttled_clock = rq_clock(rq);
	raw_spin_lock(&cfs_b->lock);
	/*
	 * Add to the _head_ of the list, so that an already-started
	 * distribute_cfs_runtime will not see us. If disribute_cfs_runtime is
	 * not running add to the tail so that later runqueues don't get starved.
	 */
	if (cfs_b->distribute_running)
		list_add_rcu(&cfs_rq->throttled_list, &cfs_b->throttled_cfs_rq);
	else
		list_add_tail_rcu(&cfs_rq->throttled_list, &cfs_b->throttled_cfs_rq);

	if (!cfs_b->timer_active)
		__start_cfs_bandwidth(cfs_b, false);
	raw_spin_unlock(&cfs_b->lock);

	/* Log effect on hmp stats after throttling */
	trace_sched_cpu_load(rq, idle_cpu(cpu_of(rq)),
			     mostly_idle_cpu(cpu_of(rq)),
			     sched_irqload(cpu_of(rq)),
			     power_cost_at_freq(cpu_of(rq), 0),
			     cpu_temp(cpu_of(rq)));
}

void unthrottle_cfs_rq(struct cfs_rq *cfs_rq)
{
	struct rq *rq = rq_of(cfs_rq);
	struct cfs_bandwidth *cfs_b = tg_cfs_bandwidth(cfs_rq->tg);
	struct sched_entity *se;
	int enqueue = 1;
	long task_delta;
	struct cfs_rq *tcfs_rq = cfs_rq;

	se = cfs_rq->tg->se[cpu_of(rq)];

	cfs_rq->throttled = 0;

	update_rq_clock(rq);

	raw_spin_lock(&cfs_b->lock);
	cfs_b->throttled_time += rq_clock(rq) - cfs_rq->throttled_clock;
	list_del_rcu(&cfs_rq->throttled_list);
	raw_spin_unlock(&cfs_b->lock);

	/* update hierarchical throttle state */
	walk_tg_tree_from(cfs_rq->tg, tg_nop, tg_unthrottle_up, (void *)rq);

	if (!cfs_rq->load.weight)
		return;

	task_delta = cfs_rq->h_nr_running;
	for_each_sched_entity(se) {
		if (se->on_rq)
			enqueue = 0;

		cfs_rq = cfs_rq_of(se);
		if (enqueue)
			enqueue_entity(cfs_rq, se, ENQUEUE_WAKEUP);
		cfs_rq->h_nr_running += task_delta;
		inc_throttled_cfs_rq_hmp_stats(&cfs_rq->hmp_stats, tcfs_rq);

		if (cfs_rq_throttled(cfs_rq))
			break;
	}

	if (!se) {
		add_nr_running(rq, task_delta);
		inc_throttled_cfs_rq_hmp_stats(&rq->hmp_stats, tcfs_rq);
	}

	/* determine whether we need to wake up potentially idle cpu */
	if (rq->curr == rq->idle && rq->cfs.nr_running)
		resched_curr(rq);

	/* Log effect on hmp stats after un-throttling */
	trace_sched_cpu_load(rq, idle_cpu(cpu_of(rq)),
			     mostly_idle_cpu(cpu_of(rq)),
			     sched_irqload(cpu_of(rq)),
			     power_cost_at_freq(cpu_of(rq), 0),
			     cpu_temp(cpu_of(rq)));
}

static u64 distribute_cfs_runtime(struct cfs_bandwidth *cfs_b,
		u64 remaining, u64 expires)
{
	struct cfs_rq *cfs_rq;
	u64 runtime;
	u64 starting_runtime = remaining;

	rcu_read_lock();
	list_for_each_entry_rcu(cfs_rq, &cfs_b->throttled_cfs_rq,
				throttled_list) {
		struct rq *rq = rq_of(cfs_rq);

		raw_spin_lock(&rq->lock);
		if (!cfs_rq_throttled(cfs_rq))
			goto next;

		runtime = -cfs_rq->runtime_remaining + 1;
		if (runtime > remaining)
			runtime = remaining;
		remaining -= runtime;

		cfs_rq->runtime_remaining += runtime;
		cfs_rq->runtime_expires = expires;

		/* we check whether we're throttled above */
		if (cfs_rq->runtime_remaining > 0)
			unthrottle_cfs_rq(cfs_rq);

next:
		raw_spin_unlock(&rq->lock);

		if (!remaining)
			break;
	}
	rcu_read_unlock();

	return starting_runtime - remaining;
}

/*
 * Responsible for refilling a task_group's bandwidth and unthrottling its
 * cfs_rqs as appropriate. If there has been no activity within the last
 * period the timer is deactivated until scheduling resumes; cfs_b->idle is
 * used to track this state.
 */
static int do_sched_cfs_period_timer(struct cfs_bandwidth *cfs_b, int overrun)
{
	u64 runtime, runtime_expires;
	int throttled;

	/* no need to continue the timer with no bandwidth constraint */
	if (cfs_b->quota == RUNTIME_INF)
		goto out_deactivate;

	throttled = !list_empty(&cfs_b->throttled_cfs_rq);
	cfs_b->nr_periods += overrun;

	/*
	 * idle depends on !throttled (for the case of a large deficit), and if
	 * we're going inactive then everything else can be deferred
	 */
	if (cfs_b->idle && !throttled)
		goto out_deactivate;

	/*
	 * if we have relooped after returning idle once, we need to update our
	 * status as actually running, so that other cpus doing
	 * __start_cfs_bandwidth will stop trying to cancel us.
	 */
	cfs_b->timer_active = 1;

	__refill_cfs_bandwidth_runtime(cfs_b);

	if (!throttled) {
		/* mark as potentially idle for the upcoming period */
		cfs_b->idle = 1;
		return 0;
	}

	/* account preceding periods in which throttling occurred */
	cfs_b->nr_throttled += overrun;

	runtime_expires = cfs_b->runtime_expires;

	/*
	 * This check is repeated as we are holding onto the new bandwidth while
	 * we unthrottle. This can potentially race with an unthrottled group
	 * trying to acquire new bandwidth from the global pool. This can result
	 * in us over-using our runtime if it is all used during this loop, but
	 * only by limited amounts in that extreme case.
	 */
	while (throttled && cfs_b->runtime > 0 && !cfs_b->distribute_running) {
		runtime = cfs_b->runtime;
		cfs_b->distribute_running = 1;
		raw_spin_unlock(&cfs_b->lock);
		/* we can't nest cfs_b->lock while distributing bandwidth */
		runtime = distribute_cfs_runtime(cfs_b, runtime,
						 runtime_expires);
		raw_spin_lock(&cfs_b->lock);

		cfs_b->distribute_running = 0;
		throttled = !list_empty(&cfs_b->throttled_cfs_rq);

		cfs_b->runtime -= min(runtime, cfs_b->runtime);
	}

	/*
	 * While we are ensured activity in the period following an
	 * unthrottle, this also covers the case in which the new bandwidth is
	 * insufficient to cover the existing bandwidth deficit.  (Forcing the
	 * timer to remain active while there are any throttled entities.)
	 */
	cfs_b->idle = 0;

	return 0;

out_deactivate:
	cfs_b->timer_active = 0;
	return 1;
}

/* a cfs_rq won't donate quota below this amount */
static const u64 min_cfs_rq_runtime = 1 * NSEC_PER_MSEC;
/* minimum remaining period time to redistribute slack quota */
static const u64 min_bandwidth_expiration = 2 * NSEC_PER_MSEC;
/* how long we wait to gather additional slack before distributing */
static const u64 cfs_bandwidth_slack_period = 5 * NSEC_PER_MSEC;

/*
 * Are we near the end of the current quota period?
 *
 * Requires cfs_b->lock for hrtimer_expires_remaining to be safe against the
 * hrtimer base being cleared by __hrtimer_start_range_ns. In the case of
 * migrate_hrtimers, base is never cleared, so we are fine.
 */
static int runtime_refresh_within(struct cfs_bandwidth *cfs_b, u64 min_expire)
{
	struct hrtimer *refresh_timer = &cfs_b->period_timer;
	s64 remaining;

	/* if the call-back is running a quota refresh is already occurring */
	if (hrtimer_callback_running(refresh_timer))
		return 1;

	/* is a quota refresh about to occur? */
	remaining = ktime_to_ns(hrtimer_expires_remaining(refresh_timer));
	if (remaining < (s64)min_expire)
		return 1;

	return 0;
}

static void start_cfs_slack_bandwidth(struct cfs_bandwidth *cfs_b)
{
	u64 min_left = cfs_bandwidth_slack_period + min_bandwidth_expiration;

	/* if there's a quota refresh soon don't bother with slack */
	if (runtime_refresh_within(cfs_b, min_left))
		return;

	start_bandwidth_timer(&cfs_b->slack_timer,
				ns_to_ktime(cfs_bandwidth_slack_period));
}

/* we know any runtime found here is valid as update_curr() precedes return */
static void __return_cfs_rq_runtime(struct cfs_rq *cfs_rq)
{
	struct cfs_bandwidth *cfs_b = tg_cfs_bandwidth(cfs_rq->tg);
	s64 slack_runtime = cfs_rq->runtime_remaining - min_cfs_rq_runtime;

	if (slack_runtime <= 0)
		return;

	raw_spin_lock(&cfs_b->lock);
	if (cfs_b->quota != RUNTIME_INF &&
	    cfs_rq->runtime_expires == cfs_b->runtime_expires) {
		cfs_b->runtime += slack_runtime;

		/* we are under rq->lock, defer unthrottling using a timer */
		if (cfs_b->runtime > sched_cfs_bandwidth_slice() &&
		    !list_empty(&cfs_b->throttled_cfs_rq))
			start_cfs_slack_bandwidth(cfs_b);
	}
	raw_spin_unlock(&cfs_b->lock);

	/* even if it's not valid for return we don't want to try again */
	cfs_rq->runtime_remaining -= slack_runtime;
}

static __always_inline void return_cfs_rq_runtime(struct cfs_rq *cfs_rq)
{
	if (!cfs_bandwidth_used())
		return;

	if (!cfs_rq->runtime_enabled || cfs_rq->nr_running)
		return;

	__return_cfs_rq_runtime(cfs_rq);
}

/*
 * This is done with a timer (instead of inline with bandwidth return) since
 * it's necessary to juggle rq->locks to unthrottle their respective cfs_rqs.
 */
static void do_sched_cfs_slack_timer(struct cfs_bandwidth *cfs_b)
{
	u64 runtime = 0, slice = sched_cfs_bandwidth_slice();
	u64 expires;

	/* confirm we're still not at a refresh boundary */
	raw_spin_lock(&cfs_b->lock);
	if (cfs_b->distribute_running) {
		raw_spin_unlock(&cfs_b->lock);
		return;
	}

	if (runtime_refresh_within(cfs_b, min_bandwidth_expiration)) {
		raw_spin_unlock(&cfs_b->lock);
		return;
	}

	if (cfs_b->quota != RUNTIME_INF && cfs_b->runtime > slice)
		runtime = cfs_b->runtime;

	expires = cfs_b->runtime_expires;
	if (runtime)
		cfs_b->distribute_running = 1;

	raw_spin_unlock(&cfs_b->lock);

	if (!runtime)
		return;

	runtime = distribute_cfs_runtime(cfs_b, runtime, expires);

	raw_spin_lock(&cfs_b->lock);
	if (expires == cfs_b->runtime_expires)
		cfs_b->runtime -= min(runtime, cfs_b->runtime);
	cfs_b->distribute_running = 0;
	raw_spin_unlock(&cfs_b->lock);
}

/*
 * When a group wakes up we want to make sure that its quota is not already
 * expired/exceeded, otherwise it may be allowed to steal additional ticks of
 * runtime as update_curr() throttling can not not trigger until it's on-rq.
 */
static void check_enqueue_throttle(struct cfs_rq *cfs_rq)
{
	if (!cfs_bandwidth_used())
		return;

	/* Synchronize hierarchical throttle counter: */
	if (unlikely(!cfs_rq->throttle_uptodate)) {
		struct rq *rq = rq_of(cfs_rq);
		struct cfs_rq *pcfs_rq;
		struct task_group *tg;

		cfs_rq->throttle_uptodate = 1;

		/* Get closest up-to-date node, because leaves go first: */
		for (tg = cfs_rq->tg->parent; tg; tg = tg->parent) {
			pcfs_rq = tg->cfs_rq[cpu_of(rq)];
			if (pcfs_rq->throttle_uptodate)
				break;
		}
		if (tg) {
			cfs_rq->throttle_count = pcfs_rq->throttle_count;
			cfs_rq->throttled_clock_task = rq_clock_task(rq);
		}
	}

	/* an active group must be handled by the update_curr()->put() path */
	if (!cfs_rq->runtime_enabled || cfs_rq->curr)
		return;

	/* ensure the group is not already throttled */
	if (cfs_rq_throttled(cfs_rq))
		return;

	/* update runtime allocation */
	account_cfs_rq_runtime(cfs_rq, 0);
	if (cfs_rq->runtime_remaining <= 0)
		throttle_cfs_rq(cfs_rq);
}

/* conditionally throttle active cfs_rq's from put_prev_entity() */
static bool check_cfs_rq_runtime(struct cfs_rq *cfs_rq)
{
	if (!cfs_bandwidth_used())
		return false;

	if (likely(!cfs_rq->runtime_enabled || cfs_rq->runtime_remaining > 0))
		return false;

	/*
	 * it's possible for a throttled entity to be forced into a running
	 * state (e.g. set_curr_task), in this case we're finished.
	 */
	if (cfs_rq_throttled(cfs_rq))
		return true;

	throttle_cfs_rq(cfs_rq);
	return true;
}

static enum hrtimer_restart sched_cfs_slack_timer(struct hrtimer *timer)
{
	struct cfs_bandwidth *cfs_b =
		container_of(timer, struct cfs_bandwidth, slack_timer);
	do_sched_cfs_slack_timer(cfs_b);

	return HRTIMER_NORESTART;
}

extern const u64 max_cfs_quota_period;

static enum hrtimer_restart sched_cfs_period_timer(struct hrtimer *timer)
{
	struct cfs_bandwidth *cfs_b =
		container_of(timer, struct cfs_bandwidth, period_timer);
	ktime_t now;
	int overrun;
	int idle = 0;
	int count = 0;

	raw_spin_lock(&cfs_b->lock);
	for (;;) {
		now = hrtimer_cb_get_time(timer);
		overrun = hrtimer_forward(timer, now, cfs_b->period);

		if (!overrun)
			break;

		if (++count > 3) {
			u64 new, old = ktime_to_ns(cfs_b->period);

			/*
			 * Grow period by a factor of 2 to avoid losing precision.
			 * Precision loss in the quota/period ratio can cause __cfs_schedulable
			 * to fail.
			 */
			new = old * 2;
			if (new < max_cfs_quota_period) {
				cfs_b->period = ns_to_ktime(new);
				cfs_b->quota *= 2;

				pr_warn_ratelimited(
	"cfs_period_timer[cpu%d]: period too short, scaling up (new cfs_period_us = %lld, cfs_quota_us = %lld)\n",
					smp_processor_id(),
					div_u64(new, NSEC_PER_USEC),
					div_u64(cfs_b->quota, NSEC_PER_USEC));
			} else {
				pr_warn_ratelimited(
	"cfs_period_timer[cpu%d]: period too short, but cannot scale up without losing precision (cfs_period_us = %lld, cfs_quota_us = %lld)\n",
					smp_processor_id(),
					div_u64(old, NSEC_PER_USEC),
					div_u64(cfs_b->quota, NSEC_PER_USEC));
			}

			/* reset count so we don't come right back in here */
			count = 0;
		}

		idle = do_sched_cfs_period_timer(cfs_b, overrun);
	}
	raw_spin_unlock(&cfs_b->lock);

	return idle ? HRTIMER_NORESTART : HRTIMER_RESTART;
}

void init_cfs_bandwidth(struct cfs_bandwidth *cfs_b)
{
	raw_spin_lock_init(&cfs_b->lock);
	cfs_b->runtime = 0;
	cfs_b->quota = RUNTIME_INF;
	cfs_b->period = ns_to_ktime(default_cfs_period());

	INIT_LIST_HEAD(&cfs_b->throttled_cfs_rq);
	hrtimer_init(&cfs_b->period_timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	cfs_b->period_timer.function = sched_cfs_period_timer;
	hrtimer_init(&cfs_b->slack_timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	cfs_b->slack_timer.function = sched_cfs_slack_timer;
	cfs_b->distribute_running = 0;
}

static void init_cfs_rq_runtime(struct cfs_rq *cfs_rq)
{
	cfs_rq->runtime_enabled = 0;
	INIT_LIST_HEAD(&cfs_rq->throttled_list);
	init_cfs_rq_hmp_stats(cfs_rq);
}

/* requires cfs_b->lock, may release to reprogram timer */
void __start_cfs_bandwidth(struct cfs_bandwidth *cfs_b, bool force)
{
	/*
	 * The timer may be active because we're trying to set a new bandwidth
	 * period or because we're racing with the tear-down path
	 * (timer_active==0 becomes visible before the hrtimer call-back
	 * terminates).  In either case we ensure that it's re-programmed
	 */
	while (unlikely(hrtimer_active(&cfs_b->period_timer)) &&
	       hrtimer_try_to_cancel(&cfs_b->period_timer) < 0) {
		/* bounce the lock to allow do_sched_cfs_period_timer to run */
		raw_spin_unlock(&cfs_b->lock);
		cpu_relax();
		raw_spin_lock(&cfs_b->lock);
		/* if someone else restarted the timer then we're done */
		if (!force && cfs_b->timer_active)
			return;
	}

	cfs_b->timer_active = 1;
	start_bandwidth_timer(&cfs_b->period_timer, cfs_b->period);
}

static void destroy_cfs_bandwidth(struct cfs_bandwidth *cfs_b)
{
	hrtimer_cancel(&cfs_b->period_timer);
	hrtimer_cancel(&cfs_b->slack_timer);
}

static void __maybe_unused update_runtime_enabled(struct rq *rq)
{
	struct cfs_rq *cfs_rq;

	for_each_leaf_cfs_rq(rq, cfs_rq) {
		struct cfs_bandwidth *cfs_b = &cfs_rq->tg->cfs_bandwidth;

		raw_spin_lock(&cfs_b->lock);
		cfs_rq->runtime_enabled = cfs_b->quota != RUNTIME_INF;
		raw_spin_unlock(&cfs_b->lock);
	}
}

static void __maybe_unused unthrottle_offline_cfs_rqs(struct rq *rq)
{
	struct cfs_rq *cfs_rq;

	for_each_leaf_cfs_rq(rq, cfs_rq) {
		if (!cfs_rq->runtime_enabled)
			continue;

		/*
		 * clock_task is not advancing so we just need to make sure
		 * there's some valid quota amount
		 */
		cfs_rq->runtime_remaining = 1;
		/*
		 * Offline rq is schedulable till cpu is completely disabled
		 * in take_cpu_down(), so we prevent new cfs throttling here.
		 */
		cfs_rq->runtime_enabled = 0;

		if (cfs_rq_throttled(cfs_rq))
			unthrottle_cfs_rq(cfs_rq);
	}
}

#else /* CONFIG_CFS_BANDWIDTH */
static inline u64 cfs_rq_clock_task(struct cfs_rq *cfs_rq)
{
	return rq_clock_task(rq_of(cfs_rq));
}

static void account_cfs_rq_runtime(struct cfs_rq *cfs_rq, u64 delta_exec) {}
static bool check_cfs_rq_runtime(struct cfs_rq *cfs_rq) { return false; }
static void check_enqueue_throttle(struct cfs_rq *cfs_rq) {}
static __always_inline void return_cfs_rq_runtime(struct cfs_rq *cfs_rq) {}

static inline int cfs_rq_throttled(struct cfs_rq *cfs_rq)
{
	return 0;
}

static inline int throttled_hierarchy(struct cfs_rq *cfs_rq)
{
	return 0;
}

static inline int throttled_lb_pair(struct task_group *tg,
				    int src_cpu, int dest_cpu)
{
	return 0;
}

void init_cfs_bandwidth(struct cfs_bandwidth *cfs_b) {}

#ifdef CONFIG_FAIR_GROUP_SCHED
static void init_cfs_rq_runtime(struct cfs_rq *cfs_rq) {}
#endif

static inline struct cfs_bandwidth *tg_cfs_bandwidth(struct task_group *tg)
{
	return NULL;
}
static inline void destroy_cfs_bandwidth(struct cfs_bandwidth *cfs_b) {}
static inline void update_runtime_enabled(struct rq *rq) {}
static inline void unthrottle_offline_cfs_rqs(struct rq *rq) {}

#endif /* CONFIG_CFS_BANDWIDTH */

/**************************************************
 * CFS operations on tasks:
 */

#ifdef CONFIG_SCHED_HRTICK
static void hrtick_start_fair(struct rq *rq, struct task_struct *p)
{
	struct sched_entity *se = &p->se;
	struct cfs_rq *cfs_rq = cfs_rq_of(se);

	WARN_ON(task_rq(p) != rq);

	if (rq->cfs.h_nr_running > 1) {
		u64 slice = sched_slice(cfs_rq, se);
		u64 ran = se->sum_exec_runtime - se->prev_sum_exec_runtime;
		s64 delta = slice - ran;

		if (delta < 0) {
			if (rq->curr == p)
				resched_curr(rq);
			return;
		}
		hrtick_start(rq, delta);
	}
}

/*
 * called from enqueue/dequeue and updates the hrtick when the
 * current task is from our class.
 */
static void hrtick_update(struct rq *rq)
{
	struct task_struct *curr = rq->curr;

	if (!hrtick_enabled(rq) || curr->sched_class != &fair_sched_class)
		return;

	hrtick_start_fair(rq, curr);
}
#else /* !CONFIG_SCHED_HRTICK */
static inline void
hrtick_start_fair(struct rq *rq, struct task_struct *p)
{
}

static inline void hrtick_update(struct rq *rq)
{
}
#endif

/*
 * The enqueue_task method is called before nr_running is
 * increased. Here we update the fair scheduling stats and
 * then put the task into the rbtree:
 */
static void
enqueue_task_fair(struct rq *rq, struct task_struct *p, int flags)
{
	struct cfs_rq *cfs_rq;
	struct sched_entity *se = &p->se;

	for_each_sched_entity(se) {
		if (se->on_rq)
			break;
		cfs_rq = cfs_rq_of(se);
		enqueue_entity(cfs_rq, se, flags);

		/*
		 * end evaluation on encountering a throttled cfs_rq
		 *
		 * note: in the case of encountering a throttled cfs_rq we will
		 * post the final h_nr_running increment below.
		*/
		if (cfs_rq_throttled(cfs_rq))
			break;
		cfs_rq->h_nr_running++;
		inc_cfs_rq_hmp_stats(cfs_rq, p, 1);

		flags = ENQUEUE_WAKEUP;
	}

	for_each_sched_entity(se) {
		cfs_rq = cfs_rq_of(se);
		cfs_rq->h_nr_running++;
		inc_cfs_rq_hmp_stats(cfs_rq, p, 1);

		if (cfs_rq_throttled(cfs_rq))
			break;

		update_load_avg(se, 1);
		update_cfs_shares(cfs_rq);
	}

	if (!se) {
		add_nr_running(rq, 1);
		inc_rq_hmp_stats(rq, p, 1);
	}
	hrtick_update(rq);
}

static void set_next_buddy(struct sched_entity *se);

/*
 * The dequeue_task method is called before nr_running is
 * decreased. We remove the task from the rbtree and
 * update the fair scheduling stats:
 */
static void dequeue_task_fair(struct rq *rq, struct task_struct *p, int flags)
{
	struct cfs_rq *cfs_rq;
	struct sched_entity *se = &p->se;
	int task_sleep = flags & DEQUEUE_SLEEP;

	for_each_sched_entity(se) {
		cfs_rq = cfs_rq_of(se);
		dequeue_entity(cfs_rq, se, flags);

		/*
		 * end evaluation on encountering a throttled cfs_rq
		 *
		 * note: in the case of encountering a throttled cfs_rq we will
		 * post the final h_nr_running decrement below.
		*/
		if (cfs_rq_throttled(cfs_rq))
			break;
		cfs_rq->h_nr_running--;
		dec_cfs_rq_hmp_stats(cfs_rq, p, 1);

		/* Don't dequeue parent if it has other entities besides us */
		if (cfs_rq->load.weight) {
			/* Avoid re-evaluating load for this entity: */
			se = parent_entity(se);
			/*
			 * Bias pick_next to pick a task from this cfs_rq, as
			 * p is sleeping when it is within its sched_slice.
			 */
			if (task_sleep && se && !throttled_hierarchy(cfs_rq))
				set_next_buddy(se);
			break;
		}
		flags |= DEQUEUE_SLEEP;
	}

	for_each_sched_entity(se) {
		cfs_rq = cfs_rq_of(se);
		cfs_rq->h_nr_running--;
		dec_cfs_rq_hmp_stats(cfs_rq, p, 1);

		if (cfs_rq_throttled(cfs_rq))
			break;

		update_load_avg(se, 1);
		update_cfs_shares(cfs_rq);
	}

	if (!se) {
		sub_nr_running(rq, 1);
		dec_rq_hmp_stats(rq, p, 1);
	}
	hrtick_update(rq);
}

#ifdef CONFIG_SMP

/*
 * per rq 'load' arrray crap; XXX kill this.
 */

/*
 * The exact cpuload calculated at every tick would be:
 *
 *   load' = (1 - 1/2^i) * load + (1/2^i) * cur_load
 *
 * If a cpu misses updates for n ticks (as it was idle) and update gets
 * called on the n+1-th tick when cpu may be busy, then we have:
 *
 *   load_n   = (1 - 1/2^i)^n * load_0
 *   load_n+1 = (1 - 1/2^i)   * load_n + (1/2^i) * cur_load
 *
 * decay_load_missed() below does efficient calculation of
 *
 *   load' = (1 - 1/2^i)^n * load
 *
 * Because x^(n+m) := x^n * x^m we can decompose any x^n in power-of-2 factors.
 * This allows us to precompute the above in said factors, thereby allowing the
 * reduction of an arbitrary n in O(log_2 n) steps. (See also
 * fixed_power_int())
 *
 * The calculation is approximated on a 128 point scale.
 */
#define DEGRADE_SHIFT		7

static const u8 degrade_zero_ticks[CPU_LOAD_IDX_MAX] = {0, 8, 32, 64, 128};
static const u8 degrade_factor[CPU_LOAD_IDX_MAX][DEGRADE_SHIFT + 1] = {
	{   0,   0,  0,  0,  0,  0, 0, 0 },
	{  64,  32,  8,  0,  0,  0, 0, 0 },
	{  96,  72, 40, 12,  1,  0, 0, 0 },
	{ 112,  98, 75, 43, 15,  1, 0, 0 },
	{ 120, 112, 98, 76, 45, 16, 2, 0 }
};

/*
 * Update cpu_load for any missed ticks, due to tickless idle. The backlog
 * would be when CPU is idle and so we just decay the old load without
 * adding any new load.
 */
static unsigned long
decay_load_missed(unsigned long load, unsigned long missed_updates, int idx)
{
	int j = 0;

	if (!missed_updates)
		return load;

	if (missed_updates >= degrade_zero_ticks[idx])
		return 0;

	if (idx == 1)
		return load >> missed_updates;

	while (missed_updates) {
		if (missed_updates % 2)
			load = (load * degrade_factor[idx][j]) >> DEGRADE_SHIFT;

		missed_updates >>= 1;
		j++;
	}
	return load;
}

/*
 * Update rq->cpu_load[] statistics. This function is usually called every
 * scheduler tick (TICK_NSEC). With tickless idle this will not be called
 * every tick. We fix it up based on jiffies.
 */
static void __update_cpu_load(struct rq *this_rq, unsigned long this_load,
			      unsigned long pending_updates)
{
	int i, scale;

	this_rq->nr_load_updates++;

	/* Update our load: */
	this_rq->cpu_load[0] = this_load; /* Fasttrack for idx 0 */
	for (i = 1, scale = 2; i < CPU_LOAD_IDX_MAX; i++, scale += scale) {
		unsigned long old_load, new_load;

		/* scale is effectively 1 << i now, and >> i divides by scale */

		old_load = this_rq->cpu_load[i];
		old_load = decay_load_missed(old_load, pending_updates - 1, i);
		new_load = this_load;
		/*
		 * Round up the averaging division if load is increasing. This
		 * prevents us from getting stuck on 9 if the load is 10, for
		 * example.
		 */
		if (new_load > old_load)
			new_load += scale - 1;

		this_rq->cpu_load[i] = (old_load * (scale - 1) + new_load) >> i;
	}

	sched_avg_update(this_rq);
}

/* Used instead of source_load when we know the type == 0 */
static unsigned long weighted_cpuload(const int cpu)
{
	return cfs_rq_runnable_load_avg(&cpu_rq(cpu)->cfs);
}

#ifdef CONFIG_NO_HZ_COMMON
/*
 * There is no sane way to deal with nohz on smp when using jiffies because the
 * cpu doing the jiffies update might drift wrt the cpu doing the jiffy reading
 * causing off-by-one errors in observed deltas; {0,2} instead of {1,1}.
 *
 * Therefore we cannot use the delta approach from the regular tick since that
 * would seriously skew the load calculation. However we'll make do for those
 * updates happening while idle (nohz_idle_balance) or coming out of idle
 * (tick_nohz_idle_exit).
 *
 * This means we might still be one tick off for nohz periods.
 */

/*
 * Called from nohz_idle_balance() to update the load ratings before doing the
 * idle balance.
 */
static void update_idle_cpu_load(struct rq *this_rq)
{
	unsigned long curr_jiffies = READ_ONCE(jiffies);
	unsigned long load = weighted_cpuload(cpu_of(this_rq));
	unsigned long pending_updates;

	/*
	 * bail if there's load or we're actually up-to-date.
	 */
	if (load || curr_jiffies == this_rq->last_load_update_tick)
		return;

	pending_updates = curr_jiffies - this_rq->last_load_update_tick;
	this_rq->last_load_update_tick = curr_jiffies;

	__update_cpu_load(this_rq, load, pending_updates);
}

/*
 * Called from tick_nohz_idle_exit() -- try and fix up the ticks we missed.
 */
void update_cpu_load_nohz(void)
{
	struct rq *this_rq = this_rq();
	unsigned long curr_jiffies = READ_ONCE(jiffies);
	unsigned long pending_updates;

	if (curr_jiffies == this_rq->last_load_update_tick)
		return;

	raw_spin_lock(&this_rq->lock);
	pending_updates = curr_jiffies - this_rq->last_load_update_tick;
	if (pending_updates) {
		this_rq->last_load_update_tick = curr_jiffies;
		/*
		 * We were idle, this means load 0, the current load might be
		 * !0 due to remote wakeups and the sort.
		 */
		__update_cpu_load(this_rq, 0, pending_updates);
	}
	raw_spin_unlock(&this_rq->lock);
}
#endif /* CONFIG_NO_HZ */

/*
 * Called from scheduler_tick()
 */
void update_cpu_load_active(struct rq *this_rq)
{
	unsigned long load = weighted_cpuload(cpu_of(this_rq));
	/*
	 * See the mess around update_idle_cpu_load() / update_cpu_load_nohz().
	 */
	this_rq->last_load_update_tick = jiffies;
	__update_cpu_load(this_rq, load, 1);
}

/*
 * Return a low guess at the load of a migration-source cpu weighted
 * according to the scheduling class and "nice" value.
 *
 * We want to under-estimate the load of migration sources, to
 * balance conservatively.
 */
static unsigned long source_load(int cpu, int type)
{
	struct rq *rq = cpu_rq(cpu);
	unsigned long total = weighted_cpuload(cpu);

	if (type == 0 || !sched_feat(LB_BIAS))
		return total;

	return min(rq->cpu_load[type-1], total);
}

/*
 * Return a high guess at the load of a migration-target cpu weighted
 * according to the scheduling class and "nice" value.
 */
static unsigned long target_load(int cpu, int type)
{
	struct rq *rq = cpu_rq(cpu);
	unsigned long total = weighted_cpuload(cpu);

	if (type == 0 || !sched_feat(LB_BIAS))
		return total;

	return max(rq->cpu_load[type-1], total);
}

static unsigned long capacity_of(int cpu)
{
	return cpu_rq(cpu)->cpu_capacity;
}

static unsigned long capacity_orig_of(int cpu)
{
	return cpu_rq(cpu)->cpu_capacity_orig;
}

static unsigned long cpu_avg_load_per_task(int cpu)
{
	struct rq *rq = cpu_rq(cpu);
	unsigned long nr_running = READ_ONCE(rq->cfs.h_nr_running);
	unsigned long load_avg = weighted_cpuload(cpu);

	if (nr_running)
		return load_avg / nr_running;

	return 0;
}

static void record_wakee(struct task_struct *p)
{
	/*
	 * Rough decay (wiping) for cost saving, don't worry
	 * about the boundary, really active task won't care
	 * about the loss.
	 */
	if (time_after(jiffies, current->wakee_flip_decay_ts + HZ)) {
		current->wakee_flips >>= 1;
		current->wakee_flip_decay_ts = jiffies;
	}

	if (current->last_wakee != p) {
		current->last_wakee = p;
		current->wakee_flips++;
	}
}

static void task_waking_fair(struct task_struct *p)
{
	struct sched_entity *se = &p->se;
	struct cfs_rq *cfs_rq = cfs_rq_of(se);
	u64 min_vruntime;

#ifndef CONFIG_64BIT
	u64 min_vruntime_copy;

	do {
		min_vruntime_copy = cfs_rq->min_vruntime_copy;
		smp_rmb();
		min_vruntime = cfs_rq->min_vruntime;
	} while (min_vruntime != min_vruntime_copy);
#else
	min_vruntime = cfs_rq->min_vruntime;
#endif

	se->vruntime -= min_vruntime;
	record_wakee(p);
}

#ifdef CONFIG_FAIR_GROUP_SCHED
/*
 * effective_load() calculates the load change as seen from the root_task_group
 *
 * Adding load to a group doesn't make a group heavier, but can cause movement
 * of group shares between cpus. Assuming the shares were perfectly aligned one
 * can calculate the shift in shares.
 *
 * Calculate the effective load difference if @wl is added (subtracted) to @tg
 * on this @cpu and results in a total addition (subtraction) of @wg to the
 * total group weight.
 *
 * Given a runqueue weight distribution (rw_i) we can compute a shares
 * distribution (s_i) using:
 *
 *   s_i = rw_i / \Sum rw_j						(1)
 *
 * Suppose we have 4 CPUs and our @tg is a direct child of the root group and
 * has 7 equal weight tasks, distributed as below (rw_i), with the resulting
 * shares distribution (s_i):
 *
 *   rw_i = {   2,   4,   1,   0 }
 *   s_i  = { 2/7, 4/7, 1/7,   0 }
 *
 * As per wake_affine() we're interested in the load of two CPUs (the CPU the
 * task used to run on and the CPU the waker is running on), we need to
 * compute the effect of waking a task on either CPU and, in case of a sync
 * wakeup, compute the effect of the current task going to sleep.
 *
 * So for a change of @wl to the local @cpu with an overall group weight change
 * of @wl we can compute the new shares distribution (s'_i) using:
 *
 *   s'_i = (rw_i + @wl) / (@wg + \Sum rw_j)				(2)
 *
 * Suppose we're interested in CPUs 0 and 1, and want to compute the load
 * differences in waking a task to CPU 0. The additional task changes the
 * weight and shares distributions like:
 *
 *   rw'_i = {   3,   4,   1,   0 }
 *   s'_i  = { 3/8, 4/8, 1/8,   0 }
 *
 * We can then compute the difference in effective weight by using:
 *
 *   dw_i = S * (s'_i - s_i)						(3)
 *
 * Where 'S' is the group weight as seen by its parent.
 *
 * Therefore the effective change in loads on CPU 0 would be 5/56 (3/8 - 2/7)
 * times the weight of the group. The effect on CPU 1 would be -4/56 (4/8 -
 * 4/7) times the weight of the group.
 */
static long effective_load(struct task_group *tg, int cpu, long wl, long wg)
{
	struct sched_entity *se = tg->se[cpu];

	if (!tg->parent)	/* the trivial, non-cgroup case */
		return wl;

	for_each_sched_entity(se) {
		struct cfs_rq *cfs_rq = se->my_q;
		long W, w = cfs_rq_load_avg(cfs_rq);

		tg = cfs_rq->tg;

		/*
		 * W = @wg + \Sum rw_j
		 */
		W = wg + atomic_long_read(&tg->load_avg);

		/* Ensure \Sum rw_j >= rw_i */
		W -= cfs_rq->tg_load_avg_contrib;
		W += w;

		/*
		 * w = rw_i + @wl
		 */
		w += wl;

		/*
		 * wl = S * s'_i; see (2)
		 */
		if (W > 0 && w < W)
			wl = (w * (long)tg->shares) / W;
		else
			wl = tg->shares;

		/*
		 * Per the above, wl is the new se->load.weight value; since
		 * those are clipped to [MIN_SHARES, ...) do so now. See
		 * calc_cfs_shares().
		 */
		if (wl < MIN_SHARES)
			wl = MIN_SHARES;

		/*
		 * wl = dw_i = S * (s'_i - s_i); see (3)
		 */
		wl -= se->avg.load_avg;

		/*
		 * Recursively apply this logic to all parent groups to compute
		 * the final effective load change on the root group. Since
		 * only the @tg group gets extra weight, all parent groups can
		 * only redistribute existing shares. @wl is the shift in shares
		 * resulting from this level per the above.
		 */
		wg = 0;
	}

	return wl;
}
#else

static long effective_load(struct task_group *tg, int cpu, long wl, long wg)
{
	return wl;
}

#endif

/*
 * Detect M:N waker/wakee relationships via a switching-frequency heuristic.
 * A waker of many should wake a different task than the one last awakened
 * at a frequency roughly N times higher than one of its wakees.  In order
 * to determine whether we should let the load spread vs consolodating to
 * shared cache, we look for a minimum 'flip' frequency of llc_size in one
 * partner, and a factor of lls_size higher frequency in the other.  With
 * both conditions met, we can be relatively sure that the relationship is
 * non-monogamous, with partner count exceeding socket size.  Waker/wakee
 * being client/server, worker/dispatcher, interrupt source or whatever is
 * irrelevant, spread criteria is apparent partner count exceeds socket size.
 */
static int wake_wide(struct task_struct *p)
{
	unsigned int master = current->wakee_flips;
	unsigned int slave = p->wakee_flips;
	int factor = this_cpu_read(sd_llc_size);

	if (master < slave)
		swap(master, slave);
	if (slave < factor || master < slave * factor)
		return 0;
	return 1;
}

static int wake_affine(struct sched_domain *sd, struct task_struct *p, int sync)
{
	s64 this_load, load;
	s64 this_eff_load, prev_eff_load;
	int idx, this_cpu, prev_cpu;
	struct task_group *tg;
	unsigned long weight;
	int balanced;

	idx	  = sd->wake_idx;
	this_cpu  = smp_processor_id();
	prev_cpu  = task_cpu(p);
	load	  = source_load(prev_cpu, idx);
	this_load = target_load(this_cpu, idx);

	/*
	 * If sync wakeup then subtract the (maximum possible)
	 * effect of the currently running task from the load
	 * of the current CPU:
	 */
	if (sync) {
		tg = task_group(current);
		weight = current->se.avg.load_avg;

		this_load += effective_load(tg, this_cpu, -weight, -weight);
		load += effective_load(tg, prev_cpu, 0, -weight);
	}

	tg = task_group(p);
	weight = p->se.avg.load_avg;

	/*
	 * In low-load situations, where prev_cpu is idle and this_cpu is idle
	 * due to the sync cause above having dropped this_load to 0, we'll
	 * always have an imbalance, but there's really nothing you can do
	 * about that, so that's good too.
	 *
	 * Otherwise check if either cpus are near enough in load to allow this
	 * task to be woken on this_cpu.
	 */
	this_eff_load = 100;
	this_eff_load *= capacity_of(prev_cpu);

	prev_eff_load = 100 + (sd->imbalance_pct - 100) / 2;
	prev_eff_load *= capacity_of(this_cpu);

	if (this_load > 0) {
		this_eff_load *= this_load +
			effective_load(tg, this_cpu, weight, weight);

		prev_eff_load *= load + effective_load(tg, prev_cpu, 0, weight);
	}

	balanced = this_eff_load <= prev_eff_load;

	schedstat_inc(p, se.statistics.nr_wakeups_affine_attempts);

	if (!balanced)
		return 0;

	schedstat_inc(sd, ttwu_move_affine);
	schedstat_inc(p, se.statistics.nr_wakeups_affine);

	return 1;
}

/*
 * find_idlest_group finds and returns the least busy CPU group within the
 * domain.
 */
static struct sched_group *
find_idlest_group(struct sched_domain *sd, struct task_struct *p,
		  int this_cpu, int sd_flag)
{
	struct sched_group *idlest = NULL, *group = sd->groups;
	unsigned long min_load = ULONG_MAX, this_load = 0;
	int load_idx = sd->forkexec_idx;
	int imbalance = 100 + (sd->imbalance_pct-100)/2;

	if (sd_flag & SD_BALANCE_WAKE)
		load_idx = sd->wake_idx;

	do {
		unsigned long load, avg_load;
		int local_group;
		int i;

		/* Skip over this group if it has no CPUs allowed */
		if (!cpumask_intersects(sched_group_cpus(group),
					tsk_cpus_allowed(p)))
			continue;

		local_group = cpumask_test_cpu(this_cpu,
					       sched_group_cpus(group));

		/* Tally up the load of all CPUs in the group */
		avg_load = 0;

		for_each_cpu(i, sched_group_cpus(group)) {
			/* Bias balancing toward cpus of our domain */
			if (local_group)
				load = source_load(i, load_idx);
			else
				load = target_load(i, load_idx);

			avg_load += load;
		}

		/* Adjust by relative CPU capacity of the group */
		avg_load = (avg_load * SCHED_CAPACITY_SCALE) / group->sgc->capacity;

		if (local_group) {
			this_load = avg_load;
		} else if (avg_load < min_load) {
			min_load = avg_load;
			idlest = group;
		}
	} while (group = group->next, group != sd->groups);

	if (!idlest || 100*this_load < imbalance*min_load)
		return NULL;
	return idlest;
}

/*
 * find_idlest_cpu - find the idlest cpu among the cpus in group.
 */
static int
find_idlest_cpu(struct sched_group *group, struct task_struct *p, int this_cpu)
{
	unsigned long load, min_load = ULONG_MAX;
	unsigned int min_exit_latency = UINT_MAX;
	u64 latest_idle_timestamp = 0;
	int least_loaded_cpu = this_cpu;
	int shallowest_idle_cpu = -1;
	int i;

	/* Check if we have any choice: */
	if (group->group_weight == 1)
		return cpumask_first(sched_group_cpus(group));

	/* Traverse only the allowed CPUs */
	for_each_cpu_and(i, sched_group_cpus(group), tsk_cpus_allowed(p)) {
		if (idle_cpu(i)) {
			struct rq *rq = cpu_rq(i);
			struct cpuidle_state *idle = idle_get_state(rq);
			if (idle && idle->exit_latency < min_exit_latency) {
				/*
				 * We give priority to a CPU whose idle state
				 * has the smallest exit latency irrespective
				 * of any idle timestamp.
				 */
				min_exit_latency = idle->exit_latency;
				latest_idle_timestamp = rq->idle_stamp;
				shallowest_idle_cpu = i;
			} else if ((!idle || idle->exit_latency == min_exit_latency) &&
				   rq->idle_stamp > latest_idle_timestamp) {
				/*
				 * If equal or no active idle state, then
				 * the most recently idled CPU might have
				 * a warmer cache.
				 */
				latest_idle_timestamp = rq->idle_stamp;
				shallowest_idle_cpu = i;
			}
		} else {
			load = weighted_cpuload(i);
			if (load < min_load || (load == min_load && i == this_cpu)) {
				min_load = load;
				least_loaded_cpu = i;
			}
		}
	}

	return shallowest_idle_cpu != -1 ? shallowest_idle_cpu : least_loaded_cpu;
}

/*
 * Try and locate an idle CPU in the sched_domain.
 */
static int select_idle_sibling(struct task_struct *p, int target)
{
	struct sched_domain *sd;
	struct sched_group *sg;
	int i = task_cpu(p);

	if (idle_cpu(target))
		return target;

	/*
	 * If the prevous cpu is cache affine and idle, don't be stupid.
	 */
	if (i != target && cpus_share_cache(i, target) && idle_cpu(i))
		return i;

	if (!sysctl_sched_wake_to_idle &&
	    !(current->flags & PF_WAKE_UP_IDLE) &&
	    !(p->flags & PF_WAKE_UP_IDLE))
		return target;

	/*
	 * Otherwise, iterate the domains and find an elegible idle cpu.
	 */
	sd = rcu_dereference(per_cpu(sd_llc, target));
	for_each_lower_domain(sd) {
		sg = sd->groups;
		do {
			if (!cpumask_intersects(sched_group_cpus(sg),
						tsk_cpus_allowed(p)))
				goto next;

			for_each_cpu(i, sched_group_cpus(sg)) {
				if (i == target || !idle_cpu(i))
					goto next;
			}

			target = cpumask_first_and(sched_group_cpus(sg),
					tsk_cpus_allowed(p));
			goto done;
next:
			sg = sg->next;
		} while (sg != sd->groups);
	}
done:
	return target;
}

/*
 * cpu_util returns the amount of capacity of a CPU that is used by CFS
 * tasks. The unit of the return value must be the one of capacity so we can
 * compare the utilization with the capacity of the CPU that is available for
 * CFS task (ie cpu_capacity).
 *
 * cfs_rq.avg.util_avg is the sum of running time of runnable tasks plus the
 * recent utilization of currently non-runnable tasks on a CPU. It represents
 * the amount of utilization of a CPU in the range [0..capacity_orig] where
 * capacity_orig is the cpu_capacity available at the highest frequency
 * (arch_scale_freq_capacity()).
 * The utilization of a CPU converges towards a sum equal to or less than the
 * current capacity (capacity_curr <= capacity_orig) of the CPU because it is
 * the running time on this CPU scaled by capacity_curr.
 *
 * Nevertheless, cfs_rq.avg.util_avg can be higher than capacity_curr or even
 * higher than capacity_orig because of unfortunate rounding in
 * cfs.avg.util_avg or just after migrating tasks and new task wakeups until
 * the average stabilizes with the new running time. We need to check that the
 * utilization stays within the range of [0..capacity_orig] and cap it if
 * necessary. Without utilization capping, a group could be seen as overloaded
 * (CPU0 utilization at 121% + CPU1 utilization at 80%) whereas CPU1 has 20% of
 * available capacity. We allow utilization to overshoot capacity_curr (but not
 * capacity_orig) as it useful for predicting the capacity required after task
 * migrations (scheduler-driven DVFS).
 */
static int cpu_util(int cpu)
{
	unsigned long util = cpu_rq(cpu)->cfs.avg.util_avg;
	unsigned long capacity = capacity_orig_of(cpu);

	return (util >= capacity) ? capacity : util;
}

/*
 * select_task_rq_fair: Select target runqueue for the waking task in domains
 * that have the 'sd_flag' flag set. In practice, this is SD_BALANCE_WAKE,
 * SD_BALANCE_FORK, or SD_BALANCE_EXEC.
 *
 * Balances load by selecting the idlest cpu in the idlest group, or under
 * certain conditions an idle sibling cpu if the domain has SD_WAKE_AFFINE set.
 *
 * Returns the target cpu number.
 *
 * preempt must be disabled.
 */
static int
select_task_rq_fair(struct task_struct *p, int prev_cpu, int sd_flag, int wake_flags)
{
	struct sched_domain *tmp, *affine_sd = NULL, *sd = NULL;
	int cpu = smp_processor_id();
	int new_cpu = prev_cpu;
	int want_affine = 0;
	int sync = wake_flags & WF_SYNC;

	if (p->nr_cpus_allowed == 1)
		return prev_cpu;

	if (sched_enable_hmp)
		return select_best_cpu(p, prev_cpu, 0, sync);

	if (sd_flag & SD_BALANCE_WAKE)
		want_affine = !wake_wide(p) && cpumask_test_cpu(cpu, tsk_cpus_allowed(p));

	rcu_read_lock();
	for_each_domain(cpu, tmp) {
		if (!(tmp->flags & SD_LOAD_BALANCE))
			break;

		/*
		 * If both cpu and prev_cpu are part of this domain,
		 * cpu is a valid SD_WAKE_AFFINE target.
		 */
		if (want_affine && (tmp->flags & SD_WAKE_AFFINE) &&
		    cpumask_test_cpu(prev_cpu, sched_domain_span(tmp))) {
			affine_sd = tmp;
			break;
		}

		if (tmp->flags & sd_flag)
			sd = tmp;
		else if (!want_affine)
			break;
	}

	if (affine_sd) {
		sd = NULL; /* Prefer wake_affine over balance flags */
		if (cpu != prev_cpu && wake_affine(affine_sd, p, sync))
			new_cpu = cpu;
	}

	if (!sd) {
		if (sd_flag & SD_BALANCE_WAKE) /* XXX always ? */
			new_cpu = select_idle_sibling(p, new_cpu);

	} else while (sd) {
		struct sched_group *group;
		int weight;

		if (!(sd->flags & sd_flag)) {
			sd = sd->child;
			continue;
		}

		group = find_idlest_group(sd, p, cpu, sd_flag);
		if (!group) {
			sd = sd->child;
			continue;
		}

		new_cpu = find_idlest_cpu(group, p, cpu);
		if (new_cpu == -1 || new_cpu == cpu) {
			/* Now try balancing at a lower domain level of cpu */
			sd = sd->child;
			continue;
		}

		/* Now try balancing at a lower domain level of new_cpu */
		cpu = new_cpu;
		weight = sd->span_weight;
		sd = NULL;
		for_each_domain(cpu, tmp) {
			if (weight <= tmp->span_weight)
				break;
			if (tmp->flags & sd_flag)
				sd = tmp;
		}
		/* while loop will break here if sd == NULL */
	}
	rcu_read_unlock();

	return new_cpu;
}

/*
 * Called immediately before a task is migrated to a new cpu; task_cpu(p) and
 * cfs_rq_of(p) references at time of call are still valid and identify the
 * previous cpu. The caller guarantees p->pi_lock or task_rq(p)->lock is held.
 */
static void migrate_task_rq_fair(struct task_struct *p, int next_cpu)
{
	/*
	 * We are supposed to update the task to "current" time, then its up to date
	 * and ready to go to new CPU/cfs_rq. But we have difficulty in getting
	 * what current time is, so simply throw away the out-of-date time. This
	 * will result in the wakee task is less decayed, but giving the wakee more
	 * load sounds not bad.
	 */
	remove_entity_load_avg(&p->se);

	/* Tell new CPU we are migrated */
	p->se.avg.last_update_time = 0;

	/* We have migrated, no longer consider this task hot */
	p->se.exec_start = 0;
}

static void task_dead_fair(struct task_struct *p)
{
	remove_entity_load_avg(&p->se);
}
#endif /* CONFIG_SMP */

static unsigned long
wakeup_gran(struct sched_entity *curr, struct sched_entity *se)
{
	unsigned long gran = sysctl_sched_wakeup_granularity;

	/*
	 * Since its curr running now, convert the gran from real-time
	 * to virtual-time in his units.
	 *
	 * By using 'se' instead of 'curr' we penalize light tasks, so
	 * they get preempted easier. That is, if 'se' < 'curr' then
	 * the resulting gran will be larger, therefore penalizing the
	 * lighter, if otoh 'se' > 'curr' then the resulting gran will
	 * be smaller, again penalizing the lighter task.
	 *
	 * This is especially important for buddies when the leftmost
	 * task is higher priority than the buddy.
	 */
	return calc_delta_fair(gran, se);
}

/*
 * Should 'se' preempt 'curr'.
 *
 *             |s1
 *        |s2
 *   |s3
 *         g
 *      |<--->|c
 *
 *  w(c, s1) = -1
 *  w(c, s2) =  0
 *  w(c, s3) =  1
 *
 */
static int
wakeup_preempt_entity(struct sched_entity *curr, struct sched_entity *se)
{
	s64 gran, vdiff = curr->vruntime - se->vruntime;

	if (vdiff <= 0)
		return -1;

	gran = wakeup_gran(curr, se);
	if (vdiff > gran)
		return 1;

	return 0;
}

static void set_last_buddy(struct sched_entity *se)
{
	if (entity_is_task(se) && unlikely(task_of(se)->policy == SCHED_IDLE))
		return;

	for_each_sched_entity(se)
		cfs_rq_of(se)->last = se;
}

static void set_next_buddy(struct sched_entity *se)
{
	if (entity_is_task(se) && unlikely(task_of(se)->policy == SCHED_IDLE))
		return;

	for_each_sched_entity(se)
		cfs_rq_of(se)->next = se;
}

static void set_skip_buddy(struct sched_entity *se)
{
	for_each_sched_entity(se)
		cfs_rq_of(se)->skip = se;
}

/*
 * Preempt the current task with a newly woken task if needed:
 */
static void check_preempt_wakeup(struct rq *rq, struct task_struct *p, int wake_flags)
{
	struct task_struct *curr = rq->curr;
	struct sched_entity *se = &curr->se, *pse = &p->se;
	struct cfs_rq *cfs_rq = task_cfs_rq(curr);
	int scale = cfs_rq->nr_running >= sched_nr_latency;
	int next_buddy_marked = 0;

	if (unlikely(se == pse))
		return;

	/*
	 * This is possible from callers such as attach_tasks(), in which we
	 * unconditionally check_prempt_curr() after an enqueue (which may have
	 * lead to a throttle).  This both saves work and prevents false
	 * next-buddy nomination below.
	 */
	if (unlikely(throttled_hierarchy(cfs_rq_of(pse))))
		return;

	if (sched_feat(NEXT_BUDDY) && scale && !(wake_flags & WF_FORK)) {
		set_next_buddy(pse);
		next_buddy_marked = 1;
	}

	/*
	 * We can come here with TIF_NEED_RESCHED already set from new task
	 * wake up path.
	 *
	 * Note: this also catches the edge-case of curr being in a throttled
	 * group (e.g. via set_curr_task), since update_curr() (in the
	 * enqueue of curr) will have resulted in resched being set.  This
	 * prevents us from potentially nominating it as a false LAST_BUDDY
	 * below.
	 */
	if (test_tsk_need_resched(curr))
		return;

	/* Idle tasks are by definition preempted by non-idle tasks. */
	if (unlikely(curr->policy == SCHED_IDLE) &&
	    likely(p->policy != SCHED_IDLE))
		goto preempt;

	/*
	 * Batch and idle tasks do not preempt non-idle tasks (their preemption
	 * is driven by the tick):
	 */
	if (unlikely(p->policy != SCHED_NORMAL) || !sched_feat(WAKEUP_PREEMPTION))
		return;

	find_matching_se(&se, &pse);
	update_curr(cfs_rq_of(se));
	BUG_ON(!pse);
	if (wakeup_preempt_entity(se, pse) == 1) {
		/*
		 * Bias pick_next to pick the sched entity that is
		 * triggering this preemption.
		 */
		if (!next_buddy_marked)
			set_next_buddy(pse);
		goto preempt;
	}

	return;

preempt:
	resched_curr(rq);
	/*
	 * Only set the backward buddy when the current task is still
	 * on the rq. This can happen when a wakeup gets interleaved
	 * with schedule on the ->pre_schedule() or idle_balance()
	 * point, either of which can * drop the rq lock.
	 *
	 * Also, during early boot the idle thread is in the fair class,
	 * for obvious reasons its a bad idea to schedule back to it.
	 */
	if (unlikely(!se->on_rq || curr == rq->idle))
		return;

	if (sched_feat(LAST_BUDDY) && scale && entity_is_task(se))
		set_last_buddy(se);
}

static struct task_struct *
pick_next_task_fair(struct rq *rq, struct task_struct *prev)
{
	struct cfs_rq *cfs_rq = &rq->cfs;
	struct sched_entity *se;
	struct task_struct *p;
	int new_tasks;

again:
#ifdef CONFIG_FAIR_GROUP_SCHED
	if (!cfs_rq->nr_running)
		goto idle;

	if (prev->sched_class != &fair_sched_class)
		goto simple;

	/*
	 * Because of the set_next_buddy() in dequeue_task_fair() it is rather
	 * likely that a next task is from the same cgroup as the current.
	 *
	 * Therefore attempt to avoid putting and setting the entire cgroup
	 * hierarchy, only change the part that actually changes.
	 */

	do {
		struct sched_entity *curr = cfs_rq->curr;

		/*
		 * Since we got here without doing put_prev_entity() we also
		 * have to consider cfs_rq->curr. If it is still a runnable
		 * entity, update_curr() will update its vruntime, otherwise
		 * forget we've ever seen it.
		 */
		if (curr) {
			if (curr->on_rq)
				update_curr(cfs_rq);
			else
				curr = NULL;

			/*
			 * This call to check_cfs_rq_runtime() will do the
			 * throttle and dequeue its entity in the parent(s).
			 * Therefore the 'simple' nr_running test will indeed
			 * be correct.
			 */
			if (unlikely(check_cfs_rq_runtime(cfs_rq)))
				goto simple;
		}

		se = pick_next_entity(cfs_rq, curr);
		cfs_rq = group_cfs_rq(se);
	} while (cfs_rq);

	p = task_of(se);

	/*
	 * Since we haven't yet done put_prev_entity and if the selected task
	 * is a different task than we started out with, try and touch the
	 * least amount of cfs_rqs.
	 */
	if (prev != p) {
		struct sched_entity *pse = &prev->se;

		while (!(cfs_rq = is_same_group(se, pse))) {
			int se_depth = se->depth;
			int pse_depth = pse->depth;

			if (se_depth <= pse_depth) {
				put_prev_entity(cfs_rq_of(pse), pse);
				pse = parent_entity(pse);
			}
			if (se_depth >= pse_depth) {
				set_next_entity(cfs_rq_of(se), se);
				se = parent_entity(se);
			}
		}

		put_prev_entity(cfs_rq, pse);
		set_next_entity(cfs_rq, se);
	}

	if (hrtick_enabled(rq))
		hrtick_start_fair(rq, p);

	return p;
simple:
	cfs_rq = &rq->cfs;
#endif

	if (!cfs_rq->nr_running)
		goto idle;

	put_prev_task(rq, prev);

	do {
		se = pick_next_entity(cfs_rq, NULL);
		set_next_entity(cfs_rq, se);
		cfs_rq = group_cfs_rq(se);
	} while (cfs_rq);

	p = task_of(se);

	if (hrtick_enabled(rq))
		hrtick_start_fair(rq, p);

	return p;

idle:
	new_tasks = idle_balance(rq);
	/*
	 * Because idle_balance() releases (and re-acquires) rq->lock, it is
	 * possible for any higher priority task to appear. In that case we
	 * must re-start the pick_next_entity() loop.
	 */
	if (new_tasks < 0)
		return RETRY_TASK;

	if (new_tasks > 0)
		goto again;

	return NULL;
}

/*
 * Account for a descheduled task:
 */
static void put_prev_task_fair(struct rq *rq, struct task_struct *prev)
{
	struct sched_entity *se = &prev->se;
	struct cfs_rq *cfs_rq;

	for_each_sched_entity(se) {
		cfs_rq = cfs_rq_of(se);
		put_prev_entity(cfs_rq, se);
	}
}

/*
 * sched_yield() is very simple
 *
 * The magic of dealing with the ->skip buddy is in pick_next_entity.
 */
static void yield_task_fair(struct rq *rq)
{
	struct task_struct *curr = rq->curr;
	struct cfs_rq *cfs_rq = task_cfs_rq(curr);
	struct sched_entity *se = &curr->se;

	/*
	 * Are we the only task in the tree?
	 */
	if (unlikely(rq->nr_running == 1))
		return;

	clear_buddies(cfs_rq, se);

	if (curr->policy != SCHED_BATCH) {
		update_rq_clock(rq);
		/*
		 * Update run-time statistics of the 'current'.
		 */
		update_curr(cfs_rq);
		/*
		 * Tell update_rq_clock() that we've just updated,
		 * so we don't do microscopic update in schedule()
		 * and double the fastpath cost.
		 */
		 rq->skip_clock_update = 1;
	}

	set_skip_buddy(se);
}

static bool yield_to_task_fair(struct rq *rq, struct task_struct *p, bool preempt)
{
	struct sched_entity *se = &p->se;

	/* throttled hierarchies are not runnable */
	if (!se->on_rq || throttled_hierarchy(cfs_rq_of(se)))
		return false;

	/* Tell the scheduler that we'd really like pse to run next. */
	set_next_buddy(se);

	yield_task_fair(rq);

	return true;
}

#ifdef CONFIG_SMP
/**************************************************
 * Fair scheduling class load-balancing methods.
 *
 * BASICS
 *
 * The purpose of load-balancing is to achieve the same basic fairness the
 * per-cpu scheduler provides, namely provide a proportional amount of compute
 * time to each task. This is expressed in the following equation:
 *
 *   W_i,n/P_i == W_j,n/P_j for all i,j                               (1)
 *
 * Where W_i,n is the n-th weight average for cpu i. The instantaneous weight
 * W_i,0 is defined as:
 *
 *   W_i,0 = \Sum_j w_i,j                                             (2)
 *
 * Where w_i,j is the weight of the j-th runnable task on cpu i. This weight
 * is derived from the nice value as per prio_to_weight[].
 *
 * The weight average is an exponential decay average of the instantaneous
 * weight:
 *
 *   W'_i,n = (2^n - 1) / 2^n * W_i,n + 1 / 2^n * W_i,0               (3)
 *
 * C_i is the compute capacity of cpu i, typically it is the
 * fraction of 'recent' time available for SCHED_OTHER task execution. But it
 * can also include other factors [XXX].
 *
 * To achieve this balance we define a measure of imbalance which follows
 * directly from (1):
 *
 *   imb_i,j = max{ avg(W/C), W_i/C_i } - min{ avg(W/C), W_j/C_j }    (4)
 *
 * We them move tasks around to minimize the imbalance. In the continuous
 * function space it is obvious this converges, in the discrete case we get
 * a few fun cases generally called infeasible weight scenarios.
 *
 * [XXX expand on:
 *     - infeasible weights;
 *     - local vs global optima in the discrete case. ]
 *
 *
 * SCHED DOMAINS
 *
 * In order to solve the imbalance equation (4), and avoid the obvious O(n^2)
 * for all i,j solution, we create a tree of cpus that follows the hardware
 * topology where each level pairs two lower groups (or better). This results
 * in O(log n) layers. Furthermore we reduce the number of cpus going up the
 * tree to only the first of the previous level and we decrease the frequency
 * of load-balance at each level inv. proportional to the number of cpus in
 * the groups.
 *
 * This yields:
 *
 *     log_2 n     1     n
 *   \Sum       { --- * --- * 2^i } = O(n)                            (5)
 *     i = 0      2^i   2^i
 *                               `- size of each group
 *         |         |     `- number of cpus doing load-balance
 *         |         `- freq
 *         `- sum over all levels
 *
 * Coupled with a limit on how many tasks we can migrate every balance pass,
 * this makes (5) the runtime complexity of the balancer.
 *
 * An important property here is that each CPU is still (indirectly) connected
 * to every other cpu in at most O(log n) steps:
 *
 * The adjacency matrix of the resulting graph is given by:
 *
 *             log_2 n     
 *   A_i,j = \Union     (i % 2^k == 0) && i / 2^(k+1) == j / 2^(k+1)  (6)
 *             k = 0
 *
 * And you'll find that:
 *
 *   A^(log_2 n)_i,j != 0  for all i,j                                (7)
 *
 * Showing there's indeed a path between every cpu in at most O(log n) steps.
 * The task movement gives a factor of O(m), giving a convergence complexity
 * of:
 *
 *   O(nm log n),  n := nr_cpus, m := nr_tasks                        (8)
 *
 *
 * WORK CONSERVING
 *
 * In order to avoid CPUs going idle while there's still work to do, new idle
 * balancing is more aggressive and has the newly idle cpu iterate up the domain
 * tree itself instead of relying on other CPUs to bring it work.
 *
 * This adds some complexity to both (5) and (8) but it reduces the total idle
 * time.
 *
 * [XXX more?]
 *
 *
 * CGROUPS
 *
 * Cgroups make a horror show out of (2), instead of a simple sum we get:
 *
 *                                s_k,i
 *   W_i,0 = \Sum_j \Prod_k w_k * -----                               (9)
 *                                 S_k
 *
 * Where
 *
 *   s_k,i = \Sum_j w_i,j,k  and  S_k = \Sum_i s_k,i                 (10)
 *
 * w_i,j,k is the weight of the j-th runnable task in the k-th cgroup on cpu i.
 *
 * The big problem is S_k, its a global sum needed to compute a local (W_i)
 * property.
 *
 * [XXX write more on how we solve this.. _after_ merging pjt's patches that
 *      rewrite all of this once again.]
 */ 

static unsigned long __read_mostly max_load_balance_interval = HZ/10;

enum fbq_type { regular, remote, all };

#define LBF_ALL_PINNED	0x01
#define LBF_NEED_BREAK	0x02
#define LBF_DST_PINNED  0x04
#define LBF_SOME_PINNED	0x08
#define LBF_IGNORE_SMALL_TASKS 0x10
#define LBF_EA_ACTIVE_BALANCE 0x20
#define LBF_SCHED_BOOST_ACTIVE_BALANCE 0x40
#define LBF_BIG_TASK_ACTIVE_BALANCE 0x80
#define LBF_HMP_ACTIVE_BALANCE (LBF_EA_ACTIVE_BALANCE | \
				LBF_SCHED_BOOST_ACTIVE_BALANCE | \
				LBF_BIG_TASK_ACTIVE_BALANCE)
#define LBF_IGNORE_BIG_TASKS 0x100
#define LBF_IGNORE_PREFERRED_CLUSTER_TASKS 0x200

struct lb_env {
	struct sched_domain	*sd;

	struct rq		*src_rq;
	int			src_cpu;

	int			dst_cpu;
	struct rq		*dst_rq;

	struct cpumask		*dst_grpmask;
	int			new_dst_cpu;
	enum cpu_idle_type	idle;
	long			imbalance;
	/* The set of CPUs under consideration for load-balancing */
	struct cpumask		*cpus;
	unsigned int		busiest_grp_capacity;
	unsigned int		busiest_nr_running;

	unsigned int		flags;

	unsigned int		loop;
	unsigned int		loop_break;
	unsigned int		loop_max;

	enum fbq_type		fbq_type;
	struct list_head	tasks;
};

static DEFINE_PER_CPU(bool, dbs_boost_needed);
static DEFINE_PER_CPU(int, dbs_boost_load_moved);

/*
 * Is this task likely cache-hot:
 */
static int task_hot(struct task_struct *p, struct lb_env *env)
{
	s64 delta;

	lockdep_assert_held(&env->src_rq->lock);

	if (p->sched_class != &fair_sched_class)
		return 0;

	if (unlikely(p->policy == SCHED_IDLE))
		return 0;

	/*
	 * Buddy candidates are cache hot:
	 */
	if (sched_feat(CACHE_HOT_BUDDY) && env->dst_rq->nr_running &&
			(&p->se == cfs_rq_of(&p->se)->next ||
			 &p->se == cfs_rq_of(&p->se)->last))
		return 1;

	if (sysctl_sched_migration_cost == -1)
		return 1;
	if (sysctl_sched_migration_cost == 0)
		return 0;

	delta = rq_clock_task(env->src_rq) - p->se.exec_start;

	return delta < (s64)sysctl_sched_migration_cost;
}

#ifdef CONFIG_NUMA_BALANCING
/* Returns true if the destination node has incurred more faults */
static bool migrate_improves_locality(struct task_struct *p, struct lb_env *env)
{
	struct numa_group *numa_group = rcu_dereference(p->numa_group);
	int src_nid, dst_nid;

	if (!sched_feat(NUMA_FAVOUR_HIGHER) || !p->numa_faults_memory ||
	    !(env->sd->flags & SD_NUMA)) {
		return false;
	}

	src_nid = cpu_to_node(env->src_cpu);
	dst_nid = cpu_to_node(env->dst_cpu);

	if (src_nid == dst_nid)
		return false;

	if (numa_group) {
		/* Task is already in the group's interleave set. */
		if (node_isset(src_nid, numa_group->active_nodes))
			return false;

		/* Task is moving into the group's interleave set. */
		if (node_isset(dst_nid, numa_group->active_nodes))
			return true;

		return group_faults(p, dst_nid) > group_faults(p, src_nid);
	}

	/* Encourage migration to the preferred node. */
	if (dst_nid == p->numa_preferred_nid)
		return true;

	return task_faults(p, dst_nid) > task_faults(p, src_nid);
}


static bool migrate_degrades_locality(struct task_struct *p, struct lb_env *env)
{
	struct numa_group *numa_group = rcu_dereference(p->numa_group);
	int src_nid, dst_nid;

	if (!sched_feat(NUMA) || !sched_feat(NUMA_RESIST_LOWER))
		return false;

	if (!p->numa_faults_memory || !(env->sd->flags & SD_NUMA))
		return false;

	src_nid = cpu_to_node(env->src_cpu);
	dst_nid = cpu_to_node(env->dst_cpu);

	if (src_nid == dst_nid)
		return false;

	if (numa_group) {
		/* Task is moving within/into the group's interleave set. */
		if (node_isset(dst_nid, numa_group->active_nodes))
			return false;

		/* Task is moving out of the group's interleave set. */
		if (node_isset(src_nid, numa_group->active_nodes))
			return true;

		return group_faults(p, dst_nid) < group_faults(p, src_nid);
	}

	/* Migrating away from the preferred node is always bad. */
	if (src_nid == p->numa_preferred_nid)
		return true;

	return task_faults(p, dst_nid) < task_faults(p, src_nid);
}

#else
static inline bool migrate_improves_locality(struct task_struct *p,
					     struct lb_env *env)
{
	return false;
}

static inline bool migrate_degrades_locality(struct task_struct *p,
					     struct lb_env *env)
{
	return false;
}
#endif

/*
 * can_migrate_task - may task p from runqueue rq be migrated to this_cpu?
 */
static
int can_migrate_task(struct task_struct *p, struct lb_env *env)
{
	int tsk_cache_hot = 0;
	int twf, group_cpus;

	lockdep_assert_held(&env->src_rq->lock);

	/*
	 * We do not migrate tasks that are:
	 * 1) throttled_lb_pair, or
	 * 2) cannot be migrated to this CPU due to cpus_allowed, or
	 * 3) running (obviously), or
	 * 4) are cache-hot on their current CPU.
	 */
	if (throttled_lb_pair(task_group(p), env->src_cpu, env->dst_cpu))
		return 0;

	if (!cpumask_test_cpu(env->dst_cpu, tsk_cpus_allowed(p))) {
		int cpu;

		schedstat_inc(p, se.statistics.nr_failed_migrations_affine);

		env->flags |= LBF_SOME_PINNED;

		/*
		 * Remember if this task can be migrated to any other cpu in
		 * our sched_group. We may want to revisit it if we couldn't
		 * meet load balance goals by pulling other tasks on src_cpu.
		 *
		 * Also avoid computing new_dst_cpu if we have already computed
		 * one in current iteration.
		 */
		if (!env->dst_grpmask || (env->flags & LBF_DST_PINNED))
			return 0;

		/* Prevent to re-select dst_cpu via env's cpus */
		for_each_cpu_and(cpu, env->dst_grpmask, env->cpus) {
			if (cpumask_test_cpu(cpu, tsk_cpus_allowed(p))) {
				env->flags |= LBF_DST_PINNED;
				env->new_dst_cpu = cpu;
				break;
			}
		}

		return 0;
	}

	/* Record that we found atleast one task that could run on dst_cpu */
	env->flags &= ~LBF_ALL_PINNED;

	if (cpu_capacity(env->dst_cpu) > cpu_capacity(env->src_cpu) &&
		nr_big_tasks(env->src_rq) && !is_big_task(p))
		return 0;

	if (env->flags & LBF_IGNORE_SMALL_TASKS && is_small_task(p))
		return 0;

	twf = task_will_fit(p, env->dst_cpu);

	/*
	 * Attempt to not pull tasks that don't fit. We may get lucky and find
	 * one that actually fits.
	 */
	if (env->flags & LBF_IGNORE_BIG_TASKS && !twf)
		return 0;

	if (env->flags & LBF_IGNORE_PREFERRED_CLUSTER_TASKS &&
			!preferred_cluster(cpu_rq(env->dst_cpu)->cluster, p))
		return 0;

	/*
	 * Group imbalance can sometimes cause work to be pulled across groups
	 * even though the group could have managed the imbalance on its own.
	 * Prevent inter-cluster migrations for big tasks when the number of
	 * tasks is lower than the capacity of the group.
	 */
	group_cpus = DIV_ROUND_UP(env->busiest_grp_capacity,
						 SCHED_CAPACITY_SCALE);
	if (!twf && env->busiest_nr_running <= group_cpus)
		return 0;

	if (task_running(env->src_rq, p)) {
		schedstat_inc(p, se.statistics.nr_failed_migrations_running);
		return 0;
	}

	/*
	 * Aggressive migration if:
	 * 1) IDLE or NEWLY_IDLE balance.
	 * 2) destination numa is preferred
	 * 3) task is cache cold, or
	 * 4) too many balance attempts have failed.
	 */
	tsk_cache_hot = task_hot(p, env);
	if (!tsk_cache_hot)
		tsk_cache_hot = migrate_degrades_locality(p, env);

	if (env->idle != CPU_NOT_IDLE ||
	    migrate_improves_locality(p, env) || !tsk_cache_hot ||
	    env->sd->nr_balance_failed > env->sd->cache_nice_tries) {
		if (tsk_cache_hot) {
			schedstat_inc(env->sd, lb_hot_gained[env->idle]);
			schedstat_inc(p, se.statistics.nr_forced_migrations);
		}
		return 1;
	}

	schedstat_inc(p, se.statistics.nr_failed_migrations_hot);
	return 0;
}

/*
 * detach_task() -- detach the task for the migration specified in env
 */
static void detach_task(struct task_struct *p, struct lb_env *env)
{
	lockdep_assert_held(&env->src_rq->lock);

	deactivate_task(env->src_rq, p, DEQUEUE_MIGRATING);
	p->on_rq = TASK_ON_RQ_MIGRATING;
	double_lock_balance(env->src_rq, env->dst_rq);
	set_task_cpu(p, env->dst_cpu);
	double_unlock_balance(env->src_rq, env->dst_rq);
}

/*
 * detach_one_task() -- tries to dequeue exactly one task from env->src_rq, as
 * part of active balancing operations within "domain".
 *
 * Returns a task if successful and NULL otherwise.
 */
static struct task_struct *detach_one_task(struct lb_env *env)
{
	struct task_struct *p, *n;

	lockdep_assert_held(&env->src_rq->lock);

	list_for_each_entry_safe(p, n, &env->src_rq->cfs_tasks, se.group_node) {
		if (!can_migrate_task(p, env))
			continue;

		detach_task(p, env);

		/*
		 * Right now, this is only the second place where
		 * lb_gained[env->idle] is updated (other is detach_tasks)
		 * so we can safely collect stats here rather than
		 * inside detach_tasks().
		 */
		schedstat_inc(env->sd, lb_gained[env->idle]);
		per_cpu(dbs_boost_load_moved, env->dst_cpu) += pct_task_load(p);

		return p;
	}
	return NULL;
}

static const unsigned int sched_nr_migrate_break = 32;

/*
 * detach_tasks() -- tries to detach up to imbalance weighted load from
 * busiest_rq, as part of a balancing operation within domain "sd".
 *
 * Returns number of detached tasks if successful and 0 otherwise.
 */
static int detach_tasks(struct lb_env *env)
{
	struct list_head *tasks = &env->src_rq->cfs_tasks;
	struct task_struct *p;
	unsigned long load;
	int detached = 0;
	int orig_loop = env->loop;

	lockdep_assert_held(&env->src_rq->lock);

	if (env->imbalance <= 0)
		return 0;

	env->flags |= LBF_IGNORE_PREFERRED_CLUSTER_TASKS;
	if (cpu_capacity(env->dst_cpu) > cpu_capacity(env->src_cpu))
		env->flags |= LBF_IGNORE_SMALL_TASKS;
	else if (cpu_capacity(env->dst_cpu) < cpu_capacity(env->src_cpu) &&
							!sched_boost())
		env->flags |= LBF_IGNORE_BIG_TASKS;

redo:
	while (!list_empty(tasks)) {
		/*
		 * We don't want to steal all, otherwise we may be treated likewise,
		 * which could at worst lead to a livelock crash.
		 */
		if (env->idle != CPU_NOT_IDLE && env->src_rq->nr_running <= 1)
			break;

		p = list_first_entry(tasks, struct task_struct, se.group_node);

		env->loop++;
		/* We've more or less seen every task there is, call it quits */
		if (env->loop > env->loop_max)
			break;

		/* take a breather every nr_migrate tasks */
		if (env->loop > env->loop_break) {
			env->loop_break += sched_nr_migrate_break;
			env->flags |= LBF_NEED_BREAK;
			break;
		}

		if (!can_migrate_task(p, env))
			goto next;

		/*
		 * Depending of the number of CPUs and tasks and the
		 * cgroup hierarchy, task_h_load() can return a null
		 * value. Make sure that env->imbalance decreases
		 * otherwise detach_tasks() will stop only after
		 * detaching up to loop_max tasks.
		 */
		load = max_t(unsigned long, task_h_load(p), 1);


		if (sched_feat(LB_MIN) && load < 16 && !env->sd->nr_balance_failed)
			goto next;

		/*
		 * p is not running task when we goes until here, so if p is one
		 * of the 2 task in src cpu rq and not the running one,
		 * that means it is the only task that can be balanced.
		 * So only when there is other tasks can be balanced or
		 * there is situation to ignore big task, it is needed
		 * to skip the task load bigger than 2*imbalance.
		 */
		if (((cpu_rq(env->src_cpu)->nr_running > 2) ||
			(env->flags & LBF_IGNORE_BIG_TASKS)) &&
			((load / 2) > env->imbalance))
			goto next;

		detach_task(p, env);
		list_add(&p->se.group_node, &env->tasks);

		detached++;
		env->imbalance -= load;
		per_cpu(dbs_boost_load_moved, env->dst_cpu) += pct_task_load(p);

#ifdef CONFIG_PREEMPT
		/*
		 * NEWIDLE balancing is a source of latency, so preemptible
		 * kernels will stop after the first task is detached to minimize
		 * the critical section.
		 */
		if (env->idle == CPU_NEWLY_IDLE)
			break;
#endif

		/*
		 * We only want to steal up to the prescribed amount of
		 * weighted load.
		 */
		if (env->imbalance <= 0)
			break;

		continue;
next:
		list_move_tail(&p->se.group_node, tasks);
	}

	if (env->flags & (LBF_IGNORE_SMALL_TASKS | LBF_IGNORE_BIG_TASKS |
LBF_IGNORE_PREFERRED_CLUSTER_TASKS)
							     && !detached) {
		tasks = &env->src_rq->cfs_tasks;
		env->flags &= ~(LBF_IGNORE_SMALL_TASKS | LBF_IGNORE_BIG_TASKS |
LBF_IGNORE_PREFERRED_CLUSTER_TASKS);
		env->loop = orig_loop;
		goto redo;
	}

	/*
	 * Right now, this is one of only two places we collect this stat
	 * so we can safely collect detach_one_task() stats here rather
	 * than inside detach_one_task().
	 */
	schedstat_add(env->sd, lb_gained[env->idle], detached);

	return detached;
}

/*
 * attach_task() -- attach the task detached by detach_task() to its new rq.
 */
static void attach_task(struct rq *rq, struct task_struct *p)
{
	lockdep_assert_held(&rq->lock);

	BUG_ON(task_rq(p) != rq);
	p->on_rq = TASK_ON_RQ_QUEUED;
	activate_task(rq, p, ENQUEUE_MIGRATING);
	check_preempt_curr(rq, p, 0);
	if (task_notify_on_migrate(p))
		per_cpu(dbs_boost_needed, task_cpu(p)) = true;
}

/*
 * attach_one_task() -- attaches the task returned from detach_one_task() to
 * its new rq.
 */
static void attach_one_task(struct rq *rq, struct task_struct *p)
{
	raw_spin_lock(&rq->lock);
	attach_task(rq, p);
	raw_spin_unlock(&rq->lock);
}

/*
 * attach_tasks() -- attaches all tasks detached by detach_tasks() to their
 * new rq.
 */
static void attach_tasks(struct lb_env *env)
{
	struct list_head *tasks = &env->tasks;
	struct task_struct *p;

	raw_spin_lock(&env->dst_rq->lock);

	while (!list_empty(tasks)) {
		p = list_first_entry(tasks, struct task_struct, se.group_node);
		list_del_init(&p->se.group_node);

		attach_task(env->dst_rq, p);
	}

	raw_spin_unlock(&env->dst_rq->lock);
}

#ifdef CONFIG_FAIR_GROUP_SCHED
static void update_blocked_averages(int cpu)
{
	struct rq *rq = cpu_rq(cpu);
	struct cfs_rq *cfs_rq;
	unsigned long flags;

	raw_spin_lock_irqsave(&rq->lock, flags);
	update_rq_clock(rq);
	/*
	 * Iterates the task_group tree in a bottom up fashion, see
	 * list_add_leaf_cfs_rq() for details.
	 */
	for_each_leaf_cfs_rq(rq, cfs_rq) {
		/* throttled entities do not contribute to load */
		if (throttled_hierarchy(cfs_rq))
			continue;

		if (update_cfs_rq_load_avg(cfs_rq_clock_task(cfs_rq), cfs_rq))
			update_tg_load_avg(cfs_rq, 0);
	}

	raw_spin_unlock_irqrestore(&rq->lock, flags);
}

/*
 * Compute the hierarchical load factor for cfs_rq and all its ascendants.
 * This needs to be done in a top-down fashion because the load of a child
 * group is a fraction of its parents load.
 */
static void update_cfs_rq_h_load(struct cfs_rq *cfs_rq)
{
	struct rq *rq = rq_of(cfs_rq);
	struct sched_entity *se = cfs_rq->tg->se[cpu_of(rq)];
	unsigned long now = jiffies;
	unsigned long load;

	if (cfs_rq->last_h_load_update == now)
		return;

	WRITE_ONCE(cfs_rq->h_load_next, NULL);
	for_each_sched_entity(se) {
		cfs_rq = cfs_rq_of(se);
		WRITE_ONCE(cfs_rq->h_load_next, se);
		if (cfs_rq->last_h_load_update == now)
			break;
	}

	if (!se) {
		cfs_rq->h_load = cfs_rq_load_avg(cfs_rq);
		cfs_rq->last_h_load_update = now;
	}

	while ((se = READ_ONCE(cfs_rq->h_load_next)) != NULL) {
		load = cfs_rq->h_load;
		load = div64_ul(load * se->avg.load_avg,
			cfs_rq_load_avg(cfs_rq) + 1);
		cfs_rq = group_cfs_rq(se);
		cfs_rq->h_load = load;
		cfs_rq->last_h_load_update = now;
	}
}

static unsigned long task_h_load(struct task_struct *p)
{
	struct cfs_rq *cfs_rq = task_cfs_rq(p);

	update_cfs_rq_h_load(cfs_rq);
	return div64_ul(p->se.avg.load_avg * cfs_rq->h_load,
			cfs_rq_load_avg(cfs_rq) + 1);
}
#else
static inline void update_blocked_averages(int cpu)
{
	struct rq *rq = cpu_rq(cpu);
	struct cfs_rq *cfs_rq = &rq->cfs;
	unsigned long flags;

	raw_spin_lock_irqsave(&rq->lock, flags);
	update_rq_clock(rq);
	update_cfs_rq_load_avg(cfs_rq_clock_task(cfs_rq), cfs_rq);
	raw_spin_unlock_irqrestore(&rq->lock, flags);
}

static unsigned long task_h_load(struct task_struct *p)
{
	return p->se.avg.load_avg;
}
#endif

/********** Helpers for find_busiest_group ************************/

enum group_type {
	group_other = 0,
	group_ea,
	group_imbalanced,
	group_overloaded,
};

/*
 * sg_lb_stats - stats of a sched_group required for load_balancing
 */
struct sg_lb_stats {
	unsigned long avg_load; /*Avg load across the CPUs of the group */
	unsigned long group_load; /* Total load over the CPUs of the group */
	unsigned long sum_weighted_load; /* Weighted load of group's tasks */
	unsigned long load_per_task;
	unsigned long group_capacity;
	unsigned long group_util; /* Total utilization of the group */
	unsigned int sum_nr_running; /* Nr tasks running in the group */
#ifdef CONFIG_SCHED_HMP
	unsigned long sum_nr_big_tasks, sum_nr_small_tasks;
	u64 group_cpu_load; /* Scaled load of all CPUs of the group */
#endif
	unsigned int idle_cpus;
	unsigned int group_weight;
	enum group_type group_type;
	int group_no_capacity;
#ifdef CONFIG_NUMA_BALANCING
	unsigned int nr_numa_running;
	unsigned int nr_preferred_running;
#endif
};

/*
 * sd_lb_stats - Structure to store the statistics of a sched_domain
 *		 during load balancing.
 */
struct sd_lb_stats {
	struct sched_group *busiest;	/* Busiest group in this sd */
	struct sched_group *local;	/* Local group in this sd */
	unsigned long total_load;	/* Total load of all groups in sd */
	unsigned long total_capacity;	/* Total capacity of all groups in sd */
	unsigned long avg_load;	/* Average load across all groups in sd */

	struct sg_lb_stats busiest_stat;/* Statistics of the busiest group */
	struct sg_lb_stats local_stat;	/* Statistics of the local group */
};

static inline void init_sd_lb_stats(struct sd_lb_stats *sds)
{
	/*
	 * Skimp on the clearing to avoid duplicate work. We can avoid clearing
	 * local_stat because update_sg_lb_stats() does a full clear/assignment.
	 * We must however clear busiest_stat::avg_load because
	 * update_sd_pick_busiest() reads this before assignment.
	 */
	*sds = (struct sd_lb_stats){
		.busiest = NULL,
		.local = NULL,
		.total_load = 0UL,
		.total_capacity = 0UL,
		.busiest_stat = {
			.avg_load = 0UL,
			.sum_nr_running = 0,
			.group_type = group_other,
		},
	};
}

#ifdef CONFIG_SCHED_HMP

static int
bail_inter_cluster_balance(struct lb_env *env, struct sd_lb_stats *sds)
{
	int nr_cpus, local_cpu, busiest_cpu;

	local_cpu = group_first_cpu(sds->local);
	busiest_cpu = group_first_cpu(sds->busiest);

	if (cpu_capacity(local_cpu) <= cpu_capacity(busiest_cpu))
		return 0;

	if (sds->busiest_stat.sum_nr_big_tasks)
		return 0;

	nr_cpus = cpumask_weight(sched_group_cpus(sds->busiest));
	if ((sds->busiest_stat.group_cpu_load < nr_cpus * sched_spill_load) &&
		(sds->busiest_stat.sum_nr_running <
			nr_cpus * sysctl_sched_spill_nr_run))
		return 1;

	return 0;
}

#else	/* CONFIG_SCHED_HMP */

static inline int
bail_inter_cluster_balance(struct lb_env *env, struct sd_lb_stats *sds)
{
	return 0;
}

#endif	/* CONFIG_SCHED_HMP */

/**
 * get_sd_load_idx - Obtain the load index for a given sched domain.
 * @sd: The sched_domain whose load_idx is to be obtained.
 * @idle: The idle status of the CPU for whose sd load_idx is obtained.
 *
 * Return: The load index.
 */
static inline int get_sd_load_idx(struct sched_domain *sd,
					enum cpu_idle_type idle)
{
	int load_idx;

	switch (idle) {
	case CPU_NOT_IDLE:
		load_idx = sd->busy_idx;
		break;

	case CPU_NEWLY_IDLE:
		load_idx = sd->newidle_idx;
		break;
	default:
		load_idx = sd->idle_idx;
		break;
	}

	return load_idx;
}

static unsigned long scale_rt_capacity(int cpu)
{
	struct rq *rq = cpu_rq(cpu);
	u64 total, used, age_stamp, avg;
	s64 delta;

	/*
	 * Since we're reading these variables without serialization make sure
	 * we read them once before doing sanity checks on them.
	 */
	age_stamp = READ_ONCE(rq->age_stamp);
	avg = READ_ONCE(rq->rt_avg);
	delta = __rq_clock_broken(rq) - age_stamp;

	if (unlikely(delta < 0))
		delta = 0;

	total = sched_avg_period() + delta;

	used = div_u64(avg, total);

	/*
	 * deadline bandwidth is defined at system level so we must
	 * weight this bandwidth with the max capacity of the system.
	 * As a reminder, avg_bw is 20bits width and
	 * scale_cpu_capacity is 10 bits width
	 */
	used += div_u64(rq->dl.avg_bw, arch_scale_cpu_capacity(NULL, cpu));

	if (likely(used < SCHED_CAPACITY_SCALE))
		return SCHED_CAPACITY_SCALE - used;

	return 1;
}

static void update_cpu_capacity(struct sched_domain *sd, int cpu)
{
	unsigned long capacity = arch_scale_cpu_capacity(sd, cpu);
	struct sched_group *sdg = sd->groups;

	cpu_rq(cpu)->cpu_capacity_orig = capacity;

	capacity *= scale_rt_capacity(cpu);
	capacity >>= SCHED_CAPACITY_SHIFT;

	if (!capacity)
		capacity = 1;

	cpu_rq(cpu)->cpu_capacity = capacity;
	sdg->sgc->capacity = capacity;
}

void update_group_capacity(struct sched_domain *sd, int cpu)
{
	struct sched_domain *child = sd->child;
	struct sched_group *group, *sdg = sd->groups;
	unsigned long capacity;
	unsigned long interval;

	interval = msecs_to_jiffies(sd->balance_interval);
	interval = clamp(interval, 1UL, max_load_balance_interval);
	sdg->sgc->next_update = jiffies + interval;

	if (!child) {
		update_cpu_capacity(sd, cpu);
		return;
	}

	capacity = 0;

	if (child->flags & SD_OVERLAP) {
		/*
		 * SD_OVERLAP domains cannot assume that child groups
		 * span the current group.
		 */

		for_each_cpu(cpu, sched_group_cpus(sdg)) {
			struct sched_group_capacity *sgc;
			struct rq *rq = cpu_rq(cpu);

			/*
			 * build_sched_domains() -> init_sched_groups_capacity()
			 * gets here before we've attached the domains to the
			 * runqueues.
			 *
			 * Use capacity_of(), which is set irrespective of domains
			 * in update_cpu_capacity().
			 *
			 * This avoids capacity from being 0 and
			 * causing divide-by-zero issues on boot.
			 */
			if (unlikely(!rq->sd)) {
				capacity += capacity_of(cpu);
				continue;
			}

			sgc = rq->sd->groups->sgc;
			capacity += sgc->capacity;
		}
	} else  {
		/*
		 * !SD_OVERLAP domains can assume that child groups
		 * span the current group.
		 */ 

		group = child->groups;
		do {
			capacity += group->sgc->capacity;
			group = group->next;
		} while (group != child->groups);
	}

	sdg->sgc->capacity = capacity;
}

/*
 * Check whether the capacity of the rq has been noticeably reduced by side
 * activity. The imbalance_pct is used for the threshold.
 * Return true is the capacity is reduced
 */
static inline int
check_cpu_capacity(struct rq *rq, struct sched_domain *sd)
{
	return ((rq->cpu_capacity * sd->imbalance_pct) <
				(rq->cpu_capacity_orig * 100));
}

/*
 * Group imbalance indicates (and tries to solve) the problem where balancing
 * groups is inadequate due to tsk_cpus_allowed() constraints.
 *
 * Imagine a situation of two groups of 4 cpus each and 4 tasks each with a
 * cpumask covering 1 cpu of the first group and 3 cpus of the second group.
 * Something like:
 *
 * 	{ 0 1 2 3 } { 4 5 6 7 }
 * 	        *     * * *
 *
 * If we were to balance group-wise we'd place two tasks in the first group and
 * two tasks in the second group. Clearly this is undesired as it will overload
 * cpu 3 and leave one of the cpus in the second group unused.
 *
 * The current solution to this issue is detecting the skew in the first group
 * by noticing the lower domain failed to reach balance and had difficulty
 * moving tasks due to affinity constraints.
 *
 * When this is so detected; this group becomes a candidate for busiest; see
 * update_sd_pick_busiest(). And calculate_imbalance() and
 * find_busiest_group() avoid some of the usual balance conditions to allow it
 * to create an effective group imbalance.
 *
 * This is a somewhat tricky proposition since the next run might not find the
 * group imbalance and decide the groups need to be balanced again. A most
 * subtle and fragile situation.
 */

static inline int sg_imbalanced(struct sched_group *group)
{
	return group->sgc->imbalance;
}

/*
 * group_has_capacity returns true if the group has spare capacity that could
 * be used by some tasks.
 * We consider that a group has spare capacity if the  * number of task is
 * smaller than the number of CPUs or if the utilization is lower than the
 * available capacity for CFS tasks.
 * For the latter, we use a threshold to stabilize the state, to take into
 * account the variance of the tasks' load and to return true if the available
 * capacity in meaningful for the load balancer.
 * As an example, an available capacity of 1% can appear but it doesn't make
 * any benefit for the load balance.
 */
static inline bool
group_has_capacity(struct lb_env *env, struct sg_lb_stats *sgs)
{
	if (sgs->sum_nr_running < sgs->group_weight)
		return true;

	if ((sgs->group_capacity * 100) >
			(sgs->group_util * env->sd->imbalance_pct))
		return true;

	return false;
}

/*
 *  group_is_overloaded returns true if the group has more tasks than it can
 *  handle.
 *  group_is_overloaded is not equals to !group_has_capacity because a group
 *  with the exact right number of tasks, has no more spare capacity but is not
 *  overloaded so both group_has_capacity and group_is_overloaded return
 *  false.
 */
static inline bool
group_is_overloaded(struct lb_env *env, struct sg_lb_stats *sgs)
{
	if (sgs->sum_nr_running <= sgs->group_weight)
		return false;

	if ((sgs->group_capacity * 100) <
			(sgs->group_util * env->sd->imbalance_pct))
		return true;

	return false;
}

static enum group_type group_classify(struct lb_env *env,
		struct sched_group *group,
		struct sg_lb_stats *sgs)
{
	int cpu;

	if (sgs->group_no_capacity) {
		env->flags &= ~LBF_EA_ACTIVE_BALANCE;
		return group_overloaded;
	}

	if (sg_imbalanced(group)) {
		env->flags &= ~LBF_EA_ACTIVE_BALANCE;
		return group_imbalanced;
	}


	/* Mark a less power-efficient CPU as busy only if we haven't
	 * seen a busy group yet and we are close to throttling. We want to
	 * prioritize spreading work over power optimization.
	 */
	cpu = group_first_cpu(group);
	if (sysctl_sched_enable_power_aware &&
	    (cpu_capacity(env->dst_cpu) == cpu_capacity(cpu)) &&
	    sgs->sum_nr_running && (env->idle != CPU_NOT_IDLE) &&
	    power_cost_at_freq(env->dst_cpu, 0) <
	    power_cost_at_freq(cpu, 0) &&
	    !is_task_migration_throttled(cpu_rq(cpu)->curr) &&
	    is_cpu_throttling_imminent(cpu)) {
		env->flags |= LBF_EA_ACTIVE_BALANCE;
		return group_ea;
	}

	return group_other;
}

/**
 * update_sg_lb_stats - Update sched_group's statistics for load balancing.
 * @env: The load balancing environment.
 * @group: sched_group whose statistics are to be updated.
 * @load_idx: Load index of sched_domain of this_cpu for load calc.
 * @local_group: Does group contain this_cpu.
 * @sgs: variable to hold the statistics for this group.
 * @overload: Indicate more than one runnable task for any CPU.
 */
static inline void update_sg_lb_stats(struct lb_env *env,
			struct sched_group *group, int load_idx,
			int local_group, struct sg_lb_stats *sgs,
			bool *overload)
{
	unsigned long load;
	int i, nr_running;

	memset(sgs, 0, sizeof(*sgs));

	for_each_cpu_and(i, sched_group_cpus(group), env->cpus) {
		struct rq *rq = cpu_rq(i);

		trace_sched_cpu_load(cpu_rq(i), idle_cpu(i),
				     mostly_idle_cpu(i),
				     sched_irqload(i),
				     power_cost_at_freq(i, 0),
				     cpu_temp(i));

		/* Bias balancing toward cpus of our domain */
		if (local_group)
			load = target_load(i, load_idx);
		else
			load = source_load(i, load_idx);

		sgs->group_load += load;
		sgs->group_util += cpu_util(i);
		sgs->sum_nr_running += rq->cfs.h_nr_running;

		nr_running = rq->nr_running;
		if (nr_running > 1)
			*overload = true;

#ifdef CONFIG_SCHED_HMP
		sgs->sum_nr_big_tasks += rq->hmp_stats.nr_big_tasks;
		sgs->sum_nr_small_tasks += rq->hmp_stats.nr_small_tasks;
		sgs->group_cpu_load += cpu_load(i);
#endif

#ifdef CONFIG_NUMA_BALANCING
		sgs->nr_numa_running += rq->nr_numa_running;
		sgs->nr_preferred_running += rq->nr_preferred_running;
#endif
		sgs->sum_weighted_load += weighted_cpuload(i);
		/*
		 * No need to call idle_cpu() if nr_running is not 0
		 */
		if (!nr_running && idle_cpu(i))
			sgs->idle_cpus++;
	}

	/* Adjust by relative CPU capacity of the group */
	sgs->group_capacity = group->sgc->capacity;
	sgs->avg_load = (sgs->group_load*SCHED_CAPACITY_SCALE) / sgs->group_capacity;

	if (sgs->sum_nr_running)
		sgs->load_per_task = sgs->sum_weighted_load / sgs->sum_nr_running;

	sgs->group_weight = group->group_weight;

	sgs->group_no_capacity = group_is_overloaded(env, sgs);
	sgs->group_type = group_classify(env, group, sgs);
}

#ifdef CONFIG_SCHED_HMP
static bool update_sd_pick_busiest_active_balance(struct lb_env *env,
						  struct sd_lb_stats *sds,
						  struct sched_group *sg,
						  struct sg_lb_stats *sgs)
{
	if (env->idle != CPU_NOT_IDLE &&
	    cpu_capacity(env->dst_cpu) > group_rq_capacity(sg)) {
		if (sched_boost() && !sds->busiest && sgs->sum_nr_running) {
			env->flags |= LBF_SCHED_BOOST_ACTIVE_BALANCE;
			return true;
		}

		if (sgs->sum_nr_big_tasks >
				sds->busiest_stat.sum_nr_big_tasks) {
			env->flags |= LBF_BIG_TASK_ACTIVE_BALANCE;
			return true;
		}
	}

	return false;
}
#else
static bool update_sd_pick_busiest_active_balance(struct lb_env *env,
						  struct sd_lb_stats *sds,
						  struct sched_group *sg,
						  struct sg_lb_stats *sgs)
{
	return false;
}
#endif

/**
 * update_sd_pick_busiest - return 1 on busiest group
 * @env: The load balancing environment.
 * @sds: sched_domain statistics
 * @sg: sched_group candidate to be checked for being the busiest
 * @sgs: sched_group statistics
 *
 * Determine if @sg is a busier group than the previously selected
 * busiest group.
 *
 * Return: %true if @sg is a busier group than the previously selected
 * busiest group. %false otherwise.
 */
static bool update_sd_pick_busiest(struct lb_env *env,
				   struct sd_lb_stats *sds,
				   struct sched_group *sg,
				   struct sg_lb_stats *sgs)
{
	struct sg_lb_stats *busiest = &sds->busiest_stat;

	if (update_sd_pick_busiest_active_balance(env, sds, sg, sgs))
		return true;

	if (sgs->group_type > busiest->group_type)
		return true;

	if (sgs->group_type < busiest->group_type) {
		if (sgs->group_type == group_ea)
			env->flags &= ~LBF_EA_ACTIVE_BALANCE;
		return false;
	}

	if (env->flags & LBF_EA_ACTIVE_BALANCE) {
		if (power_cost_at_freq(group_first_cpu(sg), 0) <=
		    power_cost_at_freq(group_first_cpu(sds->busiest), 0))
			return false;

		return true;
	}

	if (sgs->avg_load <= busiest->avg_load)
		return false;

	/* This is the busiest node in its class. */
	if (!(env->sd->flags & SD_ASYM_PACKING))
		return true;

	/*
	 * ASYM_PACKING needs to move all the work to the lowest
	 * numbered CPUs in the group, therefore mark all groups
	 * higher than ourself as busy.
	 */
	if (sgs->sum_nr_running && env->dst_cpu < group_first_cpu(sg)) {
		if (!sds->busiest)
			return true;

		if (group_first_cpu(sds->busiest) > group_first_cpu(sg))
			return true;
	}

	return false;
}

#ifdef CONFIG_NUMA_BALANCING
static inline enum fbq_type fbq_classify_group(struct sg_lb_stats *sgs)
{
	if (sgs->sum_nr_running > sgs->nr_numa_running)
		return regular;
	if (sgs->sum_nr_running > sgs->nr_preferred_running)
		return remote;
	return all;
}

static inline enum fbq_type fbq_classify_rq(struct rq *rq)
{
	if (rq->nr_running > rq->nr_numa_running)
		return regular;
	if (rq->nr_running > rq->nr_preferred_running)
		return remote;
	return all;
}
#else
static inline enum fbq_type fbq_classify_group(struct sg_lb_stats *sgs)
{
	return all;
}

static inline enum fbq_type fbq_classify_rq(struct rq *rq)
{
	return regular;
}
#endif /* CONFIG_NUMA_BALANCING */

/**
 * update_sd_lb_stats - Update sched_domain's statistics for load balancing.
 * @env: The load balancing environment.
 * @sds: variable to hold the statistics for this sched_domain.
 */
static inline void update_sd_lb_stats(struct lb_env *env, struct sd_lb_stats *sds)
{
	struct sched_domain *child = env->sd->child;
	struct sched_group *sg = env->sd->groups;
	struct sg_lb_stats tmp_sgs;
	int load_idx, prefer_sibling = 0;
	bool overload = false;

	if (child && child->flags & SD_PREFER_SIBLING)
		prefer_sibling = 1;

	load_idx = get_sd_load_idx(env->sd, env->idle);

	do {
		struct sg_lb_stats *sgs = &tmp_sgs;
		int local_group;

		local_group = cpumask_test_cpu(env->dst_cpu, sched_group_cpus(sg));
		if (local_group) {
			sds->local = sg;
			sgs = &sds->local_stat;

			if (env->idle != CPU_NEWLY_IDLE ||
			    time_after_eq(jiffies, sg->sgc->next_update))
				update_group_capacity(env->sd, env->dst_cpu);
		}

		update_sg_lb_stats(env, sg, load_idx, local_group, sgs,
						&overload);

		if (local_group)
			goto next_group;

		/*
		 * In case the child domain prefers tasks go to siblings
		 * first, lower the sg capacity so that we'll try
		 * and move all the excess tasks away. We lower the capacity
		 * of a group only if the local group has the capacity to fit
		 * these excess tasks. The extra check prevents the case where
		 * you always pull from the heaviest group when it is already
		 * under-utilized (possible with a large weight task outweighs
		 * the tasks on the system).
		 */
		if (prefer_sibling && sds->local &&
		    group_has_capacity(env, &sds->local_stat) &&
		    (sgs->sum_nr_running > 1)) {
			sgs->group_no_capacity = 1;
			sgs->group_type = group_overloaded;
		}

		if (update_sd_pick_busiest(env, sds, sg, sgs)) {
			sds->busiest = sg;
			sds->busiest_stat = *sgs;
			env->busiest_nr_running = sgs->sum_nr_running;
			env->busiest_grp_capacity = sgs->group_capacity;
		}

next_group:
		/* Now, start updating sd_lb_stats */
		sds->total_load += sgs->group_load;
		sds->total_capacity += sgs->group_capacity;

		sg = sg->next;
	} while (sg != env->sd->groups);

	if (env->sd->flags & SD_NUMA)
		env->fbq_type = fbq_classify_group(&sds->busiest_stat);

	if (!env->sd->parent) {
		/* update overload indicator if we are at root domain */
		if (env->dst_rq->rd->overload != overload)
			env->dst_rq->rd->overload = overload;
	}

}

/**
 * check_asym_packing - Check to see if the group is packed into the
 *			sched doman.
 *
 * This is primarily intended to used at the sibling level.  Some
 * cores like POWER7 prefer to use lower numbered SMT threads.  In the
 * case of POWER7, it can move to lower SMT modes only when higher
 * threads are idle.  When in lower SMT modes, the threads will
 * perform better since they share less core resources.  Hence when we
 * have idle threads, we want them to be the higher ones.
 *
 * This packing function is run on idle threads.  It checks to see if
 * the busiest CPU in this domain (core in the P7 case) has a higher
 * CPU number than the packing function is being run on.  Here we are
 * assuming lower CPU number will be equivalent to lower a SMT thread
 * number.
 *
 * Return: 1 when packing is required and a task should be moved to
 * this CPU.  The amount of the imbalance is returned in *imbalance.
 *
 * @env: The load balancing environment.
 * @sds: Statistics of the sched_domain which is to be packed
 */
static int check_asym_packing(struct lb_env *env, struct sd_lb_stats *sds)
{
	int busiest_cpu;

	if (!(env->sd->flags & SD_ASYM_PACKING))
		return 0;

	if (!sds->busiest)
		return 0;

	busiest_cpu = group_first_cpu(sds->busiest);
	if (env->dst_cpu > busiest_cpu)
		return 0;

	env->imbalance = DIV_ROUND_CLOSEST(
		sds->busiest_stat.avg_load * sds->busiest_stat.group_capacity,
		SCHED_CAPACITY_SCALE);

	return 1;
}

/**
 * fix_small_imbalance - Calculate the minor imbalance that exists
 *			amongst the groups of a sched_domain, during
 *			load balancing.
 * @env: The load balancing environment.
 * @sds: Statistics of the sched_domain whose imbalance is to be calculated.
 */
static inline
void fix_small_imbalance(struct lb_env *env, struct sd_lb_stats *sds)
{
	unsigned long tmp, capa_now = 0, capa_move = 0;
	unsigned int imbn = 2;
	unsigned long scaled_busy_load_per_task;
	struct sg_lb_stats *local, *busiest;

	local = &sds->local_stat;
	busiest = &sds->busiest_stat;

	if (!local->sum_nr_running)
		local->load_per_task = cpu_avg_load_per_task(env->dst_cpu);
	else if (busiest->load_per_task > local->load_per_task)
		imbn = 1;

	scaled_busy_load_per_task =
		(busiest->load_per_task * SCHED_CAPACITY_SCALE) /
		busiest->group_capacity;

	if (busiest->avg_load + scaled_busy_load_per_task >=
	    local->avg_load + (scaled_busy_load_per_task * imbn)) {
		env->imbalance = busiest->load_per_task;
		return;
	}

	/*
	 * OK, we don't have enough imbalance to justify moving tasks,
	 * however we may be able to increase total CPU capacity used by
	 * moving them.
	 */

	capa_now += busiest->group_capacity *
			min(busiest->load_per_task, busiest->avg_load);
	capa_now += local->group_capacity *
			min(local->load_per_task, local->avg_load);
	capa_now /= SCHED_CAPACITY_SCALE;

	/* Amount of load we'd subtract */
	if (busiest->avg_load > scaled_busy_load_per_task) {
		capa_move += busiest->group_capacity *
			    min(busiest->load_per_task,
				busiest->avg_load - scaled_busy_load_per_task);
	}

	/* Amount of load we'd add */
	if (busiest->avg_load * busiest->group_capacity <
	    busiest->load_per_task * SCHED_CAPACITY_SCALE) {
		tmp = (busiest->avg_load * busiest->group_capacity) /
		      local->group_capacity;
	} else {
		tmp = (busiest->load_per_task * SCHED_CAPACITY_SCALE) /
		      local->group_capacity;
	}
	capa_move += local->group_capacity *
		    min(local->load_per_task, local->avg_load + tmp);
	capa_move /= SCHED_CAPACITY_SCALE;

	/* Move if we gain throughput */
	if (capa_move > capa_now)
		env->imbalance = busiest->load_per_task;
}

/**
 * calculate_imbalance - Calculate the amount of imbalance present within the
 *			 groups of a given sched_domain during load balance.
 * @env: load balance environment
 * @sds: statistics of the sched_domain whose imbalance is to be calculated.
 */
static inline void calculate_imbalance(struct lb_env *env, struct sd_lb_stats *sds)
{
	unsigned long max_pull, load_above_capacity = ~0UL;
	struct sg_lb_stats *local, *busiest;

	local = &sds->local_stat;
	busiest = &sds->busiest_stat;

	if (busiest->group_type == group_imbalanced) {
		/*
		 * In the group_imb case we cannot rely on group-wide averages
		 * to ensure cpu-load equilibrium, look at wider averages. XXX
		 */
		busiest->load_per_task =
			min(busiest->load_per_task, sds->avg_load);
	}

	/*
	 * In the presence of smp nice balancing, certain scenarios can have
	 * max load less than avg load(as we skip the groups at or below
	 * its cpu_capacity, while calculating max_load..)
	 */
	if (busiest->avg_load <= sds->avg_load ||
	    local->avg_load >= sds->avg_load) {
		env->imbalance = 0;
		return fix_small_imbalance(env, sds);
	}

	/*
	 * If there aren't any idle cpus, avoid creating some.
	 */
	if (busiest->group_type == group_overloaded &&
	    local->group_type   == group_overloaded) {
		load_above_capacity = busiest->sum_nr_running *
					SCHED_LOAD_SCALE;
		if (load_above_capacity > busiest->group_capacity)
			load_above_capacity -= busiest->group_capacity;
		else
			load_above_capacity = ~0UL;
	}

	/*
	 * We're trying to get all the cpus to the average_load, so we don't
	 * want to push ourselves above the average load, nor do we wish to
	 * reduce the max loaded cpu below the average load. At the same time,
	 * we also don't want to reduce the group load below the group capacity
	 * (so that we can implement power-savings policies etc). Thus we look
	 * for the minimum possible imbalance.
	 */
	max_pull = min(busiest->avg_load - sds->avg_load, load_above_capacity);

	/* How much load to actually move to equalise the imbalance */
	env->imbalance = min(
		max_pull * busiest->group_capacity,
		(sds->avg_load - local->avg_load) * local->group_capacity
	) / SCHED_CAPACITY_SCALE;

	/*
	 * if *imbalance is less than the average load per runnable task
	 * there is no guarantee that any tasks will be moved so we'll have
	 * a think about bumping its value to force at least one task to be
	 * moved
	 */
	if (env->imbalance < busiest->load_per_task)
		return fix_small_imbalance(env, sds);
}

/******* find_busiest_group() helpers end here *********************/

/**
 * find_busiest_group - Returns the busiest group within the sched_domain
 * if there is an imbalance. If there isn't an imbalance, and
 * the user has opted for power-savings, it returns a group whose
 * CPUs can be put to idle by rebalancing those tasks elsewhere, if
 * such a group exists.
 *
 * Also calculates the amount of weighted load which should be moved
 * to restore balance.
 *
 * @env: The load balancing environment.
 *
 * Return:	- The busiest group if imbalance exists.
 *		- If no imbalance and user has opted for power-savings balance,
 *		   return the least loaded group whose CPUs can be
 *		   put to idle by rebalancing its tasks onto our group.
 */
static struct sched_group *find_busiest_group(struct lb_env *env)
{
	struct sg_lb_stats *local, *busiest;
	struct sd_lb_stats sds;

	init_sd_lb_stats(&sds);

	/*
	 * Compute the various statistics relavent for load balancing at
	 * this level.
	 */
	update_sd_lb_stats(env, &sds);
	local = &sds.local_stat;
	busiest = &sds.busiest_stat;

	/* ASYM feature bypasses nice load balance check */
	if ((env->idle == CPU_IDLE || env->idle == CPU_NEWLY_IDLE) &&
	    check_asym_packing(env, &sds))
		return sds.busiest;

	/* There is no busy sibling group to pull tasks from */
	if (!sds.busiest || busiest->sum_nr_running == 0)
		goto out_balanced;

	if (env->flags & LBF_HMP_ACTIVE_BALANCE)
		goto force_balance;

	if (bail_inter_cluster_balance(env, &sds))
		goto out_balanced;

	sds.avg_load = (SCHED_CAPACITY_SCALE * sds.total_load)
						/ sds.total_capacity;

	/*
	 * If the busiest group is imbalanced the below checks don't
	 * work because they assume all things are equal, which typically
	 * isn't true due to cpus_allowed constraints and the like.
	 */
	if (busiest->group_type == group_imbalanced)
		goto force_balance;

	/* SD_BALANCE_NEWIDLE trumps SMP nice when underutilized */
	if (env->idle == CPU_NEWLY_IDLE && group_has_capacity(env, local) &&
	    busiest->group_no_capacity)
		goto force_balance;

	/*
	 * If the local group is busier than the selected busiest group
	 * don't try and pull any tasks.
	 */
	if (local->avg_load >= busiest->avg_load)
		goto out_balanced;

	/*
	 * Don't pull any tasks if this group is already above the domain
	 * average load.
	 */
	if (local->avg_load >= sds.avg_load)
		goto out_balanced;

	if (env->idle == CPU_IDLE) {
		/*
		 * This cpu is idle. If the busiest group is not overloaded
		 * and there is no imbalance between this and busiest group
		 * wrt idle cpus, it is balanced. The imbalance becomes
		 * significant if the diff is greater than 1 otherwise we
		 * might end up to just move the imbalance on another group
		 */
		if ((busiest->group_type != group_overloaded) &&
				(local->idle_cpus <= (busiest->idle_cpus + 1)))
			goto out_balanced;
	} else {
		/*
		 * In the CPU_NEWLY_IDLE, CPU_NOT_IDLE cases, use
		 * imbalance_pct to be conservative.
		 */
		if (100 * busiest->avg_load <=
				env->sd->imbalance_pct * local->avg_load)
			goto out_balanced;
	}

force_balance:
	/* Looks like there is an imbalance. Compute it */
	calculate_imbalance(env, &sds);
	return sds.busiest;

out_balanced:
	env->imbalance = 0;
	return NULL;
}

#ifdef CONFIG_SCHED_HMP
static struct rq *find_busiest_queue_hmp(struct lb_env *env,
				     struct sched_group *group)
{
	struct rq *busiest = NULL, *busiest_big = NULL;
	u64 max_runnable_avg = 0, max_runnable_avg_big = 0;
	int max_nr_big = 0, nr_big;
	bool find_big = !!(env->flags & LBF_BIG_TASK_ACTIVE_BALANCE);
	int i;

	for_each_cpu(i, sched_group_cpus(group)) {
		struct rq *rq = cpu_rq(i);
		u64 cumulative_runnable_avg =
				rq->hmp_stats.cumulative_runnable_avg;

		if (!cpumask_test_cpu(i, env->cpus))
			continue;


		if (find_big) {
			nr_big = nr_big_tasks(rq);
			if (nr_big > max_nr_big ||
			    (nr_big > 0 && nr_big == max_nr_big &&
			     cumulative_runnable_avg > max_runnable_avg_big)) {
				max_runnable_avg_big = cumulative_runnable_avg;
				busiest_big = rq;
				max_nr_big = nr_big;
				continue;
			}
		}

		if (cumulative_runnable_avg > max_runnable_avg) {
			max_runnable_avg = cumulative_runnable_avg;
			busiest = rq;
		}
	}

	if (busiest_big)
		return busiest_big;

	env->flags &= ~LBF_BIG_TASK_ACTIVE_BALANCE;
	return busiest;
}
#else
static inline struct rq *find_busiest_queue_hmp(struct lb_env *env,
                                    struct sched_group *group)
{
	return NULL;
}
#endif

/*
 * find_busiest_queue - find the busiest runqueue among the cpus in group.
 */
static struct rq *find_busiest_queue(struct lb_env *env,
				     struct sched_group *group)
{
	struct rq *busiest = NULL, *rq;
	unsigned long busiest_load = 0, busiest_capacity = 1;
	int i;

	if (sched_enable_hmp)
		return find_busiest_queue_hmp(env, group);

	for_each_cpu_and(i, sched_group_cpus(group), env->cpus) {
		unsigned long capacity, wl;
		enum fbq_type rt;

		rq = cpu_rq(i);
		rt = fbq_classify_rq(rq);

		/*
		 * We classify groups/runqueues into three groups:
		 *  - regular: there are !numa tasks
		 *  - remote:  there are numa tasks that run on the 'wrong' node
		 *  - all:     there is no distinction
		 *
		 * In order to avoid migrating ideally placed numa tasks,
		 * ignore those when there's better options.
		 *
		 * If we ignore the actual busiest queue to migrate another
		 * task, the next balance pass can still reduce the busiest
		 * queue by moving tasks around inside the node.
		 *
		 * If we cannot move enough load due to this classification
		 * the next pass will adjust the group classification and
		 * allow migration of more tasks.
		 *
		 * Both cases only affect the total convergence complexity.
		 */
		if (rt > env->fbq_type)
			continue;

		capacity = capacity_of(i);

		wl = weighted_cpuload(i);

		/*
		 * When comparing with imbalance, use weighted_cpuload()
		 * which is not scaled with the cpu capacity.
		 */
		if (rq->nr_running == 1 && wl > env->imbalance &&
		    !check_cpu_capacity(rq, env->sd))
			continue;

		/*
		 * For the load comparisons with the other cpu's, consider
		 * the weighted_cpuload() scaled with the cpu capacity, so
		 * that the load can be moved away from the cpu that is
		 * potentially running at a lower capacity.
		 *
		 * Thus we're looking for max(wl_i / capacity_i), crosswise
		 * multiplication to rid ourselves of the division works out
		 * to: wl_i * capacity_j > wl_j * capacity_i;  where j is
		 * our previous maximum.
		 */
		if (wl * busiest_capacity > busiest_load * capacity) {
			busiest_load = wl;
			busiest_capacity = capacity;
			busiest = rq;
		}
	}

	return busiest;
}

/*
 * Max backoff if we encounter pinned tasks. Pretty arbitrary value, but
 * so long as it is large enough.
 */
#define MAX_PINNED_INTERVAL	16

/* Working cpumask for load_balance and load_balance_newidle. */
DEFINE_PER_CPU(cpumask_var_t, load_balance_mask);

#define NEED_ACTIVE_BALANCE_THRESHOLD 10

static int need_active_balance(struct lb_env *env)
{
	struct sched_domain *sd = env->sd;

	if (env->flags & LBF_HMP_ACTIVE_BALANCE)
		return 1;

	if (env->idle == CPU_NEWLY_IDLE) {

		/*
		 * ASYM_PACKING needs to force migrate tasks from busy but
		 * higher numbered CPUs in order to pack all tasks in the
		 * lowest numbered CPUs.
		 */
		if ((sd->flags & SD_ASYM_PACKING) && env->src_cpu > env->dst_cpu)
			return 1;
	}

	/*
	 * The dst_cpu is idle and the src_cpu CPU has only 1 CFS task.
	 * It's worth migrating the task if the src_cpu's capacity is reduced
	 * because of other sched_class or IRQs if more capacity stays
	 * available on dst_cpu.
	 * Avoid pulling the CFS task if it is the only task running.
	 */
	if ((env->idle != CPU_NOT_IDLE) &&
	    (env->src_rq->nr_running > 1) &&
	    (env->src_rq->cfs.h_nr_running == 1)) {
		if ((check_cpu_capacity(env->src_rq, sd)) &&
		    (capacity_of(env->src_cpu)*sd->imbalance_pct < capacity_of(env->dst_cpu)*100))
			return 1;
	}

	return unlikely(sd->nr_balance_failed >
			sd->cache_nice_tries + NEED_ACTIVE_BALANCE_THRESHOLD);
}

static int should_we_balance(struct lb_env *env)
{
	struct sched_group *sg = env->sd->groups;
	struct cpumask *sg_cpus, *sg_mask;
	int cpu, balance_cpu = -1;

	/*
	 * In the newly idle case, we will allow all the cpu's
	 * to do the newly idle load balance.
	 *
	 * However, we bail out if we already have tasks, to optimize
	 * wakeup latency.
	 */
	if (env->idle == CPU_NEWLY_IDLE) {
		if (env->dst_rq->nr_running > 0)
			return 0;
		return 1;
	}

	sg_cpus = sched_group_cpus(sg);
	sg_mask = sched_group_mask(sg);
	/* Try to find first idle cpu */
	for_each_cpu_and(cpu, sg_cpus, env->cpus) {
		if (!cpumask_test_cpu(cpu, sg_mask) || !idle_cpu(cpu))
			continue;

		balance_cpu = cpu;
		break;
	}

	if (balance_cpu == -1)
		balance_cpu = group_balance_cpu(sg);

	/*
	 * First idle cpu or the first cpu(busiest) in this sched group
	 * is eligible for doing load balancing at this and above domains.
	 */
	return balance_cpu == env->dst_cpu;
}

/*
 * Check this_cpu to ensure it is balanced within domain. Attempt to move
 * tasks if there is an imbalance.
 */
static int load_balance(int this_cpu, struct rq *this_rq,
			struct sched_domain *sd, enum cpu_idle_type idle,
			int *continue_balancing)
{
	int ld_moved = 0, cur_ld_moved, active_balance = 0;
	struct sched_domain *sd_parent = sd->parent;
	struct sched_group *group = NULL;
	struct rq *busiest = NULL;
	unsigned long flags;
	struct cpumask *cpus = this_cpu_cpumask_var_ptr(load_balance_mask);

	struct lb_env env = {
		.sd		= sd,
		.dst_cpu	= this_cpu,
		.dst_rq		= this_rq,
		.dst_grpmask    = sched_group_cpus(sd->groups),
		.idle		= idle,
		.loop_break	= sched_nr_migrate_break,
		.cpus		= cpus,
		.fbq_type	= all,
		.tasks		= LIST_HEAD_INIT(env.tasks),
		.imbalance	= 0,
		.flags		= 0,
		.loop		= 0,
		.busiest_nr_running     = 0,
		.busiest_grp_capacity   = 0,
	};

	/*
	 * For NEWLY_IDLE load_balancing, we don't need to consider
	 * other cpus in our group
	 */
	if (idle == CPU_NEWLY_IDLE)
		env.dst_grpmask = NULL;

	cpumask_copy(cpus, cpu_active_mask);

	per_cpu(dbs_boost_load_moved, this_cpu) = 0;
	schedstat_inc(sd, lb_count[idle]);

redo:
	if (!should_we_balance(&env)) {
		*continue_balancing = 0;
		goto out_balanced;
	}

	group = find_busiest_group(&env);
	if (!group) {
		schedstat_inc(sd, lb_nobusyg[idle]);
		goto out_balanced;
	}

	busiest = find_busiest_queue(&env, group);
	if (!busiest) {
		schedstat_inc(sd, lb_nobusyq[idle]);
		goto out_balanced;
	}

	BUG_ON(busiest == env.dst_rq);

	schedstat_add(sd, lb_imbalance[idle], env.imbalance);

	env.src_cpu = busiest->cpu;
	env.src_rq = busiest;
    
	ld_moved = 0;
	if (busiest->nr_running > 1) {
		/*
		 * Attempt to move tasks. If find_busiest_group has found
		 * an imbalance but busiest->nr_running <= 1, the group is
		 * still unbalanced. ld_moved simply stays zero, so it is
		 * correctly treated as an imbalance.
		 */
		env.flags |= LBF_ALL_PINNED;
		env.loop_max  = min(sysctl_sched_nr_migrate, busiest->nr_running);

more_balance:
		raw_spin_lock_irqsave(&busiest->lock, flags);

		/* The world might have changed. Validate assumptions */
		if (busiest->nr_running <= 1) {
			raw_spin_unlock_irqrestore(&busiest->lock, flags);
			env.flags &= ~LBF_ALL_PINNED;
			goto no_move;
		}

		/*
		 * cur_ld_moved - load moved in current iteration
		 * ld_moved     - cumulative load moved across iterations
		 */
		cur_ld_moved = detach_tasks(&env);

		/*
		 * We've detached some tasks from busiest_rq. Every
		 * task is masked "TASK_ON_RQ_MIGRATING", so we can safely
		 * unlock busiest->lock, and we are able to be sure
		 * that nobody can manipulate the tasks in parallel.
		 * See task_rq_lock() family for the details.
		 */

		raw_spin_unlock(&busiest->lock);

		if (cur_ld_moved) {
			attach_tasks(&env);
			ld_moved += cur_ld_moved;
		}

		local_irq_restore(flags);

		if (env.flags & LBF_NEED_BREAK) {
			env.flags &= ~LBF_NEED_BREAK;
			goto more_balance;
		}

		/*
		 * Revisit (affine) tasks on src_cpu that couldn't be moved to
		 * us and move them to an alternate dst_cpu in our sched_group
		 * where they can run. The upper limit on how many times we
		 * iterate on same src_cpu is dependent on number of cpus in our
		 * sched_group.
		 *
		 * This changes load balance semantics a bit on who can move
		 * load to a given_cpu. In addition to the given_cpu itself
		 * (or a ilb_cpu acting on its behalf where given_cpu is
		 * nohz-idle), we now have balance_cpu in a position to move
		 * load to given_cpu. In rare situations, this may cause
		 * conflicts (balance_cpu and given_cpu/ilb_cpu deciding
		 * _independently_ and at _same_ time to move some load to
		 * given_cpu) causing exceess load to be moved to given_cpu.
		 * This however should not happen so much in practice and
		 * moreover subsequent load balance cycles should correct the
		 * excess load moved.
		 */
		if ((env.flags & LBF_DST_PINNED) && env.imbalance > 0) {

			/* Prevent to re-select dst_cpu via env's cpus */
			cpumask_clear_cpu(env.dst_cpu, env.cpus);

			env.dst_rq	 = cpu_rq(env.new_dst_cpu);
			env.dst_cpu	 = env.new_dst_cpu;
			env.flags	&= ~LBF_DST_PINNED;
			env.loop	 = 0;
			env.loop_break	 = sched_nr_migrate_break;

			/*
			 * Go back to "more_balance" rather than "redo" since we
			 * need to continue with same src_cpu.
			 */
			goto more_balance;
		}

		/*
		 * We failed to reach balance because of affinity.
		 */
		if (sd_parent) {
			int *group_imbalance = &sd_parent->groups->sgc->imbalance;

			if ((env.flags & LBF_SOME_PINNED) && env.imbalance > 0)
				*group_imbalance = 1;
		}

		/* All tasks on this runqueue were pinned by CPU affinity */
		if (unlikely(env.flags & LBF_ALL_PINNED)) {
			cpumask_clear_cpu(cpu_of(busiest), cpus);
			/*
			 * dst_cpu is not a valid busiest cpu in the following
			 * check since load cannot be pulled from dst_cpu to be
			 * put on dst_cpu.
			 */
			cpumask_clear_cpu(env.dst_cpu, cpus);
			/*
			 * Go back to "redo" iff the load-balance cpumask
			 * contains other potential busiest cpus for the
			 * current sched domain.
			 */
			if (cpumask_intersects(cpus, sched_domain_span(env.sd))) {
				/*
				 * Now that the check has passed, reenable
				 * dst_cpu so that load can be calculated on
				 * it in the redo path.
				 */
				cpumask_set_cpu(env.dst_cpu, cpus);
				env.loop = 0;
				env.loop_break = sched_nr_migrate_break;
				goto redo;
			}
			goto out_all_pinned;
		}
	}

no_move:
	if (!ld_moved) {
		if (!(env.flags & LBF_HMP_ACTIVE_BALANCE))
			schedstat_inc(sd, lb_failed[idle]);

		/*
		 * Increment the failure counter only on periodic balance.
		 * We do not want newidle balance, which can be very
		 * frequent, pollute the failure counter causing
		 * excessive cache_hot migrations and active balances.
		 */
		if (idle != CPU_NEWLY_IDLE &&
		    !(env.flags & LBF_HMP_ACTIVE_BALANCE))
			sd->nr_balance_failed++;

		if (need_active_balance(&env)) {
			raw_spin_lock_irqsave(&busiest->lock, flags);

			/* don't kick the active_load_balance_cpu_stop,
			 * if the curr task on busiest cpu can't be
			 * moved to this_cpu
			 */
			if (!cpumask_test_cpu(this_cpu,
					tsk_cpus_allowed(busiest->curr))) {
				raw_spin_unlock_irqrestore(&busiest->lock,
							    flags);
				env.flags |= LBF_ALL_PINNED;
				goto out_one_pinned;
			}

			/*
			 * ->active_balance synchronizes accesses to
			 * ->active_balance_work.  Once set, it's cleared
			 * only after active load balance is finished.
			 */
			if (!busiest->active_balance) {
				busiest->active_balance = 1;
				busiest->push_cpu = this_cpu;
				active_balance = 1;
				mark_reserved(this_cpu);
			}
			raw_spin_unlock_irqrestore(&busiest->lock, flags);

			if (active_balance) {
				stop_one_cpu_nowait(cpu_of(busiest),
					active_load_balance_cpu_stop, busiest,
					&busiest->active_balance_work);
				*continue_balancing = 0;
			}

			/*
			 * We've kicked active balancing, reset the failure
			 * counter.
			 */
			sd->nr_balance_failed =
			    sd->cache_nice_tries +
			    NEED_ACTIVE_BALANCE_THRESHOLD - 1;
		}
	} else {
		sd->nr_balance_failed = 0;
		if (per_cpu(dbs_boost_needed, this_cpu)) {
			struct migration_notify_data mnd;

			mnd.src_cpu = cpu_of(busiest);
			mnd.dest_cpu = this_cpu;
			mnd.load = per_cpu(dbs_boost_load_moved, this_cpu);
			if (mnd.load > 100)
				mnd.load = 100;
			atomic_notifier_call_chain(&migration_notifier_head,
						   0, (void *)&mnd);
			per_cpu(dbs_boost_needed, this_cpu) = false;
			per_cpu(dbs_boost_load_moved, this_cpu) = 0;

		}

		/* Assumes one 'busiest' cpu that we pulled tasks from */
		if (!same_freq_domain(this_cpu, cpu_of(busiest))) {
			check_for_freq_change(this_rq);
			check_for_freq_change(busiest);
		}
	}
	if (likely(!active_balance)) {
		/* We were unbalanced, so reset the balancing interval */
		sd->balance_interval = sd->min_interval;
	} else {
		/*
		 * If we've begun active balancing, start to back off. This
		 * case may not be covered by the all_pinned logic if there
		 * is only 1 task on the busy runqueue (because we don't call
		 * detach_tasks).
		 */
		if (sd->balance_interval < sd->max_interval)
			sd->balance_interval *= 2;
	}

	goto out;

out_balanced:
	/*
	 * We reach balance although we may have faced some affinity
	 * constraints. Clear the imbalance flag only if other tasks got
	 * a chance to move and fix the imbalance.
	 */
	if (sd_parent && !(env.flags & LBF_ALL_PINNED)) {
		int *group_imbalance = &sd_parent->groups->sgc->imbalance;

		if (*group_imbalance)
			*group_imbalance = 0;
	}

out_all_pinned:
	/*
	 * We reach balance because all tasks are pinned at this level so
	 * we can't migrate them. Let the imbalance flag set so parent level
	 * can try to migrate them.
	 */
	schedstat_inc(sd, lb_balanced[idle]);

	sd->nr_balance_failed = 0;

out_one_pinned:
	ld_moved = 0;

	/*
	 * idle_balance() disregards balance intervals, so we could repeatedly
	 * reach this code, which would lead to balance_interval skyrocketting
	 * in a short amount of time. Skip the balance_interval increase logic
	 * to avoid that.
	 */
	if (env.idle == CPU_NEWLY_IDLE)
		goto out;

	/* tune up the balancing interval */
	if (((env.flags & LBF_ALL_PINNED) &&
			sd->balance_interval < MAX_PINNED_INTERVAL) ||
			(sd->balance_interval < sd->max_interval))
		sd->balance_interval *= 2;
out:
	trace_sched_load_balance(this_cpu, idle, *continue_balancing,
				 group ? group->cpumask[0] : 0,
				 busiest ? busiest->nr_running : 0,
				 env.imbalance, env.flags, ld_moved,
				 sd->balance_interval);
	return ld_moved;
}

static inline unsigned long
get_sd_balance_interval(struct sched_domain *sd, int cpu_busy)
{
	unsigned long interval = sd->balance_interval;

	if (cpu_busy)
		interval *= sd->busy_factor;

	/* scale ms to jiffies */
	interval = msecs_to_jiffies(interval);
	interval = clamp(interval, 1UL, max_load_balance_interval);

	return interval;
}

static inline void
update_next_balance(struct sched_domain *sd, int cpu_busy, unsigned long *next_balance)
{
	unsigned long interval, next;

	interval = get_sd_balance_interval(sd, cpu_busy);
	next = sd->last_balance + interval;

	if (time_after(*next_balance, next))
		*next_balance = next;
}

/*
 * idle_balance is called by schedule() if this_cpu is about to become
 * idle. Attempts to pull tasks from other CPUs.
 */
static int idle_balance(struct rq *this_rq)
{
	unsigned long next_balance = jiffies + HZ;
	int this_cpu = this_rq->cpu;
	struct sched_domain *sd;
	int pulled_task = 0;
	u64 curr_cost = 0;
	int i, cost;
	int min_power = INT_MAX;
	int balance_cpu = -1;
	struct rq *balance_rq = NULL;

	idle_enter_fair(this_rq);

	/*
	 * We must set idle_stamp _before_ calling idle_balance(), such that we
	 * measure the duration of idle_balance() as idle time.
	 */
	this_rq->idle_stamp = rq_clock(this_rq);

	if (this_rq->avg_idle < sysctl_sched_migration_cost ||
	    !this_rq->rd->overload) {
		rcu_read_lock();
		sd = rcu_dereference_check_sched_domain(this_rq->sd);
		if (sd)
			update_next_balance(sd, 0, &next_balance);
		rcu_read_unlock();

		goto out;
	}

	/*
	 * If this CPU is not the most power-efficient idle CPU in the
	 * lowest level domain, run load balance on behalf of that
	 * most power-efficient idle CPU.
	 */
	rcu_read_lock();
	sd = rcu_dereference(per_cpu(sd_llc, this_cpu));
	if (sd && sysctl_sched_enable_power_aware) {
		for_each_cpu(i, sched_domain_span(sd)) {
			if (i == this_cpu || idle_cpu(i)) {
				cost = power_cost_at_freq(i, 0);
				if (cost < min_power) {
					min_power = cost;
					balance_cpu = i;
				}
			}
		}
		BUG_ON(balance_cpu == -1);

	} else {
		balance_cpu = this_cpu;
	}
	rcu_read_unlock();
	balance_rq = cpu_rq(balance_cpu);

	/*
	 * Drop the rq->lock, but keep IRQ/preempt disabled.
	 */
	raw_spin_unlock(&this_rq->lock);

	update_blocked_averages(balance_cpu);
	rcu_read_lock();
	for_each_domain(balance_cpu, sd) {
		int continue_balancing = 1;
		u64 t0, domain_cost;

		if (!(sd->flags & SD_LOAD_BALANCE))
			continue;

		if (balance_rq->avg_idle < curr_cost + sd->max_newidle_lb_cost) {
			update_next_balance(sd, 0, &next_balance);
			break;
		}

		if (sd->flags & SD_BALANCE_NEWIDLE) {
			t0 = sched_clock_cpu(balance_cpu);

			pulled_task = load_balance(balance_cpu, balance_rq,
						   sd, CPU_NEWLY_IDLE,
						   &continue_balancing);

			domain_cost = sched_clock_cpu(balance_cpu) - t0;
			if (domain_cost > sd->max_newidle_lb_cost)
				sd->max_newidle_lb_cost = domain_cost;

			curr_cost += domain_cost;
		}

		update_next_balance(sd, 0, &next_balance);

		/*
		 * Stop searching for tasks to pull if there are
		 * now runnable tasks on the balance rq or if
		 * continue_balancing has been unset (only possible
		 * due to active migration).
		 */
		if (pulled_task || balance_rq->nr_running > 0 ||
						!continue_balancing)
			break;
	}
	rcu_read_unlock();

	raw_spin_lock(&this_rq->lock);

	if (curr_cost > this_rq->max_idle_balance_cost)
		this_rq->max_idle_balance_cost = curr_cost;

	/*
	 * While browsing the domains, we released the rq lock, a task could
	 * have been enqueued in the meantime. Since we're not going idle,
	 * pretend we pulled a task.
	 */
	if (this_rq->cfs.h_nr_running && !pulled_task)
		pulled_task = 1;

out:
	/* Move the next balance forward */
	if (time_after(this_rq->next_balance, next_balance))
		this_rq->next_balance = next_balance;

	/* Is there a task of a high priority class? */
	if (this_rq->nr_running != this_rq->cfs.h_nr_running)
		pulled_task = -1;

	if (pulled_task && balance_cpu == this_cpu) {
		idle_exit_fair(this_rq);
		this_rq->idle_stamp = 0;
	}

	return pulled_task;
}

/*
 * active_load_balance_cpu_stop is run by cpu stopper. It pushes
 * running tasks off the busiest CPU onto idle CPUs. It requires at
 * least 1 task to be running on each physical CPU where possible, and
 * avoids physical / logical imbalances.
 */
static int active_load_balance_cpu_stop(void *data)
{
	struct rq *busiest_rq = data;
	int busiest_cpu = cpu_of(busiest_rq);
	int target_cpu = busiest_rq->push_cpu;
	struct rq *target_rq = cpu_rq(target_cpu);
	struct sched_domain *sd = NULL;
	struct task_struct *p = NULL;
	struct task_struct *push_task;
	int push_task_detached = 0;
	struct lb_env env = {
		.sd			= sd,
		.dst_cpu		= target_cpu,
		.dst_rq			= target_rq,
		.src_cpu		= busiest_rq->cpu,
		.src_rq			= busiest_rq,
		.idle			= CPU_IDLE,
		.busiest_nr_running 	= 0,
		.busiest_grp_capacity 	= 0,
		.flags			= 0,
		.loop			= 0,
	};
	bool moved = false;

	raw_spin_lock_irq(&busiest_rq->lock);

	per_cpu(dbs_boost_load_moved, target_cpu) = 0;

	/* make sure the requested cpu hasn't gone down in the meantime */
	if (unlikely(busiest_cpu != smp_processor_id() ||
		     !busiest_rq->active_balance))
		goto out_unlock;

	/* Is there any task to move? */
	if (busiest_rq->nr_running <= 1)
		goto out_unlock;

	/*
	 * This condition is "impossible", if it occurs
	 * we need to fix it. Originally reported by
	 * Bjorn Helgaas on a 128-cpu setup.
	 */
	BUG_ON(busiest_rq == target_rq);

	push_task = busiest_rq->push_task;
	target_cpu = busiest_rq->push_cpu;
	if (push_task) {
		if (task_on_rq_queued(push_task) &&
			push_task->state == TASK_RUNNING &&
			task_cpu(push_task) == busiest_cpu &&
					cpu_online(target_cpu)) {
			detach_task(push_task, &env);
			push_task_detached = 1;
			moved = true;
		}
		goto out_unlock;
	}

	/* Search for an sd spanning us and the target CPU. */
	rcu_read_lock();
	for_each_domain(target_cpu, sd) {
		if ((sd->flags & SD_LOAD_BALANCE) &&
		    cpumask_test_cpu(busiest_cpu, sched_domain_span(sd)))
				break;
	}

	if (likely(sd)) {
		env.sd = sd;
		schedstat_inc(sd, alb_count);

		p = detach_one_task(&env);
		if (p) {
			schedstat_inc(sd, alb_pushed);
			moved = true;
		} else {
			schedstat_inc(sd, alb_failed);
		}
	}
	rcu_read_unlock();
out_unlock:
	busiest_rq->active_balance = 0;
	push_task = busiest_rq->push_task;
	target_cpu = busiest_rq->push_cpu;
	clear_reserved(target_cpu);

	if (push_task)
		busiest_rq->push_task = NULL;

	raw_spin_unlock(&busiest_rq->lock);

	if (push_task) {
		if (push_task_detached)
			attach_one_task(target_rq, push_task);
		put_task_struct(push_task);
	}

	if (p)
		attach_one_task(target_rq, p);

	local_irq_enable();

	if (moved && !same_freq_domain(busiest_cpu, target_cpu)) {
		check_for_freq_change(busiest_rq);
		check_for_freq_change(target_rq);
	}

	if (per_cpu(dbs_boost_needed, target_cpu)) {
		struct migration_notify_data mnd;

		mnd.src_cpu = cpu_of(busiest_rq);
		mnd.dest_cpu = target_cpu;
		mnd.load = per_cpu(dbs_boost_load_moved, target_cpu);
		if (mnd.load > 100)
			mnd.load = 100;
		atomic_notifier_call_chain(&migration_notifier_head,
					   0, (void *)&mnd);

		per_cpu(dbs_boost_needed, target_cpu) = false;
		per_cpu(dbs_boost_load_moved, target_cpu) = 0;
	}
	return 0;
}

static inline int on_null_domain(struct rq *rq)
{
	return unlikely(!rcu_dereference_sched(rq->sd));
}

#ifdef CONFIG_NO_HZ_COMMON
/*
 * idle load balancing details
 * - When one of the busy CPUs notice that there may be an idle rebalancing
 *   needed, they will kick the idle load balancer, which then does idle
 *   load balancing for all the idle CPUs.
 */
static struct {
	cpumask_var_t idle_cpus_mask;
	atomic_t nr_cpus;
	unsigned long next_balance;     /* in jiffy units */
} nohz ____cacheline_aligned;

static inline int find_new_ilb(int type)
{
	int ilb;

	if (sched_enable_hmp)
		return find_new_hmp_ilb(type);

	ilb = cpumask_first(nohz.idle_cpus_mask);

	if (ilb < nr_cpu_ids && idle_cpu(ilb))
		return ilb;

	return nr_cpu_ids;
}

/*
 * Kick a CPU to do the nohz balancing, if it is time for it. We pick the
 * nohz_load_balancer CPU (if there is one) otherwise fallback to any idle
 * CPU (if there is one).
 */
static void nohz_balancer_kick(int type)
{
	int ilb_cpu;

	nohz.next_balance++;

	ilb_cpu = find_new_ilb(type);

	if (ilb_cpu >= nr_cpu_ids)
		return;

	if (test_and_set_bit(NOHZ_BALANCE_KICK, nohz_flags(ilb_cpu)))
		return;
	/*
	 * Use smp_send_reschedule() instead of resched_cpu().
	 * This way we generate a sched IPI on the target cpu which
	 * is idle. And the softirq performing nohz idle load balance
	 * will be run before returning from the IPI.
	 */
	smp_send_reschedule(ilb_cpu);
	return;
}

static inline void nohz_balance_exit_idle(int cpu)
{
	if (unlikely(test_bit(NOHZ_TICK_STOPPED, nohz_flags(cpu)))) {
		/*
		 * Completely isolated CPUs don't ever set, so we must test.
		 */
		if (likely(cpumask_test_cpu(cpu, nohz.idle_cpus_mask))) {
			cpumask_clear_cpu(cpu, nohz.idle_cpus_mask);
			atomic_dec(&nohz.nr_cpus);
		}
		clear_bit(NOHZ_TICK_STOPPED, nohz_flags(cpu));
	}
}

static inline void set_cpu_sd_state_busy(void)
{
	struct sched_domain *sd;
	int cpu = smp_processor_id();

	rcu_read_lock();
	sd = rcu_dereference(per_cpu(sd_busy, cpu));

	if (!sd || !sd->nohz_idle)
		goto unlock;
	sd->nohz_idle = 0;

	atomic_inc(&sd->groups->sgc->nr_busy_cpus);
unlock:
	rcu_read_unlock();
}

void set_cpu_sd_state_idle(void)
{
	struct sched_domain *sd;
	int cpu = smp_processor_id();

	rcu_read_lock();
	sd = rcu_dereference(per_cpu(sd_busy, cpu));

	if (!sd || sd->nohz_idle)
		goto unlock;
	sd->nohz_idle = 1;

	atomic_dec(&sd->groups->sgc->nr_busy_cpus);
unlock:
	rcu_read_unlock();
}

/*
 * This routine will record that the cpu is going idle with tick stopped.
 * This info will be used in performing idle load balancing in the future.
 */
void nohz_balance_enter_idle(int cpu)
{
	/*
	 * If this cpu is going down, then nothing needs to be done.
	 */
	if (!cpu_active(cpu))
		return;

	if (test_bit(NOHZ_TICK_STOPPED, nohz_flags(cpu)))
		return;

	/*
	 * If we're a completely isolated CPU, we don't play.
	 */
	if (on_null_domain(cpu_rq(cpu)))
		return;

	cpumask_set_cpu(cpu, nohz.idle_cpus_mask);
	atomic_inc(&nohz.nr_cpus);
	set_bit(NOHZ_TICK_STOPPED, nohz_flags(cpu));
}

static int sched_ilb_notifier(struct notifier_block *nfb,
					unsigned long action, void *hcpu)
{
	switch (action & ~CPU_TASKS_FROZEN) {
	case CPU_DYING:
		nohz_balance_exit_idle(smp_processor_id());
		return NOTIFY_OK;
	default:
		return NOTIFY_DONE;
	}
}
#endif

static DEFINE_SPINLOCK(balancing);

/*
 * Scale the max load_balance interval with the number of CPUs in the system.
 * This trades load-balance latency on larger machines for less cross talk.
 */
void update_max_interval(void)
{
	max_load_balance_interval = HZ*num_online_cpus()/10;
}

/*
 * It checks each scheduling domain to see if it is due to be balanced,
 * and initiates a balancing operation if so.
 *
 * Balancing parameters are set up in init_sched_domains.
 */
static void rebalance_domains(struct rq *rq, enum cpu_idle_type idle)
{
	int continue_balancing = 1;
	int cpu = rq->cpu;
	unsigned long interval;
	struct sched_domain *sd;
	/* Earliest time when we have to do rebalance again */
	unsigned long next_balance = jiffies + 60*HZ;
	int update_next_balance = 0;
	int need_serialize, need_decay = 0;
	u64 max_cost = 0;

	update_blocked_averages(cpu);

	rcu_read_lock();
	for_each_domain(cpu, sd) {
		/*
		 * Decay the newidle max times here because this is a regular
		 * visit to all the domains. Decay ~1% per second.
		 */
		if (time_after(jiffies, sd->next_decay_max_lb_cost)) {
			sd->max_newidle_lb_cost =
				(sd->max_newidle_lb_cost * 253) / 256;
			sd->next_decay_max_lb_cost = jiffies + HZ;
			need_decay = 1;
		}
		max_cost += sd->max_newidle_lb_cost;

		if (!(sd->flags & SD_LOAD_BALANCE))
			continue;

		/*
		 * Stop the load balance at this level. There is another
		 * CPU in our sched group which is doing load balancing more
		 * actively.
		 */
		if (!continue_balancing) {
			if (need_decay)
				continue;
			break;
		}

		interval = get_sd_balance_interval(sd, idle != CPU_IDLE);

		need_serialize = sd->flags & SD_SERIALIZE;
		if (need_serialize) {
			if (!spin_trylock(&balancing))
				goto out;
		}

		if (time_after_eq(jiffies, sd->last_balance + interval)) {
			if (load_balance(cpu, rq, sd, idle, &continue_balancing)) {
				/*
				 * The LBF_DST_PINNED logic could have changed
				 * env->dst_cpu, so we can't know our idle
				 * state even if we migrated tasks. Update it.
				 */
				idle = idle_cpu(cpu) ? CPU_IDLE : CPU_NOT_IDLE;
			}
			sd->last_balance = jiffies;
			interval = get_sd_balance_interval(sd, idle != CPU_IDLE);
		}
		if (need_serialize)
			spin_unlock(&balancing);
out:
		if (time_after(next_balance, sd->last_balance + interval)) {
			next_balance = sd->last_balance + interval;
			update_next_balance = 1;
		}
	}
	if (need_decay) {
		/*
		 * Ensure the rq-wide value also decays but keep it at a
		 * reasonable floor to avoid funnies with rq->avg_idle.
		 */
		rq->max_idle_balance_cost =
			max((u64)sysctl_sched_migration_cost, max_cost);
	}
	rcu_read_unlock();

	/*
	 * next_balance will be updated only when there is a need.
	 * When the cpu is attached to null domain for ex, it will not be
	 * updated.
	 */
	if (likely(update_next_balance)) {
		rq->next_balance = next_balance;

#ifdef CONFIG_NO_HZ_COMMON
		/*
		 * If this CPU has been elected to perform the nohz idle
		 * balance. Other idle CPUs have already rebalanced with
		 * nohz_idle_balance() and nohz.next_balance has been
		 * updated accordingly. This CPU is now running the idle load
		 * balance for itself and we need to update the
		 * nohz.next_balance accordingly.
		 */
		if ((idle == CPU_IDLE) && time_after(nohz.next_balance, rq->next_balance))
			nohz.next_balance = rq->next_balance;
#endif
	}
}

#ifdef CONFIG_NO_HZ_COMMON

static int select_lowest_power_cpu(struct cpumask *cpus)
{
	int i, cost;
	int lowest_power_cpu = -1;
	int lowest_power = INT_MAX;

	if (sysctl_sched_enable_power_aware) {
		for_each_cpu(i, cpus) {
			cost = power_cost_at_freq(i, 0);
			if (cost < lowest_power) {
				lowest_power_cpu = i;
				lowest_power = cost;
			}
		}
		BUG_ON(lowest_power_cpu == -1);
		return lowest_power_cpu;
	} else {
		return cpumask_first(cpus);
	}
}

/*
 * In CONFIG_NO_HZ_COMMON case, the idle balance kickee will do the
 * rebalancing for all the cpus for whom scheduler ticks are stopped.
 */
static void nohz_idle_balance(struct rq *this_rq, enum cpu_idle_type idle)
{
	int this_cpu = this_rq->cpu;
	struct rq *rq;
	int balance_cpu;
	/* Earliest time when we have to do rebalance again */
	unsigned long next_balance = jiffies + 60*HZ;
	int update_next_balance = 0;
	struct cpumask cpus_to_balance;

	if (idle != CPU_IDLE ||
	    !test_bit(NOHZ_BALANCE_KICK, nohz_flags(this_cpu)))
		goto end;

	cpumask_copy(&cpus_to_balance, nohz.idle_cpus_mask);

	while (!cpumask_empty(&cpus_to_balance)) {
		balance_cpu = select_lowest_power_cpu(&cpus_to_balance);

		cpumask_clear_cpu(balance_cpu, &cpus_to_balance);
		if (balance_cpu == this_cpu || !idle_cpu(balance_cpu))
			continue;

		/*
		 * If this cpu gets work to do, stop the load balancing
		 * work being done for other cpus. Next load
		 * balancing owner will pick it up.
		 */
		if (need_resched())
			break;

		rq = cpu_rq(balance_cpu);

		/*
		 * If time for next balance is due,
		 * do the balance.
		 */
		if (time_after_eq(jiffies, rq->next_balance)) {
			raw_spin_lock_irq(&rq->lock);
			update_rq_clock(rq);
			update_idle_cpu_load(rq);
			raw_spin_unlock_irq(&rq->lock);
			rebalance_domains(rq, CPU_IDLE);
		}

		if (time_after(next_balance, rq->next_balance)) {
			next_balance = rq->next_balance;
			update_next_balance = 1;
		}
	}
	/*
	 * next_balance will be updated only when there is a need.
	 * When the CPU is attached to null domain for ex, it will not be
	 * updated.
	 */
	if (likely(update_next_balance))
		nohz.next_balance = next_balance;
end:
	clear_bit(NOHZ_BALANCE_KICK, nohz_flags(this_cpu));
}

#ifdef CONFIG_SCHED_HMP
static inline int _nohz_kick_needed_hmp(struct rq *rq, int cpu, int *type)
{
	struct sched_domain *sd;
	int i, rcpu = cpu_of(rq);

	if (cpu_mostly_idle_freq(rcpu) &&
		cpu_cur_freq(rcpu) < cpu_mostly_idle_freq(rcpu)
		 && cpu_max_freq(rcpu) > cpu_mostly_idle_freq(rcpu))
			return 0;

	if (rq->nr_running >= 2 &&
		(rq->nr_running - rq->hmp_stats.nr_small_tasks >= 2 ||
		rq->nr_running > rq->mostly_idle_nr_run ||
		cpu_load(cpu) > rq->mostly_idle_load)) {

		if (cpu_capacity(cpu_of(rq) == max_capacity))
			return 1;

		rcu_read_lock();
		sd = rcu_dereference_check_sched_domain(rq->sd);
		if (!sd) {
			rcu_read_unlock();
			return 0;
		}

		for_each_cpu(i, sched_domain_span(sd)) {
			if (cpu_load(i) < sched_spill_load) {
				/* Change the kick type to limit to CPUs that
				 * are of equal or lower capacity.
				 */
				*type = NOHZ_KICK_RESTRICT;
				break;
			}
		}
		rcu_read_unlock();
		return 1;
	}

	return 0;
}
#else
static inline int _nohz_kick_needed_hmp(struct rq *rq, int cpu, int *type)
{
	return 0;
}
#endif

static inline int _nohz_kick_needed(struct rq *rq, int cpu, int *type)
{
	unsigned long now = jiffies;

	if (sched_enable_hmp)
		return _nohz_kick_needed_hmp(rq, cpu, type);

	/*
	 * None are in tickless mode and hence no need for NOHZ idle load
	 * balancing.
	 */
	if (likely(!atomic_read(&nohz.nr_cpus)))
		return false;

	if (time_before(now, nohz.next_balance))
		return false;

	return (rq->nr_running >= 2);
}

/*
 * Current heuristic for kicking the idle load balancer in the presence
 * of an idle cpu in the system.
 *   - This rq has more than one task.
 *   - This rq has at least one CFS task and the capacity of the CPU is
 *     significantly reduced because of RT tasks or IRQs.
 *   - At parent of LLC scheduler domain level, this cpu's scheduler group has
 *     multiple busy cpu.
 *   - For SD_ASYM_PACKING, if the lower numbered cpu's in the scheduler
 *     domain span are idle.
 */
static inline int nohz_kick_needed(struct rq *rq, int *type)
{
#ifndef CONFIG_SCHED_HMP
	struct sched_domain *sd;
	struct sched_group_capacity *sgc;
	int nr_busy;
#endif
	int cpu = rq->cpu;
	bool kick = false;

	if (unlikely(rq->idle_balance))
		return false;

       /*
	* We may be recently in ticked or tickless idle mode. At the first
	* busy tick after returning from idle, we will update the busy stats.
	*/
	set_cpu_sd_state_busy();
	nohz_balance_exit_idle(cpu);

	if (_nohz_kick_needed(rq, cpu, type))
		return true;

#ifndef CONFIG_SCHED_HMP
	rcu_read_lock();
	sd = rcu_dereference(per_cpu(sd_busy, cpu));

	if (sd) {
		sgc = sd->groups->sgc;
		nr_busy = atomic_read(&sgc->nr_busy_cpus);

		if (nr_busy > 1) {
			kick = true;
			goto unlock;
		}

	}

	sd = rcu_dereference(rq->sd);
	if (sd) {
		if ((rq->cfs.h_nr_running >= 1) &&
				check_cpu_capacity(rq, sd)) {
			kick = true;
			goto unlock;
		}
	}

	sd = rcu_dereference(per_cpu(sd_asym, cpu));
	if (sd && (cpumask_first_and(nohz.idle_cpus_mask,
				  sched_domain_span(sd)) < cpu)) {
		kick = true;
		goto unlock;
	}

unlock:
	rcu_read_unlock();
#endif
	return kick;
}
#else
static void nohz_idle_balance(struct rq *this_rq, enum cpu_idle_type idle) { }
#endif

/*
 * run_rebalance_domains is triggered when needed from the scheduler tick.
 * Also triggered for nohz idle balancing (with nohz_balancing_kick set).
 */
static void run_rebalance_domains(struct softirq_action *h)
{
	struct rq *this_rq = this_rq();
	enum cpu_idle_type idle = this_rq->idle_balance ?
						CPU_IDLE : CPU_NOT_IDLE;

	/*
	 * If this cpu has a pending nohz_balance_kick, then do the
	 * balancing on behalf of the other idle cpus whose ticks are
	 * stopped. Do nohz_idle_balance *before* rebalance_domains to
	 * give the idle cpus a chance to load balance. Else we may
	 * load balance only within the local sched_domain hierarchy
	 * and abort nohz_idle_balance altogether if we pull some load.
	 */
	nohz_idle_balance(this_rq, idle);
	rebalance_domains(this_rq, idle);
}

/*
 * Trigger the SCHED_SOFTIRQ if it is time to do periodic load balancing.
 */
void trigger_load_balance(struct rq *rq)
{
	int type = NOHZ_KICK_ANY;

	/* Don't need to rebalance while attached to NULL domain */
	if (unlikely(on_null_domain(rq)))
		return;

	if (time_after_eq(jiffies, rq->next_balance))
		raise_softirq(SCHED_SOFTIRQ);
#ifdef CONFIG_NO_HZ_COMMON
	if (nohz_kick_needed(rq, &type))
		nohz_balancer_kick(type);
#endif
}

static void rq_online_fair(struct rq *rq)
{
	update_sysctl();

	update_runtime_enabled(rq);
}

static void rq_offline_fair(struct rq *rq)
{
	update_sysctl();

	/* Ensure any throttled groups are reachable by pick_next_task */
	unthrottle_offline_cfs_rqs(rq);
}

#endif /* CONFIG_SMP */

/*
 * scheduler tick hitting a task of our scheduling class:
 */
static void task_tick_fair(struct rq *rq, struct task_struct *curr, int queued)
{
	struct cfs_rq *cfs_rq;
	struct sched_entity *se = &curr->se;

	for_each_sched_entity(se) {
		cfs_rq = cfs_rq_of(se);
		entity_tick(cfs_rq, se, queued);
	}

	if (numabalancing_enabled)
		task_tick_numa(rq, curr);
}

/*
 * called on fork with the child task as argument from the parent's context
 *  - child not yet on the tasklist
 *  - preemption disabled
 */
static void task_fork_fair(struct task_struct *p)
{
	struct cfs_rq *cfs_rq;
	struct sched_entity *se = &p->se, *curr;
	int this_cpu = smp_processor_id();
	struct rq *rq = this_rq();
	unsigned long flags;

	raw_spin_lock_irqsave(&rq->lock, flags);

	update_rq_clock(rq);

	cfs_rq = task_cfs_rq(current);
	curr = cfs_rq->curr;

	/*
	 * Not only the cpu but also the task_group of the parent might have
	 * been changed after parent->se.parent,cfs_rq were copied to
	 * child->se.parent,cfs_rq. So call __set_task_cpu() to make those
	 * of child point to valid ones.
	 */
	rcu_read_lock();
	__set_task_cpu(p, this_cpu);
	rcu_read_unlock();

	update_curr(cfs_rq);

	if (curr)
		se->vruntime = curr->vruntime;
	place_entity(cfs_rq, se, 1);

	if (sysctl_sched_child_runs_first && curr && entity_before(curr, se)) {
		/*
		 * Upon rescheduling, sched_class::put_prev_task() will place
		 * 'current' within the tree based on its new key value.
		 */
		swap(curr->vruntime, se->vruntime);
		resched_curr(rq);
	}

	se->vruntime -= cfs_rq->min_vruntime;

	raw_spin_unlock_irqrestore(&rq->lock, flags);
}

/*
 * Priority of the task has changed. Check to see if we preempt
 * the current task.
 */
static void
prio_changed_fair(struct rq *rq, struct task_struct *p, int oldprio)
{
	if (!task_on_rq_queued(p))
		return;

	/*
	 * Reschedule if we are currently running on this runqueue and
	 * our priority decreased, or if we are not currently running on
	 * this runqueue and our priority is higher than the current's
	 */
	if (rq->curr == p) {
		if (p->prio > oldprio)
			resched_curr(rq);
	} else
		check_preempt_curr(rq, p, 0);
}

static inline bool vruntime_normalized(struct task_struct *p)
{
	struct sched_entity *se = &p->se;

	/*
	 * In both the TASK_ON_RQ_QUEUED and TASK_ON_RQ_MIGRATING cases,
	 * the dequeue_entity(.flags=0) will already have normalized the
	 * vruntime.
	 */
	if (p->on_rq)
		return true;

	/*
	 * When !on_rq, vruntime of the task has usually NOT been normalized.
	 * But there are some cases where it has already been normalized:
	 *
	 * - A forked child which is waiting for being woken up by
	 *   wake_up_new_task().
	 * - A task which has been woken up by try_to_wake_up() and
	 *   waiting for actually being woken up by sched_ttwu_pending().
	 */
	if (!se->sum_exec_runtime || p->state == TASK_WAKING)
		return true;

	return false;
}

static void detach_task_cfs_rq(struct task_struct *p)
{
	struct sched_entity *se = &p->se;
	struct cfs_rq *cfs_rq = cfs_rq_of(se);

	if (!vruntime_normalized(p)) {
		/*
		 * Fix up our vruntime so that the current sleep doesn't
		 * cause 'unlimited' sleep bonus.
		 */
		place_entity(cfs_rq, se, 0);
		se->vruntime -= cfs_rq->min_vruntime;
	}

	/* Catch up with the cfs_rq and remove our load when we leave */
	detach_entity_load_avg(cfs_rq, se);
}

static void attach_task_cfs_rq(struct task_struct *p)
{
	struct sched_entity *se = &p->se;
	struct cfs_rq *cfs_rq = cfs_rq_of(se);

#ifdef CONFIG_FAIR_GROUP_SCHED
	/*
	 * Since the real-depth could have been changed (only FAIR
	 * class maintain depth value), reset depth properly.
	 */
	se->depth = se->parent ? se->parent->depth + 1 : 0;
#endif

	/* Synchronize task with its cfs_rq */
	attach_entity_load_avg(cfs_rq, se);

	if (!vruntime_normalized(p))
		se->vruntime += cfs_rq->min_vruntime;
}

static void switched_from_fair(struct rq *rq, struct task_struct *p)
{
	detach_task_cfs_rq(p);
}

static void switched_to_fair(struct rq *rq, struct task_struct *p)
{
	attach_task_cfs_rq(p);

	if (task_on_rq_queued(p)) {
		/*
		 * We were most likely switched from sched_rt, so
		 * kick off the schedule if running, otherwise just see
		 * if we can still preempt the current task.
		 */
		if (rq->curr == p)
			resched_curr(rq);
		else
			check_preempt_curr(rq, p, 0);
	}
}

/* Account for a task changing its policy or group.
 *
 * This routine is mostly called to set cfs_rq->curr field when a task
 * migrates between groups/classes.
 */
static void set_curr_task_fair(struct rq *rq)
{
	struct sched_entity *se = &rq->curr->se;

	for_each_sched_entity(se) {
		struct cfs_rq *cfs_rq = cfs_rq_of(se);

		set_next_entity(cfs_rq, se);
		/* ensure bandwidth has been allocated on our new cfs_rq */
		account_cfs_rq_runtime(cfs_rq, 0);
	}
}

void init_cfs_rq(struct cfs_rq *cfs_rq)
{
	cfs_rq->tasks_timeline = RB_ROOT;
	cfs_rq->min_vruntime = (u64)(-(1LL << 20));
#ifndef CONFIG_64BIT
	cfs_rq->min_vruntime_copy = cfs_rq->min_vruntime;
#endif
#ifdef CONFIG_SMP
	atomic_long_set(&cfs_rq->removed_load_avg, 0);
	atomic_long_set(&cfs_rq->removed_util_avg, 0);
#endif
}

#ifdef CONFIG_FAIR_GROUP_SCHED
static void task_move_group_fair(struct task_struct *p)
{
	detach_task_cfs_rq(p);
	set_task_rq(p, task_cpu(p));

#ifdef CONFIG_SMP
	/* Tell se's cfs_rq has been changed -- migrated */
	p->se.avg.last_update_time = 0;
#endif
	attach_task_cfs_rq(p);
}

void free_fair_sched_group(struct task_group *tg)
{
	int i;

	destroy_cfs_bandwidth(tg_cfs_bandwidth(tg));

	for_each_possible_cpu(i) {
		if (tg->cfs_rq)
			kfree(tg->cfs_rq[i]);
		if (tg->se)
			kfree(tg->se[i]);
	}

	kfree(tg->cfs_rq);
	kfree(tg->se);
}

int alloc_fair_sched_group(struct task_group *tg, struct task_group *parent)
{
	struct cfs_rq *cfs_rq;
	struct sched_entity *se;
	int i;

	tg->cfs_rq = kzalloc(sizeof(cfs_rq) * nr_cpu_ids, GFP_KERNEL);
	if (!tg->cfs_rq)
		goto err;
	tg->se = kzalloc(sizeof(se) * nr_cpu_ids, GFP_KERNEL);
	if (!tg->se)
		goto err;

	tg->shares = NICE_0_LOAD;

	init_cfs_bandwidth(tg_cfs_bandwidth(tg));

	for_each_possible_cpu(i) {
		cfs_rq = kzalloc_node(sizeof(struct cfs_rq),
				      GFP_KERNEL, cpu_to_node(i));
		if (!cfs_rq)
			goto err;

		se = kzalloc_node(sizeof(struct sched_entity),
				  GFP_KERNEL, cpu_to_node(i));
		if (!se)
			goto err_free_rq;

		init_cfs_rq(cfs_rq);
		init_tg_cfs_entry(tg, cfs_rq, se, i, parent->se[i]);
		init_entity_runnable_average(se);
	}

	return 1;

err_free_rq:
	kfree(cfs_rq);
err:
	return 0;
}

void unregister_fair_sched_group(struct task_group *tg)
{
	unsigned long flags;
	struct rq *rq;
	int cpu;

	for_each_possible_cpu(cpu) {
		if (tg->se[cpu])
			remove_entity_load_avg(tg->se[cpu]);

		/*
		 * Only empty task groups can be destroyed; so we can speculatively
		 * check on_list without danger of it being re-added.
		 */
		if (!tg->cfs_rq[cpu]->on_list)
			continue;

		rq = cpu_rq(cpu);

		raw_spin_lock_irqsave(&rq->lock, flags);
		list_del_leaf_cfs_rq(tg->cfs_rq[cpu]);
		raw_spin_unlock_irqrestore(&rq->lock, flags);
	}
}

void init_tg_cfs_entry(struct task_group *tg, struct cfs_rq *cfs_rq,
			struct sched_entity *se, int cpu,
			struct sched_entity *parent)
{
	struct rq *rq = cpu_rq(cpu);

	cfs_rq->tg = tg;
	cfs_rq->rq = rq;
	init_cfs_rq_runtime(cfs_rq);

	tg->cfs_rq[cpu] = cfs_rq;
	tg->se[cpu] = se;

	/* se could be NULL for root_task_group */
	if (!se)
		return;

	if (!parent) {
		se->cfs_rq = &rq->cfs;
		se->depth = 0;
	} else {
		se->cfs_rq = parent->my_q;
		se->depth = parent->depth + 1;
	}

	se->my_q = cfs_rq;
	/* guarantee group entities always have weight */
	update_load_set(&se->load, NICE_0_LOAD);
	se->parent = parent;
}

static DEFINE_MUTEX(shares_mutex);

int sched_group_set_shares(struct task_group *tg, unsigned long shares)
{
	int i;
	unsigned long flags;

	/*
	 * We can't change the weight of the root cgroup.
	 */
	if (!tg->se[0])
		return -EINVAL;

	shares = clamp(shares, scale_load(MIN_SHARES), scale_load(MAX_SHARES));

	mutex_lock(&shares_mutex);
	if (tg->shares == shares)
		goto done;

	tg->shares = shares;
	for_each_possible_cpu(i) {
		struct rq *rq = cpu_rq(i);
		struct sched_entity *se;

		se = tg->se[i];
		/* Propagate contribution to hierarchy */
		raw_spin_lock_irqsave(&rq->lock, flags);

		/* Possible calls to update_curr() need rq clock */
		update_rq_clock(rq);
		for_each_sched_entity(se)
			update_cfs_shares(group_cfs_rq(se));
		raw_spin_unlock_irqrestore(&rq->lock, flags);
	}

done:
	mutex_unlock(&shares_mutex);
	return 0;
}
#else /* CONFIG_FAIR_GROUP_SCHED */

void free_fair_sched_group(struct task_group *tg) { }

int alloc_fair_sched_group(struct task_group *tg, struct task_group *parent)
{
	return 1;
}

void unregister_fair_sched_group(struct task_group *tg) { }

#endif /* CONFIG_FAIR_GROUP_SCHED */


static unsigned int get_rr_interval_fair(struct rq *rq, struct task_struct *task)
{
	struct sched_entity *se = &task->se;
	unsigned int rr_interval = 0;

	/*
	 * Time slice is 0 for SCHED_OTHER tasks that are on an otherwise
	 * idle runqueue:
	 */
	if (rq->cfs.load.weight)
		rr_interval = NS_TO_JIFFIES(sched_slice(cfs_rq_of(se), se));

	return rr_interval;
}

/*
 * All the scheduling class methods:
 */
const struct sched_class fair_sched_class = {
	.next			= &idle_sched_class,
	.enqueue_task		= enqueue_task_fair,
	.dequeue_task		= dequeue_task_fair,
	.yield_task		= yield_task_fair,
	.yield_to_task		= yield_to_task_fair,

	.check_preempt_curr	= check_preempt_wakeup,

	.pick_next_task		= pick_next_task_fair,
	.put_prev_task		= put_prev_task_fair,

#ifdef CONFIG_SMP
	.select_task_rq		= select_task_rq_fair,
	.migrate_task_rq	= migrate_task_rq_fair,

	.rq_online		= rq_online_fair,
	.rq_offline		= rq_offline_fair,

	.task_waking		= task_waking_fair,
	.task_dead		= task_dead_fair,
#endif

	.set_curr_task          = set_curr_task_fair,
	.task_tick		= task_tick_fair,
	.task_fork		= task_fork_fair,

	.prio_changed		= prio_changed_fair,
	.switched_from		= switched_from_fair,
	.switched_to		= switched_to_fair,

	.get_rr_interval	= get_rr_interval_fair,

	.update_curr		= update_curr_fair,

#ifdef CONFIG_FAIR_GROUP_SCHED
	.task_move_group	= task_move_group_fair,
#endif
#ifdef CONFIG_SCHED_HMP
	.inc_hmp_sched_stats	= inc_hmp_sched_stats_fair,
	.dec_hmp_sched_stats	= dec_hmp_sched_stats_fair,
	.fixup_hmp_sched_stats	= fixup_hmp_sched_stats_fair,
#endif
};

#ifdef CONFIG_SCHED_DEBUG
void print_cfs_stats(struct seq_file *m, int cpu)
{
	struct cfs_rq *cfs_rq;

	rcu_read_lock();
	for_each_leaf_cfs_rq(cpu_rq(cpu), cfs_rq)
		print_cfs_rq(m, cpu, cfs_rq);
	rcu_read_unlock();
}
#endif

__init void init_sched_fair_class(void)
{
#ifdef CONFIG_SMP
	open_softirq(SCHED_SOFTIRQ, run_rebalance_domains);

#ifdef CONFIG_NO_HZ_COMMON
	nohz.next_balance = jiffies;
	zalloc_cpumask_var(&nohz.idle_cpus_mask, GFP_NOWAIT);
	cpu_notifier(sched_ilb_notifier, 0);
#endif
#endif /* SMP */

}
