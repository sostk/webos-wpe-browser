# Widevine / EME playback test on the TV

EME needs a secure context, and the TV has no web server, so the page is served from the TV's own loopback
(`http://127.0.0.1` counts as secure) by `minihttpd`. The page reports each step to `GET /log?<msg>`, which
`minihttpd` appends to a file you can read over `ares-shell`.

```sh
docker/run.sh '/build/br-out/host/bin/arm-webos-linux-gnueabi-gcc -O2 -static -o /src/build/minihttpd /src/tools/eme-test/minihttpd.c'
ares-shell -d tv -r 'mkdir -p /tmp/emetest'
ares-push -d tv build/minihttpd tools/eme-test/eme.html /tmp/emetest/
ares-shell -d tv -r 'cd /tmp/emetest && (setsid ./minihttpd 8099 /tmp/emetest /tmp/emetest/report.txt </dev/null >/dev/null 2>&1 &)'
ares-launch -d tv org.sostk.wpebrowser -p '{"target":"http://127.0.0.1:8099/eme.html?quick&codec=avc1"}'
ares-shell -d tv -r 'sleep 70; cat /tmp/emetest/report.txt'
ares-shell -d tv -r 'kill $(pidof minihttpd); rm -rf /tmp/emetest'
```

Query options: `quick` skips the requestMediaKeySystemAccess probes; `codec=avc1|vp09|hev1` picks the codec;
`mpd=` and `lic=` choose the stream and Widevine licence server; `minh=` keeps only renditions at least that tall;
`acodec=` prefers that audio codec; `start=` starts at that time (s);
`seekat=A,B` seeks to B s once playback reaches A s;
`maxh=H` caps renditions at that height, and `lift=T` removes the cap at T s (an ABR step up; `liftmin=H` then also keeps only renditions at least H tall);
`noabr` turns adaptive bitrate off. Content: Shaka's Angel One (Widevine, 60 s, H.264/AAC and VP9/Opus) with
Google's no-auth test license proxy. Shaka Player is loaded from jsDelivr.
