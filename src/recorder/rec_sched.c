/* -*- Mode: C; tab-width: 8; c-basic-offset: 8; indent-tabs-mode: t; -*- */

#include "rec_sched.h"

#include <assert.h>
#include <signal.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/queue.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>

#include "recorder.h"

#include "../share/config.h"
#include "../share/dbg.h"
#include "../share/hpc.h"
#include "../share/sys.h"
#include "../share/task.h"

struct tasklist_entry {
	struct context ctx;
	CIRCLEQ_ENTRY(tasklist_entry) entries;
};

CIRCLEQ_HEAD(tasklist, tasklist_entry) head = CIRCLEQ_HEAD_INITIALIZER(head);

static struct tasklist_entry* tid_to_entry[MAX_TID];
static struct tasklist_entry* current_entry;
static int num_active_threads;

static struct tasklist_entry* get_entry(pid_t tid)
{
	return tid_to_entry[tid];
}

static struct context* get_task(pid_t tid)
{
	struct tasklist_entry* entry = get_entry(tid);
	return entry ? &entry->ctx : NULL;
}

static struct tasklist_entry* next_entry(struct tasklist_entry* elt)
{
	return CIRCLEQ_LOOP_NEXT(&head, elt, entries);
}

static void note_switch(struct context* prev_ctx, struct context* ctx,
			int max_events)
{
	if (prev_ctx == ctx) {
		ctx->switch_counter--;
	} else {
		ctx->switch_counter = max_events;
	}
}

/**
 * Retrieves a thread from the pool of active threads in a
 * round-robin fashion.
 */
struct context* rec_sched_get_active_thread(const struct flags* flags,
					    struct context* ctx,
					    int* by_waitpid)
{
	int max_events = flags->max_events;
	struct tasklist_entry* entry = current_entry;
	struct context* next_ctx = NULL;

	debug("Scheduling next task");

	*by_waitpid = 0;

	if (!entry) {
		entry = current_entry = CIRCLEQ_FIRST(&head);
	}

	if (ctx && !ctx->switchable) {
		debug("  (%d is un-switchable)", ctx->tid);
		/* TODO: if |ctx| is blocked on a syscall, returning
		 * it now will cause a spin-loop when the recorder
		 * sees that |ctx|'s state hasn't changed and asks us
		 * to schedule another context, from which we'll be
		 * forced to return |ctx| again because it's not
		 * switchable.  Then the recorder will ask us to
		 * schedule another context, and ... */
		return ctx;
	}

	/* Prefer switching to the next task if the current one
	 * exceeded its event limit. */
	if (ctx && ctx->switch_counter < 0) {
		debug("  previous task exceeded event limit, preferring next");
		entry = current_entry = next_entry(entry);
		ctx->switch_counter = max_events;
	}

	/* Go around the task list exactly one time looking for a
	 * runnable thread. */
	do {
		pid_t tid;

		next_ctx = &entry->ctx;
		tid = next_ctx->tid;
		if (PROCESSING_SYSCALL != next_ctx->exec_state) {
			debug("  %d isn't blocked, done", tid);
			break;
		}
		debug("  %d is blocked, checking status ...", tid);
		if (0 != sys_waitpid_nonblock(tid, &next_ctx->status)) {
			*by_waitpid = 1;
			debug("  ready!");
			break;
		}
		debug("  still blocked");
		next_ctx = NULL;
		entry = next_entry(entry);
	} while (entry != current_entry);

	if (!next_ctx) {
		/* All the tasks are blocked. Wait for the next one to
		 * change state. */
		int status;
		pid_t tid;

		debug("  all tasks blocked, waiting for runnable (%d total)",
		      num_active_threads);
		while (-1 == (tid = waitpid(-1, &status,
					    __WALL | WSTOPPED | WUNTRACED))) {
			if (EINTR == errno) {
				debug("  waitpid() interrupted by EINTR");
				continue;
			}
			fatal("Failed to waitpid()");
		}
		debug("  %d changed state", tid);

		entry = get_entry(tid);
		next_ctx = &entry->ctx;

		next_ctx->status = status;
		*by_waitpid = 1;
	}

	current_entry = entry;
	note_switch(ctx, next_ctx, max_events);
	return next_ctx;
}

/**
 * Sends a SIGINT to all processes/threads.
 */
void rec_sched_exit_all()
{
	while (!CIRCLEQ_EMPTY(&head)) {
		struct tasklist_entry* entry = CIRCLEQ_FIRST(&head);
		struct context* ctx = &entry->ctx;

		sys_kill(ctx->tid, SIGINT);
		rec_sched_deregister_thread(&ctx);
	}
}

int rec_sched_get_num_threads()
{
	return num_active_threads;
}

/**
 * Registers a new thread to the runtime system. This includes
 * initialization of the hardware performance counters
 */
void rec_sched_register_thread(const struct flags* flags,
			       pid_t parent, pid_t child)
{
	struct tasklist_entry* entry = sys_malloc_zero(sizeof(*entry));
	struct context* ctx = &entry->ctx;

	assert(child > 0 && child < MAX_TID);

	ctx->exec_state = RUNNABLE;
	ctx->status = 0;
	ctx->rec_tid = ctx->tid = child;
	ctx->child_mem_fd = sys_open_child_mem(child);
	if (parent) {
		struct context* parent_ctx = get_task(parent);
		ctx->syscallbuf_lib_start = parent_ctx->syscallbuf_lib_start;
		ctx->syscallbuf_lib_end = parent_ctx->syscallbuf_lib_end;
	}
	/* These will be initialized when the syscall buffer is. */
	ctx->desched_fd = ctx->desched_fd_child = -1;

	sys_ptrace_setup(child);

	init_hpc(ctx);
	start_hpc(ctx, flags->max_rbc);

	CIRCLEQ_INSERT_TAIL(&head, entry, entries);
	num_active_threads++;

	tid_to_entry[child] = entry;
}

/**
 * De-regsiter a thread and de-allocate all resources. This function
 * should be called when a thread exits.
 */
void rec_sched_deregister_thread(struct context** ctx_ptr)
{
	struct context* ctx = *ctx_ptr;
	pid_t tid = ctx->tid;
	struct tasklist_entry* entry = get_entry(tid);

	if (entry == current_entry) {
		current_entry = next_entry(entry);
		if (entry == current_entry) {
			assert(num_active_threads == 1);
			current_entry = NULL;
		}
	}

	CIRCLEQ_REMOVE(&head, entry, entries);
	tid_to_entry[tid] = NULL;
	num_active_threads--;
	assert(num_active_threads >= 0);

	/* delete all counter data */
	cleanup_hpc(ctx);

	sys_close(ctx->child_mem_fd);
	close(ctx->desched_fd);

	sys_ptrace_detach(tid);

	/* finally, free the memory */
	sys_free((void**)entry);
	*ctx_ptr = NULL;
}
