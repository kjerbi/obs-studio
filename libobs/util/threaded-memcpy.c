/*
* Copyright (c) 2017 Zachary Lund <zachary.lund@streamlabs.com>
*
* Based on Michael Dirks (aka Xaymar) threaded memcpy tests
*
* Permission to use, copy, modify, and distribute this software for any
* purpose with or without fee is hereby granted, provided that the above
* copyright notice and this permission notice appear in all copies.
*
* THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
* WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
* MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
* ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
* WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
* ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
* OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
*/

#include "threading.h"
#include "bmem.h"

/* Prevents WinAPI redefinition down the road. */
#define NOMINMAX

#define max(x, y) (((x) > (y)) ? (x) : (y))
#define min(x, y) (((x) < (y)) ? (x) : (y))

/* TODO: Will need adjustment */
#define MAX_THREAD_COUNT 8

struct memcpy_thread_work {
	void* from;
	void* to;
	size_t size;
	os_sem_t *semaphore;
	size_t block_size;
	size_t block_size_rem;
	struct memcpy_thread_work *next;
};

/* Static global data */
const size_t block_size = 64 * 1024;

struct memcpy_environment {
	int thread_count;
	pthread_t threads[MAX_THREAD_COUNT];
	struct memcpy_thread_work *work_queue;
	pthread_mutex_t work_queue_mutex;
	pthread_cond_t work_queue_cond;
	int running;
};

static void *start_memcpy_thread(void* context)
{
	struct memcpy_environment *env = context;
	struct memcpy_thread_work *work;

	os_sem_t* semaphore;
	void *from, *to;
	size_t size;

	for (;;) {
		pthread_mutex_lock(&env->work_queue_mutex);

		while (!env->work_queue && env->running) {
			pthread_cond_wait(&env->work_queue_cond, &env->work_queue_mutex);
		}

		if (!env->running) {
			pthread_mutex_unlock(&env->work_queue_mutex);
			break;
		}

		work = env->work_queue;
		from = work->from;
		to = work->to;
		size = work->block_size + work->block_size_rem;
		semaphore = work->semaphore;

		if (work->size > size) {
			work->from = ((uint8_t*)work->from) + size;
			work->to = ((uint8_t*)work->to) + size;
			work->size -= size;
		}
		else {
			if (env->work_queue->next != NULL) {
				env->work_queue = env->work_queue->next;
			} else {
				env->work_queue = NULL;
			}
		}

		pthread_mutex_unlock(&env->work_queue_mutex);

		memcpy((uint8_t*)to, (uint8_t*)from, size);

		/* Notify the calling thread that this thread is done working */
		os_sem_post(work->semaphore);
	}

	return 0;
}

/* Not thread safe but only needs to be called once for all threads */
struct memcpy_environment *init_threaded_memcpy_pool(int threads)
{
	struct memcpy_environment *env =
		bmalloc(sizeof(struct memcpy_environment));

	/* TODO: Determine system physical core count at runtime. */
	if (!threads)
		env->thread_count = MAX_THREAD_COUNT;
	else
		env->thread_count = threads;

	env->work_queue = NULL;
	env->running = true;
	pthread_cond_init(&env->work_queue_cond, NULL);
	pthread_mutex_init(&env->work_queue_mutex, NULL);

	for (int i = 0; i < env->thread_count; ++i) {
		pthread_create(
			&env->threads[i],
			NULL,
			start_memcpy_thread,
			(void*)env);
	}

	return env;
}

void destroy_threaded_memcpy_pool(struct memcpy_environment *env)
{
	pthread_mutex_lock(&env->work_queue_mutex);
	env->running = false;
	pthread_mutex_unlock(&env->work_queue_mutex);

	for (int i = 0; i < env->thread_count; ++i) {
		/* Since we don't know which thread wakes up, wake them all up. */
		pthread_cond_broadcast(&env->work_queue_cond);
		pthread_join(env->threads[i], NULL);
	}

	pthread_cond_destroy(&env->work_queue_cond);
	pthread_mutex_destroy(&env->work_queue_mutex);
	bfree(env);
}

void threaded_memcpy(void *destination, void *source, size_t size, struct memcpy_environment *env)
{
	struct memcpy_thread_work work;

	os_sem_t *finish_signal;

	size_t blocks =
		min(max(size, block_size) / block_size, env->thread_count);

	os_sem_init(&finish_signal, 0);

	work.block_size = size / blocks;
	work.block_size_rem = size - (work.block_size * blocks);
	work.size = size;
	work.to = (uint8_t*)destination;
	work.from = (uint8_t*)source;
	work.semaphore = finish_signal;
	work.next = NULL;

	pthread_mutex_lock(&env->work_queue_mutex);

	if (env->work_queue == NULL) {
		env->work_queue = &work;
	} else {
		struct memcpy_thread_work *iter = env->work_queue;
		while (iter->next) iter = iter->next;
		iter->next = &work;
	}

	for (int i = 0; i < blocks; ++i)
		pthread_cond_signal(&env->work_queue_cond);

	pthread_mutex_unlock(&env->work_queue_mutex);

	/* Wait for a signal from each job */
	for (int i = 0; i < blocks; ++i) {
		os_sem_wait(finish_signal);
	}

	os_sem_destroy(finish_signal);
}