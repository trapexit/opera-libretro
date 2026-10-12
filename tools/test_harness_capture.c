// Owns a bounded FIFO of RGB captures and one file-writing worker.
#define _POSIX_C_SOURCE 200809L

#include "test_harness_capture.h"

#include <pthread.h>

#include <assert.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

typedef struct CaptureFrame
{
  char *_path;
  unsigned char *_pixels;
  unsigned _width;
  unsigned _height;
} CaptureFrame;

// The mutex guards all shared fields. A dequeued frame belongs exclusively
// to the worker; the producer never shares a core-owned framebuffer with it.
struct HarnessCaptureQueue
{
  pthread_mutex_t _mutex;
  pthread_cond_t _changed;
  pthread_t _worker;
  HarnessCaptureWriter _writer;
  CaptureFrame _frames[HARNESS_CAPTURE_QUEUE_FRAMES];
  size_t _next;
  size_t _used;
  size_t _written;
  bool _closing;
  bool _failed;
};

static
void
_check_thread_result(int result_)
{
  // Invalid synchronization state is a programming error, not a write error.
  assert(result_ == 0);
  if(result_ != 0)
    abort();
}


static
bool
_take_capture(HarnessCaptureQueue *queue_,
              CaptureFrame        *frame_)
{
  _check_thread_result(pthread_mutex_lock(&queue_->_mutex));
  while((queue_->_used == 0) && !queue_->_closing)
    _check_thread_result(pthread_cond_wait(&queue_->_changed, &queue_->_mutex));
  if(queue_->_used == 0)
    {
      _check_thread_result(pthread_mutex_unlock(&queue_->_mutex));
      return false;
    }

  assert(queue_->_used <= HARNESS_CAPTURE_QUEUE_FRAMES);
  *frame_ = queue_->_frames[queue_->_next];
  queue_->_next = ((queue_->_next + 1) % HARNESS_CAPTURE_QUEUE_FRAMES);
  queue_->_used--;
  _check_thread_result(pthread_cond_broadcast(&queue_->_changed));
  _check_thread_result(pthread_mutex_unlock(&queue_->_mutex));
  return true;
}


static
void *
_write_captures(void *context_)
{
  HarnessCaptureQueue *queue = context_;
  CaptureFrame frame;

  // Finish drains the FIFO before this event pump exits.
  while(_take_capture(queue, &frame))
    {
      int result;

      result = queue->_writer(frame._path, frame._pixels, frame._width, frame._height);
      free(frame._path);
      free(frame._pixels);
      _check_thread_result(pthread_mutex_lock(&queue->_mutex));
      if(result == 0)
        queue->_written++;
      else
        queue->_failed = true;
      _check_thread_result(pthread_cond_broadcast(&queue->_changed));
      _check_thread_result(pthread_mutex_unlock(&queue->_mutex));
    }

  return NULL;
}


HarnessCaptureQueue *
harness_capture_queue_create(HarnessCaptureWriter writer_)
{
  HarnessCaptureQueue *queue;

  assert(writer_ != NULL);
  queue = calloc(1, sizeof(*queue));
  if(queue == NULL)
    return NULL;
  queue->_writer = writer_;
  if(pthread_mutex_init(&queue->_mutex, NULL) != 0)
    {
      free(queue);
      return NULL;
    }

  if(pthread_cond_init(&queue->_changed, NULL) != 0)
    {
      _check_thread_result(pthread_mutex_destroy(&queue->_mutex));
      free(queue);
      return NULL;
    }

  if(pthread_create(&queue->_worker, NULL, _write_captures, queue) != 0)
    {
      _check_thread_result(pthread_cond_destroy(&queue->_changed));
      _check_thread_result(pthread_mutex_destroy(&queue->_mutex));
      free(queue);
      return NULL;
    }

  return queue;
}


int
harness_capture_queue_submit(HarnessCaptureQueue *queue_,
                             const char          *path_,
                             unsigned char       *pixels_,
                             unsigned             width_,
                             unsigned             height_)
{
  CaptureFrame frame;
  size_t slot;

  assert(queue_ != NULL);
  assert(path_ != NULL);
  assert(pixels_ != NULL);
  assert((width_ > 0) && (height_ > 0));
  frame._path = strdup(path_);
  if(frame._path == NULL)
    return -1;
  frame._pixels = pixels_;
  frame._width = width_;
  frame._height = height_;
  _check_thread_result(pthread_mutex_lock(&queue_->_mutex));
  while((queue_->_used == HARNESS_CAPTURE_QUEUE_FRAMES) && !queue_->_failed)
    _check_thread_result(pthread_cond_wait(&queue_->_changed, &queue_->_mutex));
  if(queue_->_failed || queue_->_closing)
    {
      _check_thread_result(pthread_mutex_unlock(&queue_->_mutex));
      free(frame._path);
      return -1;
    }

  assert(queue_->_used < HARNESS_CAPTURE_QUEUE_FRAMES);
  slot = ((queue_->_next + queue_->_used) % HARNESS_CAPTURE_QUEUE_FRAMES);
  queue_->_frames[slot] = frame;
  queue_->_used++;
  _check_thread_result(pthread_cond_broadcast(&queue_->_changed));
  _check_thread_result(pthread_mutex_unlock(&queue_->_mutex));
  return 0;
}


int
harness_capture_queue_finish(HarnessCaptureQueue *queue_,
                             size_t              *written_)
{
  int result;

  assert(written_ != NULL);
  *written_ = 0;
  if(queue_ == NULL)
    return 0;
  _check_thread_result(pthread_mutex_lock(&queue_->_mutex));
  queue_->_closing = true;
  _check_thread_result(pthread_cond_broadcast(&queue_->_changed));
  _check_thread_result(pthread_mutex_unlock(&queue_->_mutex));
  _check_thread_result(pthread_join(queue_->_worker, NULL));
  *written_ = queue_->_written;
  result = queue_->_failed ? -1 : 0;
  _check_thread_result(pthread_cond_destroy(&queue_->_changed));
  _check_thread_result(pthread_mutex_destroy(&queue_->_mutex));
  free(queue_);
  return result;
}
