#define _POSIX_C_SOURCE 200809L

#include "test_harness_present.h"
#include "test_harness_presenter_background.h"

#include <SDL.h>

#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

#include <errno.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#define PRESENTER_WIDTH 960
#define PRESENTER_HEIGHT 768
#define PRESENTER_STATUS_HEIGHT 54
#define PRESENTER_TEXT_SCALE 3
#define PRESENTER_FPS_SAMPLES 60
#define PRESENTER_MESSAGE_TIMEOUT_MS 1000
#define PRESENTER_BACKGROUND_AUDIO_LEAD_US INT64_C(100000)
#define PRESENTER_CLIENT_READ_BUDGET_BYTES (16U * 1024U * 1024U)
#define PRESENTER_CLIENT_MESSAGE_BUDGET 32U

typedef enum PresenterSource
{
  PresenterSource_WAITING = 0,
  PresenterSource_BACKGROUND,
  PresenterSource_HARNESS
} PresenterSource;

typedef struct HarnessReader
{
  uint8_t  header[OPERA_PRESENT_HEADER_SIZE];
  size_t   header_received;
  uint32_t message_type;
  uint32_t payload_size;
  size_t   payload_received;
  uint64_t deadline_ms;
} HarnessReader;

typedef struct presenter_t presenter_t;
struct presenter_t
{
  SDL_Window *window;
  SDL_Renderer *renderer;
  SDL_Texture *harness_texture;
  SDL_Texture *background_texture;
  SDL_AudioDeviceID audio_device;
  SDL_AudioStream *audio_stream;
  SDL_AudioSpec obtained_audio;
  PresenterBackground *background;
  PresenterBackgroundInfo background_info;
  int listen_fd;
  int background_listen_fd;
  int client_fd;
  int pending_fd;
  char socket_path[PATH_MAX];
  char background_socket_path[PATH_MAX];
  const char *audio_device_name;
  char label[OPERA_PRESENT_MAX_LABEL + 1U];
  char status[256];
  uint8_t *payload;
  size_t payload_capacity;
  HarnessReader reader;
  unsigned harness_video_width;
  unsigned harness_video_height;
  double harness_display_aspect_ratio;
  uint64_t frame_number;
  uint32_t source_sample_rate;
  uint64_t fps_timestamps[PRESENTER_FPS_SAMPLES];
  uint64_t background_clock_ticks;
  int64_t background_clock_pts_us;
  unsigned fps_timestamp_count;
  unsigned fps_timestamp_next;
  double measured_fps;
  bool socket_bound;
  bool background_socket_bound;
  bool background_info_reported;
  bool background_client_active;
  bool background_frame_ready;
  bool background_clock_started;
  bool harness_frame_ready;
  bool running;
  bool quit;
  PresenterSource source;
};

static volatile sig_atomic_t g_signal_quit;

// Queue all converted audio currently available from the conversion stream.
static
int
drain_audio_stream(presenter_t *presenter_);

// Drops queued playback and conversion data before an immediate source cut.
static
void
_clear_audio(presenter_t *presenter_);

// Reuse the playback device while adapting conversion to a new source rate.
static
int
configure_audio(presenter_t *presenter_,
                uint32_t     sample_rate_);

// Makes client framing incremental so the event loop never waits inside recv().
static
int
_set_nonblocking(int fd_);

// Returns the current background media timestamp on its monotonic timeline.
static
int64_t
_background_current_pts(presenter_t *presenter_);

static
int
_default_background_socket_path(char   *path_,
                                size_t  path_size_);

static
uint64_t
_performance_microseconds(void);

static
void
_reset_reader(presenter_t *presenter_);

static
void
_show_waiting(presenter_t *presenter_);

static
int
_show_background(presenter_t *presenter_);

static
void
_show_available_source(presenter_t *presenter_);

static
int
_update_background_video(presenter_t                      *presenter_,
                         const PresenterBackgroundPacket *packet_);

static
void
_clear_background_media(presenter_t *presenter_);

static
void
_finish_background_client(presenter_t *presenter_);

static
int
_pump_background_packets(presenter_t *presenter_);

static
void
_update_background(presenter_t *presenter_);

static
int
_dispatch_message(presenter_t *presenter_,
                  uint32_t     type_,
                  uint32_t     size_);

static
int
_parse_message_header(presenter_t *presenter_);

static
int
_finish_message(presenter_t *presenter_);

static
int
_receive_client_messages(presenter_t *presenter_);

static
bool
_client_message_expired(const presenter_t *presenter_);

static
int
_open_listener(int        *fd_,
               bool       *bound_,
               const char *path_);

static
void
_accept_harness_client(presenter_t *presenter_);

static
void
_accept_background_client(presenter_t *presenter_);

static
void
handle_signal(int signal_)
{
  (void)signal_;
  g_signal_quit = 1;
}

static
void
print_usage(FILE *f_)
{
  fprintf(f_,
          "Usage: opera-test-harness-presenter [options]\n"
          "\n"
          "  --socket PATH             harness socket; defaults below XDG_RUNTIME_DIR\n"
          "  --background-socket PATH  FFmpeg NUT socket; defaults below XDG_RUNTIME_DIR\n"
          "  --audio-device NAME      SDL playback device; default system device\n"
          "  --list-audio-devices     list SDL playback devices and exit\n"
          "  --help                   show this help\n");
}

static
int
default_socket_path(char   *path_,
                    size_t  path_size_)
{
  const char *runtime_dir;
  int count;

  runtime_dir = getenv("XDG_RUNTIME_DIR");
  if((runtime_dir != NULL) && (runtime_dir[0] != 0))
    count = snprintf(path_, path_size_, "%s/opera-test-harness-presenter.sock",
                     runtime_dir);
  else
    count = snprintf(path_, path_size_, "/tmp/opera-test-harness-presenter-%lu.sock",
                     (unsigned long)getuid());

  return ((count > 0) && ((size_t)count < path_size_)) ? 0 : -1;
}

static
int
_default_background_socket_path(char   *path_,
                                size_t  path_size_)
{
  const char *runtime_dir;
  int count;

  runtime_dir = getenv("XDG_RUNTIME_DIR");
  if((runtime_dir != NULL) && (runtime_dir[0] != 0))
    count = snprintf(path_, path_size_,
                     "%s/opera-test-harness-presenter-background.sock",
                     runtime_dir);
  else
    count = snprintf(path_, path_size_,
                     "/tmp/opera-test-harness-presenter-background-%lu.sock",
                     (unsigned long)getuid());

  return ((count > 0) && ((size_t)count < path_size_)) ? 0 : -1;
}

