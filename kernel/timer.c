/*
 * Copyright (c) 2008-2009 Travis Geiselbrecht
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files
 * (the "Software"), to deal in the Software without restriction,
 * including without limitation the rights to use, copy, modify, merge,
 * publish, distribute, sublicense, and/or sell copies of the Software,
 * and to permit persons to whom the Software is furnished to do so,
 * subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

/**
 * @file
 * @brief  Kernel timer subsystem
 * @defgroup timer Timers
 *
 * The timer subsystem allows functions to be scheduled for later
 * execution.  Each timer object is used to cause one function to
 * be executed at a later time.
 *
 * Timer callback functions are called in interrupt context.
 *
 * @{
 */
#include <debug.h>
#include <assert.h>
#include <list.h>
#include <kernel/thread.h>
#include <kernel/timer.h>
#include <kernel/debug.h>
#include <platform/timer.h>
#include <platform.h>

#define LOCAL_TRACE 0

static struct list_node timer_queue;

static enum handler_return timer_tick(void *arg, lk_time_t now);

/**
 * @brief  Initialize a timer object
 */
void timer_initialize(timer_t *timer)
{
	*timer = (timer_t)TIMER_INITIAL_VALUE(*timer);
}

static void insert_timer_in_queue(timer_t *timer)
{
	timer_t *entry;

	LTRACEF("timer %p, scheduled %lu, periodic %lu\n", timer, timer->scheduled_time, timer->periodic_time);

	list_for_every_entry(&timer_queue, entry, timer_t, node) {
		if (TIME_GT(entry->scheduled_time, timer->scheduled_time)) {
			list_add_before(&entry->node, &timer->node);
			return;
		}
	}

	/* walked off the end of the list */
	list_add_tail(&timer_queue, &timer->node);
}

static void timer_set(timer_t *timer, lk_time_t delay, lk_time_t period, timer_callback callback, void *arg)
{
	lk_time_t now;

	LTRACEF("timer %p, delay %lu, period %lu, callback %p, arg %p, now %lu\n", timer, delay, period, callback, arg, now);

	DEBUG_ASSERT(timer->magic == TIMER_MAGIC);

	if (list_in_list(&timer->node)) {
		panic("timer %p already in list\n", timer);
	}

	now = current_time();
	timer->scheduled_time = now + delay;
	timer->periodic_time = period;
	timer->callback = callback;
	timer->arg = arg;

	LTRACEF("scheduled time %lu\n", timer->scheduled_time);

	enter_critical_section();

	insert_timer_in_queue(timer);

#if PLATFORM_HAS_DYNAMIC_TIMER
	if (list_peek_head_type(&timer_queue, timer_t, node) == timer) {
		/* we just modified the head of the timer queue */
		LTRACEF("setting new timer for %u msecs\n", (uint)delay);
		platform_set_oneshot_timer(timer_tick, NULL, delay);
	}
#endif

	exit_critical_section();
}

/**
 * @brief  Set up a timer that executes once
 *
 * This function specifies a callback function to be called after a specified
 * delay.  The function will be called one time.
 *
 * @param  timer The timer to use
 * @param  delay The delay, in ms, before the timer is executed
 * @param  callback  The function to call when the timer expires
 * @param  arg  The argument to pass to the callback
 *
 * The timer function is declared as:
 *   enum handler_return callback(struct timer *, lk_time_t now, void *arg) { ... }
 */
void timer_set_oneshot(timer_t *timer, lk_time_t delay, timer_callback callback, void *arg)
{
	if (delay == 0)
		delay = 1;
	timer_set(timer, delay, 0, callback, arg);
}

/**
 * @brief  Set up a timer that executes repeatedly
 *
 * This function specifies a callback function to be called after a specified
 * delay.  The function will be called repeatedly.
 *
 * @param  timer The timer to use
 * @param  delay The delay, in ms, before the timer is executed
 * @param  callback  The function to call when the timer expires
 * @param  arg  The argument to pass to the callback
 *
 * The timer function is declared as:
 *   enum handler_return callback(struct timer *, lk_time_t now, void *arg) { ... }
 */
void timer_set_periodic(timer_t *timer, lk_time_t period, timer_callback callback, void *arg)
{
	if (period == 0)
		period = 1;
	timer_set(timer, period, period, callback, arg);
}

/**
 * @brief  Cancel a pending timer
 */
