#!/bin/sh
set -eu

presenter=${1:-./opera-test-harness-presenter}
harness=${2:-./opera-test-harness}
tmp_dir=$(mktemp -d /tmp/opera-presenter-test.XXXXXX)
presenter_pid=
feed_pid=
socket_path="$tmp_dir/opera-test-harness-presenter.sock"
background_socket_path="$tmp_dir/opera-test-harness-presenter-background.sock"

cleanup()
{
  if [ -n "$feed_pid" ]; then
    kill "$feed_pid" 2>/dev/null || true
    wait "$feed_pid" 2>/dev/null || true
  fi
  if [ -n "$presenter_pid" ]; then
    kill "$presenter_pid" 2>/dev/null || true
    wait "$presenter_pid" 2>/dev/null || true
  fi
  rm -rf "$tmp_dir"
}
trap cleanup EXIT HUP INT TERM

SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy \
  XDG_RUNTIME_DIR="$tmp_dir" "$presenter" \
  >"$tmp_dir/presenter.log" 2>&1 &
presenter_pid=$!

i=0
while [ ! -S "$socket_path" ]; do
  i=$((i + 1))
  if [ "$i" -ge 100 ]; then
    cat "$tmp_dir/presenter.log" >&2
    echo "presenter socket did not appear" >&2
    exit 1
  fi
  sleep 0.02
done

i=0
while [ ! -S "$background_socket_path" ]; do
  i=$((i + 1))
  if [ "$i" -ge 100 ]; then
    cat "$tmp_dir/presenter.log" >&2
    echo "presenter background socket did not appear" >&2
    exit 1
  fi
  sleep 0.02
done

if SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy \
  XDG_RUNTIME_DIR="$tmp_dir" "$presenter" \
  >"$tmp_dir/second-presenter.log" 2>&1; then
  echo "second presenter unexpectedly acquired the active socket" >&2
  exit 1
fi
if [ ! -S "$socket_path" ]; then
  cat "$tmp_dir/second-presenter.log" >&2
  echo "second presenter removed the active socket" >&2
  exit 1
fi

if ! command -v ffmpeg >/dev/null 2>&1; then
  echo "ffmpeg is required for presenter background tests" >&2
  exit 1
fi

ffmpeg -hide_banner -loglevel error \
  -re -f lavfi -i testsrc2=size=320x240:rate=30 \
  -re -f lavfi -i sine=frequency=440:sample_rate=48000 \
  -t 20 -map 0:v:0 -map 1:a:0 \
  -c:v rawvideo -pix_fmt yuv420p \
  -c:a pcm_s16le -ac 2 -ar 48000 -f nut \
  "unix://$background_socket_path" \
  >"$tmp_dir/feed-one.log" 2>&1 &
feed_pid=$!

i=0
while ! grep -Fq 'source=background' "$tmp_dir/presenter.log"; do
  i=$((i + 1))
  if [ "$i" -ge 200 ]; then
    cat "$tmp_dir/presenter.log" >&2
    cat "$tmp_dir/feed-one.log" >&2
    echo "presenter did not activate the background feed" >&2
    exit 1
  fi
  sleep 0.02
done

python3 tools/tests/presenter_protocol_client.py "$socket_path"

i=0
while [ "$(grep -Fc 'source=background' "$tmp_dir/presenter.log" || true)" -lt 4 ]; do
  i=$((i + 1))
  if [ "$i" -ge 100 ]; then
    break
  fi
  sleep 0.02
done
if [ "$(grep -Fc 'source=harness' "$tmp_dir/presenter.log")" -lt 3 ]; then
  cat "$tmp_dir/presenter.log" >&2
  echo "harness did not preempt the background feed" >&2
  exit 1
fi
if [ "$(grep -Fc 'source=background' "$tmp_dir/presenter.log")" -lt 4 ]; then
  cat "$tmp_dir/presenter.log" >&2
  echo "background feed did not resume after harness sessions" >&2
  exit 1
fi
first_background_pts=$(sed -n 's/.*source=background pts_us=//p' \
  "$tmp_dir/presenter.log" | head -n 1)