static
int
_set_nonblocking(int fd_)
{
  int flags;

  flags = fcntl(fd_, F_GETFL, 0);
  if(flags < 0)
    return -1;
  return fcntl(fd_, F_SETFL, flags | O_NONBLOCK);
}

static
int
send_all(int           fd_,
         const void   *data_,
         size_t        size_)
{
  const uint8_t *data;
  size_t written;

  data = data_;
  written = 0;
  while(written < size_)
    {
      ssize_t count;

      count = send(fd_, data + written, size_ - written, MSG_NOSIGNAL);
      if(count > 0)
        {
          written += (size_t)count;
          continue;
        }
      if((count < 0) && (errno == EINTR))
        continue;
      return -1;
    }
  return 0;
}

static
int
send_message(int           fd_,
             uint32_t      type_,
             const void   *payload_,
             uint32_t      payload_size_)
{
  uint8_t header[OPERA_PRESENT_HEADER_SIZE];

  opera_present_make_header(header, type_, payload_size_);
  if(send_all(fd_, header, sizeof(header)) != 0)
    return -1;
  if((payload_size_ > 0U) &&
     (send_all(fd_, payload_, payload_size_) != 0))
    return -1;
  return 0;
}

static
int
send_ack(int      fd_,
         uint32_t status_)
{
  uint8_t payload[4];

  opera_present_put_u32(payload, status_);
  return send_message(fd_, OPERA_PRESENT_ACK, payload, sizeof(payload));
}

static const uint8_t FONT_DIGITS[10][5] =
  {
    {0x3e,0x51,0x49,0x45,0x3e}, {0x00,0x42,0x7f,0x40,0x00},
    {0x42,0x61,0x51,0x49,0x46}, {0x21,0x41,0x45,0x4b,0x31},
    {0x18,0x14,0x12,0x7f,0x10}, {0x27,0x45,0x45,0x45,0x39},
    {0x3c,0x4a,0x49,0x49,0x30}, {0x01,0x71,0x09,0x05,0x03},
    {0x36,0x49,0x49,0x49,0x36}, {0x06,0x49,0x49,0x29,0x1e}
  };

static const uint8_t FONT_LETTERS[26][5] =
  {
    {0x7e,0x11,0x11,0x11,0x7e}, {0x7f,0x49,0x49,0x49,0x36},
    {0x3e,0x41,0x41,0x41,0x22}, {0x7f,0x41,0x41,0x22,0x1c},
    {0x7f,0x49,0x49,0x49,0x41}, {0x7f,0x09,0x09,0x09,0x01},
    {0x3e,0x41,0x49,0x49,0x7a}, {0x7f,0x08,0x08,0x08,0x7f},
    {0x00,0x41,0x7f,0x41,0x00}, {0x20,0x40,0x41,0x3f,0x01},
    {0x7f,0x08,0x14,0x22,0x41}, {0x7f,0x40,0x40,0x40,0x40},
    {0x7f,0x02,0x0c,0x02,0x7f}, {0x7f,0x04,0x08,0x10,0x7f},
    {0x3e,0x41,0x41,0x41,0x3e}, {0x7f,0x09,0x09,0x09,0x06},
    {0x3e,0x41,0x51,0x21,0x5e}, {0x7f,0x09,0x19,0x29,0x46},
    {0x46,0x49,0x49,0x49,0x31}, {0x01,0x01,0x7f,0x01,0x01},
    {0x3f,0x40,0x40,0x40,0x3f}, {0x1f,0x20,0x40,0x20,0x1f},
    {0x3f,0x40,0x38,0x40,0x3f}, {0x63,0x14,0x08,0x14,0x63},
    {0x07,0x08,0x70,0x08,0x07}, {0x61,0x51,0x49,0x45,0x43}
  };

static
void
font_columns(char           ch_,
             const uint8_t **columns_)
{
  static const uint8_t blank[5] = {0,0,0,0,0};
  static const uint8_t colon[5] = {0,0x36,0x36,0,0};
  static const uint8_t dash[5] = {0x08,0x08,0x08,0x08,0x08};
  static const uint8_t dot[5] = {0,0x60,0x60,0,0};
  static const uint8_t slash[5] = {0x20,0x10,0x08,0x04,0x02};
  static const uint8_t underscore[5] = {0x40,0x40,0x40,0x40,0x40};

  if((ch_ >= 'a') && (ch_ <= 'z'))
    ch_ = (char)(ch_ - 'a' + 'A');
  if((ch_ >= 'A') && (ch_ <= 'Z'))
    *columns_ = FONT_LETTERS[ch_ - 'A'];
  else if((ch_ >= '0') && (ch_ <= '9'))
    *columns_ = FONT_DIGITS[ch_ - '0'];
  else if(ch_ == ':')
    *columns_ = colon;
  else if(ch_ == '-')
    *columns_ = dash;
  else if(ch_ == '.')
    *columns_ = dot;
  else if(ch_ == '/')
    *columns_ = slash;
  else if(ch_ == '_')
    *columns_ = underscore;
  else
    *columns_ = blank;
}

static
void
draw_text(SDL_Renderer *renderer_,
          int           x_,
          int           y_,
          const char   *text_,
          int           scale_,
          int           max_width_)
{
  int cursor;

  cursor = x_;
  while((*text_ != 0) && ((cursor + (6 * scale_)) <= (x_ + max_width_)))
    {
      const uint8_t *columns;
      int x;

      font_columns(*text_, &columns);
      for(x = 0; x < 5; x++)
        {
          int y;

          for(y = 0; y < 7; y++)
            {
              if((columns[x] & (uint8_t)(1U << y)) != 0)
                {
                  SDL_Rect pixel;

                  pixel.x = cursor + (x * scale_);
                  pixel.y = y_ + (y * scale_);
                  pixel.w = scale_;
                  pixel.h = scale_;
                  SDL_RenderFillRect(renderer_, &pixel);
                }
            }
        }
      cursor += 6 * scale_;
      text_++;
    }
}

