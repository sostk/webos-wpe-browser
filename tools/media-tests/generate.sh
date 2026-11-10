#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Generate our own deterministic audiovisual test content. No TV commands.
set -eu
cd "$(dirname "$0")"
mkdir -p generated
ffmpeg -hide_banner -loglevel error -y -f lavfi -i testsrc2=size=320x180:rate=24 \
    -f lavfi -i sine=frequency=440:sample_rate=48000 -t 8 \
    -c:v libx264 -profile:v baseline -pix_fmt yuv420p -g 24 -bf 0 -c:a aac -b:a 96k \
    -movflags +frag_keyframe+empty_moov+default_base_moof generated/avc-aac.mp4
ffmpeg -hide_banner -loglevel error -y -f lavfi -i testsrc2=size=320x180:rate=24 \
    -f lavfi -i sine=frequency=660:sample_rate=48000 -t 8 \
    -c:v libvpx-vp9 -deadline realtime -cpu-used 8 -g 24 -c:a libopus generated/vp9-opus.webm
ffmpeg -hide_banner -loglevel error -y -i generated/avc-aac.mp4 -c copy \
    -hls_time 2 -hls_list_size 0 -hls_segment_filename generated/segment%02d.ts generated/stream.m3u8
ffmpeg -hide_banner -loglevel error -y -i generated/avc-aac.mp4 -c copy -f dash generated/stream.mpd

ffmpeg -hide_banner -loglevel error -y -i generated/avc-aac.mp4 -c copy -movflags +faststart generated/indexed-avc-aac.mp4