last_background_pts=$(sed -n 's/.*source=background pts_us=//p' \
  "$tmp_dir/presenter.log" | tail -n 1)
if [ -z "$first_background_pts" ] || [ -z "$last_background_pts" ] ||
   [ "$last_background_pts" -le "$first_background_pts" ]; then
  cat "$tmp_dir/presenter.log" >&2
  echo "background timeline did not advance during harness preemption" >&2
  exit 1
fi

kill "$feed_pid"
wait "$feed_pid" 2>/dev/null || true
feed_pid=
i=0
while ! grep -Fq 'source=waiting' "$tmp_dir/presenter.log"; do
  i=$((i + 1))
  if [ "$i" -ge 200 ]; then
    cat "$tmp_dir/presenter.log" >&2
    echo "presenter did not return to waiting after feed loss" >&2
    exit 1
  fi
  sleep 0.02
done

ffmpeg -hide_banner -loglevel error \
  -re -f lavfi -i testsrc2=size=320x240:rate=30 \
  -re -f lavfi -i sine=frequency=440:sample_rate=48000 \
  -t 1 -map 0:v:0 -map 1:a:0 \
  -c:v rawvideo -pix_fmt rgb24 \
  -c:a pcm_s16le -ac 2 -ar 48000 -f nut \
  "unix://$background_socket_path" \
  >"$tmp_dir/feed-invalid.log" 2>&1 &
feed_pid=$!
wait "$feed_pid" 2>/dev/null || true
feed_pid=
i=0
while [ "$(grep -Fc 'background feed failed' "$tmp_dir/presenter.log" || true)" -lt 1 ]; do
  i=$((i + 1))
  if [ "$i" -ge 200 ]; then
    cat "$tmp_dir/presenter.log" >&2
    cat "$tmp_dir/feed-invalid.log" >&2
    echo "presenter did not reject an unsupported background feed" >&2
    exit 1
  fi
  sleep 0.02
done

ffmpeg -hide_banner -loglevel error \
  -re -f lavfi -i testsrc2=size=160x120:rate=30 \
  -re -f lavfi -i sine=frequency=880:sample_rate=48000 \
  -t 1 -map 0:v:0 -map 1:a:0 \
  -c:v rawvideo -pix_fmt yuv420p \
  -c:a pcm_s16le -ac 2 -ar 48000 -f nut \
  "unix://$background_socket_path" \
  >"$tmp_dir/feed-two.log" 2>&1 &
feed_pid=$!
i=0
while [ "$(grep -Fc 'background feed ready' "$tmp_dir/presenter.log" || true)" -lt 2 ]; do
  i=$((i + 1))
  if [ "$i" -ge 200 ]; then
    cat "$tmp_dir/presenter.log" >&2
    cat "$tmp_dir/feed-two.log" >&2
    echo "presenter did not accept a reconnected background feed" >&2
    exit 1
  fi
  sleep 0.02
done
wait "$feed_pid" 2>/dev/null || true
feed_pid=

if XDG_RUNTIME_DIR="$tmp_dir" "$harness" \
  --core "$harness" \
  --bios "$harness" \
  --frames 1 --output-dir "$tmp_dir/auto-run" \
  --metrics "$tmp_dir/auto.json" \
  >"$tmp_dir/auto.log" 2>&1; then
  echo "auto-present probe unexpectedly passed missing runtime prerequisites" >&2
  exit 1
fi
grep -Fq '"presentation_requested": false' "$tmp_dir/auto.json"
grep -Fq '"presentation_auto_detected": true' "$tmp_dir/auto.json"
grep -Fq '"presentation_disabled": false' "$tmp_dir/auto.json"
grep -Fq '"presentation_connected": true' "$tmp_dir/auto.json"
grep -Fq '"frames_run": 0' "$tmp_dir/auto.json"

if XDG_RUNTIME_DIR="$tmp_dir" "$harness" --no-present \
  --core "$harness" \
  --bios "$harness" \
  --frames 1 --output-dir "$tmp_dir/disabled-run" \
  --metrics "$tmp_dir/disabled.json" \
  >"$tmp_dir/disabled.log" 2>&1; then
  echo "disabled-present probe unexpectedly passed missing runtime prerequisites" >&2
  exit 1