static
void
close_audio(presenter_t *presenter_)
{
  if(presenter_->audio_stream != NULL)
    SDL_FreeAudioStream(presenter_->audio_stream);
  presenter_->audio_stream = NULL;
  if(presenter_->audio_device != 0)
    SDL_CloseAudioDevice(presenter_->audio_device);
  presenter_->audio_device = 0;
  memset(&presenter_->obtained_audio, 0, sizeof(presenter_->obtained_audio));
}

static
int
drain_audio_stream(presenter_t *presenter_)
{
  for(;;)
    {
      uint8_t converted[8192];
      int available;
      int count;

      available = SDL_AudioStreamAvailable(presenter_->audio_stream);
      if(available < 0)
        return -1;
      if(available == 0)
        return 0;
      if(available > (int)sizeof(converted))
        available = (int)sizeof(converted);
      count = SDL_AudioStreamGet(presenter_->audio_stream, converted, available);
      if(count <= 0)
        return -1;
      if(SDL_QueueAudio(presenter_->audio_device,
                        converted, (uint32_t)count) != 0)
        return -1;
    }
}

static
void
_clear_audio(presenter_t *presenter_)
{
  if(presenter_->audio_device != 0)
    SDL_ClearQueuedAudio(presenter_->audio_device);
  if(presenter_->audio_stream != NULL)
    SDL_AudioStreamClear(presenter_->audio_stream);
}

static
int
configure_audio(presenter_t *presenter_,
                uint32_t     sample_rate_)
{
  SDL_AudioSpec desired;
  int allowed_changes;

  memset(&desired, 0, sizeof(desired));
  desired.freq = (int)sample_rate_;
  desired.format = AUDIO_S16LSB;
  desired.channels = 2;
  desired.samples = 1024;
  if(presenter_->audio_device == 0)
    {
      allowed_changes = SDL_AUDIO_ALLOW_FREQUENCY_CHANGE |
        SDL_AUDIO_ALLOW_FORMAT_CHANGE |
        SDL_AUDIO_ALLOW_CHANNELS_CHANGE;
      presenter_->audio_device = SDL_OpenAudioDevice(
        presenter_->audio_device_name, 0, &desired,
        &presenter_->obtained_audio, allowed_changes);
      if(presenter_->audio_device == 0)
        return -1;
      SDL_PauseAudioDevice(presenter_->audio_device, 0);
    }

  if(presenter_->source_sample_rate == sample_rate_)
    return 0;

  if(presenter_->audio_stream != NULL)
    SDL_FreeAudioStream(presenter_->audio_stream);
  presenter_->audio_stream = NULL;
  presenter_->source_sample_rate = 0U;

  if((presenter_->obtained_audio.freq != desired.freq) ||
     (presenter_->obtained_audio.format != desired.format) ||
     (presenter_->obtained_audio.channels != desired.channels))
    {
      presenter_->audio_stream = SDL_NewAudioStream(
        desired.format, desired.channels, desired.freq,
        presenter_->obtained_audio.format,
        presenter_->obtained_audio.channels,
        presenter_->obtained_audio.freq);
      if(presenter_->audio_stream == NULL)
        return -1;
    }
  presenter_->source_sample_rate = sample_rate_;
  return 0;
}

static
int
queue_audio(presenter_t  *presenter_,
            const uint8_t *data_,
            uint32_t       size_)
{
  if(presenter_->audio_device == 0)
    return -1;
  if(presenter_->audio_stream == NULL)
    return SDL_QueueAudio(presenter_->audio_device, data_, size_);

  if(SDL_AudioStreamPut(presenter_->audio_stream, data_, (int)size_) != 0)
    return -1;
  return drain_audio_stream(presenter_);
}

static
void
render(presenter_t *presenter_)
{
  SDL_Texture *texture;
  unsigned video_width;
  unsigned video_height;
  double display_aspect_ratio;
  int output_width;
  int output_height;
  int status_height;

  SDL_GetRendererOutputSize(presenter_->renderer, &output_width, &output_height);
  SDL_SetRenderDrawColor(presenter_->renderer, 8, 10, 14, 255);
  SDL_RenderClear(presenter_->renderer);

  texture = NULL;
  video_width = 0U;
  video_height = 0U;
  display_aspect_ratio = 0.0;
  status_height = PRESENTER_STATUS_HEIGHT;
  if(presenter_->source == PresenterSource_BACKGROUND)
    {
      texture = presenter_->background_texture;
      video_width = presenter_->background_info.width;
      video_height = presenter_->background_info.height;
      display_aspect_ratio = presenter_->background_info.display_aspect_ratio;
      status_height = 0;
    }
  else if((presenter_->source == PresenterSource_HARNESS) &&
          presenter_->harness_frame_ready)
    {
      texture = presenter_->harness_texture;
      video_width = presenter_->harness_video_width;
      video_height = presenter_->harness_video_height;
      display_aspect_ratio = presenter_->harness_display_aspect_ratio;
    }

  if(status_height > 0)
    {
      SDL_Rect status_rect;

      status_rect.x = 0;
      status_rect.y = 0;
      status_rect.w = output_width;
      status_rect.h = status_height;
      if(presenter_->source == PresenterSource_HARNESS)
        SDL_SetRenderDrawColor(presenter_->renderer, 23, 82, 112, 255);
      else
        SDL_SetRenderDrawColor(presenter_->renderer, 42, 46, 54, 255);
      SDL_RenderFillRect(presenter_->renderer, &status_rect);

      SDL_SetRenderDrawColor(presenter_->renderer, 240, 244, 250, 255);
      draw_text(presenter_->renderer, 16, 10, presenter_->status,
                PRESENTER_TEXT_SCALE, output_width - 32);
    }

  if((texture != NULL) && (video_width > 0U) && (video_height > 0U))
    {
      int area_height;
      int dest_width;
      int dest_height;
      double aspect_ratio;
      SDL_Rect destination;

      area_height = output_height - status_height;
      if(area_height <= 0)
        {
          SDL_RenderPresent(presenter_->renderer);
          return;
        }
      aspect_ratio = display_aspect_ratio;
      if(aspect_ratio <= 0.0)
        aspect_ratio = (double)video_width / (double)video_height;
      if(((double)output_width / (double)area_height) > aspect_ratio)
        {
          dest_height = area_height;
          dest_width = (int)((double)dest_height * aspect_ratio + 0.5);
        }
      else
        {
          dest_width = output_width;
          dest_height = (int)((double)dest_width / aspect_ratio + 0.5);
        }
      if(dest_width < 1)
        dest_width = 1;
      if(dest_height < 1)
        dest_height = 1;
      destination.x = (output_width - dest_width) / 2;
      destination.y = status_height + ((area_height - dest_height) / 2);
      destination.w = dest_width;
      destination.h = dest_height;
      SDL_RenderCopy(presenter_->renderer, texture, NULL, &destination);
    }
  SDL_RenderPresent(presenter_->renderer);
}

