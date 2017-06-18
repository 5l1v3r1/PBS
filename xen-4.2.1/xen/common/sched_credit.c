/****************************************************************************
 * (C) 2005-2006 - Emmanuel Ackaouy - XenSource Inc.
 ****************************************************************************
 *
 *        File: common/csched_credit.c
 *      Author: Emmanuel Ackaouy
 *
 * Description: Credit-based SMP CPU scheduler
 */

#include <xen/config.h>
#include <xen/init.h>
#include <xen/lib.h>
#include <xen/sched.h>
#include <xen/domain.h>
#include <xen/delay.h>
#include <xen/event.h>
#include <xen/time.h>
#include <xen/perfc.h>
#include <xen/sched-if.h>
#include <xen/softirq.h>
#include <asm/atomic.h>
#include <asm/bitops.h>
#include <asm/pmustate.h>
#include <asm/perfctr.h>
#include <xen/errno.h>
#include <xen/keyhandler.h>

/*
 * CSCHED_STATS
 *
 * Manage very basic per-vCPU counters and stats.
 *
 * Useful for debugging live systems. The stats are displayed
 * with runq dumps ('r' on the Xen console).
 */
#ifdef PERF_COUNTERS
#define CSCHED_STATS
#endif


/*
 * Basic constants
 */
#define CSCHED_DEFAULT_WEIGHT       256
#define CSCHED_TICKS_PER_TSLICE     3
/* Default timeslice: 30ms */
#define CSCHED_DEFAULT_TSLICE_MS    30
#define CSCHED_CREDITS_PER_MSEC     1000

 /*add start*/
#define CSCHED_DEFAULT_TSLICE_US   100
#define CSCHED_CREDIT_PER_US       1
#define CSCHED_TIME_APPLY          3000
#define CSCHED_METRIC_TICK_PERIOD  1000
 /*add end*/


/*
 * Priorities
 */
#define CSCHED_PRI_TS_BOOST      0      /* time-share waking up */
#define CSCHED_PRI_TS_UNDER     -1      /* time-share w/ credits */
#define CSCHED_PRI_TS_OVER      -2      /* time-share w/o credits */
#define CSCHED_PRI_IDLE         -64     /* idle */


/*
 * Flags
 */
#define CSCHED_FLAG_VCPU_PARKED    0x0001  /* VCPU over capped credits */
#define CSCHED_FLAG_VCPU_YIELD     0x0002  /* VCPU yielding */


/*
 * Useful macros
 */
#define CSCHED_PRIV(_ops)   \
    ((struct csched_private *)((_ops)->sched_data))
#define CSCHED_PCPU(_c)     \
    ((struct csched_pcpu *)per_cpu(schedule_data, _c).sched_priv)
#define CSCHED_VCPU(_vcpu)  ((struct csched_vcpu *) (_vcpu)->sched_priv)
#define CSCHED_DOM(_dom)    ((struct csched_dom *) (_dom)->sched_priv)
#define RUNQ(_cpu)          (&(CSCHED_PCPU(_cpu)->runq))


/*
 * Stats
 */
#define CSCHED_STAT_CRANK(_X)               (perfc_incr(_X))

#ifdef CSCHED_STATS

#define CSCHED_VCPU_STATS_RESET(_V)                     \
    do                                                  \
    {                                                   \
        memset(&(_V)->stats, 0, sizeof((_V)->stats));   \
    } while ( 0 )

#define CSCHED_VCPU_STAT_CRANK(_V, _X)      (((_V)->stats._X)++)

#define CSCHED_VCPU_STAT_SET(_V, _X, _Y)    (((_V)->stats._X) = (_Y))

#else /* CSCHED_STATS */

#define CSCHED_VCPU_STATS_RESET(_V)         do {} while ( 0 )
#define CSCHED_VCPU_STAT_CRANK(_V, _X)      do {} while ( 0 )
#define CSCHED_VCPU_STAT_SET(_V, _X, _Y)    do {} while ( 0 )

#endif /* CSCHED_STATS */

#define HISTO_BUCKETS				30	// For spinlock accounting
#define SWITCH_BOUNDARY				900
#define SLICE_UPDATE_WINDOW			3
#define EVENT_TRACKING_WINDOW		5
#define ALPHA						4

#define SPIN_LOW_PHASE				1
#define SPIN_HIGH_PHASE				2

struct csched_private *global_prv;
/*
 * Boot parameters
 */
static bool_t __read_mostly sched_credit_default_yield;
boolean_param("sched_credit_default_yield", sched_credit_default_yield);
static int __read_mostly sched_credit_tslice_us = CSCHED_DEFAULT_TSLICE_US;
integer_param("sched_credit_tslice_us", sched_credit_tslice_us);

/*
 * Physical CPU
 */
struct csched_pcpu {
    struct list_head runq;
    uint32_t runq_sort_last;
    struct timer ticker;
	struct timer metric_ticker;
    unsigned int tick;
    unsigned int idle_bias;

	/*added*/
	unsigned int runnable_tasks;
	spinlock_t runq_lock;
};

/*
 * Virtual CPU
 */
struct csched_vcpu {
    struct list_head runq_elem;
    struct list_head active_vcpu_elem;
    struct csched_dom *sdom;
    struct vcpu *vcpu;
    atomic_t credit;
    s_time_t start_time;   /* When we were scheduled (used for credit) */
    uint16_t flags;
    int16_t pri;
	unsigned long long prev_pmc[4];
	/*
    unsigned long long vcpu_histo_wait_count[HISTO_BUCKETS+1];
    unsigned long long vcpu_histo_wait_time[HISTO_BUCKETS+1];
	unsigned long long vcpu_histo_hold_count[HISTO_BUCKETS+1];
	unsigned long long vcpu_histo_hold_time[HISTO_BUCKETS+1];
    unsigned long long vcpu_wait_time_sum;
    unsigned long long vcpu_hold_time_sum;
	*/
#ifdef CSCHED_STATS
    struct {
        int credit_last;
        uint32_t credit_incr;
        uint32_t state_active;
        uint32_t state_idle;
        uint32_t migrate_q;
        uint32_t migrate_r;
    } stats;
#endif
};

/*
 * monitor state
 */
struct metric_state
{
	unsigned time_slice;
	uint16_t spinlock_latency;
	uint16_t cache_miss_rate;
};

struct event_sample
{
	uint16_t spinlock;
	uint32_t inst_retired;
	uint32_t cache_misses;
};

struct subms_data
{
	unsigned long long spinlock_accum;
	unsigned long long cache_miss_accum;
};

/*
 * Domain
 */
struct csched_dom {
    struct list_head active_vcpu;
    struct list_head active_sdom_elem;
    struct domain *dom;
    uint16_t active_vcpu_count;
    uint16_t weight;
    uint16_t cap;

	/*added*/
	unsigned tslice_us;
	uint8_t slice_update_window;
	uint8_t event_tracking_window;
	uint8_t event_stable_count;
	uint8_t phase;
	uint16_t cache_miss_rate;		//scale to 1000 times
	uint16_t cpi;					//cycle per instruction
	uint16_t tick_period_us;
	uint64_t spinlock_count;
	unsigned long long pmc[4];
	unsigned long long spinlock_latency;
	unsigned long long spinlock_metric_update; 
	unsigned long pending_requests;
	struct event_sample filter[EVENT_TRACKING_WINDOW];
	struct subms_data submilli[10];
	/*
	unsigned long long dom_histo_wait_count[HISTO_BUCKETS+1];
	unsigned long long dom_histo_wait_time[HISTO_BUCKETS+1];
	unsigned long long dom_histo_hold_count[HISTO_BUCKETS+1];
	unsigned long long dom_histo_hold_time[HISTO_BUCKETS+1];
	unsigned long long dom_wait_time_sum;
	unsigned long long dom_hold_time_sum;
	*/
};

/*
 * System-wide private data
 */
struct csched_private {
    /* lock for the whole pluggable scheduler, nests inside cpupool_lock */
    spinlock_t lock;
    struct list_head active_sdom;
    uint32_t ncpus;
    struct timer  master_ticker;
	/*added*/
	struct timer slice_ticker;
    unsigned int master;
    cpumask_var_t idlers;
    cpumask_var_t cpus;
    uint32_t weight;
    uint32_t credit;
    int credit_balance;
    uint32_t runq_sort;
	uint32_t metric_update;
	uint32_t metric_update_last;
    unsigned ratelimit_us;
    /* Period of master and tick in milliseconds */
    unsigned tslice_us, tick_period_us, ticks_per_tslice;
    unsigned credits_per_tslice;
};


extern int do_vcrd_op(unsigned long long time, int lock)
{
    //static unsigned long long total_time = 0;
    //static unsigned long long count = 0;
	struct domain *dom = current->domain;
	struct csched_dom *sdom = CSCHED_DOM(dom);
	
	sdom->spinlock_latency += time;
	sdom->spinlock_metric_update += time;
	sdom->spinlock_count++;
	
	/*
    if(dom->domain_id == 1)
    {
        total_time += time;
        count++;
        printk(XENLOG_WARNING "total_time:%lld, total_count:%lld\n", total_time, count);
    }
	*/
	
    return 1;
}


/*added*/
/*
extern int do_spinstat_op(unsigned long long time, int type)
{
	struct vcpu *vc = current;
	struct csched_vcpu * const svc = CSCHED_VCPU(vc);
	struct domain *dom = current->domain;
	struct csched_dom *sdom = CSCHED_DOM(dom);
	unsigned index = fls(time); 
	
	//static unsigned long long total_time = 0;

	if(dom->domain_id == 1 && type == 2)
	{
		total_time += time;
		printk(XENLOG_WARNING "lock holding total time: %llums time: %llums\n", total_time/1000, time/1000);
	}
	
	switch(type)
	{
		case 1:
			if(likely(index < HISTO_BUCKETS))
			{
				++svc->vcpu_histo_wait_count[index];
				svc->vcpu_histo_wait_time[index] += time;
				++sdom->dom_histo_wait_count[index];
				sdom->dom_histo_wait_time[index] += time;
			}
			else
			{
				++svc->vcpu_histo_wait_count[HISTO_BUCKETS];
				svc->vcpu_histo_wait_time[HISTO_BUCKETS] += time;
				++sdom->dom_histo_wait_count[HISTO_BUCKETS];
				sdom->dom_histo_wait_time[HISTO_BUCKETS] += time;
			}
			svc->vcpu_wait_time_sum += time;
			sdom->dom_wait_time_sum += time;
			break;
		case 2:
			if(likely(index < HISTO_BUCKETS))
			{
				++svc->vcpu_histo_hold_count[index];
				svc->vcpu_histo_hold_time[index] += time;
				++sdom->dom_histo_hold_count[index];
				sdom->dom_histo_hold_time[index] += time;
			}
			else
			{
				++svc->vcpu_histo_hold_count[HISTO_BUCKETS];
				svc->vcpu_histo_hold_time[HISTO_BUCKETS] += time;
				++sdom->dom_histo_hold_count[HISTO_BUCKETS];
				sdom->dom_histo_hold_time[HISTO_BUCKETS] += time;
			}
			svc->vcpu_hold_time_sum += time;
			sdom->dom_hold_time_sum += time;
			break;
	}
	if(dom->domain_id == 1)
	{
		printk(XENLOG_WARNING "wait_time:%llu    loop_count:%llu    histo_index:%d\n", wait_time, loop_count, index);
	}
	return 0;
}
*/