void timer_cancel(timer_t *timer)
{
	DEBUG_ASSERT(timer->magic == TIMER_MAGIC);

	enter_critical_section();

#if PLATFORM_HAS_DYNAMIC_TIMER
	timer_t *oldhead = list_peek_head_type(&timer_queue, timer_t, node);
#endif

	if (list_in_list(&timer->node))
		list_delete(&timer->node);

	/* to keep it from being reinserted into the queue if called from
	 * periodic timer callback.
	 */
	timer->periodic_time = 0;
	timer->callback = NULL;
	timer->arg = NULL;

#if PLATFORM_HAS_DYNAMIC_TIMER
	/* see if we've just modified the head of the timer queue */
	timer_t *newhead = list_peek_head_type(&timer_queue, timer_t, node);
	if (newhead == NULL) {
		LTRACEF("clearing old hw timer, nothing in the queue\n");
		platform_stop_timer();
	} else if (newhead != oldhead) {
		lk_time_t delay;
		lk_time_t now = current_time();

		if (TIME_LT(newhead->scheduled_time, now))
			delay = 0;
		else
			delay = newhead->scheduled_time - now;

		LTRACEF("setting new timer to %d\n", delay);
		platform_set_oneshot_timer(timer_tick, NULL, delay);
	}
#endif

	exit_critical_section();
}

/* called at interrupt time to process any pending timers */
static enum handler_return timer_tick(void *arg, lk_time_t now)
{
	timer_t *timer;
	enum handler_return ret = INT_NO_RESCHEDULE;

	THREAD_STATS_INC(timer_ints);
//	KEVLOG_TIMER_TICK(); // enable only if necessary

	LTRACEF("now %lu, sp %p\n", now, __GET_FRAME());

	for (;;) {
		/* see if there's an event to process */
		timer = list_peek_head_type(&timer_queue, timer_t, node);
		if (likely(timer == 0))
			break;
		LTRACEF("next item on timer queue %p at %lu now %lu (%p, arg %p)\n", timer, timer->scheduled_time, now, timer->callback, timer->arg);
		if (likely(TIME_LT(now, timer->scheduled_time)))
			break;

		/* process it */
		LTRACEF("timer %p\n", timer);
		DEBUG_ASSERT(timer && timer->magic == TIMER_MAGIC);
		list_delete(&timer->node);

		LTRACEF("dequeued timer %p, scheduled %lu periodic %lu\n", timer, timer->scheduled_time, timer->periodic_time);

		THREAD_STATS_INC(timers);

		bool periodic = timer->periodic_time > 0;

		LTRACEF("timer %p firing callback %p, arg %p\n", timer, timer->callback, timer->arg);
		KEVLOG_TIMER_CALL(timer->callback, timer->arg);
		if (timer->callback(timer, now, timer->arg) == INT_RESCHEDULE)
			ret = INT_RESCHEDULE;

		/* if it was a periodic timer and it hasn't been requeued
		 * by the callback put it back in the list
		 */
		if (periodic && !list_in_list(&timer->node) && timer->periodic_time > 0) {
			LTRACEF("periodic timer, period %u\n", (uint)timer->periodic_time);
			timer->scheduled_time = now + timer->periodic_time;
			insert_timer_in_queue(timer);
		}
	}

#if PLATFORM_HAS_DYNAMIC_TIMER
	/* reset the timer to the next event */
	timer = list_peek_head_type(&timer_queue, timer_t, node);
	if (timer) {
		/* has to be the case or it would have fired already */
		DEBUG_ASSERT(TIME_GT(timer->scheduled_time, now));

		lk_time_t delay = timer->scheduled_time - now;

		LTRACEF("setting new timer for %u msecs for event %p\n", (uint)delay, timer);
		platform_set_oneshot_timer(timer_tick, NULL, delay);
	}
#else
	/* let the scheduler have a shot to do quantum expiration, etc */
	/* in case of dynamic timer, the scheduler will set up a periodic timer */
	if (thread_timer_tick() == INT_RESCHEDULE)
		ret = INT_RESCHEDULE;
#endif

	DEBUG_ASSERT(in_critical_section());
	return ret;
}

void timer_init(void)
{
	list_initialize(&timer_queue);

	/* register for a periodic timer tick */
	platform_set_periodic_timer(timer_tick, NULL, 10); /* 10ms */
}