static
void
set_status(presenter_t *presenter_,
           const char  *state_)
{
  snprintf(presenter_->status, sizeof(presenter_->status), "%s", state_);
  SDL_SetWindowTitle(presenter_->window, presenter_->status);
}

static
void
set_running_status(presenter_t *presenter_)
{
  if(presenter_->measured_fps > 0.0)
    snprintf(presenter_->status, sizeof(presenter_->status),
             "FRAME %llu - %.2f FPS",
             (unsigned long long)presenter_->frame_number,
             presenter_->measured_fps);
  else
    snprintf(presenter_->status, sizeof(presenter_->status),
             "FRAME %llu",
             (unsigned long long)presenter_->frame_number);
  SDL_SetWindowTitle(presenter_->window, presenter_->status);
}

static
uint64_t
_performance_microseconds(void)
{
  uint64_t counter;
  uint64_t frequency;

  counter = SDL_GetPerformanceCounter();
  frequency = SDL_GetPerformanceFrequency();
  if(frequency == 0U)
    return 0U;
  return ((counter / frequency) * UINT64_C(1000000)) +
    (((counter % frequency) * UINT64_C(1000000)) / frequency);
}

static
void
_reset_reader(presenter_t *presenter_)
{
  memset(&presenter_->reader, 0, sizeof(presenter_->reader));
}

static
void
_show_waiting(presenter_t *presenter_)
{
  bool changed;

  changed = (presenter_->source != PresenterSource_WAITING);
  _clear_audio(presenter_);
  presenter_->source = PresenterSource_WAITING;
  set_status(presenter_, "WAITING");
  render(presenter_);
  if(changed)
    fprintf(stderr, "opera-test-harness-presenter: source=waiting\n");
}

static
int
_show_background(presenter_t *presenter_)
{
  _clear_audio(presenter_);
  if(configure_audio(presenter_, presenter_->background_info.sample_rate) != 0)
    return -1;
  presenter_->source = PresenterSource_BACKGROUND;
  set_status(presenter_, "BACKGROUND");
  render(presenter_);
  if(presenter_->background_clock_started)
    fprintf(stderr,
            "opera-test-harness-presenter: source=background pts_us=%lld\n",
            (long long)_background_current_pts(presenter_));
  else
    fprintf(stderr, "opera-test-harness-presenter: source=background\n");
  return 0;
}

static
void
_show_available_source(presenter_t *presenter_)
{
  if(presenter_->background_frame_ready)
    {
      if(_show_background(presenter_) == 0)
        return;
      fprintf(stderr,
              "opera-test-harness-presenter: background audio setup failed: %s\n",
              SDL_GetError());
    }
  _show_waiting(presenter_);
}

static
int
_update_background_video(presenter_t                      *presenter_,
                         const PresenterBackgroundPacket *packet_)
{
  const uint8_t *y_plane;
  const uint8_t *u_plane;
  const uint8_t *v_plane;
  size_t y_size;
  size_t chroma_size;
  unsigned width;
  unsigned height;

  width = presenter_->background_info.width;
  height = presenter_->background_info.height;
  y_size = (size_t)width * (size_t)height;
  chroma_size = y_size / 4U;
  if(packet_->size != (y_size + (2U * chroma_size)))
    return -1;
  if(presenter_->background_texture == NULL)
    {
      presenter_->background_texture = SDL_CreateTexture(
        presenter_->renderer, SDL_PIXELFORMAT_IYUV,
        SDL_TEXTUREACCESS_STREAMING, (int)width, (int)height);
      if(presenter_->background_texture == NULL)
        return -1;
      SDL_SetTextureScaleMode(presenter_->background_texture,
                              SDL_ScaleModeLinear);
    }

  y_plane = packet_->data;
  u_plane = y_plane + y_size;
  v_plane = u_plane + chroma_size;
  if(SDL_UpdateYUVTexture(presenter_->background_texture, NULL,
                          y_plane, (int)width,
                          u_plane, (int)(width / 2U),
                          v_plane, (int)(width / 2U)) != 0)
    return -1;
  presenter_->background_frame_ready = true;
  if(presenter_->source == PresenterSource_WAITING)
    return _show_background(presenter_);
  if(presenter_->source == PresenterSource_BACKGROUND)
    render(presenter_);
  return 0;
}

static
void
_clear_background_media(presenter_t *presenter_)
{
  presenter_->background_info_reported = false;
  presenter_->background_frame_ready = false;
  presenter_->background_clock_started = false;
  memset(&presenter_->background_info, 0, sizeof(presenter_->background_info));
  if(presenter_->background_texture != NULL)
    SDL_DestroyTexture(presenter_->background_texture);
  presenter_->background_texture = NULL;
}

static
void
_finish_background_client(presenter_t *presenter_)
{
  char error[256];

  error[0] = 0;
  presenter_background_has_ended(presenter_->background,
                                 error, sizeof(error));
  if(error[0] != 0)
    fprintf(stderr, "opera-test-harness-presenter: background feed failed: %s\n",
            error);
  else
    fprintf(stderr, "opera-test-harness-presenter: background feed disconnected\n");
  presenter_background_stop(presenter_->background);
  presenter_->background_client_active = false;
  _clear_background_media(presenter_);
  if(presenter_->source == PresenterSource_BACKGROUND)
    _show_waiting(presenter_);
}

static
int64_t
_background_current_pts(presenter_t *presenter_)
{
  uint64_t elapsed;

  elapsed = _performance_microseconds() - presenter_->background_clock_ticks;
  if(elapsed > (uint64_t)INT64_MAX)
    elapsed = (uint64_t)INT64_MAX;
  if((presenter_->background_clock_pts_us > 0) &&
     ((uint64_t)presenter_->background_clock_pts_us >
      ((uint64_t)INT64_MAX - elapsed)))
    return INT64_MAX;
  return presenter_->background_clock_pts_us + (int64_t)elapsed;
}