static void csched_event_window_shift(struct csched_dom *sdom,
								  unsigned long long spinlock,
								  unsigned long long inst_retired, 
								  unsigned long long cache_misses)
{
	int i;
	for(i = 0; i < EVENT_TRACKING_WINDOW - 1; i++)
		sdom->filter[i] = sdom->filter[i+1];

	sdom->filter[EVENT_TRACKING_WINDOW-1].spinlock = spinlock;
	sdom->filter[EVENT_TRACKING_WINDOW-1].inst_retired = inst_retired;
	sdom->filter[EVENT_TRACKING_WINDOW-1].cache_misses = cache_misses;
}

static void csched_event_window_clear(struct csched_dom *sdom)
{
	int i;
	for(i = 0; i < EVENT_TRACKING_WINDOW; i++)
	{
		sdom->filter[i].spinlock = 0;
		sdom->filter[i].inst_retired = 0;
		sdom->filter[i].cache_misses = 0;
	}
}

static void csched_decrease_time_slice(struct csched_dom *sdom)
{
	if(sdom->tslice_us >= SWITCH_BOUNDARY * 3)
		sdom->tslice_us = sdom->tslice_us / 300 * 100;
	else
		sdom->tslice_us = (sdom->tslice_us >= 300 ? sdom->tslice_us - 200 : 100);
}

static void csched_increase_time_slice(struct csched_dom *sdom)
{
	//if(sdom->tslice_us >= SWITCH_BOUNDARY)
	//	sdom->tslice_us = (sdom->tslice_us * 3 >= 30000 ? 30000 : sdom->tslice_us * 3);
	//else
		sdom->tslice_us = (sdom->tslice_us + 100 >= 1100 ? 1100 : sdom->tslice_us + 100);
}

static void csched_submilli_metric_update(struct csched_dom *sdom,
									unsigned long long inst_retired,
									unsigned long long cache_misses)
{
	//int idx = sdom->tslice_us / 1000 > 0 ? 9 : sdom->tslice_us / 100 - 1;
	int miss_rate_curr = (inst_retired != 0 ? cache_misses * 100000 / inst_retired : 0);
	int miss_rate_window = 0;
	int i, evt_idx, err, count = 0;
	unsigned long long inst_mean = 0, cache_miss_mean = 0, spinlock_mean = 0, sum = 0;
	uint64_t avg_spinlock;

	avg_spinlock = sdom->spinlock_count > 0 ? sdom->spinlock_metric_update / sdom->spinlock_count : 0;
	if(sdom->event_tracking_window > 0)
	{
		evt_idx = EVENT_TRACKING_WINDOW - sdom->event_tracking_window;
		sdom->filter[evt_idx].spinlock = avg_spinlock;
		sdom->filter[evt_idx].inst_retired = inst_retired;
		sdom->filter[evt_idx].cache_misses = cache_misses;
		sdom->event_tracking_window--;
		if(miss_rate_curr > 0 && miss_rate_curr < 100)
			csched_decrease_time_slice(sdom);
	}
	else
	{
		for(i = 0; i < EVENT_TRACKING_WINDOW; i++)
			sum += sdom->filter[i].inst_retired;
		inst_mean = sum / EVENT_TRACKING_WINDOW;
		
		sum = 0;
		for(i = 0; i < EVENT_TRACKING_WINDOW; i++)
			sum += sdom->filter[i].cache_misses;
		cache_miss_mean = sum / EVENT_TRACKING_WINDOW;

		sum = 0;
		for(i = 0; i < EVENT_TRACKING_WINDOW; i++)
		{
			if(sdom->filter[i].spinlock > 10000)
			{
				count++;
				sum += sdom->filter[i].spinlock;
			}
		}
		spinlock_mean = count > 0 ? sum / count : 0;

		miss_rate_window = inst_mean > 0 ? cache_miss_mean * 100000 / inst_mean : 0;
		if(miss_rate_window > 0)
			err = miss_rate_curr * 100 / miss_rate_window;
		else
		{
			if(miss_rate_curr == 0)	err = 100;
			else err = 0;
		}

		if((err >= 70 && err <= 130) || 
				(err > 130 && miss_rate_window >= 100) ||
				(miss_rate_curr < 100 && miss_rate_window < 100))
		{
			sdom->event_stable_count++;
			csched_event_window_shift(sdom, avg_spinlock, inst_retired, cache_misses);
			if(miss_rate_window >= 100)
			{
				sdom->phase = SPIN_LOW_PHASE;
				//sdom->submilli[idx].cache_miss_accum = sdom->submilli[idx].cache_miss_accum / ALPHA + miss_rate_window * (ALPHA - 1) / ALPHA ;
				csched_increase_time_slice(sdom);
			}
			else
			{
				sdom->phase = SPIN_HIGH_PHASE;
				//sdom->submilli[idx].spinlock_accum = sdom->submilli[idx].spinlock_accum / ALPHA + spinlock_mean * (ALPHA - 1 ) / ALPHA;
				csched_decrease_time_slice(sdom);
			}
			/*else
				csched_increase_time_slice(sdom);*/
			sdom->tick_period_us = sdom->tslice_us / CSCHED_TICKS_PER_TSLICE;
		}
		else
		{
			sdom->event_stable_count = 0;
			csched_event_window_clear(sdom);
			sdom->filter[0].spinlock = avg_spinlock;
			sdom->filter[0].inst_retired = inst_retired;
			sdom->filter[0].cache_misses = cache_misses;
			sdom->event_tracking_window = EVENT_TRACKING_WINDOW - 1;
			if(miss_rate_curr < 100)
				csched_decrease_time_slice(sdom);
		}
	}

	/*if(avg_spinlock > 1024)
		sdom->submilli[idx].spinlock_accum = sdom->submilli[idx].spinlock_accum / ALPHA + avg_spinlock * (ALPHA - 1) / ALPHA;*/
	
	/*if(sdom->dom->domain_id == 4 && sdom->tslice_us == 1111)
		printk(XENLOG_WARNING "inst:%8lld   cache miss:%8lld   miss window:%5d   miss curr:%5d   window:%3d   spinlock accum:%5lld   cache miss accum:%5lld   time slice:%5d\n", inst_retired, cache_misses, miss_rate_window, miss_rate_curr, sdom->event_tracking_window, sdom->submilli[idx].spinlock_accum, sdom->submilli[idx].cache_miss_accum, sdom->tslice_us);*/
/*	if(sdom->dom->domain_id == 4)
		printk(XENLOG_WARNING "inst:%10lld   cache miss:%8lld   miss window:%5d   miss curr:%5d   window:%3d   spinlock latency:%8lld   time slice:%5d\n", inst_retired, cache_misses, miss_rate_window, miss_rate_curr, sdom->event_tracking_window, sdom->spinlock_metric_update, sdom->tslice_us);*/
}

static void csched_dom_metric_update(unsigned int cpu)
{
	struct domain *dom;
	struct vcpu *vc;
	static unsigned long long cache_miss_sum, inst_sum, cycle_sum;
	int i, update_epoch;
	unsigned long long curr_pmc[4];
	//uint16_tdd spinlock_log;
	struct csched_private *prv = CSCHED_PRIV(per_cpu(scheduler, cpu));
	
	update_epoch = prv->metric_update;
	cache_miss_sum = inst_sum = cycle_sum = 0;

	for(i = 0; i < 4; i++)
		curr_pmc[i] = 0;

	if(prv->master == cpu)
	{
		for_each_domain(dom)
		{
			struct csched_dom *sdom = CSCHED_DOM(dom);

			if(is_idle_domain(dom)) continue;
			sdom->pending_requests = dom->pending_requests;
			dom->pending_requests = 0;

			for_each_vcpu(dom, vc)
			{
				for(i = 0; i < 4; i++)
				{
					struct csched_vcpu *svc = CSCHED_VCPU(vc);
					curr_pmc[i] += (vc->pmc[i] - svc->prev_pmc[i]);
					sdom->pmc[i] = curr_pmc[i];
					svc->prev_pmc[i] = vc->pmc[i];
				}
			}

			//spinlock_log = fls(sdom->spinlock_metric_update);
			//spin_sum += spinlock_log;
			inst_sum += curr_pmc[0];
			cycle_sum += curr_pmc[1];
			cache_miss_sum += curr_pmc[3];

			//if(sdom->tslice_us <= 1111)
				csched_submilli_metric_update(sdom, curr_pmc[0], curr_pmc[3]);

			sdom->cache_miss_rate = (inst_sum != 0 ? cache_miss_sum * 100000 / inst_sum : 0);
			sdom->cpi = (inst_sum != 0 ? cycle_sum * 1000 / inst_sum : 0);
			if(update_epoch != prv->metric_update_last)
			{
				inst_sum = cycle_sum = cache_miss_sum = 0;
				prv->metric_update_last = update_epoch;
			}
			sdom->spinlock_metric_update = 0;
			sdom->spinlock_count = 0;
		}
	}
}

static void csched_metric_tick(void *_cpu)
{
    unsigned int cpu = (unsigned long)_cpu;
    struct csched_pcpu *spc = CSCHED_PCPU(cpu);
    struct csched_private *prv = CSCHED_PRIV(per_cpu(scheduler, cpu));

	if(prv->tslice_us <= 1111)
	{
		pmu_save_regs(current);
		pmu_restore_regs(current);
	}

	csched_dom_metric_update(cpu);
	
    set_timer(&spc->metric_ticker, NOW() + MICROSECS(CSCHED_METRIC_TICK_PERIOD) );
}

/*added*/
/*static void update_time_slice(struct csched_dom *sdom)
{
	int spin_high_count = 0;
	int spin_low_count = 0;
	list_for_each_safe()
}*/

