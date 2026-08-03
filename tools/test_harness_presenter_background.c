// This module owns interruptible NUT demuxing and bounded background A/V queues.

#include "test_harness_presenter_background.h"

#include <SDL.h>

#include <libavcodec/codec_id.h>
#include <libavcodec/version.h>
#include <libavformat/avformat.h>
#include <libavutil/error.h>
#include <libavutil/imgutils.h>
#include <libavutil/pixfmt.h>

#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum
{
  BACKGROUND_AVIO_BUFFER_BYTES = 32768,
  BACKGROUND_ERROR_BYTES       = 256,
  BACKGROUND_MAX_DIMENSION     = 8192,
  BACKGROUND_MAX_PACKET_BYTES  = 64 * 1024 * 1024,
  BACKGROUND_MAX_QUEUE_BYTES   = 32 * 1024 * 1024,
  BACKGROUND_READ_POLL_MS      = 100
};

typedef struct BackgroundPacketNode BackgroundPacketNode;
struct BackgroundPacketNode
{
  PresenterBackgroundPacket packet;
  BackgroundPacketNode      *next;
};

typedef struct BackgroundQueue
{
  BackgroundPacketNode *head;
  BackgroundPacketNode *tail;
} BackgroundQueue;

struct PresenterBackground
{
  SDL_Thread              *thread;
  SDL_mutex               *mutex;
  SDL_cond                *queue_changed;
  SDL_atomic_t             stop_requested;
  BackgroundQueue          video_queue;
  BackgroundQueue          audio_queue;
  PresenterBackgroundInfo  info;
  size_t                   queue_bytes;
  size_t                   video_packet_bytes;
  int                      fd;
  int                      video_stream;
  int                      audio_stream;
  char                     error[BACKGROUND_ERROR_BYTES];
  bool                     ready;
  bool                     ended;
};

// Reads from the producer without preventing a requested worker shutdown.
static
int
_read_socket(void    *opaque_,
             uint8_t *buffer_,
             int      size_);

// Records a terminal worker state for the main event loop.
static
void
_finish_worker(PresenterBackground *background_,
               int                  error_code_,
               const char          *context_);

// Validates the one-video/one-audio raw stream contract and publishes metadata.
static
int
_read_stream_info(PresenterBackground *background_,
                  AVFormatContext     *format_);

// Converts a packet timestamp to microseconds without inventing missing PTS.
static
int64_t
_packet_pts_us(const AVPacket *packet_,
               const AVStream *stream_);

// Copies one validated packet into the bounded queue selected by its stream.
static
int
_queue_packet(PresenterBackground *background_,
              const AVPacket      *packet_,
              AVFormatContext     *format_);

// Opens and drains one accepted NUT stream until EOF, failure, or interruption.
static
int
_worker_main(void *opaque_);

// Frees every node in a queue while its owner is locked or stopped.
static
void
_clear_queue(BackgroundQueue *queue_);

// Resets all connection-specific state while retaining synchronization objects.
static
void
_reset_connection(PresenterBackground *background_);

// Returns the queue for one packet kind.
static
BackgroundQueue *
_select_queue(PresenterBackground           *background_,
              PresenterBackgroundPacketType  type_);


PresenterBackground *
presenter_background_create(void)
{
  PresenterBackground *background;

  background = calloc(1, sizeof(*background));
  if(background == NULL)
    return NULL;
  background->fd = -1;
  background->mutex = SDL_CreateMutex();
  background->queue_changed = SDL_CreateCond();
  if((background->mutex == NULL) || (background->queue_changed == NULL))
    {
      presenter_background_destroy(background);
      return NULL;
    }
  return background;
}


void
presenter_background_destroy(PresenterBackground *background_)
{
  if(background_ == NULL)
    return;
  presenter_background_stop(background_);
  if(background_->queue_changed != NULL)
    SDL_DestroyCond(background_->queue_changed);
  if(background_->mutex != NULL)
    SDL_DestroyMutex(background_->mutex);
  free(background_);
}