static
int
_pump_background_packets(presenter_t *presenter_)
{
  PresenterBackgroundPacket packet;
  int64_t current_pts;

  if(!presenter_->background_clock_started)
    {
      if(!presenter_background_first_pts(presenter_->background,
                                         &presenter_->background_clock_pts_us))
        return 0;
      if(presenter_->background_clock_pts_us == PRESENTER_BACKGROUND_NO_PTS)
        presenter_->background_clock_pts_us = 0;
      presenter_->background_clock_ticks = _performance_microseconds();
      presenter_->background_clock_started = true;
    }
  current_pts = _background_current_pts(presenter_);
  while(presenter_background_pop_due(
          presenter_->background, &packet,
          PresenterBackgroundPacketType_VIDEO, current_pts))
    {
      int result;

      result = _update_background_video(presenter_, &packet);
      presenter_background_release_packet(&packet);
      if(result != 0)
        return -1;
    }
  while(presenter_background_pop_due(
          presenter_->background, &packet,
          PresenterBackgroundPacketType_AUDIO,
          current_pts + PRESENTER_BACKGROUND_AUDIO_LEAD_US))
    {
      int result;

      result = 0;
      if(presenter_->source == PresenterSource_BACKGROUND)
        result = queue_audio(presenter_, packet.data, (uint32_t)packet.size);
      presenter_background_release_packet(&packet);
      if(result != 0)
        return -1;
    }
  return 0;
}

static
void
_update_background(presenter_t *presenter_)
{
  PresenterBackgroundInfo info;

  if(presenter_background_has_ended(presenter_->background, NULL, 0U))
    {
      _finish_background_client(presenter_);
      return;
    }
  if(!presenter_background_get_info(presenter_->background, &info))
    return;
  if(!presenter_->background_info_reported)
    {
      presenter_->background_info = info;
      presenter_->background_info_reported = true;
      fprintf(stderr,
              "opera-test-harness-presenter: background feed ready: %ux%u, %u Hz\n",
              info.width, info.height, info.sample_rate);
    }
  if(_pump_background_packets(presenter_) != 0)
    {
      fprintf(stderr,
              "opera-test-harness-presenter: background playback failed: %s\n",
              SDL_GetError());
      presenter_background_stop(presenter_->background);
      presenter_->background_client_active = false;
      _clear_background_media(presenter_);
      if(presenter_->source == PresenterSource_BACKGROUND)
        _show_waiting(presenter_);
    }
}

static
void
reset_measured_fps(presenter_t *presenter_)
{
  presenter_->fps_timestamp_count = 0;
  presenter_->fps_timestamp_next = 0;
  presenter_->measured_fps = 0.0;
}

static
void
update_measured_fps(presenter_t *presenter_)
{
  uint64_t frequency;
  uint64_t elapsed;
  uint64_t now;
  unsigned newest;
  unsigned oldest;

  now = SDL_GetPerformanceCounter();
  presenter_->fps_timestamps[presenter_->fps_timestamp_next] = now;
  presenter_->fps_timestamp_next =
    (presenter_->fps_timestamp_next + 1U) % PRESENTER_FPS_SAMPLES;
  if(presenter_->fps_timestamp_count < PRESENTER_FPS_SAMPLES)
    presenter_->fps_timestamp_count++;
  if(presenter_->fps_timestamp_count < 2U)
    return;

  newest = (presenter_->fps_timestamp_next + PRESENTER_FPS_SAMPLES - 1U) %
    PRESENTER_FPS_SAMPLES;
  oldest = (presenter_->fps_timestamp_count < PRESENTER_FPS_SAMPLES) ?
    0U : presenter_->fps_timestamp_next;
  elapsed = presenter_->fps_timestamps[newest] -
    presenter_->fps_timestamps[oldest];
  frequency = SDL_GetPerformanceFrequency();
  if((elapsed > 0U) && (frequency > 0U))
    presenter_->measured_fps =
      ((double)(presenter_->fps_timestamp_count - 1U) *
       (double)frequency) / (double)elapsed;
}

static
void
disconnect_client(presenter_t *presenter_)
{
  bool was_active;

  was_active = (presenter_->source == PresenterSource_HARNESS);
  if(presenter_->client_fd >= 0)
    close(presenter_->client_fd);
  presenter_->client_fd = -1;
  _reset_reader(presenter_);
  if(presenter_->pending_fd >= 0)
    {
      presenter_->client_fd = presenter_->pending_fd;
      presenter_->pending_fd = -1;
      presenter_->label[0] = 0;
    }
  presenter_->running = false;
  presenter_->harness_frame_ready = false;
  presenter_->frame_number = 0;
  reset_measured_fps(presenter_);
  if(was_active)
    {
      fprintf(stderr, "opera-test-harness-presenter: source=harness ended\n");
      _show_available_source(presenter_);
    }
}

static
int
ensure_payload(presenter_t *presenter_,
               uint32_t     size_)
{
  uint8_t *next;

  if(size_ <= presenter_->payload_capacity)
    return 0;
  next = realloc(presenter_->payload, size_);
  if(next == NULL)
    return -1;
  presenter_->payload = next;
  presenter_->payload_capacity = size_;
  return 0;
}

static
int
handle_hello(presenter_t *presenter_,
             const uint8_t *payload_,
             uint32_t       size_)
{
  if(size_ > OPERA_PRESENT_MAX_LABEL)
    {
      send_ack(presenter_->client_fd, OPERA_PRESENT_ACK_PROTOCOL);
      return -1;
    }
  memcpy(presenter_->label, payload_, size_);
  presenter_->label[size_] = 0;
  return send_ack(presenter_->client_fd, OPERA_PRESENT_ACK_OK);
}

static
int
handle_start(presenter_t *presenter_,
             const uint8_t *payload_,
             uint32_t       size_)
{
  uint32_t aspect_millionths;
  uint32_t sample_rate;

  if(size_ != OPERA_PRESENT_START_SIZE)
    {
      send_ack(presenter_->client_fd, OPERA_PRESENT_ACK_PROTOCOL);
      return -1;
    }
  sample_rate = opera_present_get_u32(payload_);
  aspect_millionths = opera_present_get_u32(payload_ + 8U);
  if((sample_rate < 8000U) || (sample_rate > 384000U))
    {
      send_ack(presenter_->client_fd, OPERA_PRESENT_ACK_AUDIO);
      return -1;
    }
  _clear_audio(presenter_);
  if(configure_audio(presenter_, sample_rate) != 0)
    {
      fprintf(stderr, "opera-test-harness-presenter: audio setup failed: %s\n",
              SDL_GetError());
      send_ack(presenter_->client_fd, OPERA_PRESENT_ACK_AUDIO);
      return -1;
    }
  presenter_->harness_display_aspect_ratio =
    (double)aspect_millionths / (double)OPERA_PRESENT_ASPECT_SCALE;
  presenter_->running = true;
  presenter_->harness_frame_ready = false;
  presenter_->source = PresenterSource_HARNESS;
  presenter_->frame_number = 0;
  reset_measured_fps(presenter_);
  set_running_status(presenter_);
  render(presenter_);
  fprintf(stderr, "opera-test-harness-presenter: source=harness\n");
  return send_ack(presenter_->client_fd, OPERA_PRESENT_ACK_OK);
}