/*
static void csched_update_acct(void)
{
	struct list_head *iter_sdom, *next_sdom;
	struct csched_dom *sdom;
	struct csched_private *prv = global_prv;
	unsigned int tslice = 30000;
	unsigned int spin_low_count = 0, spin_high_count = 0;
	unsigned int spin_low_max = 100, spin_high_min = tslice;
	
	list_for_each_safe(iter_sdom, next_sdom, &prv->active_sdom)
	{
		sdom = list_entry(iter_sdom, struct csched_dom, active_sdom_elem);

		if(sdom->dom->domain_id == 0)	continue;
		if(sdom->phase == SPIN_LOW_PHASE)
		{
			spin_low_count++;
			if(sdom->tslice_us > spin_low_max)
				spin_low_max = sdom->tslice_us;
		}
		else
		{
			spin_high_count++;
			if(sdom->tslice_us < spin_high_min)
				spin_high_min = sdom->tslice_us;
		}
	}

	if(spin_low_count <= spin_high_count)
		tslice = spin_high_min;
	else
		tslice = spin_low_max;

	list_for_each_safe(iter_sdom, next_sdom, &prv->active_sdom)
	{
		sdom = list_entry(iter_sdom, struct csched_dom, active_sdom_elem);
		sdom->tslice_us = tslice;
	}

	prv->tslice_us = tslice;
	prv->ticks_per_tslice = CSCHED_TICKS_PER_TSLICE;
	if(prv->tslice_us < prv->ticks_per_tslice)
		prv->ticks_per_tslice = 1;
	prv->tick_period_us = prv->tslice_us / prv->ticks_per_tslice;
	prv->credits_per_tslice = CSCHED_CREDIT_PER_US * prv->tslice_us;
	prv->credit = prv->ncpus * prv->credits_per_tslice;
	printk(XENLOG_WARNING "Global Time Slice: %d\n", prv->tslice_us);
	printk("high count:%d    low count:%d    high min ts:%6d    low max ts:%6d    global ts:%6d\n",
			spin_high_count, spin_low_count, spin_high_min, spin_low_max, prv->tslice_us);
}*/

static void csched_dynamic_time_slice(void *dummy)
{
	struct csched_private *prv = dummy;

/*	unsigned long flags;

	spin_lock_irqsave(&prv->lock, flags);
	csched_update_acct();
	spin_unlock_irqrestore(&prv->lock, flags);
*/	
/*	
	if(prv->tslice_us > 1111)
		set_timer(&prv->slice_ticker, NOW() + MICROSECS(CSCHED_TIME_APPLY / 3));
	else
		set_timer(&prv->slice_ticker, NOW() + MICROSECS(CSCHED_TIME_APPLY));

	prv->metric_update++;
*/
/*
	for_each_domain(dom)
	{
		struct csched_dom *sdom = CSCHED_DOM(dom);
		unsigned long long latency, count, avg_latency;

		if(is_idle_domain(dom)) continue;
		latency = sdom->spinlock_latency;
		count = sdom->spinlock_count;
		avg_latency = count > 0 ? latency / count : 0;
		if(dom->domain_id == 4 && avg_latency > 0)
			printk(XENLOG_WARNING "spinlock latency: %10lld    count: %8lld    avg spinlock: %10lld    spinlock log: %8d\n",
				latency, count, avg_latency, fls(avg_latency));
		sdom->spinlock_latency = 0;
		sdom->spinlock_count = 0;
	}
*/	
	set_timer(&prv->slice_ticker, NOW() + MICROSECS(CSCHED_TIME_APPLY));
}

/*
void set_vcpu_affinity(struct vcpu *vc)
{
	struct domain *dom = vc->domain;
	cpumask_t cpus;
	struct vcpu *vcpu;
	int cpu;

	if(is_idle_domain(dom))
		return;

	cpumask_clear(&cpus);
	for_each_vcpu(dom, vcpu)
	{
		cpu = vcpu->processor;
		cpumask_set_cpu(cpu, &cpus);
	}
	for_each_vcpu(dom, vcpu)
	{
		cpumask_andnot(vcpu->cpu_affinity, &cpu_online_map, &cpus);
		cpumask_set_cpu(vcpu->processor, vcpu->cpu_affinity);
	}
}

static void get_cpu_load(int cpu)
{
	struct list_head *runq = RUNQ(cpu);
	struct csched_pcpu *spc = CSCHED_PCPU(cpu);
	struct list_head *iter;

	spin_lock(&spc->runq_lock);

	spc->runnable_tasks = 0;
	list_for_each(iter, runq)
		spc->runnable_tasks++;
	
	spin_unlock(&spc->runq_lock);
}

static int least_load_cpu_pick(struct vcpu *vc)
{
	cpumask_t cpus;
	struct csched_pcpu *spc;
	int cpu;
	unsigned int min_load;
	unsigned int load;
	int least_load_cpu;

	cpumask_and(&cpus, &cpu_online_map, vc->cpu_affinity);
	for_each_cpu(cpu, &cpus)
	{
		get_cpu_load(cpu);
	}

	least_load_cpu = cpumask_first(&cpus);
	spc = CSCHED_PCPU(least_load_cpu);
	min_load = spc->runnable_tasks;

	for_each_cpu(cpu, &cpus)
	{
		spc = CSCHED_PCPU(cpu);
		load = spc->runnable_tasks;
		if(load < min_load)
		{
			min_load = load;
			least_load_cpu = cpu;
		}
	}
	return least_load_cpu;
}

static void reset_vcpu_processor(struct vcpu *vc)
{
	int cpu;
	struct domain *dom = vc->domain;

	if(is_idle_domain(dom))
		return;

	//set_vcpu_affinity(vc);
	cpu = least_load_cpu_pick(vc);
	vc->processor = cpu;
	set_vcpu_affinity(vc);
}
*/

static void csched_tick(void *_cpu);
static void csched_acct(void *dummy);

static inline int
__vcpu_on_runq(struct csched_vcpu *svc)
{
    return !list_empty(&svc->runq_elem);
}

static inline struct csched_vcpu *
__runq_elem(struct list_head *elem)
{
    return list_entry(elem, struct csched_vcpu, runq_elem);
}

static inline void
__runq_insert(unsigned int cpu, struct csched_vcpu *svc)
{
    const struct list_head * const runq = RUNQ(cpu);
    struct list_head *iter;

    BUG_ON( __vcpu_on_runq(svc) );
    BUG_ON( cpu != svc->vcpu->processor );

    list_for_each( iter, runq )
    {
        const struct csched_vcpu * const iter_svc = __runq_elem(iter);
        if ( svc->pri > iter_svc->pri )
            break;
    }

    /* If the vcpu yielded, try to put it behind one lower-priority
     * runnable vcpu if we can.  The next runq_sort will bring it forward
     * within 30ms if the queue too long. */
    if ( svc->flags & CSCHED_FLAG_VCPU_YIELD
         && __runq_elem(iter)->pri > CSCHED_PRI_IDLE )
    {
        iter=iter->next;

        /* Some sanity checks */
        BUG_ON(iter == runq);
    }

    list_add_tail(&svc->runq_elem, iter);
}

static inline void
__runq_remove(struct csched_vcpu *svc)
{
    BUG_ON( !__vcpu_on_runq(svc) );
    list_del_init(&svc->runq_elem);
}

static void burn_credits(struct csched_vcpu *svc, s_time_t now)
{
    s_time_t delta;
    unsigned int credits;
	//int o_credits;
    /* Assert svc is current */
    ASSERT(svc==CSCHED_VCPU(per_cpu(schedule_data, svc->vcpu->processor).curr));

    if ( (delta = now - svc->start_time) <= 0 )
        return;

    credits = (delta*CSCHED_CREDITS_PER_MSEC + MILLISECS(1)/2) / MILLISECS(1);
    atomic_sub(credits, &svc->credit);
	/*o_credits = atomic_read(&svc->credit);
	if(o_credits < 0)
		svc->pri = CSCHED_PRI_TS_OVER;*/
    svc->start_time += (credits * MILLISECS(1)) / CSCHED_CREDITS_PER_MSEC;

	/*printk(XENLOG_WARNING "burn credit: domain: %2d  vcpu: %2d  credits: %8d\n", 
			svc->vcpu->domain->domain_id, svc->vcpu->vcpu_id, credits);*/
}

static bool_t __read_mostly opt_tickle_one_idle = 1;
boolean_param("tickle_one_idle_cpu", opt_tickle_one_idle);

DEFINE_PER_CPU(unsigned int, last_tickle_cpu);

static inline void
__runq_tickle(unsigned int cpu, struct csched_vcpu *new)
{
    struct csched_vcpu * const cur =
        CSCHED_VCPU(per_cpu(schedule_data, cpu).curr);
    struct csched_private *prv = CSCHED_PRIV(per_cpu(scheduler, cpu));
    cpumask_t mask;

    ASSERT(cur);
    cpumask_clear(&mask);

    /* If strictly higher priority than current VCPU, signal the CPU */
    if ( new->pri > cur->pri )
    {
        if ( cur->pri == CSCHED_PRI_IDLE )
            CSCHED_STAT_CRANK(tickle_local_idler);
        else if ( cur->pri == CSCHED_PRI_TS_OVER )
            CSCHED_STAT_CRANK(tickle_local_over);
        else if ( cur->pri == CSCHED_PRI_TS_UNDER )
            CSCHED_STAT_CRANK(tickle_local_under);
        else
            CSCHED_STAT_CRANK(tickle_local_other);

        cpumask_set_cpu(cpu, &mask);
    }

    /*
     * If this CPU has at least two runnable VCPUs, we tickle any idlers to
     * let them know there is runnable work in the system...
     */
    if ( cur->pri > CSCHED_PRI_IDLE )
    {
        if ( cpumask_empty(prv->idlers) )
        {
            CSCHED_STAT_CRANK(tickle_idlers_none);
        }
        else
        {
            cpumask_t idle_mask;

            cpumask_and(&idle_mask, prv->idlers, new->vcpu->cpu_affinity);
            if ( !cpumask_empty(&idle_mask) )
            {
                CSCHED_STAT_CRANK(tickle_idlers_some);
                if ( opt_tickle_one_idle )
                {
                    this_cpu(last_tickle_cpu) = 
                        cpumask_cycle(this_cpu(last_tickle_cpu), &idle_mask);
                    cpumask_set_cpu(this_cpu(last_tickle_cpu), &mask);
                }
                else
                    cpumask_or(&mask, &mask, &idle_mask);
            }
            cpumask_and(&mask, &mask, new->vcpu->cpu_affinity);
        }
    }

    /* Send scheduler interrupts to designated CPUs */
    if ( !cpumask_empty(&mask) )
        cpumask_raise_softirq(&mask, SCHEDULE_SOFTIRQ);
}

static void
csched_free_pdata(const struct scheduler *ops, void *pcpu, int cpu)
{
    struct csched_private *prv = CSCHED_PRIV(ops);
    struct csched_pcpu *spc = pcpu;
    unsigned long flags;

    if ( spc == NULL )
        return;

    spin_lock_irqsave(&prv->lock, flags);

    prv->credit -= prv->credits_per_tslice;
    prv->ncpus--;
    cpumask_clear_cpu(cpu, prv->idlers);
    cpumask_clear_cpu(cpu, prv->cpus);
    if ( (prv->master == cpu) && (prv->ncpus > 0) )
    {
        prv->master = cpumask_first(prv->cpus);
        migrate_timer(&prv->master_ticker, prv->master);
		migrate_timer(&prv->slice_ticker, prv->master);
    }
    kill_timer(&spc->ticker);
	kill_timer(&spc->metric_ticker);
    if ( prv->ncpus == 0 ) {
        kill_timer(&prv->master_ticker);
		kill_timer(&prv->slice_ticker);
	}

    spin_unlock_irqrestore(&prv->lock, flags);

    xfree(spc);
}