int
presenter_background_start(PresenterBackground *background_,
                           int                  fd_)
{
  if((background_ == NULL) || (fd_ < 0) || (background_->thread != NULL))
    return -1;

  _reset_connection(background_);
  background_->fd = fd_;
  SDL_AtomicSet(&background_->stop_requested, 0);
  background_->thread = SDL_CreateThread(_worker_main,
                                          "presenter-background", background_);
  if(background_->thread == NULL)
    {
      background_->fd = -1;
      return -1;
    }
  return 0;
}


void
presenter_background_stop(PresenterBackground *background_)
{
  if(background_ == NULL)
    return;
  if(background_->thread != NULL)
    {
      SDL_AtomicSet(&background_->stop_requested, 1);
      if(background_->fd >= 0)
        shutdown(background_->fd, SHUT_RDWR);
      SDL_LockMutex(background_->mutex);
      SDL_CondBroadcast(background_->queue_changed);
      SDL_UnlockMutex(background_->mutex);
      SDL_WaitThread(background_->thread, NULL);
      background_->thread = NULL;
    }
  if(background_->fd >= 0)
    close(background_->fd);
  background_->fd = -1;
  _reset_connection(background_);
}


bool
presenter_background_get_info(PresenterBackground     *background_,
                              PresenterBackgroundInfo *info_)
{
  bool ready;

  if((background_ == NULL) || (info_ == NULL))
    return false;
  SDL_LockMutex(background_->mutex);
  ready = background_->ready;
  if(ready)
    *info_ = background_->info;
  SDL_UnlockMutex(background_->mutex);
  return ready;
}


bool
presenter_background_has_ended(PresenterBackground *background_,
                               char                *error_,
                               size_t               error_size_)
{
  bool ended;

  if(background_ == NULL)
    return true;
  SDL_LockMutex(background_->mutex);
  ended = background_->ended;
  if((error_ != NULL) && (error_size_ > 0U))
    snprintf(error_, error_size_, "%s", background_->error);
  SDL_UnlockMutex(background_->mutex);
  return ended;
}


bool
presenter_background_first_pts(PresenterBackground *background_,
                               int64_t             *pts_us_)
{
  BackgroundPacketNode *audio;
  BackgroundPacketNode *video;
  int64_t pts;
  bool have_pts;

  if((background_ == NULL) || (pts_us_ == NULL))
    return false;
  SDL_LockMutex(background_->mutex);
  audio = background_->audio_queue.head;
  video = background_->video_queue.head;
  have_pts = false;
  pts = PRESENTER_BACKGROUND_NO_PTS;
  if(audio != NULL)
    {
      pts = audio->packet.pts_us;
      have_pts = true;
    }
  if((video != NULL) &&
     (!have_pts || (pts == PRESENTER_BACKGROUND_NO_PTS) ||
      ((video->packet.pts_us != PRESENTER_BACKGROUND_NO_PTS) &&
       (video->packet.pts_us < pts))))
    {
      pts = video->packet.pts_us;
      have_pts = true;
    }
  SDL_UnlockMutex(background_->mutex);
  if(have_pts)
    *pts_us_ = pts;
  return have_pts;
}


bool
presenter_background_pop_due(PresenterBackground           *background_,
                             PresenterBackgroundPacket     *packet_,
                             PresenterBackgroundPacketType  type_,
                             int64_t                        maximum_pts_us_)
{
  BackgroundPacketNode *node;
  BackgroundQueue *queue;

  if((background_ == NULL) || (packet_ == NULL) ||
     ((type_ != PresenterBackgroundPacketType_VIDEO) &&
      (type_ != PresenterBackgroundPacketType_AUDIO)))
    return false;
  SDL_LockMutex(background_->mutex);
  queue = _select_queue(background_, type_);
  node = queue->head;
  if((node == NULL) ||
     ((node->packet.pts_us != PRESENTER_BACKGROUND_NO_PTS) &&
      (node->packet.pts_us > maximum_pts_us_)))
    {
      SDL_UnlockMutex(background_->mutex);
      return false;
    }

  queue->head = node->next;
  if(queue->head == NULL)
    queue->tail = NULL;
  background_->queue_bytes -= node->packet.size;
  SDL_CondSignal(background_->queue_changed);
  SDL_UnlockMutex(background_->mutex);
  *packet_ = node->packet;
  free(node);
  return true;
}