fi
grep -Fq '"presentation_requested": false' "$tmp_dir/disabled.json"
grep -Fq '"presentation_auto_detected": false' "$tmp_dir/disabled.json"
grep -Fq '"presentation_disabled": true' "$tmp_dir/disabled.json"
grep -Fq '"presentation_connected": false' "$tmp_dir/disabled.json"

mkdir "$tmp_dir/no-presenter"
if XDG_RUNTIME_DIR="$tmp_dir/no-presenter" "$harness" \
  --core "$harness" \
  --bios "$harness" \
  --frames 1 --output-dir "$tmp_dir/headless-run" \
  --metrics "$tmp_dir/headless.json" \
  >"$tmp_dir/headless.log" 2>&1; then
  echo "headless probe unexpectedly passed missing runtime prerequisites" >&2
  exit 1
fi
grep -Fq '"presentation_auto_detected": false' "$tmp_dir/headless.json"
grep -Fq '"presentation_connected": false' "$tmp_dir/headless.json"

if XDG_RUNTIME_DIR="$tmp_dir/no-presenter" "$harness" --present \
  --core "$harness" \
  --bios "$harness" \
  --frames 1 --output-dir "$tmp_dir/default-required-run" \
  --metrics "$tmp_dir/default-required.json" \
  >"$tmp_dir/default-required.log" 2>&1; then
  echo "required presenter unexpectedly accepted a missing default socket" >&2
  exit 1
fi
grep -Fq '"status": "presentation_error"' "$tmp_dir/default-required.json"
grep -Fq '"presentation_requested": true' "$tmp_dir/default-required.json"
grep -Fq '"presentation_connected": false' "$tmp_dir/default-required.json"

if XDG_RUNTIME_DIR="$tmp_dir" "$harness" --present \
  --present-socket "$tmp_dir/missing-presenter.sock" \
  --core "$harness" \
  --bios "$harness" \
  --frames 1 --output-dir "$tmp_dir/required-run" \
  --metrics "$tmp_dir/required.json" \
  >"$tmp_dir/required.log" 2>&1; then
  echo "required presenter unexpectedly accepted a missing socket" >&2
  exit 1
fi
grep -Fq '"status": "presentation_error"' "$tmp_dir/required.json"
grep -Fq '"presentation_requested": true' "$tmp_dir/required.json"
grep -Fq '"presentation_connected": false' "$tmp_dir/required.json"

kill -0 "$presenter_pid"
kill "$presenter_pid"
i=0
while kill -0 "$presenter_pid" 2>/dev/null; do
  i=$((i + 1))
  if [ "$i" -ge 100 ]; then
    echo "presenter did not shut down after SIGTERM" >&2
    exit 1
  fi
  sleep 0.02
done
wait "$presenter_pid" 2>/dev/null || true
presenter_pid=

custom_socket_path="$tmp_dir/custom-harness.sock"
custom_background_socket_path="$tmp_dir/custom-background.sock"
SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy \
  "$presenter" --socket "$custom_socket_path" \
  --background-socket "$custom_background_socket_path" \
  >"$tmp_dir/custom-presenter.log" 2>&1 &
presenter_pid=$!
i=0
while [ ! -S "$custom_socket_path" ] ||
      [ ! -S "$custom_background_socket_path" ]; do
  i=$((i + 1))
  if [ "$i" -ge 100 ]; then
    cat "$tmp_dir/custom-presenter.log" >&2
    echo "presenter did not honor explicit socket paths" >&2
    exit 1
  fi
  sleep 0.02
done
kill "$presenter_pid"
wait "$presenter_pid" 2>/dev/null || true
presenter_pid=
if [ -e "$custom_socket_path" ] || [ -e "$custom_background_socket_path" ]; then
  echo "presenter left explicit socket paths behind after shutdown" >&2
  exit 1
fi
echo "presenter protocol tests: ok"