static void *
csched_alloc_pdata(const struct scheduler *ops, int cpu)
{
    struct csched_pcpu *spc;
    struct csched_private *prv = CSCHED_PRIV(ops);
    unsigned long flags;

    /* Allocate per-PCPU info */
    spc = xzalloc(struct csched_pcpu);
    if ( spc == NULL )
        return NULL;

    spin_lock_irqsave(&prv->lock, flags);

    /* Initialize/update system-wide config */
    prv->credit += prv->credits_per_tslice;
    prv->ncpus++;
    cpumask_set_cpu(cpu, prv->cpus);
    if ( prv->ncpus == 1 )
    {
        prv->master = cpu;
        init_timer(&prv->master_ticker, csched_acct, prv, cpu);
		init_timer(&prv->slice_ticker, csched_dynamic_time_slice, prv, cpu);
        set_timer(&prv->master_ticker,
                  NOW() + MICROSECS(prv->tslice_us));
		set_timer(&prv->slice_ticker, NOW() + MICROSECS(CSCHED_TIME_APPLY));
    }
	spin_lock_init(&spc->runq_lock);

    init_timer(&spc->ticker, csched_tick, (void *)(unsigned long)cpu, cpu);
    set_timer(&spc->ticker, NOW() + MICROSECS(prv->tick_period_us) );

	init_timer(&spc->metric_ticker, csched_metric_tick, (void *)(unsigned long)cpu, cpu);
	set_timer(&spc->metric_ticker, NOW() + MICROSECS(CSCHED_METRIC_TICK_PERIOD) );

    INIT_LIST_HEAD(&spc->runq);
    spc->runq_sort_last = prv->runq_sort;
    spc->idle_bias = nr_cpu_ids - 1;
    if ( per_cpu(schedule_data, cpu).sched_priv == NULL )
        per_cpu(schedule_data, cpu).sched_priv = spc;

    /* Start off idling... */
    BUG_ON(!is_idle_vcpu(per_cpu(schedule_data, cpu).curr));
    cpumask_set_cpu(cpu, prv->idlers);

    spin_unlock_irqrestore(&prv->lock, flags);

    return spc;
}

#ifndef NDEBUG
static inline void
__csched_vcpu_check(struct vcpu *vc)
{
    struct csched_vcpu * const svc = CSCHED_VCPU(vc);
    struct csched_dom * const sdom = svc->sdom;

    BUG_ON( svc->vcpu != vc );
    BUG_ON( sdom != CSCHED_DOM(vc->domain) );
    if ( sdom )
    {
        BUG_ON( is_idle_vcpu(vc) );
        BUG_ON( sdom->dom != vc->domain );
    }
    else
    {
        BUG_ON( !is_idle_vcpu(vc) );
    }

    CSCHED_STAT_CRANK(vcpu_check);
}
#define CSCHED_VCPU_CHECK(_vc)  (__csched_vcpu_check(_vc))
#else
#define CSCHED_VCPU_CHECK(_vc)
#endif

/*
 * Delay, in microseconds, between migrations of a VCPU between PCPUs.
 * This prevents rapid fluttering of a VCPU between CPUs, and reduces the
 * implicit overheads such as cache-warming. 1ms (1000) has been measured
 * as a good value.
 */
static unsigned int vcpu_migration_delay;
integer_param("vcpu_migration_delay", vcpu_migration_delay);

void set_vcpu_migration_delay(unsigned int delay)
{
    vcpu_migration_delay = delay;
}

unsigned int get_vcpu_migration_delay(void)
{
    return vcpu_migration_delay;
}

static inline int
__csched_vcpu_is_cache_hot(struct vcpu *v)
{
    int hot = ((NOW() - v->last_run_time) <
               ((uint64_t)vcpu_migration_delay * 1000u));

    if ( hot )
        CSCHED_STAT_CRANK(vcpu_hot);

    return hot;
}

static inline int
__csched_vcpu_is_migrateable(struct vcpu *vc, int dest_cpu)
{
    /*
     * Don't pick up work that's in the peer's scheduling tail or hot on
     * peer PCPU. Only pick up work that's allowed to run on our CPU.
     */
    return !vc->is_running &&
           !__csched_vcpu_is_cache_hot(vc) &&
           cpumask_test_cpu(dest_cpu, vc->cpu_affinity);
}

static int
_csched_cpu_pick(const struct scheduler *ops, struct vcpu *vc, bool_t commit)
{
    cpumask_t cpus;
    cpumask_t idlers;
    cpumask_t *online;
    struct csched_pcpu *spc = NULL;
    int cpu;

	/*added*/
	//struct domain *dom = vc->domain;
	//struct vcpu *vcpu;
	//set_vcpu_affinity(vc);

    /*
     * Pick from online CPUs in VCPU's affinity mask, giving a
     * preference to its current processor if it's in there.
     */
    online = cpupool_scheduler_cpumask(vc->domain->cpupool);
    cpumask_and(&cpus, online, vc->cpu_affinity);
    cpu = cpumask_test_cpu(vc->processor, &cpus)
            ? vc->processor
            : cpumask_cycle(vc->processor, &cpus);
    ASSERT( !cpumask_empty(&cpus) && cpumask_test_cpu(cpu, &cpus) );

    /*
     * Try to find an idle processor within the above constraints.
     *
     * In multi-core and multi-threaded CPUs, not all idle execution
     * vehicles are equal!
     *
     * We give preference to the idle execution vehicle with the most
     * idling neighbours in its grouping. This distributes work across
     * distinct cores first and guarantees we don't do something stupid
     * like run two VCPUs on co-hyperthreads while there are idle cores
     * or sockets.
     */
    cpumask_and(&idlers, &cpu_online_map, CSCHED_PRIV(ops)->idlers);
    cpumask_set_cpu(cpu, &idlers);
    cpumask_and(&cpus, &cpus, &idlers);
    cpumask_clear_cpu(cpu, &cpus);

    while ( !cpumask_empty(&cpus) )
    {
        cpumask_t cpu_idlers;
        cpumask_t nxt_idlers;
        int nxt, weight_cpu, weight_nxt;
        int migrate_factor;

        nxt = cpumask_cycle(cpu, &cpus);

        if ( cpumask_test_cpu(cpu, per_cpu(cpu_core_mask, nxt)) )
        {
            /* We're on the same socket, so check the busy-ness of threads.
             * Migrate if # of idlers is less at all */
            ASSERT( cpumask_test_cpu(nxt, per_cpu(cpu_core_mask, cpu)) );
            migrate_factor = 1;
            cpumask_and(&cpu_idlers, &idlers, per_cpu(cpu_sibling_mask, cpu));
            cpumask_and(&nxt_idlers, &idlers, per_cpu(cpu_sibling_mask, nxt));
        }
        else
        {
            /* We're on different sockets, so check the busy-ness of cores.
             * Migrate only if the other core is twice as idle */
            ASSERT( !cpumask_test_cpu(nxt, per_cpu(cpu_core_mask, cpu)) );
            migrate_factor = 2;
            cpumask_and(&cpu_idlers, &idlers, per_cpu(cpu_core_mask, cpu));
            cpumask_and(&nxt_idlers, &idlers, per_cpu(cpu_core_mask, nxt));
        }

        weight_cpu = cpumask_weight(&cpu_idlers);
        weight_nxt = cpumask_weight(&nxt_idlers);
        /* smt_power_savings: consolidate work rather than spreading it */
        if ( sched_smt_power_savings ?
             weight_cpu > weight_nxt :
             weight_cpu * migrate_factor < weight_nxt )
        {
            cpumask_and(&nxt_idlers, &cpus, &nxt_idlers);
            spc = CSCHED_PCPU(nxt);
            cpu = cpumask_cycle(spc->idle_bias, &nxt_idlers);
            cpumask_andnot(&cpus, &cpus, per_cpu(cpu_sibling_mask, cpu));
        }
        else
        {
            cpumask_andnot(&cpus, &cpus, &nxt_idlers);
        }
    }
	
	/*	
	if(!is_idle_domain(dom))
	{
		for_each_vcpu(dom, vcpu)
		{
			//if we want to pick a cpu where our vcpu siblings are running on,
			// we give a preference to current processor
			if(vcpu != vc && 
					vcpu->processor == cpu)
				cpu = vc->processor;
		}
	}
	*/

    if ( commit && spc )
       spc->idle_bias = cpu;

    return cpu;
}

static int
csched_cpu_pick(const struct scheduler *ops, struct vcpu *vc)
{
    return _csched_cpu_pick(ops, vc, 1);
}

static inline void
__csched_vcpu_acct_start(struct csched_private *prv, struct csched_vcpu *svc)
{
    struct csched_dom * const sdom = svc->sdom;
    unsigned long flags;

    spin_lock_irqsave(&prv->lock, flags);

    if ( list_empty(&svc->active_vcpu_elem) )
    {
        CSCHED_VCPU_STAT_CRANK(svc, state_active);
        CSCHED_STAT_CRANK(acct_vcpu_active);

        sdom->active_vcpu_count++;
        list_add(&svc->active_vcpu_elem, &sdom->active_vcpu);
        /* Make weight per-vcpu */
        prv->weight += sdom->weight;
        if ( list_empty(&sdom->active_sdom_elem) )
        {
            list_add(&sdom->active_sdom_elem, &prv->active_sdom);
        }
    }

    spin_unlock_irqrestore(&prv->lock, flags);
}

static inline void
__csched_vcpu_acct_stop_locked(struct csched_private *prv,
    struct csched_vcpu *svc)
{
    struct csched_dom * const sdom = svc->sdom;

    BUG_ON( list_empty(&svc->active_vcpu_elem) );

    CSCHED_VCPU_STAT_CRANK(svc, state_idle);
    CSCHED_STAT_CRANK(acct_vcpu_idle);

    BUG_ON( prv->weight < sdom->weight );
    sdom->active_vcpu_count--;
    list_del_init(&svc->active_vcpu_elem);
    prv->weight -= sdom->weight;
    if ( list_empty(&sdom->active_vcpu) )
    {
        list_del_init(&sdom->active_sdom_elem);
    }
}

