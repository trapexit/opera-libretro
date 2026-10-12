// Exercises bounded capture backpressure, FIFO draining, and writer failures.
#define _POSIX_C_SOURCE 200809L

#include "test_harness_capture.h"

#include <pthread.h>

#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

enum
  {
    TEST_PIXEL_BYTES = 3,
    TEST_PATH_BYTES = 32,
    TEST_NS_PER_SECOND = 1000000000,
    TEST_WAIT_NS = 100000000
  };

typedef struct Fixture
{
  pthread_mutex_t _mutex;
  pthread_cond_t _changed;
  HarnessCaptureQueue *_queue;
  size_t _calls;
  int _submit_result;
  bool _writer_entered;
  bool _producer_entered;
  bool _producer_returned;
  bool _released;
  bool _fail_first;
} Fixture;

static Fixture g_FIXTURE;

static
void
_check(int result_)
{
  assert(result_ == 0);
}


static
int
_submit_frame(HarnessCaptureQueue *queue_,
              size_t               number_)
{
  unsigned char *pixels;
  char path[TEST_PATH_BYTES];
  int result;

  pixels = malloc(TEST_PIXEL_BYTES);
  assert(pixels != NULL);
  memset(pixels, (int)number_, TEST_PIXEL_BYTES);
  snprintf(path, sizeof(path), "%zu", number_);
  result = harness_capture_queue_submit(queue_, path, pixels, 1, 1);
  if(result != 0)
    free(pixels);
  return result;
}


static
int
_write_frame(const char          *path_,
             const unsigned char *pixels_,
             unsigned             width_,
             unsigned             height_)
{
  int result;

  _check(pthread_mutex_lock(&g_FIXTURE._mutex));
  assert((width_ == 1) && (height_ == 1));
  assert(strtoul(path_, NULL, 10) == g_FIXTURE._calls);
  assert(pixels_[0] == g_FIXTURE._calls);
  assert(pixels_[1] == pixels_[0]);
  assert(pixels_[2] == pixels_[0]);
  g_FIXTURE._writer_entered = true;
  _check(pthread_cond_broadcast(&g_FIXTURE._changed));
  while(!g_FIXTURE._released)
    _check(pthread_cond_wait(&g_FIXTURE._changed, &g_FIXTURE._mutex));
  result = ((g_FIXTURE._calls == 0) && g_FIXTURE._fail_first) ? -1 : 0;
  g_FIXTURE._calls++;
  _check(pthread_mutex_unlock(&g_FIXTURE._mutex));
  return result;
}


static
void *
_submit_blocked_frame(void *context_)
{
  Fixture *fixture = context_;
  int result;

  _check(pthread_mutex_lock(&fixture->_mutex));
  fixture->_producer_entered = true;
  _check(pthread_cond_broadcast(&fixture->_changed));
  _check(pthread_mutex_unlock(&fixture->_mutex));
  result = _submit_frame(fixture->_queue, (HARNESS_CAPTURE_QUEUE_FRAMES + 1));
  _check(pthread_mutex_lock(&fixture->_mutex));
  fixture->_submit_result = result;
  fixture->_producer_returned = true;
  _check(pthread_cond_broadcast(&fixture->_changed));
  _check(pthread_mutex_unlock(&fixture->_mutex));
  return NULL;
}


static
void
_release_full_queue(void)
{
  struct timespec deadline;
  int result;

  _check(pthread_mutex_lock(&g_FIXTURE._mutex));
  while(!g_FIXTURE._producer_entered)
    _check(pthread_cond_wait(&g_FIXTURE._changed, &g_FIXTURE._mutex));
  _check(clock_gettime(CLOCK_REALTIME, &deadline));
  deadline.tv_nsec = (deadline.tv_nsec + TEST_WAIT_NS);
  if(deadline.tv_nsec >= TEST_NS_PER_SECOND)
    {
      deadline.tv_sec++;
      deadline.tv_nsec = (deadline.tv_nsec - TEST_NS_PER_SECOND);
    }

  result = 0;
  while(!g_FIXTURE._producer_returned && (result == 0))
    result = pthread_cond_timedwait(&g_FIXTURE._changed, &g_FIXTURE._mutex, &deadline);
  assert(result == ETIMEDOUT);
  assert(!g_FIXTURE._producer_returned);
  g_FIXTURE._released = true;
  _check(pthread_cond_broadcast(&g_FIXTURE._changed));
  _check(pthread_mutex_unlock(&g_FIXTURE._mutex));
}


static
void
_test_queue(bool fail_first_)
{
  pthread_t producer;
  size_t written;
  size_t i;
  int result;

  memset(&g_FIXTURE, 0, sizeof(g_FIXTURE));
  g_FIXTURE._fail_first = fail_first_;
  _check(pthread_mutex_init(&g_FIXTURE._mutex, NULL));
  _check(pthread_cond_init(&g_FIXTURE._changed, NULL));
  g_FIXTURE._queue = harness_capture_queue_create(_write_frame);
  assert(g_FIXTURE._queue != NULL);
  _check(_submit_frame(g_FIXTURE._queue, 0));
  _check(pthread_mutex_lock(&g_FIXTURE._mutex));
  while(!g_FIXTURE._writer_entered)
    _check(pthread_cond_wait(&g_FIXTURE._changed, &g_FIXTURE._mutex));
  _check(pthread_mutex_unlock(&g_FIXTURE._mutex));
  for(i = 1; i <= HARNESS_CAPTURE_QUEUE_FRAMES; i++)
    _check(_submit_frame(g_FIXTURE._queue, i));
  _check(pthread_create(&producer, NULL, _submit_blocked_frame, &g_FIXTURE));
  _release_full_queue();
  _check(pthread_join(producer, NULL));
  result = harness_capture_queue_finish(g_FIXTURE._queue, &written);
  assert(result == (fail_first_ ? -1 : 0));
  assert(g_FIXTURE._submit_result == result);
  assert(written == (size_t)(HARNESS_CAPTURE_QUEUE_FRAMES + (fail_first_ ? 0 : 2)));
  assert(g_FIXTURE._calls == (size_t)(HARNESS_CAPTURE_QUEUE_FRAMES + (fail_first_ ? 1 : 2)));
  _check(pthread_cond_destroy(&g_FIXTURE._changed));
  _check(pthread_mutex_destroy(&g_FIXTURE._mutex));
}


int
main(void)
{
  HarnessCaptureQueue *queue;
  size_t written;

  _test_queue(false);
  _test_queue(true);
  queue = harness_capture_queue_create(_write_frame);
  assert(queue != NULL);
  _check(harness_capture_queue_finish(queue, &written));
  assert(written == 0);
  _check(harness_capture_queue_finish(NULL, &written));
  assert(written == 0);
  puts("capture queue tests: ok");
  return 0;
}