void
presenter_background_release_packet(PresenterBackgroundPacket *packet_)
{
  if(packet_ == NULL)
    return;
  free(packet_->data);
  memset(packet_, 0, sizeof(*packet_));
}


static
int
_read_socket(void    *opaque_,
             uint8_t *buffer_,
             int      size_)
{
  PresenterBackground *background;
  struct pollfd fd;

  background = opaque_;
  while(!SDL_AtomicGet(&background->stop_requested))
    {
      int result;
      ssize_t count;

      fd.fd = background->fd;
      fd.events = POLLIN | POLLHUP | POLLERR;
      fd.revents = 0;
      result = poll(&fd, 1, BACKGROUND_READ_POLL_MS);
      if(result == 0)
        continue;
      if(result < 0)
        {
          if(errno == EINTR)
            continue;
          return AVERROR(errno);
        }
      count = recv(background->fd, buffer_, (size_t)size_, 0);
      if(count > 0)
        return (int)count;
      if(count == 0)
        return AVERROR_EOF;
      if((errno == EINTR) || (errno == EAGAIN) || (errno == EWOULDBLOCK))
        continue;
      return AVERROR(errno);
    }
  return AVERROR_EXIT;
}


static
void
_finish_worker(PresenterBackground *background_,
               int                  error_code_,
               const char          *context_)
{
  SDL_LockMutex(background_->mutex);
  background_->ended = true;
  background_->error[0] = 0;
  if((error_code_ < 0) && (error_code_ != AVERROR_EOF) &&
     (error_code_ != AVERROR_EXIT))
    {
      char detail[AV_ERROR_MAX_STRING_SIZE];

      av_strerror(error_code_, detail, sizeof(detail));
      snprintf(background_->error, sizeof(background_->error),
               "%s: %s", context_, detail);
    }
  SDL_CondBroadcast(background_->queue_changed);
  SDL_UnlockMutex(background_->mutex);
}


static
int
_read_stream_info(PresenterBackground *background_,
                  AVFormatContext     *format_)
{
  AVCodecParameters *audio;
  AVCodecParameters *video;
  int audio_count;
  int video_packet_bytes;
  int video_count;
  unsigned i;

  audio = NULL;
  video = NULL;
  audio_count = 0;
  video_count = 0;
  background_->audio_stream = -1;
  background_->video_stream = -1;
  for(i = 0; i < format_->nb_streams; i++)
    {
      AVCodecParameters *parameters;

      parameters = format_->streams[i]->codecpar;
      if(parameters->codec_type == AVMEDIA_TYPE_VIDEO)
        {
          video = parameters;
          background_->video_stream = (int)i;
          video_count++;
        }
      else if(parameters->codec_type == AVMEDIA_TYPE_AUDIO)
        {
          audio = parameters;
          background_->audio_stream = (int)i;
          audio_count++;
        }
    }
  if((video_count != 1) || (audio_count != 1) ||
     (video == NULL) || (audio == NULL))
    return AVERROR_INVALIDDATA;
  if((video->codec_id != AV_CODEC_ID_RAWVIDEO) ||
     (video->format != AV_PIX_FMT_YUV420P) ||
     (video->width <= 0) || (video->height <= 0) ||
     (video->width > BACKGROUND_MAX_DIMENSION) ||
     (video->height > BACKGROUND_MAX_DIMENSION) ||
     ((video->width & 1) != 0) || ((video->height & 1) != 0))
    return AVERROR_INVALIDDATA;
  if((audio->codec_id != AV_CODEC_ID_PCM_S16LE) ||
     (audio->sample_rate < 8000) || (audio->sample_rate > 384000))
    return AVERROR_INVALIDDATA;
#if LIBAVCODEC_VERSION_MAJOR >= 59
  if(audio->ch_layout.nb_channels != 2)
    return AVERROR_INVALIDDATA;
#else
  if(audio->channels != 2)
    return AVERROR_INVALIDDATA;
#endif
  video_packet_bytes = av_image_get_buffer_size(
    AV_PIX_FMT_YUV420P, video->width, video->height, 1);
  if((video_packet_bytes <= 0) ||
     ((size_t)video_packet_bytes > BACKGROUND_MAX_PACKET_BYTES))
    return AVERROR_INVALIDDATA;

  SDL_LockMutex(background_->mutex);
  background_->info.width = (unsigned)video->width;
  background_->info.height = (unsigned)video->height;
  background_->info.sample_rate = (uint32_t)audio->sample_rate;
  background_->info.display_aspect_ratio =
    (double)video->width / (double)video->height;
  if((format_->streams[background_->video_stream]->sample_aspect_ratio.num > 0) &&
     (format_->streams[background_->video_stream]->sample_aspect_ratio.den > 0))
    background_->info.display_aspect_ratio *=
      av_q2d(format_->streams[background_->video_stream]->sample_aspect_ratio);
  background_->video_packet_bytes = (size_t)video_packet_bytes;
  background_->ready = true;
  SDL_UnlockMutex(background_->mutex);
  return 0;
}