static void
csched_vcpu_acct(struct csched_private *prv, unsigned int cpu)
{
    struct csched_vcpu * const svc = CSCHED_VCPU(current);
    const struct scheduler *ops = per_cpu(scheduler, cpu);

    ASSERT( current->processor == cpu );
    ASSERT( svc->sdom != NULL );

    /*
     * If this VCPU's priority was boosted when it last awoke, reset it.
     * If the VCPU is found here, then it's consuming a non-negligeable
     * amount of CPU resources and should no longer be boosted.
     */
    if ( svc->pri == CSCHED_PRI_TS_BOOST )
        svc->pri = CSCHED_PRI_TS_UNDER;

    /*
     * Update credits
     */
    if ( !is_idle_vcpu(svc->vcpu) )
        burn_credits(svc, NOW());

    /*
     * Put this VCPU and domain back on the active list if it was
     * idling.
     *
     * If it's been active a while, check if we'd be better off
     * migrating it to run elsewhere (see multi-core and multi-thread
     * support in csched_cpu_pick()).
     */
    if ( list_empty(&svc->active_vcpu_elem) )
    {
        __csched_vcpu_acct_start(prv, svc);
    }
    else if ( _csched_cpu_pick(ops, current, 0) != cpu )
    {
        CSCHED_VCPU_STAT_CRANK(svc, migrate_r);
        CSCHED_STAT_CRANK(migrate_running);
        set_bit(_VPF_migrating, &current->pause_flags);
        cpu_raise_softirq(cpu, SCHEDULE_SOFTIRQ);
    }
}

static void *
csched_alloc_vdata(const struct scheduler *ops, struct vcpu *vc, void *dd)
{
    struct csched_vcpu *svc;
	int i;
    /* Allocate per-VCPU info */
    svc = xzalloc(struct csched_vcpu);
    if ( svc == NULL )
        return NULL;

    INIT_LIST_HEAD(&svc->runq_elem);
    INIT_LIST_HEAD(&svc->active_vcpu_elem);
    svc->sdom = dd;
    svc->vcpu = vc;
    atomic_set(&svc->credit, 0);
    svc->flags = 0U;
    svc->pri = is_idle_domain(vc->domain) ?
        CSCHED_PRI_IDLE : CSCHED_PRI_TS_UNDER;
	//reset_vcpu_processor(vc);	
	/*added*/
	for(i = 0; i < 4; i++)
		svc->prev_pmc[i] = 0;

	/*
	for(i = 0; i < HISTO_BUCKETS + 1; i++)
	{
		svc->vcpu_histo_wait_count[i] = 0;
		svc->vcpu_histo_wait_time[i] = 0;
		svc->vcpu_histo_hold_count[i] = 0;
		svc->vcpu_histo_hold_time[i] = 0;
	}
	svc->vcpu_wait_time_sum = 0;
	svc->vcpu_hold_time_sum = 0;
	*/
    CSCHED_VCPU_STATS_RESET(svc);
    CSCHED_STAT_CRANK(vcpu_init);
    return svc;
}

static void
csched_vcpu_insert(const struct scheduler *ops, struct vcpu *vc)
{
    struct csched_vcpu *svc = vc->sched_priv;

    if ( !__vcpu_on_runq(svc) && vcpu_runnable(vc) && !vc->is_running )
        __runq_insert(vc->processor, svc);
}

static void
csched_free_vdata(const struct scheduler *ops, void *priv)
{
    struct csched_vcpu *svc = priv;

    BUG_ON( !list_empty(&svc->runq_elem) );

    xfree(svc);
}

static void
csched_vcpu_remove(const struct scheduler *ops, struct vcpu *vc)
{
    struct csched_private *prv = CSCHED_PRIV(ops);
    struct csched_vcpu * const svc = CSCHED_VCPU(vc);
    struct csched_dom * const sdom = svc->sdom;
    unsigned long flags;

    CSCHED_STAT_CRANK(vcpu_destroy);

    if ( __vcpu_on_runq(svc) )
        __runq_remove(svc);

    spin_lock_irqsave(&(prv->lock), flags);

    if ( !list_empty(&svc->active_vcpu_elem) )
        __csched_vcpu_acct_stop_locked(prv, svc);

    spin_unlock_irqrestore(&(prv->lock), flags);

    BUG_ON( sdom == NULL );
    BUG_ON( !list_empty(&svc->runq_elem) );
}

static void
csched_vcpu_sleep(const struct scheduler *ops, struct vcpu *vc)
{
    struct csched_vcpu * const svc = CSCHED_VCPU(vc);

    CSCHED_STAT_CRANK(vcpu_sleep);

    BUG_ON( is_idle_vcpu(vc) );

    if ( per_cpu(schedule_data, vc->processor).curr == vc )
        cpu_raise_softirq(vc->processor, SCHEDULE_SOFTIRQ);
    else if ( __vcpu_on_runq(svc) )
        __runq_remove(svc);
}

static void
csched_vcpu_wake(const struct scheduler *ops, struct vcpu *vc)
{
    struct csched_vcpu * const svc = CSCHED_VCPU(vc);
    const unsigned int cpu = vc->processor;

    BUG_ON( is_idle_vcpu(vc) );

    if ( unlikely(per_cpu(schedule_data, cpu).curr == vc) )
    {
        CSCHED_STAT_CRANK(vcpu_wake_running);
        return;
    }
    if ( unlikely(__vcpu_on_runq(svc)) )
    {
        CSCHED_STAT_CRANK(vcpu_wake_onrunq);
        return;
    }

    if ( likely(vcpu_runnable(vc)) )
        CSCHED_STAT_CRANK(vcpu_wake_runnable);
    else
        CSCHED_STAT_CRANK(vcpu_wake_not_runnable);

    /*
     * We temporarly boost the priority of awaking VCPUs!
     *
     * If this VCPU consumes a non negligeable amount of CPU, it
     * will eventually find itself in the credit accounting code
     * path where its priority will be reset to normal.
     *
     * If on the other hand the VCPU consumes little CPU and is
     * blocking and awoken a lot (doing I/O for example), its
     * priority will remain boosted, optimizing it's wake-to-run
     * latencies.
     *
     * This allows wake-to-run latency sensitive VCPUs to preempt
     * more CPU resource intensive VCPUs without impacting overall 
     * system fairness.
     *
     * The one exception is for VCPUs of capped domains unpausing
     * after earning credits they had overspent. We don't boost
     * those.
     */
    if ( svc->pri == CSCHED_PRI_TS_UNDER &&
         !(svc->flags & CSCHED_FLAG_VCPU_PARKED) )
    {
        svc->pri = CSCHED_PRI_TS_BOOST;
    }

    /* Put the VCPU on the runq and tickle CPUs */
    __runq_insert(cpu, svc);
    __runq_tickle(cpu, svc);
}

static void
csched_vcpu_yield(const struct scheduler *ops, struct vcpu *vc)
{
    struct csched_vcpu * const sv = CSCHED_VCPU(vc);

    if ( !sched_credit_default_yield )
    {
        /* Let the scheduler know that this vcpu is trying to yield */
        sv->flags |= CSCHED_FLAG_VCPU_YIELD;
    }
}

static int
csched_dom_cntl(
    const struct scheduler *ops,
    struct domain *d,
    struct xen_domctl_scheduler_op *op)
{
    struct csched_dom * const sdom = CSCHED_DOM(d);
    struct csched_private *prv = CSCHED_PRIV(ops);
    unsigned long flags;

    /* Protect both get and put branches with the pluggable scheduler
     * lock. Runq lock not needed anywhere in here. */
    spin_lock_irqsave(&prv->lock, flags);

    if ( op->cmd == XEN_DOMCTL_SCHEDOP_getinfo )
    {
        op->u.credit.weight = sdom->weight;
        op->u.credit.cap = sdom->cap;
    }
    else
    {
        ASSERT(op->cmd == XEN_DOMCTL_SCHEDOP_putinfo);

        if ( op->u.credit.weight != 0 )
        {
            if ( !list_empty(&sdom->active_sdom_elem) )
            {
                prv->weight -= sdom->weight * sdom->active_vcpu_count;
                prv->weight += op->u.credit.weight * sdom->active_vcpu_count;
            }
            sdom->weight = op->u.credit.weight;
        }

        if ( op->u.credit.cap != (uint16_t)~0U )
            sdom->cap = op->u.credit.cap;

    }

    spin_unlock_irqrestore(&prv->lock, flags);

    return 0;
}

static int
csched_sys_cntl(const struct scheduler *ops,
                        struct xen_sysctl_scheduler_op *sc)
{
    int rc = -EINVAL;
    xen_sysctl_credit_schedule_t *params = &sc->u.sched_credit;
    struct csched_private *prv = CSCHED_PRIV(ops);

    switch ( sc->cmd )
    {
    case XEN_SYSCTL_SCHEDOP_putinfo:
        if (params->tslice_us > XEN_SYSCTL_CSCHED_TSLICE_UMAX
            || params->tslice_us < XEN_SYSCTL_CSCHED_TSLICE_UMIN 
            || params->ratelimit_us > XEN_SYSCTL_SCHED_RATELIMIT_MAX
            || params->ratelimit_us < XEN_SYSCTL_SCHED_RATELIMIT_MIN 
            || MICROSECS(params->ratelimit_us) > MICROSECS(params->tslice_us) )
                goto out;
        prv->tslice_us = params->tslice_us;
        prv->ratelimit_us = params->ratelimit_us;
        /* FALLTHRU */
    case XEN_SYSCTL_SCHEDOP_getinfo:
        params->tslice_us = prv->tslice_us;
        params->ratelimit_us = prv->ratelimit_us;
        rc = 0;
        break;
    }
    out:
    return rc;
}

static void *
csched_alloc_domdata(const struct scheduler *ops, struct domain *dom)
{
    struct csched_dom *sdom;

    sdom = xzalloc(struct csched_dom);
    if ( sdom == NULL )
        return NULL;

    /* Initialize credit and weight */
    INIT_LIST_HEAD(&sdom->active_vcpu);
    sdom->active_vcpu_count = 0;
    INIT_LIST_HEAD(&sdom->active_sdom_elem);
    sdom->dom = dom;
    sdom->weight = CSCHED_DEFAULT_WEIGHT;
    sdom->cap = 0U;

    return (void *)sdom;
}

static int
csched_dom_init(const struct scheduler *ops, struct domain *dom)
{
    struct csched_dom *sdom;
	int i;
    CSCHED_STAT_CRANK(dom_init);

    if ( is_idle_domain(dom) )
        return 0;

    sdom = csched_alloc_domdata(ops, dom);
    if ( sdom == NULL )
        return -ENOMEM;

    dom->sched_priv = sdom;
	sdom->spinlock_count = 0;
	sdom->event_stable_count = 0;
	sdom->spinlock_latency = 0;
	sdom->cache_miss_rate = 0;
	sdom->pending_requests = 0;
	sdom->phase = SPIN_LOW_PHASE;
	sdom->tslice_us = CSCHED_DEFAULT_TSLICE_US;
	sdom->tick_period_us = sdom->tslice_us / CSCHED_TICKS_PER_TSLICE; 
	sdom->slice_update_window = SLICE_UPDATE_WINDOW;
	sdom->event_tracking_window = EVENT_TRACKING_WINDOW;
	
	for(i = 0; i < 4; i++)
		sdom->pmc[i] = 0;
	
	for(i = 0; i < EVENT_TRACKING_WINDOW; i++)
	{
		sdom->filter[i].spinlock = 0;
		sdom->filter[i].inst_retired = 0;
		sdom->filter[i].cache_misses = 0;
	}
	for(i = 0; i < 10; i++)
	{
		sdom->submilli[i].spinlock_accum = 0;
		sdom->submilli[i].cache_miss_accum = 0;
	}
	/*
	for(i = 0; i < HISTO_BUCKETS; i++)
	{
		sdom->dom_histo_wait_count[i] = 0;
		sdom->dom_histo_wait_time[i] = 0;
		sdom->dom_histo_hold_count[i] = 0;
		sdom->dom_histo_hold_time[i] = 0;
	}
	sdom->dom_wait_time_sum = 0;
	sdom->dom_hold_time_sum = 0;
	*/
    return 0;
}

