#pragma once

#include <stddef.h>

enum
  {
    HARNESS_CAPTURE_QUEUE_FRAMES = 8
  };

typedef struct HarnessCaptureQueue HarnessCaptureQueue;

// The worker borrows each path and packed RGB buffer until the call returns.
typedef int (*HarnessCaptureWriter)(const char *path_,
                                    const unsigned char *pixels_,
                                    unsigned width_,
                                    unsigned height_);

// Returns NULL if the worker cannot be started. writer_ must be non-NULL.
HarnessCaptureQueue *harness_capture_queue_create(HarnessCaptureWriter writer_);

// Copies path_ and takes ownership of malloc-allocated pixels_ on success.
// Blocks on a full queue; returns -1 on allocation or writer failure, leaving
// pixels_ owned by the caller. The caller must submit valid RGB dimensions.
int harness_capture_queue_submit(HarnessCaptureQueue *queue_,
                                 const char          *path_,
                                 unsigned char       *pixels_,
                                 unsigned             width_,
                                 unsigned             height_);

// Drains accepted captures, joins the worker, and destroys queue_. NULL is
// allowed. Reports successful writes and returns -1 if any write failed.
int harness_capture_queue_finish(HarnessCaptureQueue *queue_,
                                 size_t              *written_);