static
int64_t
_packet_pts_us(const AVPacket *packet_,
               const AVStream *stream_)
{
  int64_t timestamp;

  timestamp = packet_->pts;
  if(timestamp == AV_NOPTS_VALUE)
    timestamp = packet_->dts;
  if(timestamp == AV_NOPTS_VALUE)
    return PRESENTER_BACKGROUND_NO_PTS;
  return av_rescale_q(timestamp, stream_->time_base, AV_TIME_BASE_Q);
}


static
int
_queue_packet(PresenterBackground *background_,
              const AVPacket      *packet_,
              AVFormatContext     *format_)
{
  BackgroundPacketNode *node;
  BackgroundQueue *queue;
  PresenterBackgroundPacketType type;
  size_t size;

  if(packet_->stream_index == background_->video_stream)
    {
      type = PresenterBackgroundPacketType_VIDEO;
      if((size_t)packet_->size != background_->video_packet_bytes)
        return AVERROR_INVALIDDATA;
    }
  else if(packet_->stream_index == background_->audio_stream)
    {
      type = PresenterBackgroundPacketType_AUDIO;
      if((packet_->size <= 0) || (((unsigned)packet_->size & 3U) != 0U))
        return AVERROR_INVALIDDATA;
    }
  else
    return 0;

  size = (size_t)packet_->size;
  if(size > BACKGROUND_MAX_PACKET_BYTES)
    return AVERROR_INVALIDDATA;
  node = calloc(1, sizeof(*node));
  if(node == NULL)
    return AVERROR(ENOMEM);
  node->packet.data = malloc(size);
  if(node->packet.data == NULL)
    {
      free(node);
      return AVERROR(ENOMEM);
    }
  memcpy(node->packet.data, packet_->data, size);
  node->packet.type = type;
  node->packet.size = size;
  node->packet.pts_us = _packet_pts_us(
    packet_, format_->streams[packet_->stream_index]);

  SDL_LockMutex(background_->mutex);
  while(((background_->queue_bytes + size) > BACKGROUND_MAX_QUEUE_BYTES) &&
        (background_->queue_bytes > 0U) &&
        !SDL_AtomicGet(&background_->stop_requested))
    SDL_CondWait(background_->queue_changed, background_->mutex);
  if(SDL_AtomicGet(&background_->stop_requested))
    {
      SDL_UnlockMutex(background_->mutex);
      presenter_background_release_packet(&node->packet);
      free(node);
      return AVERROR_EXIT;
    }
  queue = _select_queue(background_, type);
  if(queue->tail != NULL)
    queue->tail->next = node;
  else
    queue->head = node;
  queue->tail = node;
  background_->queue_bytes += size;
  SDL_UnlockMutex(background_->mutex);
  return 0;
}