static
int
handle_video(presenter_t *presenter_,
             const uint8_t *payload_,
             uint32_t       size_)
{
  uint64_t frame;
  uint32_t width;
  uint32_t height;
  uint64_t pixel_size;

  if(!presenter_->running ||
     (presenter_->source != PresenterSource_HARNESS) || (size_ < 16U))
    return -1;
  frame = opera_present_get_u64(payload_);
  width = opera_present_get_u32(payload_ + 8U);
  height = opera_present_get_u32(payload_ + 12U);
  pixel_size = (uint64_t)width * (uint64_t)height * UINT64_C(3);
  if((width == 0U) || (height == 0U) ||
     (pixel_size != (uint64_t)(size_ - 16U)))
    return -1;

  if((presenter_->harness_texture == NULL) ||
     (presenter_->harness_video_width != width) ||
     (presenter_->harness_video_height != height))
    {
      if(presenter_->harness_texture != NULL)
        SDL_DestroyTexture(presenter_->harness_texture);
      presenter_->harness_texture = SDL_CreateTexture(
        presenter_->renderer, SDL_PIXELFORMAT_RGB24,
        SDL_TEXTUREACCESS_STREAMING, (int)width, (int)height);
      if(presenter_->harness_texture == NULL)
        return -1;
      SDL_SetTextureScaleMode(presenter_->harness_texture,
                              SDL_ScaleModeNearest);
      presenter_->harness_video_width = width;
      presenter_->harness_video_height = height;
    }
  if(SDL_UpdateTexture(presenter_->harness_texture, NULL, payload_ + 16U,
                       (int)(width * 3U)) != 0)
    return -1;
  presenter_->harness_frame_ready = true;
  presenter_->frame_number = frame;
  update_measured_fps(presenter_);
  set_running_status(presenter_);
  render(presenter_);
  return 0;
}

static
int
handle_audio(presenter_t *presenter_,
             const uint8_t *payload_,
             uint32_t       size_)
{
  uint32_t frames;
  uint64_t bytes;

  if(!presenter_->running ||
     (presenter_->source != PresenterSource_HARNESS) || (size_ < 4U))
    return -1;
  frames = opera_present_get_u32(payload_);
  bytes = (uint64_t)frames * UINT64_C(4);
  if(bytes != (uint64_t)(size_ - 4U))
    return -1;
  return queue_audio(presenter_, payload_ + 4U, size_ - 4U);
}

static
int
_dispatch_message(presenter_t *presenter_,
                  uint32_t     type_,
                  uint32_t     size_)
{
  switch(type_)
    {
    case OPERA_PRESENT_HELLO:
      return handle_hello(presenter_, presenter_->payload, size_);
    case OPERA_PRESENT_START:
      return handle_start(presenter_, presenter_->payload, size_);
    case OPERA_PRESENT_VIDEO:
      return handle_video(presenter_, presenter_->payload, size_);
    case OPERA_PRESENT_AUDIO:
      return handle_audio(presenter_, presenter_->payload, size_);
    case OPERA_PRESENT_END:
      disconnect_client(presenter_);
      return 1;
    default:
      return -1;
    }
}

static
int
_parse_message_header(presenter_t *presenter_)
{
  HarnessReader *reader;

  reader = &presenter_->reader;
  if((opera_present_get_u32(reader->header) != OPERA_PRESENT_MAGIC) ||
     (opera_present_get_u32(reader->header + 4U) != OPERA_PRESENT_VERSION))
    return -1;
  reader->message_type = opera_present_get_u32(reader->header + 8U);
  reader->payload_size = opera_present_get_u32(reader->header + 12U);
  if(reader->payload_size > OPERA_PRESENT_MAX_PAYLOAD)
    return -1;
  return ensure_payload(presenter_, reader->payload_size);
}

static
int
_finish_message(presenter_t *presenter_)
{
  uint32_t type;
  uint32_t size;

  type = presenter_->reader.message_type;
  size = presenter_->reader.payload_size;
  _reset_reader(presenter_);
  return _dispatch_message(presenter_, type, size);
}

static
int
_receive_client_messages(presenter_t *presenter_)
{
  HarnessReader *reader;
  size_t bytes_received;
  unsigned messages_received;

  reader = &presenter_->reader;
  bytes_received = 0U;
  messages_received = 0U;
  while((bytes_received < PRESENTER_CLIENT_READ_BUDGET_BYTES) &&
        (messages_received < PRESENTER_CLIENT_MESSAGE_BUDGET))
    {
      uint8_t *destination;
      size_t remaining;
      ssize_t count;

      if(reader->header_received < sizeof(reader->header))
        {
          destination = reader->header + reader->header_received;
          remaining = sizeof(reader->header) - reader->header_received;
        }
      else
        {
          destination = presenter_->payload + reader->payload_received;
          remaining = reader->payload_size - reader->payload_received;
        }
      count = recv(presenter_->client_fd, destination, remaining, 0);
      if(count > 0)
        {
          bytes_received += (size_t)count;
          if(reader->deadline_ms == 0U)
            reader->deadline_ms = SDL_GetTicks64() +
              PRESENTER_MESSAGE_TIMEOUT_MS;
          if(reader->header_received < sizeof(reader->header))
            {
              reader->header_received += (size_t)count;
              if(reader->header_received < sizeof(reader->header))
                continue;
              if(_parse_message_header(presenter_) != 0)
                return -1;
              if(reader->payload_size == 0U)
                {
                  int result;

                  result = _finish_message(presenter_);
                  messages_received++;
                  if(result != 0)
                    return result;
                }
            }
          else
            {
              reader->payload_received += (size_t)count;
              if(reader->payload_received == reader->payload_size)
                {
                  int result;

                  result = _finish_message(presenter_);
                  messages_received++;
                  if(result != 0)
                    return result;
                }
            }
          continue;
        }
      if(count == 0)
        return -1;
      if(errno == EINTR)
        {
          if(g_signal_quit)
            return -1;
          continue;
        }
      if((errno == EAGAIN) || (errno == EWOULDBLOCK))
        return 0;
      return -1;
    }
  return 0;
}

