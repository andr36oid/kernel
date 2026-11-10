/* SPDX-License-Identifier: GPL-2.0 */
#undef TRACE_SYSTEM
#define TRACE_SYSTEM schedtune
#if !defined(_TRACE_SCHEDTUNE_H) || defined(TRACE_HEADER_MULTI_READ)
#define _TRACE_SCHEDTUNE_H
#include <linux/sched.h>
#include <linux/tracepoint.h>
TRACE_EVENT(sched_tune_tasks_update,

	TP_PROTO(struct task_struct *tsk, int cpu, int tasks, int idx,
		int boost, int max_boost, u64 group_ts),

	TP_ARGS(tsk, cpu, tasks, idx, boost, max_boost, group_ts),

	TP_STRUCT__entry(
		__array( char,  comm,   TASK_COMM_LEN   )
		__field( pid_t,         pid             )
		__field( int,           cpu             )
		__field( int,           tasks           )
		__field( int,           idx             )
		__field( int,           boost           )
		__field( int,           max_boost       )
		__field( u64,		group_ts	)
	),

	TP_fast_assign(
		memcpy(__entry->comm, tsk->comm, TASK_COMM_LEN);
		__entry->pid            = tsk->pid;
		__entry->cpu            = cpu;
		__entry->tasks          = tasks;
		__entry->idx            = idx;
		__entry->boost          = boost;
		__entry->max_boost      = max_boost;
		__entry->group_ts	= group_ts;
	),

	TP_printk("pid=%d comm=%s "
		"cpu=%d tasks=%d idx=%d boost=%d max_boost=%d timeout=%llu",
		__entry->pid, __entry->comm,
		__entry->cpu, __entry->tasks, __entry->idx,
		__entry->boost, __entry->max_boost,
		__entry->group_ts)
);

/*
 * Tracepoint for schedtune_boostgroup_update
 */
TRACE_EVENT(sched_tune_boostgroup_update,

	TP_PROTO(int cpu, int variation, int max_boost),

	TP_ARGS(cpu, variation, max_boost),

	TP_STRUCT__entry(
		__field( int,   cpu		)
		__field( int,   variation	)
		__field( int,   max_boost	)
	),

	TP_fast_assign(
		__entry->cpu            = cpu;
		__entry->variation      = variation;
		__entry->max_boost      = max_boost;
	),

	TP_printk("cpu=%d variation=%d max_boost=%d",
		__entry->cpu, __entry->variation, __entry->max_boost)
);


#endif
#include <trace/define_trace.h>