static void
csched_free_domdata(const struct scheduler *ops, void *data)
{
    xfree(data);
}

static void
csched_dom_destroy(const struct scheduler *ops, struct domain *dom)
{
    CSCHED_STAT_CRANK(dom_destroy);
    csched_free_domdata(ops, CSCHED_DOM(dom));
}

/*
 * This is a O(n) optimized sort of the runq.
 *
 * Time-share VCPUs can only be one of two priorities, UNDER or OVER. We walk
 * through the runq and move up any UNDERs that are preceded by OVERS. We
 * remember the last UNDER to make the move up operation O(1).
 */
static void
csched_runq_sort(struct csched_private *prv, unsigned int cpu)
{
    struct csched_pcpu * const spc = CSCHED_PCPU(cpu);
    struct list_head *runq, *elem, *next, *last_under;
    struct csched_vcpu *svc_elem;
    unsigned long flags;
    int sort_epoch;

    sort_epoch = prv->runq_sort;
    if ( sort_epoch == spc->runq_sort_last )
        return;

    spc->runq_sort_last = sort_epoch;

    pcpu_schedule_lock_irqsave(cpu, flags);

    runq = &spc->runq;
    elem = runq->next;
    last_under = runq;

    while ( elem != runq )
    {
        next = elem->next;
        svc_elem = __runq_elem(elem);

        if ( svc_elem->pri >= CSCHED_PRI_TS_UNDER )
        {
            /* does elem need to move up the runq? */
            if ( elem->prev != last_under )
            {
                list_del(elem);
                list_add(elem, last_under);
            }
            last_under = elem;
        }

        elem = next;
    }

    pcpu_schedule_unlock_irqrestore(cpu, flags);
}

static void
csched_acct(void* dummy)
{
    struct csched_private *prv = dummy;
    unsigned long flags;
    struct list_head *iter_vcpu, *next_vcpu;
    struct list_head *iter_sdom, *next_sdom;
    struct csched_vcpu *svc;
    struct csched_dom *sdom;
    uint32_t credit_total;
    uint32_t weight_total;
    uint32_t weight_left;
    uint32_t credit_fair;
    uint32_t credit_peak;
    uint32_t credit_cap;
	/*added*/
	uint32_t credit_prev;
    int credit_balance;
    int credit_xtra;
    int credit;


    spin_lock_irqsave(&prv->lock, flags);

    weight_total = prv->weight;
    credit_total = prv->credit;

    /* Converge balance towards 0 when it drops negative */
    if ( prv->credit_balance < 0 )
    {
        credit_total -= prv->credit_balance;
        CSCHED_STAT_CRANK(acct_balance);
    }

    if ( unlikely(weight_total == 0) )
    {
        prv->credit_balance = 0;
        spin_unlock_irqrestore(&prv->lock, flags);
        CSCHED_STAT_CRANK(acct_no_work);
        goto out;
    }

    CSCHED_STAT_CRANK(acct_run);

    weight_left = weight_total;
    credit_balance = 0;
    credit_xtra = 0;
    credit_cap = 0U;

    list_for_each_safe( iter_sdom, next_sdom, &prv->active_sdom )
    {
        sdom = list_entry(iter_sdom, struct csched_dom, active_sdom_elem);

        BUG_ON( is_idle_domain(sdom->dom) );
        BUG_ON( sdom->active_vcpu_count == 0 );
        BUG_ON( sdom->weight == 0 );
        BUG_ON( (sdom->weight * sdom->active_vcpu_count) > weight_left );

        weight_left -= ( sdom->weight * sdom->active_vcpu_count );

        /*
         * A domain's fair share is computed using its weight in competition
         * with that of all other active domains.
         *
         * At most, a domain can use credits to run all its active VCPUs
         * for one full accounting period. We allow a domain to earn more
         * only when the system-wide credit balance is negative.
         */
        credit_peak = sdom->active_vcpu_count * prv->credits_per_tslice;
        if ( prv->credit_balance < 0 )
        {
            credit_peak += ( ( -prv->credit_balance
                               * sdom->weight
                               * sdom->active_vcpu_count) +
                             (weight_total - 1)
                           ) / weight_total;
        }

        if ( sdom->cap != 0U )
        {
            credit_cap = ((sdom->cap * prv->credits_per_tslice) + 99) / 100;
            if ( credit_cap < credit_peak )
                credit_peak = credit_cap;

            /* FIXME -- set cap per-vcpu as well...? */
            credit_cap = ( credit_cap + ( sdom->active_vcpu_count - 1 )
                         ) / sdom->active_vcpu_count;
        }

        credit_fair = ( ( credit_total
                          * sdom->weight
                          * sdom->active_vcpu_count )
                        + (weight_total - 1)
                      ) / weight_total;

		/*printk(XENLOG_WARNING "domain_id:%d  slice: %5d  credit_fair:%15d  credit_total:%15d  dom_weight:%5d  dom_vcpu_count:%3d  weight_total:%5d\n",
            sdom->dom->domain_id, sdom->tslice_us, credit_fair, credit_total,sdom->weight, sdom->active_vcpu_count, weight_total);*/

        if ( credit_fair < credit_peak )
        {
            credit_xtra = 1;
        }
        else
        {
            if ( weight_left != 0U )
            {
                /* Give other domains a chance at unused credits */
                credit_total += ( ( ( credit_fair - credit_peak
                                    ) * weight_total
                                  ) + ( weight_left - 1 )
                                ) / weight_left;
            }

            if ( credit_xtra )
            {
                /*
                 * Lazily keep domains with extra credits at the head of
                 * the queue to give others a chance at them in future
                 * accounting periods.
                 */
                CSCHED_STAT_CRANK(acct_reorder);
                list_del(&sdom->active_sdom_elem);
                list_add(&sdom->active_sdom_elem, &prv->active_sdom);
            }

            credit_fair = credit_peak;
        }

        /* Compute fair share per VCPU */
        credit_fair = ( credit_fair + ( sdom->active_vcpu_count - 1 )
                      ) / sdom->active_vcpu_count;


        list_for_each_safe( iter_vcpu, next_vcpu, &sdom->active_vcpu )
        {
            svc = list_entry(iter_vcpu, struct csched_vcpu, active_vcpu_elem);
            BUG_ON( sdom != svc->sdom );
			
			credit_prev = atomic_read(&svc->credit);
            /* Increment credit */
            atomic_add(credit_fair, &svc->credit);
            credit = atomic_read(&svc->credit);

            /*
             * Recompute priority or, if VCPU is idling, remove it from
             * the active list.
             */
            if ( credit < 0 )
            {
                svc->pri = CSCHED_PRI_TS_OVER;

                /* Park running VCPUs of capped-out domains */
                if ( sdom->cap != 0U &&
                     credit < -credit_cap &&
                     !(svc->flags & CSCHED_FLAG_VCPU_PARKED) )
                {
                    CSCHED_STAT_CRANK(vcpu_park);
                    vcpu_pause_nosync(svc->vcpu);
                    svc->flags |= CSCHED_FLAG_VCPU_PARKED;
                }

                /* Lower bound on credits */
                if ( credit < -prv->credits_per_tslice )
                {
                    CSCHED_STAT_CRANK(acct_min_credit);
                    credit = -prv->credits_per_tslice;
                    atomic_set(&svc->credit, credit);
                }
            }
            else
            {
                svc->pri = CSCHED_PRI_TS_UNDER;

                /* Unpark any capped domains whose credits go positive */
                if ( svc->flags & CSCHED_FLAG_VCPU_PARKED)
                {
                    /*
                     * It's important to unset the flag AFTER the unpause()
                     * call to make sure the VCPU's priority is not boosted
                     * if it is woken up here.
                     */
                    CSCHED_STAT_CRANK(vcpu_unpark);
                    vcpu_unpause(svc->vcpu);
                    svc->flags &= ~CSCHED_FLAG_VCPU_PARKED;
                }

                /* Upper bound on credits means VCPU stops earning */
                if ( credit/100 > prv->credits_per_tslice/100 && svc->vcpu->domain->domain_id == 0)
                {
					/*printk(XENLOG_WARNING "vcpu:%2d  prev credit: %15d  credit fair: %15d  curr credit:%15d  slice_credit:%10d\n", 
							svc->vcpu->vcpu_id, credit_prev, credit_fair, credit, prv->credits_per_tslice);*/
					if(sdom->active_vcpu_count >= 2)
	                    __csched_vcpu_acct_stop_locked(prv, svc);
                    /* Divide credits in half, so that when it starts
                     * accounting again, it starts a little bit "ahead" */
                    //credit /= 2;
                    atomic_set(&svc->credit, credit);
                }
				else if(credit/100 > prv->credits_per_tslice/100 && svc->vcpu->domain->domain_id != 0)
				{
					credit /= 2;
					atomic_set(&svc->credit, credit);
				}
            }

            CSCHED_VCPU_STAT_SET(svc, credit_last, credit);
            CSCHED_VCPU_STAT_SET(svc, credit_incr, credit_fair);
            credit_balance += credit;
        }
    }

    prv->credit_balance = credit_balance;
	//printk(XENLOG_WARNING "credit_balance:%d\n", prv->credit_balance);

    spin_unlock_irqrestore(&prv->lock, flags);

    /* Inform each CPU that its runq needs to be sorted */
    prv->runq_sort++;

out:
    set_timer( &prv->master_ticker,
               NOW() + MICROSECS(prv->tslice_us));
}

static void
csched_tick(void *_cpu)
{
    unsigned int cpu = (unsigned long)_cpu;
    struct csched_pcpu *spc = CSCHED_PCPU(cpu);
    struct csched_private *prv = CSCHED_PRIV(per_cpu(scheduler, cpu));
	struct csched_vcpu * const svc = CSCHED_VCPU(current);
	struct csched_dom *sdom = svc->sdom;

    spc->tick++;

    /*
     * Accounting for running VCPU
     */
    if ( !is_idle_vcpu(current) )
        csched_vcpu_acct(prv, cpu);
	
	if(prv->tslice_us > 1111)
	{
		pmu_save_regs(current);
		pmu_restore_regs(current);
	}

    /*
     * Check if runq needs to be sorted
     *
     * Every physical CPU resorts the runq after the accounting master has
     * modified priorities. This is a special O(n) sort and runs at most
     * once per accounting period (currently 30 milliseconds).
     */
    csched_runq_sort(prv, cpu);

	if( !is_idle_vcpu(current) )
	    set_timer(&spc->ticker, NOW() + MICROSECS(sdom->tick_period_us) );
	else
		set_timer(&spc->ticker, NOW() + MICROSECS(prv->tick_period_us) );
}