static
bool
_client_message_expired(const presenter_t *presenter_)
{
  return (presenter_->reader.deadline_ms != 0U) &&
    (SDL_GetTicks64() >= presenter_->reader.deadline_ms);
}

static
int
_open_listener(int        *fd_,
               bool       *bound_,
               const char *path_)
{
  struct sockaddr_un address;
  struct stat st;
  int fd;
  int saved_errno;
  bool path_bound;

  *fd_ = -1;
  *bound_ = false;
  if(strlen(path_) >= sizeof(address.sun_path))
    {
      fprintf(stderr, "opera-test-harness-presenter: socket path is too long\n");
      errno = ENAMETOOLONG;
      return -1;
    }
  fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if(fd < 0)
    return -1;
  path_bound = false;

  if(lstat(path_, &st) == 0)
    {
      int probe;
      struct sockaddr_un existing;

      if(!S_ISSOCK(st.st_mode) || (st.st_uid != getuid()))
        {
          fprintf(stderr,
                  "opera-test-harness-presenter: refusing to replace %s\n",
                  path_);
          errno = EPERM;
          goto fail;
        }
      probe = socket(AF_UNIX, SOCK_STREAM, 0);
      if(probe >= 0)
        {
          memset(&existing, 0, sizeof(existing));
          existing.sun_family = AF_UNIX;
          memcpy(existing.sun_path, path_, strlen(path_) + 1U);
          if(connect(probe, (struct sockaddr *)&existing,
                     sizeof(existing)) == 0)
            {
              close(probe);
              errno = EADDRINUSE;
              goto fail;
            }
          close(probe);
        }
      if(unlink(path_) != 0)
        goto fail;
    }

  memset(&address, 0, sizeof(address));
  address.sun_family = AF_UNIX;
  memcpy(address.sun_path, path_, strlen(path_) + 1U);
  if(bind(fd, (struct sockaddr *)&address, sizeof(address)) != 0)
    goto fail;
  path_bound = true;
  if(chmod(path_, S_IRUSR | S_IWUSR) != 0)
    goto fail;
  if(listen(fd, 4) != 0)
    goto fail;
  *fd_ = fd;
  *bound_ = true;
  return 0;

  // Keep socket and pathname cleanup together so callers never receive a partial listener.
fail:
  saved_errno = errno;
  close(fd);
  if(path_bound)
    unlink(path_);
  errno = saved_errno;
  return -1;
}

static
void
_accept_harness_client(presenter_t *presenter_)
{
  int client;

  client = accept(presenter_->listen_fd, NULL, NULL);
  if(client < 0)
    return;
  if(_set_nonblocking(client) != 0)
    {
      fprintf(stderr,
              "opera-test-harness-presenter: cannot configure client socket: %s\n",
              strerror(errno));
      close(client);
      return;
    }
  if(presenter_->client_fd < 0)
    {
      presenter_->client_fd = client;
      _reset_reader(presenter_);
      return;
    }
  if(presenter_->pending_fd < 0)
    {
      presenter_->pending_fd = client;
      return;
    }
  send_ack(client, OPERA_PRESENT_ACK_BUSY);
  close(client);
}

static
void
_accept_background_client(presenter_t *presenter_)
{
  int client;

  client = accept(presenter_->background_listen_fd, NULL, NULL);
  if(client < 0)
    return;
  if(presenter_->background_client_active ||
     (_set_nonblocking(client) != 0) ||
     (presenter_background_start(presenter_->background, client) != 0))
    {
      close(client);
      return;
    }
  presenter_->background_client_active = true;
  fprintf(stderr, "opera-test-harness-presenter: background feed connected\n");
}