static
int
_worker_main(void *opaque_)
{
  PresenterBackground *background;
  const AVInputFormat *input_format;
  AVFormatContext *format;
  AVIOContext *io;
  AVPacket *packet;
  unsigned char *io_buffer;
  int result;
  const char *context;

  background = opaque_;
  format = NULL;
  io = NULL;
  packet = NULL;
  io_buffer = av_malloc(BACKGROUND_AVIO_BUFFER_BYTES);
  result = (io_buffer != NULL) ? 0 : AVERROR(ENOMEM);
  context = "cannot allocate FFmpeg input";
  if(result == 0)
    {
      io = avio_alloc_context(io_buffer, BACKGROUND_AVIO_BUFFER_BYTES,
                              0, background, _read_socket, NULL, NULL);
      result = (io != NULL) ? 0 : AVERROR(ENOMEM);
      context = "cannot create FFmpeg input";
    }
  if(result == 0)
    {
      format = avformat_alloc_context();
      result = (format != NULL) ? 0 : AVERROR(ENOMEM);
      context = "cannot allocate NUT demuxer";
    }
  if(result == 0)
    {
      format->pb = io;
      format->flags |= AVFMT_FLAG_CUSTOM_IO;
      input_format = av_find_input_format("nut");
      result = (input_format != NULL) ? 0 : AVERROR_DEMUXER_NOT_FOUND;
      context = "NUT demuxer is unavailable";
    }
  if(result == 0)
    {
      result = avformat_open_input(&format, NULL, input_format, NULL);
      context = "cannot open NUT feed";
    }
  if(result == 0)
    {
      result = avformat_find_stream_info(format, NULL);
      context = "cannot read NUT stream information";
    }
  if(result == 0)
    {
      result = _read_stream_info(background, format);
      context = "feed must contain YUV420P raw video and stereo s16le audio";
    }
  if(result == 0)
    {
      packet = av_packet_alloc();
      result = (packet != NULL) ? 0 : AVERROR(ENOMEM);
      context = "cannot allocate NUT packet";
    }
  while((result == 0) && !SDL_AtomicGet(&background->stop_requested))
    {
      result = av_read_frame(format, packet);
      context = "cannot read NUT packet";
      if(result < 0)
        break;
      result = _queue_packet(background, packet, format);
      context = "invalid or unqueueable NUT packet";
      av_packet_unref(packet);
    }

  if(packet != NULL)
    av_packet_free(&packet);
  if(format != NULL)
    {
      format->pb = NULL;
      avformat_close_input(&format);
    }
  if(io != NULL)
    avio_context_free(&io);
  else
    av_free(io_buffer);
  _finish_worker(background, result, context);
  return 0;
}


static
void
_clear_queue(BackgroundQueue *queue_)
{
  BackgroundPacketNode *node;

  node = queue_->head;
  while(node != NULL)
    {
      BackgroundPacketNode *next;

      next = node->next;
      presenter_background_release_packet(&node->packet);
      free(node);
      node = next;
    }
  queue_->head = NULL;
  queue_->tail = NULL;
}


static
void
_reset_connection(PresenterBackground *background_)
{
  if(background_->mutex != NULL)
    SDL_LockMutex(background_->mutex);
  _clear_queue(&background_->video_queue);
  _clear_queue(&background_->audio_queue);
  memset(&background_->info, 0, sizeof(background_->info));
  background_->queue_bytes = 0U;
  background_->video_packet_bytes = 0U;
  background_->video_stream = -1;
  background_->audio_stream = -1;
  background_->error[0] = 0;
  background_->ready = false;
  background_->ended = false;
  if(background_->mutex != NULL)
    SDL_UnlockMutex(background_->mutex);
}


static
BackgroundQueue *
_select_queue(PresenterBackground           *background_,
              PresenterBackgroundPacketType  type_)
{
  if(type_ == PresenterBackgroundPacketType_VIDEO)
    return &background_->video_queue;
  return &background_->audio_queue;
}
