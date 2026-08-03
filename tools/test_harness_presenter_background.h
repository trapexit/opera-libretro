#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PRESENTER_BACKGROUND_NO_PTS INT64_MIN

typedef struct PresenterBackground PresenterBackground;

typedef enum PresenterBackgroundPacketType
{
  PresenterBackgroundPacketType_VIDEO = 0,
  PresenterBackgroundPacketType_AUDIO = 1
} PresenterBackgroundPacketType;

typedef struct PresenterBackgroundInfo
{
  unsigned width;
  unsigned height;
  double   display_aspect_ratio;
  uint32_t sample_rate;
} PresenterBackgroundInfo;

typedef struct PresenterBackgroundPacket
{
  PresenterBackgroundPacketType type;
  uint8_t                       *data;
  size_t                         size;
  int64_t                        pts_us;
} PresenterBackgroundPacket;

// Creates an idle feed owner after SDL has initialized its threading support.
PresenterBackground *presenter_background_create(void);

// Stops any client, frees queued media, and destroys the feed owner.
void presenter_background_destroy(PresenterBackground *background_);

// Starts consuming a connected NUT socket. Ownership transfers only on success.
int presenter_background_start(PresenterBackground *background_,
                               int                  fd_);

// Interrupts and joins the worker, closes its socket, and clears queued media.
void presenter_background_stop(PresenterBackground *background_);

// Copies validated stream metadata once the NUT headers have been read.
bool presenter_background_get_info(PresenterBackground     *background_,
                                   PresenterBackgroundInfo *info_);

// Reports worker completion and copies its diagnostic when one is available.
bool presenter_background_has_ended(PresenterBackground *background_,
                                    char                *error_,
                                    size_t               error_size_);

// Returns the earliest queued timestamp, or false while no packet is available.
bool presenter_background_first_pts(PresenterBackground *background_,
                                    int64_t             *pts_us_);

// Removes one packet of the requested kind when its timestamp is due.
bool presenter_background_pop_due(PresenterBackground           *background_,
                                  PresenterBackgroundPacket     *packet_,
                                  PresenterBackgroundPacketType  type_,
                                  int64_t                        maximum_pts_us_);

// Releases packet storage returned by presenter_background_pop_due().
void presenter_background_release_packet(PresenterBackgroundPacket *packet_);