static
int
initialize_sdl(presenter_t *presenter_)
{
  if(SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_EVENTS) != 0)
    return -1;
  SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0");
  presenter_->window = SDL_CreateWindow(
    "WAITING",
    SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
    PRESENTER_WIDTH, PRESENTER_HEIGHT,
    SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
  if(presenter_->window == NULL)
    return -1;
  presenter_->renderer = SDL_CreateRenderer(
    presenter_->window, -1,
    SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
  if(presenter_->renderer == NULL)
    presenter_->renderer = SDL_CreateRenderer(presenter_->window, -1,
                                               SDL_RENDERER_SOFTWARE);
  if(presenter_->renderer == NULL)
    return -1;
  return 0;
}

static
void
list_audio_devices(void)
{
  int count;
  int i;

  if(SDL_Init(SDL_INIT_AUDIO) != 0)
    {
      fprintf(stderr, "opera-test-harness-presenter: SDL audio init failed: %s\n",
              SDL_GetError());
      exit(1);
    }
  count = SDL_GetNumAudioDevices(0);
  printf("Playback devices:\n");
  for(i = 0; i < count; i++)
    printf("  %s\n", SDL_GetAudioDeviceName(i, 0));
  SDL_Quit();
}

static
void
cleanup(presenter_t *presenter_)
{
  presenter_background_destroy(presenter_->background);
  presenter_->background = NULL;
  close_audio(presenter_);
  if(presenter_->client_fd >= 0)
    close(presenter_->client_fd);
  if(presenter_->pending_fd >= 0)
    close(presenter_->pending_fd);
  if(presenter_->listen_fd >= 0)
    close(presenter_->listen_fd);
  if(presenter_->background_listen_fd >= 0)
    close(presenter_->background_listen_fd);
  if(presenter_->socket_bound && (presenter_->socket_path[0] != 0))
    unlink(presenter_->socket_path);
  if(presenter_->background_socket_bound &&
     (presenter_->background_socket_path[0] != 0))
    unlink(presenter_->background_socket_path);
  if(presenter_->harness_texture != NULL)
    SDL_DestroyTexture(presenter_->harness_texture);
  if(presenter_->background_texture != NULL)
    SDL_DestroyTexture(presenter_->background_texture);
  if(presenter_->renderer != NULL)
    SDL_DestroyRenderer(presenter_->renderer);
  if(presenter_->window != NULL)
    SDL_DestroyWindow(presenter_->window);
  free(presenter_->payload);
  SDL_Quit();
}

int
main(int    argc_,
     char **argv_)
{
  presenter_t presenter;
  const char *socket_arg;
  const char *background_socket_arg;
  bool list_devices;
  int i;

  memset(&presenter, 0, sizeof(presenter));
  presenter.listen_fd = -1;
  presenter.background_listen_fd = -1;
  presenter.client_fd = -1;
  presenter.pending_fd = -1;
  presenter.source = PresenterSource_WAITING;
  socket_arg = NULL;
  background_socket_arg = NULL;
  list_devices = false;

  for(i = 1; i < argc_; i++)
    {
      if(strcmp(argv_[i], "--help") == 0)
        {
          print_usage(stdout);
          return 0;
        }
      if(strcmp(argv_[i], "--list-audio-devices") == 0)
        list_devices = true;
      else if((strcmp(argv_[i], "--socket") == 0) && ((i + 1) < argc_))
        socket_arg = argv_[++i];
      else if((strcmp(argv_[i], "--background-socket") == 0) &&
              ((i + 1) < argc_))
        background_socket_arg = argv_[++i];
      else if((strcmp(argv_[i], "--audio-device") == 0) && ((i + 1) < argc_))
        presenter.audio_device_name = argv_[++i];
      else
        {
          fprintf(stderr, "opera-test-harness-presenter: unknown or incomplete argument: %s\n",
                  argv_[i]);
          print_usage(stderr);
          return 1;
        }
    }
  if(list_devices)
    {
      list_audio_devices();
      return 0;
    }
  if(socket_arg != NULL)
    {
      if(strlen(socket_arg) >= sizeof(presenter.socket_path))
        {
          fprintf(stderr, "opera-test-harness-presenter: socket path is too long\n");
          return 1;
        }
      strcpy(presenter.socket_path, socket_arg);
    }
  else if(default_socket_path(presenter.socket_path,
                              sizeof(presenter.socket_path)) != 0)
    {
      fprintf(stderr, "opera-test-harness-presenter: cannot construct socket path\n");
      return 1;
    }
  if(background_socket_arg != NULL)
    {
      if(strlen(background_socket_arg) >=
         sizeof(presenter.background_socket_path))
        {
          fprintf(stderr,
                  "opera-test-harness-presenter: background socket path is too long\n");
          return 1;
        }
      strcpy(presenter.background_socket_path, background_socket_arg);
    }
  else if(_default_background_socket_path(
            presenter.background_socket_path,
            sizeof(presenter.background_socket_path)) != 0)
    {
      fprintf(stderr,
              "opera-test-harness-presenter: cannot construct background socket path\n");
      return 1;
    }

  signal(SIGINT, handle_signal);
  signal(SIGTERM, handle_signal);
  if(initialize_sdl(&presenter) != 0)
    {
      fprintf(stderr, "opera-test-harness-presenter: SDL init failed: %s\n",
              SDL_GetError());
      cleanup(&presenter);
      return 1;
    }
  presenter.background = presenter_background_create();
  if(presenter.background == NULL)
    {
      fprintf(stderr,
              "opera-test-harness-presenter: cannot initialize background feed: %s\n",
              SDL_GetError());
      cleanup(&presenter);
      return 1;
    }
  if(_open_listener(&presenter.listen_fd, &presenter.socket_bound,
                    presenter.socket_path) != 0)
    {
      fprintf(stderr, "opera-test-harness-presenter: cannot listen on %s: %s\n",
              presenter.socket_path, strerror(errno));
      cleanup(&presenter);
      return 1;
    }
  if(_open_listener(&presenter.background_listen_fd,
                    &presenter.background_socket_bound,
                    presenter.background_socket_path) != 0)
    {
      fprintf(stderr,
              "opera-test-harness-presenter: cannot listen on %s: %s\n",
              presenter.background_socket_path, strerror(errno));
      cleanup(&presenter);
      return 1;
    }
  set_status(&presenter, "WAITING");
  render(&presenter);
  fprintf(stderr, "opera-test-harness-presenter: listening on %s\n",
          presenter.socket_path);
  fprintf(stderr,
          "opera-test-harness-presenter: background listening on %s\n",
          presenter.background_socket_path);

  while(!presenter.quit && !g_signal_quit)
    {
      struct pollfd fds[3];
      nfds_t count;
      int client_index;
      SDL_Event event;
      int poll_result;

      while(SDL_PollEvent(&event))
        {
          if(event.type == SDL_QUIT)
            presenter.quit = true;
          else if((event.type == SDL_WINDOWEVENT) &&
                  ((event.window.event == SDL_WINDOWEVENT_SIZE_CHANGED) ||
                   (event.window.event == SDL_WINDOWEVENT_EXPOSED)))
            render(&presenter);
        }

      _update_background(&presenter);
      if(_client_message_expired(&presenter))
        disconnect_client(&presenter);

      count = 0;
      fds[count].fd = presenter.listen_fd;
      fds[count].events = POLLIN;
      fds[count].revents = 0;
      count++;
      fds[count].fd = presenter.background_listen_fd;
      fds[count].events = POLLIN;
      fds[count].revents = 0;
      count++;
      client_index = -1;
      if(presenter.client_fd >= 0)
        {
          client_index = (int)count;
          fds[count].fd = presenter.client_fd;
          fds[count].events = POLLIN | POLLHUP | POLLERR;
          fds[count].revents = 0;
          count++;
        }
      poll_result = poll(fds, count, 16);
      if((poll_result < 0) && (errno != EINTR))
        break;
      if((client_index >= 0) && (fds[client_index].revents != 0))
        {
          int result;

          result = 0;
          if((fds[client_index].revents & POLLIN) != 0)
            result = _receive_client_messages(&presenter);
          if((result < 0) ||
             ((result == 0) &&
              ((fds[client_index].revents & (POLLHUP | POLLERR)) != 0)))
            disconnect_client(&presenter);
        }
      if((poll_result > 0) && ((fds[0].revents & POLLIN) != 0))
        _accept_harness_client(&presenter);
      if((poll_result > 0) && ((fds[1].revents & POLLIN) != 0))
        _accept_background_client(&presenter);
    }

  cleanup(&presenter);
  return 0;
}