static struct csched_vcpu *
csched_runq_steal(int peer_cpu, int cpu, int pri)
{
    const struct csched_pcpu * const peer_pcpu = CSCHED_PCPU(peer_cpu);
    const struct vcpu * const peer_vcpu = per_cpu(schedule_data, peer_cpu).curr;
    struct csched_vcpu *speer;
    struct list_head *iter;
    struct vcpu *vc;

    /*
     * Don't steal from an idle CPU's runq because it's about to
     * pick up work from it itself.
     */
    if ( peer_pcpu != NULL && !is_idle_vcpu(peer_vcpu) )
    {
        list_for_each( iter, &peer_pcpu->runq )
        {
            speer = __runq_elem(iter);

            /*
             * If next available VCPU here is not of strictly higher
             * priority than ours, this PCPU is useless to us.
             */
            if ( speer->pri <= pri )
                break;

            /* Is this VCPU is runnable on our PCPU? */
            vc = speer->vcpu;
            BUG_ON( is_idle_vcpu(vc) );

            if (__csched_vcpu_is_migrateable(vc, cpu))
            {
                /* We got a candidate. Grab it! */
                CSCHED_VCPU_STAT_CRANK(speer, migrate_q);
                CSCHED_STAT_CRANK(migrate_queued);
                WARN_ON(vc->is_urgent);
                __runq_remove(speer);
                vc->processor = cpu;
                return speer;
            }
        }
    }

    CSCHED_STAT_CRANK(steal_peer_idle);
    return NULL;
}

static struct csched_vcpu *
csched_load_balance(struct csched_private *prv, int cpu,
    struct csched_vcpu *snext, bool_t *stolen)
{
    struct csched_vcpu *speer;
    cpumask_t workers;
    cpumask_t *online;
    int peer_cpu;

    BUG_ON( cpu != snext->vcpu->processor );
    online = cpupool_scheduler_cpumask(per_cpu(cpupool, cpu));

    /* If this CPU is going offline we shouldn't steal work. */
    if ( unlikely(!cpumask_test_cpu(cpu, online)) )
        goto out;

    if ( snext->pri == CSCHED_PRI_IDLE )
        CSCHED_STAT_CRANK(load_balance_idle);
    else if ( snext->pri == CSCHED_PRI_TS_OVER )
        CSCHED_STAT_CRANK(load_balance_over);
    else
        CSCHED_STAT_CRANK(load_balance_other);

    /*
     * Peek at non-idling CPUs in the system, starting with our
     * immediate neighbour.
     */
    cpumask_andnot(&workers, online, prv->idlers);
    cpumask_clear_cpu(cpu, &workers);
    peer_cpu = cpu;

    while ( !cpumask_empty(&workers) )
    {
        peer_cpu = cpumask_cycle(peer_cpu, &workers);
        cpumask_clear_cpu(peer_cpu, &workers);

        /*
         * Get ahold of the scheduler lock for this peer CPU.
         *
         * Note: We don't spin on this lock but simply try it. Spinning could
         * cause a deadlock if the peer CPU is also load balancing and trying
         * to lock this CPU.
         */
        if ( !pcpu_schedule_trylock(peer_cpu) )
        {
            CSCHED_STAT_CRANK(steal_trylock_failed);
            continue;
        }

        /*
         * Any work over there to steal?
         */
        speer = cpumask_test_cpu(peer_cpu, online) ?
            csched_runq_steal(peer_cpu, cpu, snext->pri) : NULL;
        pcpu_schedule_unlock(peer_cpu);
        if ( speer != NULL )
        {
            *stolen = 1;
			//set_vcpu_affinity(speer->vcpu);
            return speer;
        }
    }

 out:
    /* Failed to find more important work elsewhere... */
    __runq_remove(snext);
    return snext;
}

/*
 * This function is in the critical path. It is designed to be simple and
 * fast for the common case.
 */
static struct task_slice
csched_schedule(
    const struct scheduler *ops, s_time_t now, bool_t tasklet_work_scheduled)
{
    const int cpu = smp_processor_id();
    struct list_head * const runq = RUNQ(cpu);
    struct csched_vcpu * const scurr = CSCHED_VCPU(current);
    struct csched_private *prv = CSCHED_PRIV(ops);
    struct csched_vcpu *snext;
    struct task_slice ret;
    s_time_t runtime, tslice;

    CSCHED_STAT_CRANK(schedule);
    CSCHED_VCPU_CHECK(current);

    runtime = now - current->runstate.state_entry_time;
    if ( runtime < 0 ) /* Does this ever happen? */
        runtime = 0;

    if ( !is_idle_vcpu(scurr->vcpu) )
    {
        /* Update credits of a non-idle VCPU. */
        burn_credits(scurr, now);
        scurr->start_time -= now;
    }
    else
    {
        /* Re-instate a boosted idle VCPU as normal-idle. */
        scurr->pri = CSCHED_PRI_IDLE;
    }

    /* Choices, choices:
     * - If we have a tasklet, we need to run the idle vcpu no matter what.
     * - If sched rate limiting is in effect, and the current vcpu has
     *   run for less than that amount of time, continue the current one,
     *   but with a shorter timeslice and return it immediately
     * - Otherwise, chose the one with the highest priority (which may
     *   be the one currently running)
     * - If the currently running one is TS_OVER, see if there
     *   is a higher priority one waiting on the runqueue of another
     *   cpu and steal it.
     */

    /* If we have schedule rate limiting enabled, check to see
     * how long we've run for. */
    if ( !tasklet_work_scheduled
         && prv->ratelimit_us
         && vcpu_runnable(current)
         && !is_idle_vcpu(current)
         && runtime < MICROSECS(prv->ratelimit_us) )
    {
        snext = scurr;
        snext->start_time += now;
        perfc_incr(delay_ms);
        tslice = MICROSECS(prv->ratelimit_us);
        ret.migrated = 0;
        goto out;
    }
    tslice = MICROSECS(prv->tslice_us);

    /*
     * Select next runnable local VCPU (ie top of local runq)
     */
    if ( vcpu_runnable(current) )
        __runq_insert(cpu, scurr);
    else
        BUG_ON( is_idle_vcpu(current) || list_empty(runq) );

    snext = __runq_elem(runq->next);
    ret.migrated = 0;

    /* Tasklet work (which runs in idle VCPU context) overrides all else. */
    if ( tasklet_work_scheduled )
    {
        snext = CSCHED_VCPU(idle_vcpu[cpu]);
        snext->pri = CSCHED_PRI_TS_BOOST;
    }

    /*
     * Clear YIELD flag before scheduling out
     */
    if ( scurr->flags & CSCHED_FLAG_VCPU_YIELD )
        scurr->flags &= ~(CSCHED_FLAG_VCPU_YIELD);

    /*
     * SMP Load balance:
     *
     * If the next highest priority local runnable VCPU has already eaten
     * through its credits, look on other PCPUs to see if we have more
     * urgent work... If not, csched_load_balance() will return snext, but
     * already removed from the runq.
     */
    if ( snext->pri > CSCHED_PRI_TS_OVER )
        __runq_remove(snext);
    else
        snext = csched_load_balance(prv, cpu, snext, &ret.migrated);

    /*
     * Update idlers mask if necessary. When we're idling, other CPUs
     * will tickle us when they get extra work.
     */
    if ( snext->pri == CSCHED_PRI_IDLE )
    {
        if ( !cpumask_test_cpu(cpu, prv->idlers) )
            cpumask_set_cpu(cpu, prv->idlers);
    }
    else if ( cpumask_test_cpu(cpu, prv->idlers) )
    {
        cpumask_clear_cpu(cpu, prv->idlers);
    }

    if ( !is_idle_vcpu(snext->vcpu) )
        snext->start_time += now;

out:
    /*
     * Return task to run next...
     */
	if(!is_idle_vcpu(snext->vcpu))
		tslice = MICROSECS(snext->sdom->tslice_us);
	else
		tslice = MICROSECS(prv->tslice_us);
		/*printk(XENLOG_WARNING "idle domain id:%3d    idle vcpu id:%3d    time slice:%6ld\n",
				snext->vcpu->domain->domain_id, snext->vcpu->vcpu_id, tslice);*/

    ret.time = (is_idle_vcpu(snext->vcpu) ?
                -1 : tslice);
    ret.task = snext->vcpu;

    CSCHED_VCPU_CHECK(ret.task);
    return ret;
}

static void
csched_dump_vcpu(struct csched_vcpu *svc)
{
    struct csched_dom * const sdom = svc->sdom;

    printk("[%i.%i] pri=%i flags=%x cpu=%i",
            svc->vcpu->domain->domain_id,
            svc->vcpu->vcpu_id,
            svc->pri,
            svc->flags,
            svc->vcpu->processor);

    if ( sdom )
    {
        printk(" credit=%i [w=%u]", atomic_read(&svc->credit), sdom->weight);
#ifdef CSCHED_STATS
        printk(" (%d+%u) {a/i=%u/%u m=%u+%u}",
                svc->stats.credit_last,
                svc->stats.credit_incr,
                svc->stats.state_active,
                svc->stats.state_idle,
                svc->stats.migrate_q,
                svc->stats.migrate_r);
#endif

    }
	printk("\n");
}

static void
csched_dump_pcpu(const struct scheduler *ops, int cpu)
{
    struct list_head *runq, *iter;
    struct csched_pcpu *spc;
    struct csched_vcpu *svc;
    int loop;
#define cpustr keyhandler_scratch

    spc = CSCHED_PCPU(cpu);
    runq = &spc->runq;

    cpumask_scnprintf(cpustr, sizeof(cpustr), per_cpu(cpu_sibling_mask, cpu));
    printk(" sort=%d, sibling=%s, ", spc->runq_sort_last, cpustr);
    cpumask_scnprintf(cpustr, sizeof(cpustr), per_cpu(cpu_core_mask, cpu));
    printk("core=%s\n", cpustr);

    /* current VCPU */
    svc = CSCHED_VCPU(per_cpu(schedule_data, cpu).curr);
    if ( svc )
    {
        printk("\trun: ");
        csched_dump_vcpu(svc);
    }

    loop = 0;
    list_for_each( iter, runq )
    {
        svc = __runq_elem(iter);
        if ( svc )
        {
            printk("\t%3d: ", ++loop);
            csched_dump_vcpu(svc);
        }
    }
#undef cpustr
}

static void
csched_dump(const struct scheduler *ops)
{
    struct list_head *iter_sdom, *iter_svc;
    struct csched_private *prv = CSCHED_PRIV(ops);
    int loop;
    unsigned long flags;

	spin_lock_irqsave(&(prv->lock), flags);

#define idlers_buf keyhandler_scratch

    printk("info:\n"
           "\tncpus              = %u\n"
           "\tmaster             = %u\n"
           "\tcredit             = %u\n"
           "\tcredit balance     = %d\n"
           "\tweight             = %u\n"
           "\trunq_sort          = %u\n"
           "\tdefault-weight     = %d\n"
           "\ttslice             = %dus\n"
           "\tratelimit          = %dus\n"
           "\tcredits per msec   = %d\n"
           "\tticks per tslice   = %d\n"
           "\tmigration delay    = %uus\n",
           prv->ncpus,
           prv->master,
           prv->credit,
           prv->credit_balance,
           prv->weight,
           prv->runq_sort,
           CSCHED_DEFAULT_WEIGHT,
           prv->tslice_us,
           prv->ratelimit_us,
           CSCHED_CREDITS_PER_MSEC,
           prv->ticks_per_tslice,
           vcpu_migration_delay);

    cpumask_scnprintf(idlers_buf, sizeof(idlers_buf), prv->idlers);
    printk("idlers: %s\n", idlers_buf);

    printk("active vcpus:\n");
    loop = 0;
    list_for_each( iter_sdom, &prv->active_sdom )
    {
        struct csched_dom *sdom;
        sdom = list_entry(iter_sdom, struct csched_dom, active_sdom_elem);

        list_for_each( iter_svc, &sdom->active_vcpu )
        {
            struct csched_vcpu *svc;
            svc = list_entry(iter_svc, struct csched_vcpu, active_vcpu_elem);

            printk("\t%3d: ", ++loop);
            csched_dump_vcpu(svc);
        }
    }
	
    printk("\n");

#undef idlers_buf
    spin_unlock_irqrestore(&(prv->lock), flags);
}

/*added*/
static void 
csched_dump_customized(const struct scheduler *ops)
{
	struct csched_private *prv = CSCHED_PRIV(ops);
    struct domain *dom;
    struct vcpu *vc;
    //int i;
	unsigned long flags;
	
	spin_lock_irqsave(&(prv->lock), flags);
#define ctmstr keyhandler_scratch

	cpumask_scnprintf(ctmstr, sizeof(ctmstr), prv->cpus);
	printk("cpus: %s\n", ctmstr);

	for_each_domain(dom)
	{
		//struct csched_dom *csdom = CSCHED_DOM(dom);
		if(is_idle_domain(dom)) continue;
		printk("dom%d    \n", dom->domain_id);
	
		/*
		printk("    dom: histo wait count buckets: ");
		for(i = 0; i < HISTO_BUCKETS + 1; i++)
			printk("%llu ", csdom->dom_histo_wait_count[i]);
		printk("\n");
		printk("    dom histo wait time buckets: (ms)");
		for(i = 0; i < HISTO_BUCKETS + 1; i++)
			printk("%llu ", csdom->dom_histo_wait_time[i] / 1000000);
		printk("\n");
		printk("    dom histo hold count buckets: ");
		for(i = 0; i < HISTO_BUCKETS + 1; i++)
			printk("%llu ", csdom->dom_histo_hold_count[i]);
		printk("\n");
		printk("    dom histo hold time buckets: (ms)");
		for(i = 0; i <  HISTO_BUCKETS + 1; i++)
			printk("%llu ", csdom->dom_histo_hold_time[i] / 1000000);
		printk("\n");
		printk("    dom spin wait time sum: %llums\n", csdom->dom_wait_time_sum / 1000000);
		printk("    dom spin hold time sum: %llums\n", csdom->dom_hold_time_sum / 1000000);
	   */	
		for_each_vcpu(dom, vc)
		{
			//struct csched_vcpu *csvc = CSCHED_VCPU(vc);
			printk("    vcpu%d: \n", vc->vcpu_id);
			/*
			printk("        vcpu waiting histo count buckets: ");
			for(i = 0; i < HISTO_BUCKETS + 1; i++)
				printk("%llu ", csvc->vcpu_histo_wait_count[i]);
			printk("\n");
			printk("        vcpu waiting histo time buckets: (ms)");
			for(i = 0; i < HISTO_BUCKETS + 1; i++)
				printk("%llu ", csvc->vcpu_histo_wait_time[i] / 1000000);
			printk("\n");
			printk("        vcpu holding histo count buckets: ");
			for(i = 0; i < HISTO_BUCKETS + 1; i++)
				printk("%llu ", csvc->vcpu_histo_hold_count[i]);
			printk("\n");
			printk("        vcpu holding histo time buckets: (ms)");
			for(i = 0; i < HISTO_BUCKETS + 1; i++)
				printk("%llu ", csvc->vcpu_histo_hold_time[i] / 1000000);
			printk("\n");
			printk("        vcpu spin waiting time sum: %llums\n", csvc->vcpu_wait_time_sum / 1000000);
			printk("        vcpu spin holding time sum: %llums\n", csvc->vcpu_hold_time_sum / 1000000);
			*/
            printk("        pmuinfo: INST_RETIRED=%llu  CPU_CLK_UNHALTED=%llu  LLC_REFERENCES=%llu  LLC_MISSES=%llu\n", vc->pmc[0], vc->pmc[1], vc->pmc[2], vc->pmc[3]);
			printk("        sched_count: %llu\n", vc->sched_count);
		}
	}
    printk("\n");
#undef ctmstr

    spin_unlock_irqrestore(&(prv->lock), flags);
}	

static int
csched_init(struct scheduler *ops)
{
    struct csched_private *prv;

    prv = xzalloc(struct csched_private);
    if ( prv == NULL )
        return -ENOMEM;
    if ( !zalloc_cpumask_var(&prv->cpus) ||
         !zalloc_cpumask_var(&prv->idlers) )
    {
        free_cpumask_var(prv->cpus);
        xfree(prv);
        return -ENOMEM;
    }

    ops->sched_data = prv;
    spin_lock_init(&prv->lock);
    INIT_LIST_HEAD(&prv->active_sdom);
    prv->master = UINT_MAX;
	prv->metric_update_last = prv->metric_update;

	global_prv = prv;

    if ( sched_credit_tslice_us > XEN_SYSCTL_CSCHED_TSLICE_UMAX
         || sched_credit_tslice_us < XEN_SYSCTL_CSCHED_TSLICE_UMIN )
    {
        printk("WARNING: sched_credit_tslice_us outside of valid range [%d,%d].\n"
               " Resetting to default %u\n",
               XEN_SYSCTL_CSCHED_TSLICE_UMIN,
               XEN_SYSCTL_CSCHED_TSLICE_UMAX,
               CSCHED_DEFAULT_TSLICE_US);
        sched_credit_tslice_us = CSCHED_DEFAULT_TSLICE_US;
    }

    if ( sched_ratelimit_us > XEN_SYSCTL_SCHED_RATELIMIT_MAX
         || sched_ratelimit_us < XEN_SYSCTL_SCHED_RATELIMIT_MIN )
    {
        printk("WARNING: sched_ratelimit_us outside of valid range [%d,%d].\n"
               " Resetting to default %u\n",
               XEN_SYSCTL_SCHED_RATELIMIT_MIN,
               XEN_SYSCTL_SCHED_RATELIMIT_MAX,
               SCHED_DEFAULT_RATELIMIT_US);
        sched_ratelimit_us = SCHED_DEFAULT_RATELIMIT_US;
    }

    prv->tslice_us = sched_credit_tslice_us;
    prv->ticks_per_tslice = CSCHED_TICKS_PER_TSLICE;
    if ( prv->tslice_us < prv->ticks_per_tslice )
        prv->ticks_per_tslice = 1;
    prv->tick_period_us = prv->tslice_us / prv->ticks_per_tslice;
    prv->credits_per_tslice = CSCHED_CREDIT_PER_US * prv->tslice_us;

    if ( MICROSECS(sched_ratelimit_us) > MICROSECS(sched_credit_tslice_us) )
    {
        printk("WARNING: sched_ratelimit_us >" 
               "sched_credit_tslice_us is undefined\n"
               "Setting ratelimit_us to 1000 * tslice_ms\n");
        prv->ratelimit_us = prv->tslice_us;
    }
    else
        prv->ratelimit_us = sched_ratelimit_us;
    return 0;
}

static void
csched_deinit(const struct scheduler *ops)
{
    struct csched_private *prv;

    prv = CSCHED_PRIV(ops);
    if ( prv != NULL )
    {
        free_cpumask_var(prv->cpus);
        free_cpumask_var(prv->idlers);
        xfree(prv);
    }
}

static void csched_tick_suspend(const struct scheduler *ops, unsigned int cpu)
{
    struct csched_pcpu *spc;

    spc = CSCHED_PCPU(cpu);

    stop_timer(&spc->ticker);
	stop_timer(&spc->metric_ticker);
}

static void csched_tick_resume(const struct scheduler *ops, unsigned int cpu)
{
    struct csched_private *prv;
    struct csched_pcpu *spc;
    uint64_t now = NOW();

    spc = CSCHED_PCPU(cpu);

    prv = CSCHED_PRIV(ops);

    set_timer(&spc->ticker, now + MICROSECS(prv->tick_period_us)
            - now % MICROSECS(prv->tick_period_us) );
	set_timer(&spc->metric_ticker, now + MICROSECS(CSCHED_METRIC_TICK_PERIOD)
			- now % MICROSECS(CSCHED_METRIC_TICK_PERIOD) );
}

static struct csched_private _csched_priv;

const struct scheduler sched_credit_def = {
    .name           = "SMP Credit Scheduler",
    .opt_name       = "credit",
    .sched_id       = XEN_SCHEDULER_CREDIT,
    .sched_data     = &_csched_priv,

    .init_domain    = csched_dom_init,
    .destroy_domain = csched_dom_destroy,

    .insert_vcpu    = csched_vcpu_insert,
    .remove_vcpu    = csched_vcpu_remove,

    .sleep          = csched_vcpu_sleep,
    .wake           = csched_vcpu_wake,
    .yield          = csched_vcpu_yield,

    .adjust         = csched_dom_cntl,
    .adjust_global  = csched_sys_cntl,

    .pick_cpu       = csched_cpu_pick,
    .do_schedule    = csched_schedule,

    .dump_cpu_state = csched_dump_pcpu,
    .dump_settings  = csched_dump,
	.dump_admin_conf= csched_dump_customized,
    .init           = csched_init,
    .deinit         = csched_deinit,
    .alloc_vdata    = csched_alloc_vdata,
    .free_vdata     = csched_free_vdata,
    .alloc_pdata    = csched_alloc_pdata,
    .free_pdata     = csched_free_pdata,
    .alloc_domdata  = csched_alloc_domdata,
    .free_domdata   = csched_free_domdata,

    .tick_suspend   = csched_tick_suspend,
    .tick_resume    = csched_tick_resume,
};
