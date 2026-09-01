// Copyright 2021 Sony Group Corporation. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "gst_video_player.h"

#include "logging.h"

#include <fcntl.h>
#include <glob.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace {
// Buffer discipline retune (task #60, 2026-08-29). Doubled from 30 to 60 s to
// give the multiqueue more headroom against jitter on marginal networks. The
// ABR gates below (kHealthyBufferSecs, hardcoded 6 s / 15 s thresholds in
// AbrTick) are ALSO doubled so ABR behavior semantics are preserved — the
// gates fire at the same *fraction* of target, just with more absolute
// cushion. Cache-target on the downstream multiqueue is separately widened at
// DeepElementAddedHandler; kBufferTargetSecs is the display+ABR-scale target,
// not the queue-size setting.
constexpr double kBufferTargetSecs = 60.0;
constexpr gint64 kBufferTargetNs = 60000000000LL;

// --- Anti-flap ABR tuning ---
// The "healthy buffer" line, in seconds. A buffer at/above this PROVES the
// current rung is sustainable, so (a) a low bandwidth estimate (this link's
// estimate is very noisy — cv routinely >100%) must NOT drop the rung above it
// (the buffer absorbs the variance — otherwise pure quality pumping), and
// (b) up-switching is only allowed at/above it. Scaled 15 -> 30 s alongside
// the kBufferTargetSecs doubling (task #60) so this stays at 50% of target —
// the "comfortable" zone. Still well below the true live-edge plateau (the
// multiqueue can hold roughly 60 s of content on cache-target=60s).
constexpr double kHealthyBufferSecs = 30.0;
// Even below the floor, require a >=20% drop to persist across this many 1 s
// AbrTick cycles before acting, so a single dipped sample can't flap the rung.
constexpr int kSustainedDropTicks = 3;

// Number of consecutive AbrTick cycles the predicted throughput has to sit
// below the currently-published rate before we down-switch even though the
// buffer is still healthy. Distinguishes a genuine sustained decline (network
// has actually changed for the worse) from noisy dips (predicted swings
// around published, buffer absorbs it). Device log 2026-07-31 18:22-18:23:
// predicted 446/517/257/307 kbps for four ticks while published stayed at
// 1091 kbps. The buffer eventually drained anyway; a proactive down-switch
// on the first few ticks of undershoot would have preserved playback.
// Longer than kSustainedDropTicks (3) so noisy dips at a healthy buffer
// don't flap the rung, but short enough to catch a real decline early.
constexpr int kSustainedUndershootTicks = 5;

// Mirror of the undershoot detector for the trapped-low-rate case: after a
// buffer emergency drops the published rate to something like 177 kbps, the
// buffer can't reach the "healthy" line because the low rate starves the
// pipeline. So the normal up-switch gate never opens even though the network
// has fully recovered. Device log 2026-08-03 14:14 held 177 kbps for 6+
// minutes while predicted showed 6-18 Mbps. Escape trigger: predicted has
// been >=kTrappedRateMultiplier x published for kTrappedRateTicks consecutive
// ticks. When it fires, publish an up-switch regardless of buffer state.
constexpr int kTrappedRateTicks = 5;
constexpr double kTrappedRateMultiplier = 5.0;

// Minimum time between a (non-emergency) down-switch and a subsequent
// up-switch. The 11:20:29 -> 11:20:30 log showed a bandwidth-drop and an
// up-switch fire one second apart on a jittery link; the two decisions
// disagreed on direction but the estimate hadn't actually stabilised, and
// hlsdemux ended up re-selecting essentially the same rung. Holding the new
// (lower) rung for a stability window lets the estimate settle before we
// let it climb again.
constexpr int kPostDownDwellSecs = 20;

// --- ABR calm-mode tunables (task #46, 2026-08-17) ---
//
// Post-ABR-decision cooldown: after ANY ABR decision (first-estimate,
// up-switch, down-switch, emergency, undershoot, trapped-rate escape),
// no new ABR decision fires for this many seconds. Prevents the feedback
// loop where a rebuild's cold-start throughput samples fire a new decision
// on stale/jumpy data. BBC News 2026-08-17 log showed 30 restarts in 37 min
// (one every 74s) driving a 19x buffer-collapse rate vs single-variant
// baseline; median gap between BBC collapses was 84s — almost the exact
// ABR cadence. 60s is a segment × ~5, long enough for throughput sampling
// to stabilise, short enough that a genuine sustained decline still gets
// acted on within a segment.
constexpr int kPostAbrDecisionCooldownSecs = 60;

// Up-switch dwell: minimum interval between successive up-switches. Was
// 30s. Bumped to 90s so a brief upward network spike doesn't jump rung
// and immediately regret it on the next dip. 90s ≈ 9 typical HLS segments,
// long enough that the current rung has proven stable before climbing.
constexpr int kUpSwitchDwellSecs = 90;

// Fragile-pipeline window: an up-switch that fires within this many seconds
// of first-publish gets routed through ABR_RESTART instead of in-place
// SetConnectionSpeedKbps. The BBC News 2026-08-17 10:47 crash happened on
// an in-place up-switch to a pipeline barely 55s old that had just
// completed an in-place rendition renegotiation from 360p to 720p 4s
// earlier. libstdc++/libc heap primitive was called with a corrupted
// size argument (0xffffffffffffdea2) — consistent with a UAF or double-
// free in the GStreamer inputselector re-plug path. Redirecting fragile
// up-switches through the same clean-rebuild path Scope 1 uses for
// down-switches removes the race.
constexpr int kFragilePipelineSecs = 60;

// Post-BUFFER-COLLAPSE quiet window: an up-switch fired within this many
// seconds of the last BUFFER-COLLAPSE is considered "during recovery" and
// gets routed through ABR_RESTART. Same rationale as fragile-pipeline: a
// pipeline that just recovered from a collapse is in a re-plugging state
// where in-place variant switches are unsafe.
constexpr int kPostCollapseQuietSecs = 30;

// Up-switch stability requirement: coefficient of variation ceiling.
// Above this value, network is too jittery to be publishing rendition
// changes safely — an up-switch to a higher rung on top of unstable
// throughput samples typically triggers a buffer emergency within
// seconds. BBC News 2026-08-17 10:47:05 up-switch was decided at cv=87%,
// crashed immediately. cv=60% is a reasonable ceiling: above it, defer
// the up-switch. Down-switches under high cv are still allowed because
// they're moving toward safety, not away from it.
constexpr double kUpSwitchMaxCv = 0.60;

// Cold-start rung hint (kbps) fed to hlsdemux as "connection-speed". Sized
// so hlsdemux picks 360p on this project's manifest (960 kbps rendition):
// 1500 leaves headroom above 360p's 960 but stays below 480p's 1800. A
// conservative first pick lets ABR climb toward the rung the link actually
// sustains, instead of forcing top-rung and rebuffering on any link that
// can't feed a 720p first segment inside the preroll window (device log
// 2026-07-31 09:37:08-38 showed exactly this: healthy ~4 Mbps average
// throughput, buffer plateau at 5s during 720p first-segment fetch, false
// NETWORK_TOO_SLOW fire).
//
// KNOWN REGRESSION until Scope 1 lands: an up-switch after a low-rung
// cold-start can trip the bcm2835-codec pool geometry bug (S_FMT "Device
// has no supported format" verified on device 2026-07-30 when growing then
// shrinking the pool). On that path the pipeline errors out and Dart's
// existing reconnect flow rebuilds fresh at the last hint. Bounded churn is
// preferable to never-starting; the root fix is on a separate branch.
constexpr guint64 kColdStartConnSpeedKbps = 1500;

// Cold-start preroll target (seconds of buffered content required before Init()
// returns success). Dropped 15 -> 5 s (task #60, 2026-08-29) to prioritise
// picture-first UX: user sees the first frame within ~5 s of a channel tap,
// even on marginal links. The multiqueue keeps filling behind the picture up
// to the 60 s cache-target, so the ABR still gets a proper cushion — the
// difference is where the spinner ends: at 5 s of buffered content rather
// than 15 s. Deliberately no wall-clock cap on the preroll wait — a hard
// error (HTTP timeout, EOS, pathological-preroll NETWORK_TOO_SLOW) ends it
// early; otherwise the spinner waits as long as needed.
constexpr double kColdStartPrerollSecs = 5.0;

// Poll interval for the preroll wait. Fast enough to feel responsive on a good
// link; slow enough not to spam gst_query_new_buffering().
constexpr int kPrerollPollMs = 500;

// Stable machine prefix on the error string handed to Flutter for a stream that
// is unavailable due to entitlement (subscription lapsed / channel de-entitled),
// as opposed to a transient/network error. The Dart side branches on this: it
// must surface a "check your subscription" flow and NOT enter the reconnect
// loop (reconnect just thrashes re-auth against a stream it can no longer play).
constexpr char kStreamUnavailablePrefix[] = "STREAM_UNAVAILABLE: ";

// Stable machine prefix on the error string handed to Flutter when the preroll
// gate makes no meaningful progress for a long window — i.e. the network is
// sustained below the lowest-rung's segment rate and the 15 s cold-start line
// is genuinely unreachable. The Dart side branches on this to render a "check
// your connection" UI instead of leaving the spinner up indefinitely; it does
// NOT count against the reconnect budget the way a transient error does,
// because retrying against the same slow link will keep failing the same way.
constexpr char kNetworkTooSlowPrefix[] = "NETWORK_TOO_SLOW: ";

// Stable machine prefix on the error string handed to Flutter when the channel
// is not currently broadcasting — i.e. the origin serves a 200 but the body is
// empty or truncated so GStreamer cannot detect a container type. On device
// this manifests as "ERROR from typefindelementN: Stream doesn't contain
// enough data." (device log 2026-08-03 19:46:50-19:46:51). Distinct from the
// network error case: retrying with the same network won't help, because it's
// the origin/channel that has no signal to serve. Dart side renders a
// "This channel is currently not available" screen instead of the network UI.
constexpr char kChannelUnavailablePrefix[] = "CHANNEL_UNAVAILABLE: ";

// Pathological-preroll detector. Fires when total bytes fetched at the http
// source pad grew by less than kPathologicalPrerollMinDeltaBytes over
// kPathologicalPrerollNoProgressSecs of wallclock. Bytes-based instead of
// buffered-seconds-based because a downstream throttle (multiqueue limits,
// decoder backpressure) can freeze the buffered-seconds signal while the
// network is delivering fine — device log 2026-07-31 09:37 saw buffer
// plateau at 5 s while throughput averaged ~4 Mbps, an obvious false
// pathological signal by the old rule. Bytes-in only sees the network.
//
// 500 KB in 25 s = ~160 kbps sustained — well under even the 240p rung's
// 550 kbps. Any working link fetches more than that; only a genuinely dead
// or captive-portal-blocked network trips this floor.
constexpr int kPathologicalPrerollWarmupSecs = 5;
constexpr int kPathologicalPrerollNoProgressSecs = 25;
constexpr uint64_t kPathologicalPrerollMinDeltaBytes = 500 * 1024;

// True when a GStreamer error/warning is an HTTP entitlement failure (401/403/
// 410) rather than a transient/network fault. souphttpsrc reports 401/403 as
// GST_RESOURCE_ERROR_NOT_AUTHORIZED; we also scan the message/debug text so a
// 410 Gone (channel pulled) or a differently-mapped status is still caught.
bool IsHttpUnavailable(GError* error, const gchar* debug) {
  if (error && error->domain == GST_RESOURCE_ERROR &&
      error->code == GST_RESOURCE_ERROR_NOT_AUTHORIZED) {
    return true;
  }
  std::string text;
  if (error && error->message) text += error->message;
  if (debug) {
    text += ' ';
    text += debug;
  }
  for (const char* token : {"401", "403", "410", "Unauthorized", "Forbidden"}) {
    if (text.find(token) != std::string::npos) return true;
  }
  return false;
}

// True when a GStreamer error indicates the origin returned an empty or
// truncated body such that GStreamer's typefind step could not detect a
// container type. On device the channel-not-broadcasting case shows up as
// a GST_STREAM_ERROR_TYPE_NOT_FOUND from the typefind element with message
// "Stream doesn't contain enough data." — different signal from a network
// fault (network is fine, origin is fine, but the channel has no bytes).
bool IsChannelUnavailable(GstMessage* message, GError* error,
                          const gchar* debug) {
  if (error && error->domain == GST_STREAM_ERROR &&
      error->code == GST_STREAM_ERROR_TYPE_NOT_FOUND) {
    return true;
  }
  const gchar* src_name =
      GST_MESSAGE_SRC(message) ? GST_OBJECT_NAME(GST_MESSAGE_SRC(message))
                               : nullptr;
  if (src_name && std::string(src_name).find("typefind") != std::string::npos) {
    return true;
  }
  std::string text;
  if (error && error->message) text += error->message;
  if (debug) {
    text += ' ';
    text += debug;
  }
  if (text.find("doesn't contain enough data") != std::string::npos ||
      text.find("Can't typefind stream") != std::string::npos) {
    return true;
  }
  return false;
}

// Dump kernel/hardware state at freeze time. Baked in so we don't have to ask
// the user to run diagnostic shell commands after each freeze — the log
// contains the pre-crash kernel state on its own. Called from the watchdog
// thread right before the 30 s reconnect fires.
//
// Reads three sources, each cheap and non-blocking:
//  - /dev/kmsg: last ~80 lines of kernel ring buffer. Non-blocking O_RDONLY.
//    bcm2835-codec driver failures ("bcm2835-codec: OUTPUT queue starved",
//    VCHIQ mailbox timeouts, v4l2-mem2mem job-queue stalls) log here; a
//    silent decoder freeze usually leaves a fingerprint in the last few
//    entries.
//  - /sys/class/thermal/thermal_zone0/temp: current SoC temperature in
//    millidegrees C. Sustained 720p decode on a Pi 4 in an enclosed case can
//    push the SoC into thermal throttling, which can lock up the codec.
//  - vcgencmd get_throttled: reports current throttle flags (undervoltage,
//    thermal, frequency-capped). Read via popen — bounded output, small.
void DumpKernelFreezeDiagnostic() {
  std::cout << "FREEZE-DIAG: capturing kernel/hardware state" << std::endl;

  // /dev/kmsg: read last ~80 entries. The kmsg interface returns one record
  // per read() and never blocks when opened O_NONBLOCK. We collect all
  // available records, then print the tail.
  std::vector<std::string> kmsg_lines;
  int fd = open("/dev/kmsg", O_RDONLY | O_NONBLOCK);
  if (fd >= 0) {
    lseek(fd, 0, SEEK_DATA);  // start from the current tail, not the beginning
    char buf[8192];
    for (int i = 0; i < 512; ++i) {
      ssize_t n = read(fd, buf, sizeof(buf) - 1);
      if (n <= 0) break;
      buf[n] = '\0';
      kmsg_lines.emplace_back(buf, n);
    }
    close(fd);
    constexpr size_t kMaxLines = 80;
    size_t start = kmsg_lines.size() > kMaxLines
                       ? kmsg_lines.size() - kMaxLines
                       : 0;
    std::cout << "FREEZE-DIAG: /dev/kmsg last "
              << (kmsg_lines.size() - start) << " lines:" << std::endl;
    for (size_t i = start; i < kmsg_lines.size(); ++i) {
      // Each record is "<priority>,<seq>,<time>,<flags>;<text>\n<key=val>*"
      // Only print up to the first newline for brevity.
      std::string& line = kmsg_lines[i];
      auto nl = line.find('\n');
      std::cout << "FREEZE-DIAG:   " << line.substr(0, nl) << std::endl;
    }
  } else {
    std::cout << "FREEZE-DIAG: /dev/kmsg unavailable (errno=" << errno << ")"
              << std::endl;
  }

  // Thermal zone (temperature in millidegrees C).
  std::ifstream tf("/sys/class/thermal/thermal_zone0/temp");
  if (tf.is_open()) {
    int millideg = 0;
    tf >> millideg;
    std::cout << "FREEZE-DIAG: SoC temperature: " << (millideg / 1000.0)
              << " C" << std::endl;
  }

  // vcgencmd get_throttled: reports throttling flags. Bounded, short output.
  FILE* fp = popen("vcgencmd get_throttled 2>/dev/null", "r");
  if (fp) {
    char line[256];
    if (fgets(line, sizeof(line), fp)) {
      std::string s(line);
      if (!s.empty() && s.back() == '\n') s.pop_back();
      std::cout << "FREEZE-DIAG: " << s << std::endl;
    }
    pclose(fp);
  }
}
}  // namespace

GstVideoPlayer::GstVideoPlayer(
    const std::string& uri, std::unique_ptr<VideoPlayerStreamHandler> handler)
    : stream_handler_(std::move(handler)) {
  video_player_elinux::InitTimestampedLogging();
  gst_.pipeline = nullptr;
  gst_.playbin = nullptr;
  gst_.video_convert = nullptr;
  gst_.video_sink = nullptr;
  gst_.output = nullptr;
  gst_.bus = nullptr;
  gst_.buffer = nullptr;

  uri_ = ParseUri(uri);

  // Cold-start rung hint. soatv appends `#soatv:startup_kbps=N` to the URL
  // to thread either (a) a measured throughput sample from its auth GET
  // (essentially free measurement — see StreamAuthenticationService), or
  // (b) the specific rung an earlier ABR_RESTART decided on. Parse the
  // fragment, strip it from the URI before playbin sees it. See
  // startup_kbps_hint_ comment in the header for the full protocol.
  //
  // Parse manually rather than pulling in a full URI library — the
  // fragment format is stable and simple, and this runs exactly once per
  // player construction.
  {
    const std::string kMarker = "#soatv:startup_kbps=";
    const auto pos = uri_.find(kMarker);
    if (pos != std::string::npos) {
      const auto value_start = pos + kMarker.size();
      // Read digits until end-of-string or the next fragment/query separator.
      std::string digits;
      for (size_t i = value_start; i < uri_.size(); ++i) {
        const char c = uri_[i];
        if (c >= '0' && c <= '9') {
          digits += c;
        } else {
          break;
        }
      }
      if (!digits.empty()) {
        try {
          startup_kbps_hint_ = static_cast<guint64>(std::stoull(digits));
          std::cout << "URI-STARTUP-HINT: soatv:startup_kbps="
                    << startup_kbps_hint_ << " parsed from URI fragment"
                    << std::endl;
        } catch (const std::exception& e) {
          std::cerr << "URI-STARTUP-HINT: failed to parse '" << digits
                    << "': " << e.what() << std::endl;
        }
      }
      // Strip the fragment: whether we parsed it or not, playbin doesn't
      // need to see this private URI extension.
      uri_.resize(pos);
    }
  }

  // URI-based live hint. Defense-in-depth: hlsdemux is supposed to classify
  // the stream as live during Preroll (returns GST_STATE_CHANGE_NO_PREROLL),
  // which flips is_live_=true there. But that only fires when the media
  // playlist has an affirmative live marker (#EXT-X-PLAYLIST-TYPE:EVENT or
  // similar). Our origin currently serves a playlist with a sliding
  // #EXT-X-MEDIA-SEQUENCE (technically live) but NO affirmative type
  // marker — device log /tmp/soatv.log 2026-08-06 confirmed hlsdemux
  // classified it as VOD, is_live_ stayed false, and every live-guard
  // (Pause block, SetSeek live-ignore, EOS drop) fell through. The
  // pipeline paused on spurious lifecycle events, then hlsdemux
  // re-selected the earliest segment in the sliding window on resume,
  // visible as "video repeating".
  //
  // Trust the URL path when it contains /live/ (soatv's authenticated
  // stream URL for a live channel always has this segment: e.g.
  // https://ev-edgecache.soa.africa/edge/stream/live/<channel-id>...
  // pattern). Preroll's NO_PREROLL check still runs and can also flip
  // is_live_=true; whichever signal fires first wins.
  if (uri_.find("/live/") != std::string::npos) {
    is_live_ = true;
    std::cout << "URI-LIVE-HINT: /live/ path detected — is_live_=true "
                 "before preroll" << std::endl;
  }

  if (!CreatePipeline()) {
    std::cerr << "Failed to create a pipeline" << std::endl;
    DestroyPipeline();
    return;
  }
}

GstVideoPlayer::~GstVideoPlayer() {
#ifdef USE_EGL_IMAGE_DMABUF
  UnrefEGLImage();
#endif  // USE_EGL_IMAGE_DMABUF
  // Do NOT call Stop() here: the PAUSED->READY transition tries to deactivate
  // the v4l2 buffer pools while gst_.buffer still holds one of their buffers,
  // orphaning the pool and leaking its CMA memory on every teardown.
  // DestroyPipeline() releases the held buffer first, then goes to NULL.
  DestroyPipeline();
}

// static
void GstVideoPlayer::GstLibraryLoad() { gst_init(NULL, NULL); }

// static
void GstVideoPlayer::GstLibraryUnload() { gst_deinit(); }

bool GstVideoPlayer::Init() {
  if (!gst_.pipeline) {
    return false;
  }

  if (!Preroll()) {
    DestroyPipeline();
    return false;
  }

  // With sync=TRUE, fakesink only delivers frames once the pipeline clock is
  // running (PLAYING state). Live HLS prerolls with NO_PREROLL so sinks never
  // receive a buffer in PAUSED. Advance to PLAYING now so the V4L2 hardware
  // decoder starts producing frames and HandoffHandler can capture real
  // dimensions before OnNotifyInitialized() is sent to Flutter.
  //
  // Preroll audio-mute: because we transition to PLAYING before the preroll
  // gate below is satisfied, the audio branch would otherwise play the first
  // buffered seconds of audio while the spinner is still up (visible mismatch
  // between UI and stream). Drive mute on our custom audio_volume_ element
  // (playbin's own "mute" no-ops on this pipeline — the custom audio bin
  // doesn't implement GstStreamVolume; playsink logs "No volume control
  // found / Volume/mute is not available"). Video is unaffected —
  // HandoffHandler still needs frames during the wait to capture dimensions.
  if (audio_volume_) {
    g_object_set(audio_volume_, "mute", TRUE, NULL);
  }
  play_state_requested_.store(true);
  if (is_live_) {
    std::cout << "Init: live stream stays in PLAYING after preroll" << std::endl;
  }
  if (gst_element_set_state(gst_.pipeline, GST_STATE_PLAYING) ==
      GST_STATE_CHANGE_FAILURE) {
    std::cerr << "Init: Failed to reach PLAYING state" << std::endl;
    play_state_requested_.store(false);
    DestroyPipeline();
    return false;
  }

  // Preroll gate (task #8a — YouTube-pattern cold start): wait for
  // kColdStartPrerollSecs of *real* buffered content before returning to
  // Flutter. The pipeline is technically PLAYING here (fakesink+sync=TRUE
  // needs it so HandoffHandler can capture first-frame dimensions), but Init()
  // stays blocked and the Dart side keeps the spinner up. No wall-clock cap —
  // only a hard error breaks the wait early. Query logic mirrors
  // LogPlaybackHealth() exactly so both agree on "how much is buffered."
  {
    auto last_progress_log = std::chrono::steady_clock::now();
    double last_logged_secs = -1.0;
    // Pathological-preroll tracking. Watches BYTES fetched at the http source
    // pad, not buffered-seconds — downstream throttles (multiqueue limits,
    // decoder backpressure) can cap buffered-seconds long before the network
    // stops delivering (see device log 2026-07-31 09:37). Fires only when
    // fewer than kPathologicalPrerollMinDeltaBytes have arrived over
    // kPathologicalPrerollNoProgressSecs — a floor no working link crosses.
    const auto preroll_start = std::chrono::steady_clock::now();
    auto stall_check_time = preroll_start;
    uint64_t stall_check_bytes = 0;
    bool stall_check_armed = false;
    while (!error_notified_.load()) {
      gint64 position = 0;
      const bool has_position = gst_element_query_position(
          gst_.pipeline, GST_FORMAT_TIME, &position);

      double buffer_health_secs = -1.0;
      bool used_estimate = true;
      GstQuery* query = gst_query_new_buffering(GST_FORMAT_TIME);
      if (query && gst_element_query(gst_.pipeline, query)) {
        GstFormat format = GST_FORMAT_TIME;
        gint64 start = 0;
        gint64 stop = -1;
        gint64 estimated_total = -1;
        gst_query_parse_buffering_range(query, &format, &start, &stop,
                                        &estimated_total);
        if (format == GST_FORMAT_TIME && has_position && stop >= position) {
          buffer_health_secs = static_cast<double>(stop - position) /
                               static_cast<double>(GST_SECOND);
          used_estimate = false;
        }
      }
      if (query) gst_query_unref(query);

      const int pct = last_buffering_percent_.load();
      if (used_estimate && pct >= 0) {
        // Fallback: percent-derived estimate. Also treat pct>=100 as threshold
        // met — some live sources never emit a valid TIME buffering-range but
        // do drive buffering percent to 100 once the playbin queue is full.
        buffer_health_secs =
            (static_cast<double>(pct) / 100.0) * kBufferTargetSecs;
      }

      if (buffer_health_secs >= kColdStartPrerollSecs || pct >= 100) {
        std::cout << "PREROLL_WAIT: threshold met — buffered="
                  << static_cast<int>(buffer_health_secs) << "s pct=" << pct
                  << (used_estimate ? " (est)" : "") << std::endl;
        break;
      }

      const auto now = std::chrono::steady_clock::now();
      const auto since_log = std::chrono::duration_cast<std::chrono::seconds>(
          now - last_progress_log).count();
      // Log/publish every ~1s so Dart has a live pulse and the health log has
      // a trail. Also fire whenever the value moved by >=1s so a rapid climb
      // on a fast link isn't hidden behind the 1s throttle.
      const bool moved =
          buffer_health_secs >= 0.0 &&
          (last_logged_secs < 0.0 ||
           std::abs(buffer_health_secs - last_logged_secs) >= 1.0);
      if (since_log >= 1 || moved) {
        std::cout << "PREROLL_WAIT: buffered="
                  << static_cast<int>(buffer_health_secs >= 0.0
                                          ? buffer_health_secs
                                          : 0.0)
                  << "s/" << static_cast<int>(kColdStartPrerollSecs)
                  << "s pct=" << pct << (used_estimate ? " (est)" : "")
                  << std::endl;
        // Republish buffering percent so the Dart side has a pulse even on
        // sources whose BUFFERING messages are quiet between edges.
        if (pct >= 0) {
          stream_handler_->OnNotifyBufferingUpdate(pct);
        }
        last_progress_log = now;
        last_logged_secs = buffer_health_secs;
      }

      // Pathological-preroll detection. Watches BYTES fetched at the http
      // source pad. After the warmup window, arm a checkpoint. If fewer than
      // kPathologicalPrerollMinDeltaBytes have arrived since the checkpoint
      // within kPathologicalPrerollNoProgressSecs, the network is genuinely
      // dead (captive portal, DNS blackhole, sub-160 kbps sustained). Otherwise
      // advance the checkpoint. Fires NETWORK_TOO_SLOW so Dart can render the
      // check-connection UI.
      const auto secs_since_start = std::chrono::duration_cast<std::chrono::seconds>(
          now - preroll_start).count();
      const uint64_t bytes_now = total_bytes_fetched_.load(std::memory_order_relaxed);
      if (secs_since_start >= kPathologicalPrerollWarmupSecs) {
        if (!stall_check_armed) {
          // Arm the first checkpoint at the end of the warmup window.
          stall_check_time = now;
          stall_check_bytes = bytes_now;
          stall_check_armed = true;
        } else if (bytes_now - stall_check_bytes >=
                   kPathologicalPrerollMinDeltaBytes) {
          // Meaningful bytes since the checkpoint — advance. The link is
          // delivering; any buffering-percent plateau is downstream, not
          // network. Preroll will clear on its own or the frame-arrival
          // watchdog will act after PLAYING.
          stall_check_time = now;
          stall_check_bytes = bytes_now;
        } else {
          const auto stall_secs = std::chrono::duration_cast<std::chrono::seconds>(
              now - stall_check_time).count();
          if (stall_secs >= kPathologicalPrerollNoProgressSecs) {
            const uint64_t bytes_this_window = bytes_now - stall_check_bytes;
            std::string msg = std::string(kNetworkTooSlowPrefix) +
                "preroll stalled — only " +
                std::to_string(bytes_this_window / 1024) +
                "KB fetched in last " + std::to_string(stall_secs) + "s";
            std::cerr << msg << std::endl;
            bool expected = false;
            if (error_notified_.compare_exchange_strong(expected, true)) {
              last_error_ = msg;
              stream_handler_->OnNotifyError(msg);
            }
            break;
          }
        }
      }

      std::this_thread::sleep_for(std::chrono::milliseconds(kPrerollPollMs));
    }
  }

  // Preroll aborted by a fatal bus error (e.g. HTTP 4xx, EOS on VOD manifest,
  // souphttpsrc inactivity timeout). Bail before the first-frame wait so we
  // don't sit another 5s on a dead pipeline. Audio is still muted here from
  // the top of Init(); the pipeline is torn down immediately below so no
  // audio can leak.
  if (error_notified_.load()) {
    std::cerr << "Init: pipeline error during preroll — failing Init()"
              << std::endl;
    DestroyPipeline();
    return false;
  }

  // Wait up to 5 s for HandoffHandler to deliver the first decoded frame, or
  // for a fatal bus error (e.g. typefind failure) to arrive first. After the
  // preroll gate above, the buffer is deep so this should almost always be
  // near-instant; kept intact so the deferred-init path still exists for the
  // corner case where a frame slips past the query.
  {
    std::unique_lock<std::mutex> lock(mutex_first_frame_);
    first_frame_cv_.wait_for(lock, std::chrono::seconds(5), [this] {
      return first_frame_ready_.load() || error_notified_.load();
    });
  }

  // Unmute audio only AFTER the first decoded frame has surfaced. Previously
  // the unmute happened when the preroll gate opened, which was too early:
  // the very first video frame arrives a moment later, producing a visible
  // (audible) gap where sound plays over a still spinner. Moving the unmute
  // past first_frame_ready lines up audio start with picture start. On the
  // error path (fatal bus error before first frame), pipeline is torn down
  // below and audio stays muted throughout — no leak.
  if (audio_volume_ && first_frame_ready_.load()) {
    g_object_set(audio_volume_, "mute", mute_ ? TRUE : FALSE, NULL);
  }

  // A fatal error arrived before any frame did — the pipeline is dead, not
  // just slow. Returning true here previously told the caller Init succeeded
  // while the pipeline sat wedged at READY with zero frames forever; the
  // watchdog can't rescue this either, since its frame-stall check only runs
  // once state==PLAYING, which this path never reaches.
  if (error_notified_.load() && !first_frame_ready_.load()) {
    std::cerr << "Init: pipeline error before first frame — failing Init()"
              << std::endl;
    DestroyPipeline();
    return false;
  }

  // Complete init here (platform thread) if a frame arrived during the wait.
  // If we timed out, HandoffHandler will call OnNotifyInitialized() on the
  // first frame it delivers (deferred-init path).
  if (first_frame_ready_.load() && !initialized_.exchange(true)) {
    stream_handler_->OnNotifyInitialized();
    stream_handler_->OnNotifyPlaying(true);
  }

  StartWatchdog();
  // ABR engine RE-ENABLED on fix8-ABR. It was previously disabled because a
  // down-switch to the 480p rung crashed the HW ISP (S_FMT AB24 @ 854x480
  // fails when the converter's output format is renegotiated mid-stream). That
  // failure is now removed by pinning the ISP output geometry to a fixed
  // 1280x720 in CreatePipeline (see the sink-bin comment), so the ISP simply
  // scales a 480p rung up to 720p and its output S_FMT never changes. With the
  // switch made safe, the engine can once again drop rungs on genuine sustained
  // congestion instead of rebuffering on the top rung.
  StartAbrEngine();
  return true;
}

bool GstVideoPlayer::Play() {
  // Guard against post-teardown calls. Confirmed segfault path 2026-07-31
  // 12:04:53: a bus ERROR from videoconvert triggered DestroyPipeline (which
  // NULLs gst_.pipeline), while Dart's auto-resume was scheduled from the same
  // pause-event burst and reached us AFTER teardown. Passing NULL to
  // gst_element_set_state dereferences garbage → SIGSEGV at unmapped PC.
  // The Dart-side `_player != null` check doesn't help: the Dart handle
  // outlives the C++ pipeline. Returning false here turns the race into a
  // benign no-op; the reconnect flow rebuilds the pipeline from scratch.
  if (!gst_.pipeline) {
    std::cerr << "Play ignored: pipeline destroyed" << std::endl;
    return false;
  }
  play_state_requested_.store(true);
  if (gst_element_set_state(gst_.pipeline, GST_STATE_PLAYING) ==
      GST_STATE_CHANGE_FAILURE) {
    std::cerr << "Failed to change the state to PLAYING" << std::endl;
    play_state_requested_.store(false);
    return false;
  }

  if (is_live_) {
    std::cout << "Play: live stream confirmed as PLAYING" << std::endl;
  }
  stream_handler_->OnNotifyPlaying(true);
  return true;
}

bool GstVideoPlayer::Pause() {
  if (is_live_) {
    std::cout << "Pause ignored for live stream; keeping pipeline PLAYING"
              << std::endl;
    return true;
  }

  if (!gst_.pipeline) {
    std::cerr << "Pause ignored: pipeline destroyed" << std::endl;
    return false;
  }
  play_state_requested_.store(false);
  if (gst_element_set_state(gst_.pipeline, GST_STATE_PAUSED) ==
      GST_STATE_CHANGE_FAILURE) {
    std::cerr << "Failed to change the state to PAUSED" << std::endl;
    return false;
  }

  stream_handler_->OnNotifyPlaying(false);
  return true;
}

bool GstVideoPlayer::Stop() {
  if (!gst_.pipeline) {
    std::cerr << "Stop ignored: pipeline destroyed" << std::endl;
    return false;
  }
  play_state_requested_.store(false);
  if (gst_element_set_state(gst_.pipeline, GST_STATE_READY) ==
      GST_STATE_CHANGE_FAILURE) {
    std::cerr << "Failed to change the state to READY" << std::endl;
    return false;
  }

  stream_handler_->OnNotifyPlaying(false);
  return true;
}

bool GstVideoPlayer::SetVolume(double volume) {
  if (!gst_.playbin) {
    return false;
  }

  volume_ = volume;
  g_object_set(gst_.playbin, "volume", volume, NULL);
  return true;
}

bool GstVideoPlayer::SetPlaybackRate(double rate) {
  if (!gst_.playbin || !gst_.pipeline) {
    return false;
  }

  if (rate <= 0) {
    std::cerr << "Rate " << rate << " is not supported" << std::endl;
    return false;
  }

  // No-op when the rate isn't actually changing (the common case: the Flutter
  // side calls setPlaybackSpeed(1.0) on init/resume). Seeking to set the same
  // rate is pointless and, on a LIVE stream, the seek ALWAYS fails
  // ("Failed to set playback rate to 1 (gst_element_seek failed)") — and if the
  // caller treats that failure as fatal it can blank a reconnect instead of
  // recovering. Returning success without seeking avoids both.
  if (rate == playback_rate_) {
    return true;
  }

  auto position = GetCurrentPosition();
  if (position < 0) {
    return false;
  }

  if (!gst_element_seek(gst_.pipeline, rate, GST_FORMAT_TIME,
                        GST_SEEK_FLAG_FLUSH, GST_SEEK_TYPE_SET,
                        position * GST_MSECOND, GST_SEEK_TYPE_SET,
                        GST_CLOCK_TIME_NONE)) {
    std::cerr << "Failed to set playback rate to " << rate
              << " (gst_element_seek failed)" << std::endl;
    return false;
  }

  playback_rate_ = rate;
  mute_ = (rate < 0.5 || rate > 2);
  g_object_set(gst_.playbin, "mute", mute_, NULL);

  return true;
}

bool GstVideoPlayer::SetSeek(int64_t position) {
  if (!gst_.pipeline) {
    return false;
  }
  // Block ALL seeks on live streams. The base video_player package's
  // VideoPlayerController.play() has a "if (value.position == value.duration)
  // seekTo(0)" line that fires on the auto-resume after a spurious app-
  // lifecycle pause (cage-less GBM backend on Pi4 emits AppLifecycleState.
  // paused every ~28 s with no matching resume). On live HLS, seeking to 0
  // with GST_SEEK_FLAG_FLUSH restarts hlsdemux at the earliest position in
  // the sliding-window playlist — visible as the video "repeating itself"
  // by 10-30 s. Different HLS servers/manifests fall back differently, which
  // is why the bug is channel-specific.
  //
  // A live stream has no seekable timeline anyway (the "duration" reported by
  // playbin is the current sliding-window depth, not an addressable range),
  // so rejecting the seek is the correct behavior — not a workaround.
  // Returns true so the plugin API doesn't surface an error to Flutter.
  if (is_live_) {
    std::cout << "SetSeek ignored for live stream (position=" << position
              << "ms)" << std::endl;
    return true;
  }
  auto nanosecond = position * 1000 * 1000;
  if (!gst_element_seek(
          gst_.pipeline, playback_rate_, GST_FORMAT_TIME,
          (GstSeekFlags)(GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_KEY_UNIT),
          GST_SEEK_TYPE_SET, nanosecond, GST_SEEK_TYPE_SET,
          GST_CLOCK_TIME_NONE)) {
    std::cerr << "Failed to seek " << nanosecond << std::endl;
    return false;
  }
  return true;
}

int64_t GstVideoPlayer::GetDuration() {
  if (!gst_.pipeline) {
    return -1;
  }
  GstFormat fmt = GST_FORMAT_TIME;
  gint64 duration_msec;
  if (!gst_element_query_duration(gst_.pipeline, fmt, &duration_msec)) {
    std::cerr << "Failed to get duration" << std::endl;
    return -1;
  }
  duration_msec /= GST_MSECOND;
  return duration_msec;
}

int64_t GstVideoPlayer::GetCurrentPosition() {
  if (!gst_.pipeline) {
    return 0;
  }
  gint64 position = 0;

  // Sometimes we get an error when playing streaming videos.
  if (!gst_element_query_position(gst_.pipeline, GST_FORMAT_TIME, &position)) {
    return 0;
  }

  // TODO: We need to handle this code in the proper plase.
  // The VideoPlayer plugin doesn't have a main loop, so EOS message
  // received from GStreamer cannot be processed directly in a callback
  // function. This is because the event channel message of playback complettion
  // needs to be thrown in the main thread.
  {
    std::unique_lock<std::mutex> lock(mutex_event_completed_);
    if (is_completed_) {
      is_completed_ = false;
      lock.unlock();

      if (auto_repeat_) {
        SetSeek(0);
      } else {
        stream_handler_->OnNotifyCompleted();
      }
    }
  }

  return position / GST_MSECOND;
}

#ifdef USE_EGL_IMAGE_DMABUF
void* GstVideoPlayer::GetEGLImage(void* egl_display, void* egl_context) {
  std::shared_lock<std::shared_mutex> lock(mutex_buffer_);
  if (!gst_.buffer) {
    return nullptr;
  }

  GstMemory* memory = gst_buffer_peek_memory(gst_.buffer, 0);
  if (gst_is_dmabuf_memory(memory)) {
    UnrefEGLImage();

    gint fd = gst_dmabuf_memory_get_fd(memory);
    gst_gl_display_egl_ =
        gst_gl_display_egl_new_with_egl_display(reinterpret_cast<gpointer>(egl_display));
    gst_gl_ctx_ = gst_gl_context_new_wrapped(
        GST_GL_DISPLAY_CAST(gst_gl_display_egl_), reinterpret_cast<guintptr>(egl_context),
        GST_GL_PLATFORM_EGL, GST_GL_API_GLES2);

    gst_gl_context_activate(gst_gl_ctx_, TRUE);

    gst_egl_image_ =
        gst_egl_image_from_dmabuf(gst_gl_ctx_, fd, &gst_video_info_, 0, 0);
    return reinterpret_cast<void*>(gst_egl_image_get_image(gst_egl_image_));
  }
  return nullptr;
}

void GstVideoPlayer::UnrefEGLImage() {
  if (gst_egl_image_) {
    gst_egl_image_unref(gst_egl_image_);
    gst_object_unref(gst_gl_ctx_);
    gst_object_unref(gst_gl_display_egl_);
    gst_egl_image_ = NULL;
    gst_gl_ctx_ = NULL;
    gst_gl_display_egl_ = NULL;
  }
}
#endif  // USE_EGL_IMAGE_DMABUF

const uint8_t* GstVideoPlayer::GetFrameBuffer() {
#ifdef USE_EGL_IMAGE_DMABUF
  // When using DMA-BUF zero-copy, CPU-side framebuffer extraction is not used.
  // Callers should use GetEGLImage instead.
  return nullptr;
#else
  std::shared_lock<std::shared_mutex> lock(mutex_buffer_);
  if (!gst_.buffer) {
    return nullptr;
  }

  const uint32_t pixel_bytes = width_ * height_ * 4;
  gst_buffer_extract(gst_.buffer, 0, pixels_.get(), pixel_bytes);
  return reinterpret_cast<const uint8_t*>(pixels_.get());
#endif
}

// Pick an ALSA device for audio output by detecting which HDMI connector is
// physically connected via DRM, then mapping back to the matching ALSA card.
// DRM connector "HDMI-A-N" pairs with ALSA card id "vc4hdmi{N-1}".
// Falls back to plughw:0,0 (3.5mm jack on Pi) if no HDMI is connected.
static std::string PickAudioDevice() {
  for (int hdmi_idx = 1; hdmi_idx <= 4; ++hdmi_idx) {
    char pattern[64];
    std::snprintf(pattern, sizeof(pattern),
                  "/sys/class/drm/card*-HDMI-A-%d/status", hdmi_idx);
    glob_t g{};
    if (glob(pattern, 0, nullptr, &g) != 0) {
      globfree(&g);
      continue;
    }
    bool connected = false;
    for (size_t i = 0; i < g.gl_pathc; ++i) {
      std::ifstream f(g.gl_pathv[i]);
      std::string status;
      if (f && std::getline(f, status) && status == "connected") {
        connected = true;
        break;
      }
    }
    globfree(&g);
    if (!connected) continue;

    char target[16];
    std::snprintf(target, sizeof(target), "vc4hdmi%d", hdmi_idx - 1);
    for (int card = 0; card < 32; ++card) {
      char id_path[64];
      std::snprintf(id_path, sizeof(id_path), "/proc/asound/card%d/id", card);
      std::ifstream f(id_path);
      std::string id;
      if (f && std::getline(f, id) && id == target) {
        char dev[32];
        std::snprintf(dev, sizeof(dev), "plughw:%d,0", card);
        std::cout << "PickAudioDevice: HDMI-A-" << hdmi_idx
                  << " connected, ALSA card " << card << " (" << id
                  << ") -> " << dev << std::endl;
        return dev;
      }
    }
    std::cerr << "PickAudioDevice: HDMI-A-" << hdmi_idx
              << " connected but no ALSA card matching '" << target
              << "' found" << std::endl;
  }
  std::cout << "PickAudioDevice: No connected HDMI, falling back to plughw:0,0"
            << std::endl;
  return "plughw:0,0";
}

// Diagnostic: log the caps negotiated on either side of the ISP (v4l2convert)
// whenever they change. user_data is a static string tag identifying the pad.
// This reveals what actually happens across an ABR resolution switch: the sink
// pad shows the DECODER output (e.g. 854x480 vs 1280x720), the src pad shows
// what the ISP produces (pinned 1280x720 if the fixed-geometry hold works). If
// the sink changes to 854x480 while the src stays 1280x720, the ISP is being
// asked to upscale — and a cropped/zoomed picture then confirms it is NOT
// recomputing its scale for the new input (the crop bug we need to fix).
static GstPadProbeReturn IspCapsLogProbe(GstPad* /*pad*/, GstPadProbeInfo* info,
                                         gpointer user_data) {
  GstEvent* event = GST_PAD_PROBE_INFO_EVENT(info);
  if (event && GST_EVENT_TYPE(event) == GST_EVENT_CAPS) {
    GstCaps* caps = nullptr;
    gst_event_parse_caps(event, &caps);
    gchar* s = caps ? gst_caps_to_string(caps) : nullptr;
    std::cout << "ISP-CAPS[" << reinterpret_cast<const char*>(user_data)
              << "]: " << (s ? s : "(null)") << std::endl;
    if (s) g_free(s);
  }
  return GST_PAD_PROBE_OK;
}

static void SourceSetupCallback(GstElement* playbin, GstElement* source,
                                gpointer user_data) {
  auto* self = reinterpret_cast<GstVideoPlayer*>(user_data);
  const char* type_name = G_OBJECT_TYPE_NAME(source);
  std::cout << "SOURCE-SETUP: element=" << type_name << std::endl;

  if (g_strcmp0(type_name, "GstCurlHttpSrc") == 0) {
    GObjectClass* klass = G_OBJECT_GET_CLASS(source);

    // keep-alive=FALSE: do NOT reuse the HTTP connection across segment
    // requests. Device logs (2026-06-22) showed ~2 min of perfect playback
    // (buffer pinned at 100%) followed by a sudden buffer collapse 100%->9%
    // and a 30s frame freeze — the signature of a keep-alive socket that the
    // CDN edge silently recycled/half-closed. This build's curlhttpsrc is an
    // old version with neither low-speed-time nor connect-timeout, so it
    // cannot detect or abort the dead socket; the fetch just hangs until the
    // blunt total timeout. A fresh connection per segment means a recycled
    // socket can never poison the stream, and a single bad fetch fails in
    // isolation so hlsdemux can retry the next segment without a full reconnect.
    //
    // timeout = CURLOPT_TIMEOUT: total transfer time per segment request.
    // Lowered 30s->12s so a hung fetch aborts and hlsdemux retries BEFORE the
    // 30s frame-stall watchdog tears the whole pipeline down. A healthy ~10s
    // 720p segment (~3.4 MB @ 2.75 Mbps) downloads well within 12s.
    g_object_set(source, "timeout", (gint)12, "compress", TRUE, "keep-alive",
                 FALSE, NULL);

    // connect-timeout = CURLOPT_CONNECTTIMEOUT: not present in all GStreamer builds.
    if (g_object_class_find_property(klass, "connect-timeout")) {
      g_object_set(source, "connect-timeout", (gint)10, NULL);
      std::cout << "SOURCE-SETUP: curlhttpsrc connect-timeout=10s" << std::endl;
    } else {
      std::cout << "SOURCE-SETUP: curlhttpsrc connect-timeout unavailable (old version)" << std::endl;
    }

    // low-speed-limit / low-speed-time: abort if rate stays below 200 B/s for
    // 15 s — catches partial-body hangs that CURLOPT_TIMEOUT alone misses.
    if (g_object_class_find_property(klass, "low-speed-time")) {
      g_object_set(source, "low-speed-time", (gint)15, "low-speed-limit", (glong)200, NULL);
      std::cout << "SOURCE-SETUP: curlhttpsrc low-speed: <200B/s for 15s = abort" << std::endl;
    } else {
      std::cout << "SOURCE-SETUP: curlhttpsrc low-speed unavailable (old version)" << std::endl;
    }

    gint t = 0;
    g_object_get(source, "timeout", &t, NULL);
    std::cout << "SOURCE-SETUP: curlhttpsrc timeout=" << t << "s" << std::endl;

    if (!self->auth_headers_.all_headers.empty()) {
      GstStructure* headers = gst_structure_new_empty("extra-headers");
      for (const auto& [key, value] : self->auth_headers_.all_headers) {
        gst_structure_set(headers, key.c_str(), G_TYPE_STRING, value.c_str(),
                          NULL);
      }
      g_object_set(source, "extra-headers", headers, NULL);
      gst_structure_free(headers);
    }
  } else if (g_strcmp0(type_name, "GstSoupHTTPSrc") == 0) {
    GObjectClass* klass = G_OBJECT_GET_CLASS(source);
    std::cout << "SOURCE-SETUP: using souphttpsrc" << std::endl;

    // souphttpsrc's `timeout` is a BLOCKING-I/O (INACTIVITY) timeout: abort a
    // read that produces no data for N seconds. This is exactly what the old
    // curlhttpsrc could NOT do — curl only had a total-transfer cap, so a
    // segment fetch that TCP-connected then delivered nothing hung until the
    // blunt total timeout, draining the multiqueue and starving the decoder
    // (the recurring buffer-LOW source stall). With an inactivity timeout a
    // stalled fetch dies in 5s and hlsdemux retries, while a slow-but-
    // PROGRESSING segment keeps data flowing and is NOT cut.
    // 15s, not 5s: this is an INACTIVITY timeout (abort a read with no data for
    // N seconds). HLS segments here are ~10s, and at the live edge a legitimate
    // wait for the next segment (or a CDN that holds the request open until the
    // segment is ready) can exceed 5s — a 5s timeout aborts that healthy fetch,
    // causing stutter and preventing the buffer from building. 15s is longer
    // than a segment but still well under the 30s frame-stall watchdog, so a
    // genuinely hung fetch still aborts+retries before a reconnect.
    g_object_set(source, "timeout", (guint)15, NULL);

    // TLS: this CDN previously failed soup's strict certificate check, which is
    // why the project switched to curl. Relax strict verification so soup can
    // connect (the CDN is known/trusted). If soup STILL cannot connect after
    // this, the TLS *backend* (glib-networking) is missing from the rootfs — an
    // image fix, and we revert to curl.
    if (g_object_class_find_property(klass, "ssl-strict")) {
      g_object_set(source, "ssl-strict", FALSE, NULL);
      std::cout << "SOURCE-SETUP: souphttpsrc ssl-strict=FALSE" << std::endl;
    }
    // Retry an aborted/failed fetch a few times before erroring upstream.
    if (g_object_class_find_property(klass, "retries")) {
      g_object_set(source, "retries", (gint)3, NULL);
    }
    // Fresh connection per request — avoid a CDN-recycled half-open socket
    // (same rationale as curl keep-alive=FALSE).
    if (g_object_class_find_property(klass, "keep-alive")) {
      g_object_set(source, "keep-alive", FALSE, NULL);
    }
    if (g_object_class_find_property(klass, "compress")) {
      g_object_set(source, "compress", TRUE, NULL);
    }

    guint st = 0;
    g_object_get(source, "timeout", &st, NULL);
    std::cout << "SOURCE-SETUP: souphttpsrc timeout=" << st
              << "s (inactivity/blocking-IO)" << std::endl;

    if (!self->auth_headers_.all_headers.empty()) {
      GstStructure* headers = gst_structure_new_empty("extra-headers");
      for (const auto& [key, value] : self->auth_headers_.all_headers) {
        gst_structure_set(headers, key.c_str(), G_TYPE_STRING, value.c_str(),
                          NULL);
      }
      g_object_set(source, "extra-headers", headers, NULL);
      gst_structure_free(headers);
    }
  }
}

void GstVideoPlayer::SetAuthHeaders(
    const std::map<std::string, std::string>& headers) {
  auth_headers_.all_headers = headers;
}

bool GstVideoPlayer::CreatePipeline() {
  GstRegistry* registry = gst_registry_get();

  // PREFER souphttpsrc: its `timeout` is a blocking-I/O (inactivity) timeout
  // that aborts a stalled-body fetch (TCP-connected but delivering no data) in
  // seconds, which the old curlhttpsrc cannot do (it only had a blunt total
  // transfer cap, so a stalled fetch hung long enough to drain the buffer and
  // starve the decoder = the recurring source stall). soup's earlier TLS
  // failure is worked around with ssl-strict=FALSE in SourceSetupCallback.
  // curl is kept as a LOWER-ranked fallback in case soup is unavailable.
  GstPluginFeature* curl_feature = gst_registry_lookup_feature(registry, "curlhttpsrc");
  GstPluginFeature* soup_feature = gst_registry_lookup_feature(registry, "souphttpsrc");
  if (soup_feature) {
    gst_plugin_feature_set_rank(soup_feature, GST_RANK_PRIMARY + 300);
    gst_object_unref(soup_feature);
  } else {
    std::cerr << "CreatePipeline: WARNING - souphttpsrc not found" << std::endl;
  }
  if (curl_feature) {
    gst_plugin_feature_set_rank(curl_feature, GST_RANK_PRIMARY + 200);
    gst_object_unref(curl_feature);
  } else {
    std::cerr << "CreatePipeline: WARNING - curlhttpsrc not found" << std::endl;
  }

  // Prefer hardware H.264 decode on the Pi.
  GstPluginFeature* v4l2_h264 = gst_registry_lookup_feature(registry, "v4l2h264dec");
  if (v4l2_h264) {
    gst_plugin_feature_set_rank(v4l2_h264, GST_RANK_PRIMARY + 300);
    gst_object_unref(v4l2_h264);
  }

  gst_.pipeline = gst_pipeline_new("pipeline");
  if (!gst_.pipeline) {
    std::cerr << "Failed to create a pipeline" << std::endl;
    return false;
  }

  gst_.playbin = gst_element_factory_make("playbin", "playbin");
  if (!gst_.playbin) {
    std::cerr << "Failed to create playbin" << std::endl;
    return false;
  }

  g_signal_connect(gst_.playbin, "source-setup", G_CALLBACK(SourceSetupCallback), this);

  // ABR engine: catch hlsdemux as soon as decodebin creates it so the
  // throughput probe and connection-speed steering can attach to it.
  g_signal_connect(gst_.pipeline, "deep-element-added",
                   G_CALLBACK(DeepElementAddedHandler), this);

  // Video converter: prefer v4l2convert (hardware-accelerated) with RGBA output,
  // fall back to software videoconvert if unavailable. The HW ISP does BOTH
  // colour conversion (NV12->RGBA) and scaling, which is what lets us pin a
  // fixed output geometry below and survive ABR rendition switches.
  gst_.video_convert = gst_element_factory_make("v4l2convert", "videoconvert");
  if (!gst_.video_convert) {
    gst_.video_convert = gst_element_factory_make("videoconvert", "videoconvert");
  }
  if (!gst_.video_convert) {
    std::cerr << "Failed to create videoconvert" << std::endl;
    return false;
  }

  gst_.video_sink = gst_element_factory_make("fakesink", "videosink");
  if (!gst_.video_sink) {
    std::cerr << "Failed to create videosink" << std::endl;
    return false;
  }

  GstElement* video_queue = gst_element_factory_make("queue", "vqueue");
  if (!video_queue) {
    std::cerr << "Failed to create video queue" << std::endl;
    return false;
  }
  // Limit the queue size to prevent buffer accumulation after the hardware decoder,
  // which otherwise exhausts the Raspberry Pi's CMA (Contiguous Memory Allocator)
  // and floods the kernel with 'swiotlb buffer is full' errors.
  g_object_set(G_OBJECT(video_queue),
               "max-size-buffers", (guint)5,
               "max-size-bytes",   (guint)0,
               "max-size-time",    (guint64)0,
               NULL);


  gst_.output = gst_bin_new("output");
  if (!gst_.output) {
    std::cerr << "Failed to create output bin" << std::endl;
    return false;
  }

  gst_.bus = gst_pipeline_get_bus(GST_PIPELINE(gst_.pipeline));
  if (!gst_.bus) {
    std::cerr << "Failed to get bus" << std::endl;
    return false;
  }
  gst_bus_set_sync_handler(gst_.bus, HandleGstMessage, this, NULL);

  // sync=TRUE: fakesink paces video frames to the GStreamer clock, keeping
  // A/V in sync. async=TRUE (default): fakesink participates in preroll,
  // which is what we want — Init() waits for the first decoded frame before
  // calling OnNotifyInitialized().
  //
  // qos=TRUE + max-lateness engaged POST-first-frame: after a decoder stall
  // the sink would otherwise try to render every buffered late frame,
  // producing a burst of stutter and apparent speed-up as it catches up to
  // the audio clock. Device 2026-08-03 13:28:28-13:28:57 saw three separate
  // stall windows (5s, 3s, 7s) with a healthy buffer/network; each recovery
  // was a visible stutter cascade. With a 500ms cap the sink drops any
  // frame more than half a second late instead of racing to render it —
  // recovery becomes: freeze on last good frame, jump forward to current
  // audio-clock position, normal playback resumes cleanly.
  //
  // BUT: engaging the cap at construction breaks preroll. On a fresh
  // pipeline (initial init or post-reconnect), the videosink and the
  // pipeline clock haven't settled into a common running time yet, and
  // 500ms is small enough that arriving buffers can read as "too late"
  // before they've had a chance to be scheduled. The sink then drops every
  // frame, downstream fill stalls, the multiqueue plateaus at ~9s of
  // pending data, `Buffered Until - Position` never crosses the 15s
  // preroll gate, and playback deadlocks. Device log 2026-08-06 11:57:44 →
  // 12:11:58 captured a 14-minute stuck-preroll after a decoder-freeze
  // reconnect on talkSPORT, entirely from this mechanism.
  //
  // Fix: start with max-lateness=-1 (disabled — sink renders every frame
  // regardless of lateness), then flip to 500ms in HandoffHandler on the
  // very first delivered frame. Preroll is unblocked; steady-state stutter
  // cascade bounding still fires as designed once real playback starts.
  //
  // The gold-standard approach is manual pipeline-clock control (stall
  // both audio and video together so there's nothing to catch up to) —
  // see [[gold-standard-playback-roadmap]].
  g_object_set(G_OBJECT(gst_.video_sink),
               "sync", TRUE,
               "qos", TRUE,
               "max-lateness", (gint64)(-1),   // disabled during preroll
               NULL);
  g_object_set(G_OBJECT(gst_.video_sink), "signal-handoffs", TRUE, NULL);
  g_signal_connect(G_OBJECT(gst_.video_sink), "handoff",
                   G_CALLBACK(HandoffHandler), this);

  // queue -> v4l2convert -> video/x-raw,format=RGBA,1920x1080 -> fakesink
  //
  // SEAMLESS ABR SWITCHING (fix8-ABR): pin the converter's OUTPUT geometry to a
  // FIXED size and let the HW ISP scale whatever the decoder produces up/down to
  // it. This mirrors how phones/TVs/browsers switch renditions without a hitch:
  // the display surface is a constant size and the scaler absorbs the input
  // resolution change.
  //
  // Why the FIXED output prevents the S_FMT crash on down-switch: previously
  // the output caps constrained only the format (RGBA), so the output
  // resolution TRACKED the input. An ABR down-switch (720p->480p) therefore
  // changed the ISP's OUTPUT format mid-stream, forcing a VIDIOC_S_FMT for
  // AB24 @ 854x480 that a V4L2 M2M device rejects while streaming (EINVAL)
  // -> pipeline error -> reconnect churn -> crash. With a fixed output
  // geometry the output S_FMT is programmed ONCE at preroll and never
  // renegotiated; only the ISP's INPUT (capture-side NV12) changes on a
  // switch, which the decoder drives via the standard V4L2 source-change
  // flow. Bonus: HandoffHandler's width_/height_ never change after preroll,
  // so the Flutter pixel buffer is allocated once (no per-switch texture
  // resize). All ladder rungs are 16:9 so upscaling introduces no aspect
  // distortion.
  //
  // Why 1920x1080 and not 1280x720 (task #39, 2026-08-20): the device screen
  // is 1920x1080 anyway (see Device-Info screen size in every session). When
  // the pin was 1280x720, the ISP was a 1x passthrough on the top-rung
  // (1280x720) input — no scale operation, so a stale crop-rectangle from
  // an earlier smaller input geometry carried over on in-place up-switches.
  // Users saw the 720p output as visibly cropped after a big-jump up-switch
  // (e.g. cold-start 240p → 720p, three rungs skipped, each rung's stale
  // crop state accumulated). Session 2026-08-20 12:41:37 reproduced this
  // exactly with a 426x240 → 1280x720 in-place S_FMT.
  //
  // Pinning to 1920x1080 forces the ISP to always be an active scaler,
  // regardless of input rendition. Every in-place S_FMT triggers a real
  // scale operation, which resets the crop-rectangle to full input
  // dimensions. Also avoids a downstream compositor scale from 720 to
  // 1080 — one scale total instead of two.
  //
  // NOTE (verify on build host): the Pi's v4l2convert is a hardware scaler.
  // Upscale to 1080p is essentially free. If a future build's HW convert
  // cannot scale to 1080p, this filtered link will fail negotiation and
  // Init() returns false -> would then need a HW-scale-capable element or
  // an SW videoscale.
  gst_bin_add_many(GST_BIN(gst_.output), video_queue, gst_.video_convert,
                   gst_.video_sink, NULL);
  if (!gst_element_link(video_queue, gst_.video_convert)) {
    std::cerr << "Failed to link queue to videoconvert" << std::endl;
    return false;
  }
  auto* caps = gst_caps_from_string(
      "video/x-raw,format=RGBA,width=1920,height=1080");
  auto link_ok = gst_element_link_filtered(gst_.video_convert, gst_.video_sink, caps);
  gst_caps_unref(caps);
  if (!link_ok) {
    std::cerr << "Failed to link videoconvert to fakesink" << std::endl;
    return false;
  }

  // Diagnostic (fix8-ABR): watch the caps on both sides of the ISP so a device
  // run reveals exactly what it negotiates across a 480<->720 rendition switch
  // (see IspCapsLogProbe). Cheap — fires only on a caps change, not per frame.
  if (auto* isp_sink = gst_element_get_static_pad(gst_.video_convert, "sink")) {
    gst_pad_add_probe(isp_sink, GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM,
                      IspCapsLogProbe, (gpointer) "convert-sink", NULL);
    gst_object_unref(isp_sink);
  }
  if (auto* isp_src = gst_element_get_static_pad(gst_.video_convert, "src")) {
    gst_pad_add_probe(isp_src, GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM,
                      IspCapsLogProbe, (gpointer) "convert-src", NULL);
    gst_object_unref(isp_src);
  }

  auto* sinkpad = gst_element_get_static_pad(video_queue, "sink");
  auto* ghost_sinkpad = gst_ghost_pad_new("sink", sinkpad);
  gst_pad_set_active(ghost_sinkpad, TRUE);
  gst_element_add_pad(gst_.output, ghost_sinkpad);
  gst_object_unref(sinkpad);

  g_object_set(gst_.playbin, "uri", uri_.c_str(), NULL);
  g_object_set(gst_.playbin, "video-sink", gst_.output, NULL);

  const gint GST_PLAY_FLAG_VIDEO        = 0x00000001;
  const gint GST_PLAY_FLAG_AUDIO        = 0x00000002;
  const gint GST_PLAY_FLAG_TEXT         = 0x00000004;
  const gint GST_PLAY_FLAG_NATIVE_VIDEO = 0x00000800;
  const gint GST_PLAY_FLAG_BUFFERING    = 0x00000080;

  gint flags = GST_PLAY_FLAG_VIDEO | GST_PLAY_FLAG_AUDIO |
               GST_PLAY_FLAG_NATIVE_VIDEO | GST_PLAY_FLAG_BUFFERING;
  flags &= ~GST_PLAY_FLAG_TEXT;
  g_object_set(gst_.playbin, "flags", flags, NULL);

  // The origin emits 10 s HLS segments, so keep roughly three segments of
  // time-depth. This gives the player enough cushion for a late segment fetch.
  //
  // STARTUP rung: a high initial connection-speed makes hlsdemux pick the top
  // variant for the first segments, before the ABR engine has its >=2 samples
  // (AbrTick early-returns until then). This is NOT for speed — it's the
  // invariant that keeps the bcm2835-codec V4L2 input pool safe. Starting at a
  // lower rung and letting ABR climb sets the pool to a small geometry first,
  // then GROWS it on up-switch (OK), then tries to SHRINK it on the next
  // down-switch (FAILS: S_FMT "Device has no supported format", verified
  // 2026-07-30). Top-rung startup means the pool is set once at the largest
  // geometry that will ever be seen; every subsequent switch is a no-op or a
  // shrink the ISP can absorb via the pinned 1280x720 output geometry (see
  // the sink-bin comment). If the ABR engine never gets samples (e.g. probe
  // yields nothing) hlsdemux simply stays on the top rung.
  // Cold-start rung. If soatv threaded a `#soatv:startup_kbps=N` hint
  // through the URI fragment (from auth-GET throughput probe or a
  // preceding ABR_RESTART decision), honor it. Otherwise fall back to
  // the fixed default. Task #48 (2026-08-20) added the auth-probe path
  // so cold-starts now match the actual network instead of always
  // starting at 1500 kbps.
  const guint64 cold_start_kbps =
      startup_kbps_hint_ > 0 ? startup_kbps_hint_ : kColdStartConnSpeedKbps;
  if (startup_kbps_hint_ > 0) {
    std::cout << "STARTUP-RUNG: honoring soatv hint " << startup_kbps_hint_
              << "kbps (instead of default " << kColdStartConnSpeedKbps
              << "kbps)" << std::endl;
  }
  g_object_set(gst_.playbin,
               "buffer-size",     (gint)10485760,         // 10 MiB
               "buffer-duration", kBufferTargetNs,        // 60 s (task #60)
               "connection-speed", cold_start_kbps,
               NULL);

  // Audio: audioconvert -> audioresample -> volume -> alsasink
  // (HDMI auto-detected). Explicit alsasink is required because Buildroot
  // omits autoaudiosink. audioresample prevents ALSA sample-rate-mismatch
  // underruns under load. The "volume" element is inserted before the sink
  // so that Init() can mute audio during the preroll gate: playbin's own
  // "mute" property forwards to the audio sink's GstStreamVolume interface,
  // which our alsasink-in-a-bin does not implement (playsink logs
  // "No volume control found / Volume/mute is not available" on start),
  // so driving mute on this element is the only mute path that actually
  // silences audio.
  GstElement* audio_bin  = gst_bin_new("audio_bin");
  GstElement* conv       = gst_element_factory_make("audioconvert",  "audio_convert");
  GstElement* resample   = gst_element_factory_make("audioresample", "audio_resample");
  GstElement* volume     = gst_element_factory_make("volume",        "audio_volume");
  GstElement* audio_sink = gst_element_factory_make("alsasink",      "audio_alsa");

  if (!audio_bin || !conv || !resample || !volume || !audio_sink) {
    std::cerr << "CreatePipeline: Failed to create audio elements" << std::endl;
  } else {
    std::string audio_device = PickAudioDevice();
    g_object_set(audio_sink, "device", audio_device.c_str(), NULL);
    // ALSA cushion sizing. Previously 50 ms / 100 ms — tight enough on a Pi 4
    // under Flutter render + V4L2 decode + ISP scale + Wi-Fi soft-IRQ load
    // that a ~100 ms scheduling gap caused alsasink underruns, and because
    // the audio sink provides the pipeline clock under sync=TRUE, each
    // underrun hiccuped the clock: video branch either burst-dropped frames
    // to catch up or held frames to wait, showing visible stutter (device
    // log 2026-07-30 15:48:38-15:48:45 showed exactly this — burst deltas
    // of +80/+68/+38/+35/+17 frames in successive wallclock seconds with a
    // full buffer and 5 Mbps throughput). 100 ms / 500 ms is the standard
    // robust default for Pi-class devices; adds ~400 ms audio-branch latency
    // which is imperceptible for live TV (no interactivity beyond channel
    // changes, which rebuild the pipeline anyway).
    g_object_set(audio_sink,
                 "latency-time", (gint64)100000,   // 100 ms
                 "buffer-time",  (gint64)500000,   // 500 ms
                 NULL);

    gst_bin_add_many(GST_BIN(audio_bin), conv, resample, volume, audio_sink, NULL);
    if (!gst_element_link(conv, resample) ||
        !gst_element_link(resample, volume) ||
        !gst_element_link(volume, audio_sink)) {
      std::cerr << "CreatePipeline: Failed to link audio chain" << std::endl;
    } else {
      GstPad* apad = gst_element_get_static_pad(conv, "sink");
      GstPad* ghost_apad = gst_ghost_pad_new("sink", apad);
      gst_pad_set_active(ghost_apad, TRUE);
      gst_element_add_pad(audio_bin, ghost_apad);
      gst_object_unref(apad);
      g_object_set(gst_.playbin, "audio-sink", audio_bin, NULL);
      audio_volume_ = volume;  // ref held via bin ownership; cleared in DestroyPipeline
    }
  }

  gst_bin_add_many(GST_BIN(gst_.pipeline), gst_.playbin, NULL);

  return true;
}

bool GstVideoPlayer::Preroll() {
  auto result = gst_element_set_state(gst_.pipeline, GST_STATE_PAUSED);
  if (result == GST_STATE_CHANGE_FAILURE) {
    std::cerr << "Preroll: Failed to change state to PAUSED" << std::endl;
    return false;
  }
  // NO_PREROLL (live source) and ASYNC are both acceptable — not errors.
  // NO_PREROLL is GStreamer's authoritative "this source is LIVE" signal — far
  // more reliable than a duration/seekable heuristic (a live HLS playlist still
  // reports a small sliding-window duration, which looks like VOD). Capture it
  // so EOS handling never treats a live stream as "completed".
  if (result == GST_STATE_CHANGE_NO_PREROLL) {
    is_live_ = true;
    std::cout << "PREROLL: live source (NO_PREROLL) — EOS will not complete/seek"
              << std::endl;
  }
  // Give GStreamer up to 5 s to settle before Init() advances to PLAYING.
  GstState state;
  gst_element_get_state(gst_.pipeline, &state, NULL, 5 * GST_SECOND);
  return true;
}

void GstVideoPlayer::DestroyPipeline() {
  play_state_requested_.store(false);
  StopWatchdog();
  StopAbrEngine();

  if (gst_.video_sink) {
    g_object_set(G_OBJECT(gst_.video_sink), "signal-handoffs", FALSE, NULL);
  }

  // Release the held frame BEFORE the NULL transition. The buffer belongs to
  // v4l2convert's CMA-backed pool; a pool with an outstanding buffer cannot
  // be deactivated and is orphaned, leaking its V4L2/CMA memory. Repeated
  // reconnect cycles then exhaust CMA ("Failed to allocate required memory",
  // "export failed") and every new pipeline renders a blank screen.
  {
    std::lock_guard<std::shared_mutex> lock(mutex_buffer_);
    if (gst_.buffer) {
      gst_buffer_unref(gst_.buffer);
      gst_.buffer = nullptr;
    }
  }

  if (gst_.pipeline) {
    gst_element_set_state(gst_.pipeline, GST_STATE_NULL);
  }

  // A handoff in flight between disabling signal-handoffs and the NULL
  // transition may have stored one more frame — release it as well.
  {
    std::lock_guard<std::shared_mutex> lock(mutex_buffer_);
    if (gst_.buffer) {
      gst_buffer_unref(gst_.buffer);
      gst_.buffer = nullptr;
    }
  }

  if (gst_.bus) {
    gst_bus_set_sync_handler(gst_.bus, NULL, NULL, NULL);
    gst_object_unref(gst_.bus);
    gst_.bus = nullptr;
  }

  if (gst_.pipeline) {
    gst_object_unref(gst_.pipeline);
    gst_.pipeline = nullptr;
  }

  gst_.playbin = nullptr;
  gst_.output = nullptr;
  gst_.video_sink = nullptr;
  gst_.video_convert = nullptr;
  audio_volume_ = nullptr;

  {
    std::lock_guard<std::mutex> lock(abr_mutex_);
    if (hls_demux_) {
      gst_object_unref(hls_demux_);
      hls_demux_ = nullptr;
    }
  }
}

std::string GstVideoPlayer::ParseUri(const std::string& uri) {
  if (gst_uri_is_valid(uri.c_str())) {
    return uri;
  }

  auto* filename_uri = gst_filename_to_uri(uri.c_str(), NULL);
  if (!filename_uri) {
    std::cerr << "Faild to open " << uri.c_str() << std::endl;
    return uri;
  }
  std::string result_uri(filename_uri);
  g_free(filename_uri);

  return result_uri;
}

void GstVideoPlayer::GetVideoSize(int32_t& width, int32_t& height) {
  if (!gst_.pipeline || !gst_.video_sink) {
    std::cerr
        << "Failed to get video size. The pileline hasn't initialized yet.";
    return;
  }

  auto* sink_pad = gst_element_get_static_pad(gst_.video_sink, "sink");
  if (!sink_pad) {
    std::cerr << "Failed to get a pad";
    return;
  }

  auto* caps = gst_pad_get_current_caps(sink_pad);
  if (!caps) {
    std::cerr << "Failed to get caps" << std::endl;
    gst_object_unref(sink_pad);
    return;
  }

  auto* structure = gst_caps_get_structure(caps, 0);
  if (!structure) {
    std::cerr << "Failed to get a structure" << std::endl;
    gst_caps_unref(caps);
    gst_object_unref(sink_pad);
    return;
  }

  gst_structure_get_int(structure, "width", &width);
  gst_structure_get_int(structure, "height", &height);

#ifdef USE_EGL_IMAGE_DMABUF
  gboolean res = gst_video_info_from_caps(&gst_video_info_, caps);
  if (!res) {
    std::cerr << "Failed to get a gst_video_info" << std::endl;
    gst_caps_unref(caps);
    gst_object_unref(sink_pad);
    return;
  }
#endif  // USE_EGL_IMAGE_DMABUF

  gst_caps_unref(caps);
  gst_object_unref(sink_pad);
}

// static
void GstVideoPlayer::HandoffHandler(GstElement* fakesink, GstBuffer* buf,
                                    GstPad* new_pad, gpointer user_data) {
  auto* self = reinterpret_cast<GstVideoPlayer*>(user_data);
  auto* caps = gst_pad_get_current_caps(new_pad);
  auto* structure = gst_caps_get_structure(caps, 0);

  int width = 0, height = 0;
  gst_structure_get_int(structure, "width", &width);
  gst_structure_get_int(structure, "height", &height);
  gst_caps_unref(caps);

  {
    std::lock_guard<std::shared_mutex> lock(self->mutex_buffer_);
    if (width != self->width_ || height != self->height_) {
      self->width_ = width;
      self->height_ = height;
      if (width > 0 && height > 0)
        self->pixels_.reset(new uint32_t[width * height]);
    }
    if (self->gst_.buffer)
      gst_buffer_unref(self->gst_.buffer);
    self->gst_.buffer = gst_buffer_ref(buf);
  }

  // Wake Init()'s condition variable on the very first frame. Also engage
  // the video sink's max-lateness cap NOW that preroll is proven complete —
  // it starts at -1 (disabled) so preroll can never be blocked by a
  // frame-drop cascade, and flips to 500 ms here so subsequent decoder
  // stalls in steady-state are still bounded per the original 0c891c3
  // rationale. See CreatePipeline() comment for the full story.
  if (!self->first_frame_ready_.exchange(true)) {
    if (self->gst_.video_sink) {
      g_object_set(G_OBJECT(self->gst_.video_sink),
                   "max-lateness", (gint64)(500 * GST_MSECOND), NULL);
      std::cout << "MAX-LATENESS: engaged 500ms after first-frame delivery"
                << std::endl;
    }
    self->first_frame_cv_.notify_all();
  }

  // Deferred-init path: only fires if Init() timed out before this frame
  // arrived and left initialized_ = false. In the normal case Init() wins
  // the exchange and calls OnNotifyInitialized() itself.
  if (!self->initialized_.exchange(true)) {
    self->stream_handler_->OnNotifyInitialized();
    self->stream_handler_->OnNotifyPlaying(true);
    // Init()'s post-preroll unmute is gated on first_frame_ready_ so audio
    // stays muted through the preroll wait. On the deferred-init path Init()
    // returned before this frame arrived, so we must unmute here or audio
    // stays silent for the rest of the session.
    if (self->audio_volume_) {
      g_object_set(self->audio_volume_, "mute",
                   self->mute_ ? TRUE : FALSE, NULL);
    }
  }

  // Ground-truth liveness signal for the watchdog: only moves when a real
  // video buffer reaches the sink, so a wedged decoder stalls it.
  self->frames_handed_off_.fetch_add(1, std::memory_order_relaxed);

  self->stream_handler_->OnNotifyFrameDecoded();
}


void GstVideoPlayer::LogPlaybackHealth(
    GstState state, uint64_t frames, std::chrono::steady_clock::time_point now,
    std::chrono::steady_clock::time_point last_frame_time) {
  if (!gst_.pipeline) return;

  // Example:
  // Current Position: 120 sec
  // Buffered Until: 140 sec
  // Buffer Health: 20 sec
  // Network Throughput: 3500 kbps
  // Actual measured throughput: throughput = segment_size_bytes / download_time_seconds

  gint64 position = 0;
  const bool has_position = gst_element_query_position(
      gst_.pipeline, GST_FORMAT_TIME, &position);
  const double position_secs = has_position
      ? static_cast<double>(position) / static_cast<double>(GST_SECOND)
      : -1.0;

  const int pct = last_buffering_percent_.load();
  double buffered_until_secs = -1.0;
  double buffer_health_secs = -1.0;
  bool used_estimate = true;

  GstQuery* query = gst_query_new_buffering(GST_FORMAT_TIME);
  if (query && gst_element_query(gst_.pipeline, query)) {
    GstFormat format = GST_FORMAT_TIME;
    gint64 start = 0;
    gint64 stop = -1;
    gint64 estimated_total = -1;
    gst_query_parse_buffering_range(query, &format, &start, &stop,
                                    &estimated_total);
    if (format == GST_FORMAT_TIME && has_position && stop >= position) {
      buffered_until_secs = static_cast<double>(stop) /
                            static_cast<double>(GST_SECOND);
      buffer_health_secs = static_cast<double>(stop - position) /
                           static_cast<double>(GST_SECOND);
      used_estimate = false;
    }
  }
  if (query) gst_query_unref(query);

  if (used_estimate && has_position && pct >= 0) {
    buffer_health_secs = (static_cast<double>(pct) / 100.0) * kBufferTargetSecs;
    buffered_until_secs = position_secs + buffer_health_secs;
  }

  guint64 last_segment_bytes = 0;
  double last_segment_secs = 0.0;
  double last_segment_bps = 0.0;
  {
    std::lock_guard<std::mutex> lock(abr_mutex_);
    last_segment_bytes = last_segment_bytes_;
    last_segment_secs = last_segment_download_secs_;
    last_segment_bps = last_segment_throughput_bps_;
  }

  const auto no_frame_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
      now - last_frame_time).count();

  std::cout << "HEALTH: state=" << gst_element_state_get_name(state)
            << " Current Position: ";
  if (has_position) {
    std::cout << static_cast<int>(position_secs) << " sec";
  } else {
    std::cout << "unknown";
  }

  std::cout << " Buffered Until: ";
  if (buffered_until_secs >= 0.0) {
    std::cout << static_cast<int>(buffered_until_secs) << " sec";
  } else {
    std::cout << "unknown";
  }

  std::cout << " Buffer Health: ";
  if (buffer_health_secs >= 0.0) {
    std::cout << static_cast<int>(buffer_health_secs) << " sec";
    if (used_estimate) std::cout << " (est)";
  } else {
    std::cout << "unknown";
  }

  std::cout << " Buffer Percent: ";
  if (pct >= 0) {
    std::cout << pct << "%";
  } else {
    std::cout << "unknown";
  }

  std::cout << " Target Buffer: " << static_cast<int>(kBufferTargetSecs)
            << " sec Frames: " << frames
            << " No Frame For: " << no_frame_ms << " ms";

  if (last_segment_bytes > 0 && last_segment_secs > 0.0) {
    std::cout << " Network Throughput: "
              << static_cast<int>(last_segment_bps / 1000.0) << " kbps"
              << " (segment=" << last_segment_bytes << "B/"
              << last_segment_secs << "s)";
  } else {
    std::cout << " Network Throughput: unknown";
  }

  std::cout << std::endl;

  // Buffer-collapse detector. On device 2026-08-03 14:14:04, buffer_health
  // dropped from 30s to 2s inside a single tick with no ABR decision, no
  // rendition switch, no bus error — and playback never recovered for the
  // next ~7 minutes. We do not know what triggered it. Emit a prominent log
  // line whenever we see the same shape (drop >=5s in one tick) so the next
  // occurrence carries context to diagnose the cause.
  //
  // Passive: no behavior change. Just captures hlsdemux + multiqueue state
  // at the moment of collapse. Future work adds bus-message capture for
  // FLUSH_START / SEGMENT_START / hlsdemux ELEMENT messages.
  if (prev_buffer_health_secs_ >= 0.0 && buffer_health_secs >= 0.0 &&
      prev_buffer_health_secs_ - buffer_health_secs >= 5.0) {
    // The delta itself is worth flagging even without more state.
    std::cout << "BUFFER-COLLAPSE: dropped from "
              << static_cast<int>(prev_buffer_health_secs_)
              << "s to " << static_cast<int>(buffer_health_secs)
              << "s in one tick; pos=" << static_cast<int>(position_secs)
              << "s pct=" << pct
              << "% state=" << gst_element_state_get_name(state) << std::endl;

    // Stamp for the ABR calm-mode gate (task #46, 2026-08-17). Any up-switch
    // firing within kPostCollapseQuietSecs of this timestamp gets routed
    // through ABR_RESTART instead of in-place SetConnectionSpeedKbps —
    // the recovery-from-collapse window has re-plugging inputselectors
    // and in-place variant switches during that window crashed the app.
    last_buffer_collapse_ns_.store(
        std::chrono::steady_clock::now().time_since_epoch().count(),
        std::memory_order_relaxed);

    // Dump hlsdemux's current-bitrate — if it changed at the moment of the
    // drop, a manifest refresh / rendition selection is likely the trigger.
    GstElement* demux = nullptr;
    {
      std::lock_guard<std::mutex> lock(abr_mutex_);
      if (hls_demux_) demux = GST_ELEMENT(gst_object_ref(hls_demux_));
    }
    if (demux) {
      guint64 current_bitrate = 0;
      GObjectClass* klass = G_OBJECT_GET_CLASS(demux);
      if (g_object_class_find_property(klass, "current-bitrate")) {
        g_object_get(demux, "current-bitrate", &current_bitrate, NULL);
        std::cout << "BUFFER-COLLAPSE: hlsdemux current-bitrate="
                  << current_bitrate << std::endl;
      }
      gst_object_unref(demux);
    }

    // Dump the ring of recent bus messages. The event that caused the flush
    // is almost certainly in the last few — SEGMENT_START, STREAM_START, an
    // ELEMENT message from hlsdemux, or similar. Device 2026-08-03 16:43:05
    // captured the collapse with no context; this makes the next occurrence
    // self-explanatory.
    DumpBusMsgRing();
  }
  prev_buffer_health_secs_ = buffer_health_secs;
}

void GstVideoPlayer::PushBusMsgRing(const std::string& type,
                                     const std::string& src_name,
                                     const std::string& extra) {
  std::lock_guard<std::mutex> lock(bus_msg_ring_mutex_);
  bus_msg_ring_.push_back(
      {std::chrono::steady_clock::now(), type, src_name, extra});
  // Cap at 20 — enough to see the immediate lead-up to a collapse, small
  // enough that the dump is readable and the streaming-thread mutex stays fast.
  while (bus_msg_ring_.size() > 20) {
    bus_msg_ring_.pop_front();
  }
}

void GstVideoPlayer::DumpBusMsgRing() {
  std::deque<BusMsgEntry> snapshot;
  {
    std::lock_guard<std::mutex> lock(bus_msg_ring_mutex_);
    snapshot = bus_msg_ring_;
  }
  if (snapshot.empty()) {
    std::cout << "BUS-RING: (empty)" << std::endl;
    return;
  }
  const auto now = std::chrono::steady_clock::now();
  std::cout << "BUS-RING: last " << snapshot.size()
            << " bus messages (most recent first):" << std::endl;
  // Print newest-first so the collapse trigger is on the first line.
  for (auto it = snapshot.rbegin(); it != snapshot.rend(); ++it) {
    const auto age_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now - it->when).count();
    std::cout << "BUS-RING:   -" << age_ms << "ms " << it->type
              << " src=" << it->src_name;
    if (!it->extra.empty()) std::cout << " " << it->extra;
    std::cout << std::endl;
  }
}

bool GstVideoPlayer::TryFlushRecovery() {
  // Called by the watchdog when playback has been stuck for ~8 s but the
  // network and buffer look fine. Cycle PAUSED -> PLAYING to force the
  // pipeline's state machine to tick — this often clears a stuck internal
  // state without requiring a seek.
  //
  // The prior seek-based approach (74ad4f9) returned false on device
  // 2026-08-03 16:43:20 (`FLUSH-RECOVERY: gst_element_seek failed`), which
  // matches the general expectation that live HLS pipelines don't accept
  // seeks in their default configuration. State-cycle recovery is cheaper
  // and doesn't depend on seekability.
  //
  // Note: an earlier attempt (2026-08-12 706a167) tried sending flush-start
  // + flush-stop(reset_time=TRUE) events on the video sink pad BEFORE this
  // state cycle. Device evidence showed those flush events combined with
  // the state-cycle wedged the pipeline in PAUSED indefinitely — the ASYNC
  // PLAYING transition never completed. Reverted here. If a flush-based
  // recovery is retried in future, it needs to (a) NOT use reset_time=TRUE
  // on live pipelines (it resets running-time to 0 while the source keeps
  // producing at high timestamps, causing all buffers to be dropped as
  // "too late"), and (b) NOT be combined with a state cycle in the same
  // recovery attempt.
  //
  // If the pipeline still doesn't produce frames after this, the watchdog's
  // 10 s escalation to full reconnect catches it — now state-agnostic
  // (see StartWatchdog fatal-timeout branch).
  if (!gst_.pipeline) return false;

  std::cout << "FLUSH-RECOVERY: cycling PAUSED -> PLAYING to unblock stuck "
               "playback" << std::endl;

  // Move to PAUSED. If the pipeline was truly stuck, downstream elements will
  // flush their internal state during this transition.
  auto pause_result = gst_element_set_state(gst_.pipeline, GST_STATE_PAUSED);
  if (pause_result == GST_STATE_CHANGE_FAILURE) {
    std::cerr << "FLUSH-RECOVERY: failed to enter PAUSED" << std::endl;
    return false;
  }

  // Immediately request PLAYING again. We don't block waiting for PAUSED to
  // finish transitioning — the state-change to PLAYING will queue after it.
  auto play_result = gst_element_set_state(gst_.pipeline, GST_STATE_PLAYING);
  if (play_result == GST_STATE_CHANGE_FAILURE) {
    std::cerr << "FLUSH-RECOVERY: failed to re-enter PLAYING" << std::endl;
    return false;
  }

  std::cout << "FLUSH-RECOVERY: state cycle posted" << std::endl;
  return true;
}

void GstVideoPlayer::StartWatchdog() {
  if (watchdog_running_.exchange(true)) return; // already running
  {
    std::lock_guard<std::mutex> lock(watchdog_mutex_);
    last_buffering_progress_time_ = std::chrono::steady_clock::now();
  }
  watchdog_thread_ = std::thread([this]() {
    constexpr auto kCheckInterval = std::chrono::seconds(1);
    // 10 s (down from 30 s): silent reconnect UX holds the last frame while
    // the Dart side spins up a replacement player, so a shorter freeze window
    // is invisible to the user and cuts recovery latency by 20 s. Empirically
    // the intermediate flush-recovery at 8 s has never resumed frames on
    // device (device logs 2026-08-03), so there is no benefit to waiting past
    // the flush-recovery window before triggering the full reconnect.
    constexpr int kFrameStallTimeoutSecs = 10;  // PLAYING but no frame this long

    // Frame-arrival baseline. Locals: touched only by this thread.
    uint64_t last_seen_frames = frames_handed_off_.load(std::memory_order_relaxed);
    auto last_frame_advance_time = std::chrono::steady_clock::now();
    // Set once the pipeline reaches PLAYING at least once. Gates the state-
    // agnostic wedge escalation below — during initial preroll the pipeline
    // is legitimately in PAUSED for many seconds, so we only start escalating
    // on non-PLAYING wedges AFTER we've seen a successful PLAYING transition.
    bool has_reached_playing = false;

    while (true) {
      std::chrono::steady_clock::time_point progress_snap;
      {
        std::unique_lock<std::mutex> lock(watchdog_mutex_);
        watchdog_cv_.wait_for(lock, kCheckInterval);
        if (!watchdog_running_.load()) break;
        progress_snap = last_buffering_progress_time_;
      }

      auto now = std::chrono::steady_clock::now();
      const int pct = last_buffering_percent_.load();

      // --- Check 1 (DIAGNOSTIC ONLY — no longer reconnects): buffer-% plateau.
      // A LIVE stream sitting at the live edge legitimately plateaus below 100%:
      // the cache-target (e.g. 30s) is unsatisfiable because no segments beyond
      // the live edge exist yet, so buffering messages just stop at ~91% with no
      // % change. That is NOT a stall while frames keep arriving. Firing a full
      // re-init here tore down healthy streams — device log 2026-06-22 showed 7
      // reconnects, ALL buffer-% fires at 91-96%, with ZERO frame-arrival stalls
      // (grep -c "no video frame" = 0). Genuine starvation that actually freezes
      // playback is caught by the frame-arrival check below (Check 2), which is
      // the ground truth. So: log and FALL THROUGH — do not break, do not
      // re-baseline Check 2 (a live-edge plateau keeps the pipeline PLAYING and
      // frames flowing, so Check 2 stays silent on its own). ---
      if (pct >= 0 && pct < 100 && play_state_requested_.load()) {
        auto stalled_secs = std::chrono::duration_cast<std::chrono::seconds>(
            now - progress_snap).count();
        if (stalled_secs >= kFrameStallTimeoutSecs) {
          std::cout << "WATCHDOG: buffer=" << pct << "% no %-change for "
                    << stalled_secs << "s (diagnostic; not fatal — frame-arrival "
                       "governs reconnect)" << std::endl;
        }
      }

      // --- Check 2: Playback frozen (pipeline is PLAYING but no video frame advances) ---
      if (!gst_.pipeline) continue;  // safe: StopWatchdog joins before teardown
      GstState state = GST_STATE_VOID_PENDING;
      gst_element_get_state(gst_.pipeline, &state, nullptr, 0);
      uint64_t frames = frames_handed_off_.load(std::memory_order_relaxed);
      if (state != GST_STATE_PLAYING) {
        // Re-baseline the frame-arrival counter unless we've previously
        // reached PLAYING AND playback is currently requested. This is
        // the state-agnostic wedge escalation: if we've been playing
        // successfully before, and the user hasn't paused, but state has
        // fallen back to something other than PLAYING for the full
        // timeout, the pipeline is wedged and must reconnect. Two
        // observed wedge classes this covers:
        //   - SoftRecover state-cycle stuck in PAUSED forever (device
        //     log 2026-08-12 16:44).
        //   - Stuck-init when Play() returned ASYNC and state stayed
        //     READY (task #42, 2026-08-12 14:23). Note: THIS class isn't
        //     covered by has_reached_playing — we've never been PLAYING
        //     in that case. Task #42 stays open for that scenario.
        // Legitimate reasons state isn't PLAYING that must NOT escalate:
        //   - Initial preroll (haven't reached PLAYING yet)
        //   - User-requested pause (play_state_requested_ == false)
        if (has_reached_playing && play_state_requested_.load()) {
          LogPlaybackHealth(state, frames, now, last_frame_advance_time);
          auto wedged_secs = std::chrono::duration_cast<std::chrono::seconds>(
              now - last_frame_advance_time).count();
          if (wedged_secs >= kFrameStallTimeoutSecs) {
            std::string msg = "Playback wedged: play requested but state=" +
                              std::string(gst_element_state_get_name(state)) +
                              " for " + std::to_string(wedged_secs) + "s";
            std::cout << "WATCHDOG: " << msg << " — notifying Flutter to re-init"
                      << std::endl;
            DumpKernelFreezeDiagnostic();
            watchdog_running_.store(false);
            // Emit unconditionally (task #61) — see the "Playback frozen"
            // branch below for full rationale. A watchdog fatal must always
            // reach soatv even if an earlier soft-error already flipped
            // error_notified_.
            last_error_ = msg;
            stream_handler_->OnNotifyError(msg);
            error_notified_.store(true);
            abr_running_.store(false);
            abr_cv_.notify_all();
            break;
          }
          continue;
        }
        // Legitimate non-PLAYING (initial preroll or user pause) — baseline.
        last_seen_frames = frames;
        last_frame_advance_time = now;
        LogPlaybackHealth(state, frames, now, last_frame_advance_time);
        continue;
      }
      // We've observed state=PLAYING at least once.
      has_reached_playing = true;

      if (frames != last_seen_frames) {
        last_seen_frames = frames;
        last_frame_advance_time = now;
        LogPlaybackHealth(state, frames, now, last_frame_advance_time);
        continue;
      }

      LogPlaybackHealth(state, frames, now, last_frame_advance_time);
      auto frozen_secs = std::chrono::duration_cast<std::chrono::seconds>(
          now - last_frame_advance_time).count();
      if (frozen_secs > 0) {
        std::cout << "WATCHDOG: PLAYING but no video frame for " << frozen_secs
                  << "s" << std::endl;
      }

      // Intermediate soft-recovery attempt (task #29). At 8 s of no-frame-
      // advance with a healthy network, try to unstick the wedged decoder
      // via TryFlushRecovery() — flush-start/flush-stop events on the video
      // sink pad, followed by a state-cycle nudge. This attempts recovery
      // WITHOUT the full pipeline teardown that OnNotifyError triggers.
      // Every teardown invokes the bcm2835_codec kernel bug that leaks
      // capture buffers; avoiding teardown preserves driver-side accounting.
      // Falls through to the 10 s OnNotifyError path if frames don't
      // resume — same total user-visible timeout as before.
      //
      // Guards:
      // - frozen_secs in [kFlushRecoveryAfterSecs, kFrameStallTimeoutSecs):
      //   only in the middle window, not right after a stall starts.
      // - Recent throughput sample >= kFlushRecoveryHealthyKbps: no point
      //   flushing if there's no data upstream to decode.
      // - >= 30 s since our last flush: don't spam if the first didn't take.
      constexpr int kFlushRecoveryAfterSecs = 8;
      constexpr int kFlushRecoveryCooldownSecs = 30;
      constexpr double kFlushRecoveryHealthyKbps = 1000.0;
      if (frozen_secs >= kFlushRecoveryAfterSecs &&
          frozen_secs < kFrameStallTimeoutSecs) {
        double recent_bps = 0.0;
        {
          std::lock_guard<std::mutex> lock(abr_mutex_);
          recent_bps = last_segment_throughput_bps_;
        }
        const double recent_kbps = recent_bps / 1000.0;
        const bool cooldown_served =
            last_flush_recovery_time_.time_since_epoch().count() == 0 ||
            now - last_flush_recovery_time_ >
                std::chrono::seconds(kFlushRecoveryCooldownSecs);
        if (recent_kbps >= kFlushRecoveryHealthyKbps && cooldown_served) {
          std::cout << "WATCHDOG: attempting flush recovery — frozen_secs="
                    << frozen_secs << " recent_throughput=" << recent_kbps
                    << "kbps" << std::endl;
          last_flush_recovery_time_ = now;
          TryFlushRecovery();
          // Don't reset last_frame_advance_time here — the seek is
          // asynchronous, so HandoffHandler bumping frames_handed_off_ is what
          // will confirm recovery on the next tick. If frames still don't
          // advance, we fall through to the 30 s reconnect path.
        }
      }

      if (frozen_secs >= kFrameStallTimeoutSecs) {
        std::string msg = "Playback frozen: no video frame for " +
                          std::to_string(frozen_secs) + "s while PLAYING";
        std::cout << "WATCHDOG: " << msg << " — notifying Flutter to re-init"
                  << std::endl;
        DumpKernelFreezeDiagnostic();
        watchdog_running_.store(false);
        // Emit unconditionally (task #61). Device log 2026-08-29 01:58 showed a
        // 4-hour zombie state: our ABR fired ABR_RESTART at 01:58:05 (setting
        // error_notified_=true), soatv did not reconnect for reasons unknown,
        // decoder wedged 8s later, and this compare_exchange_strong found the
        // flag already true — OnNotifyError was silently skipped and no
        // recovery path ever fired. A watchdog fatal must always reach soatv;
        // duplicate reconnect events are safely deduped on the Dart side via
        // _reconnectInFlight. Stop the ABR thread too so its heartbeat log
        // spam stops on a dead pipeline.
        last_error_ = msg;
        stream_handler_->OnNotifyError(msg);
        error_notified_.store(true);
        abr_running_.store(false);
        abr_cv_.notify_all();
        break;
      }
    }
  });
}

void GstVideoPlayer::StopWatchdog() {
  watchdog_running_.store(false);
  watchdog_cv_.notify_all();
  if (watchdog_thread_.joinable()) {
    watchdog_thread_.join();
  }
}

// ============================================================
// ABR engine — Continuous Playback Intelligence
//
// Measurement : pad probe on hlsdemux sink counts segment-download bursts
//               (bytes / active transfer time = true network throughput)
// Prediction  : min(fast mean, harmonic mean) with a jitter discount
// Policy      : buffer-health safety zones; down-switch immediately,
//               up-switch only with headroom + dwell time (anti-flap)
// Actuation   : hlsdemux "connection-speed" (kbps) — the demuxer picks the
//               best rendition under that figure at each segment boundary,
//               so switches never interrupt playback
// ============================================================

namespace {
// connection-speed is guint (kbps) on adaptivedemux-based hlsdemux but
// guint64 on playbin — set it through GValue so the type always matches.
void SetConnectionSpeedKbps(GstElement* element, guint64 kbps) {
  GParamSpec* pspec = g_object_class_find_property(
      G_OBJECT_GET_CLASS(element), "connection-speed");
  if (!pspec) return;
  GValue v = G_VALUE_INIT;
  g_value_init(&v, pspec->value_type);
  if (pspec->value_type == G_TYPE_UINT) {
    g_value_set_uint(&v, static_cast<guint>(kbps));
  } else if (pspec->value_type == G_TYPE_UINT64) {
    g_value_set_uint64(&v, kbps);
  } else {
    g_value_unset(&v);
    return;
  }
  g_object_set_property(G_OBJECT(element), "connection-speed", &v);
  g_value_unset(&v);
}
}  // namespace

// static
void GstVideoPlayer::DeepElementAddedHandler(GstBin* /*bin*/,
                                             GstBin* /*sub_bin*/,
                                             GstElement* element,
                                             gpointer user_data) {
  auto* self = reinterpret_cast<GstVideoPlayer*>(user_data);
  gchar* name = gst_element_get_name(element);
  if (name && g_str_has_prefix(name, "hlsdemux")) {
    // Keep a ref for the ABR engine (connection-speed actuation). The throughput
    // probe does NOT go here: hlsdemux's sink pad only carries the periodic
    // manifest/playlist (a few KB) — the media segments are fetched by the
    // demux's internal source and never cross this pad, so a probe here never
    // measures a real segment (throughput read as "unknown").
    {
      std::lock_guard<std::mutex> lock(self->abr_mutex_);
      if (self->hls_demux_) gst_object_unref(self->hls_demux_);
      self->hls_demux_ = GST_ELEMENT(gst_object_ref(element));
    }
    std::cout << "ABR: found hlsdemux element: " << name << std::endl;
    // Task #50 (2026-08-22): remove hlsdemux's default 0.8 safety multiplier
    // on connection-speed. hlsdemux picks the highest rendition whose bandwidth
    // is <= connection-speed * bitrate-limit. With the default 0.8, our
    // published connection-speed (already safety-shaved by AbrTick's 0.85
    // multiplier) gets shaved AGAIN, so a published 2124 kbps effectively
    // becomes 1699 kbps — awkwardly close to the 1800 kbps 480p rung boundary,
    // producing unpredictable rung picks on borderline decisions. Setting
    // bitrate-limit=1.0 makes our published ceiling map directly to rung
    // boundaries. Does NOT disable hlsdemux's own per-segment ABR
    // measurements — it still picks the min of (its own estimate,
    // connection-speed). This is a small alignment tuning, not a hard lock.
    GObjectClass* klass = G_OBJECT_GET_CLASS(element);
    if (g_object_class_find_property(klass, "bitrate-limit")) {
      g_object_set(element, "bitrate-limit", 1.0f, NULL);
      std::cout << "ABR: set hlsdemux bitrate-limit=1.0 (removes default 0.8 shave)"
                << std::endl;
    }
  } else if (name && (g_str_has_prefix(name, "souphttpsrc") ||
                      g_str_has_prefix(name, "curlhttpsrc"))) {
    // Segments DO cross the http source's src pad — probe here for real
    // per-segment throughput (size / download-time).
    GstPad* srcpad = gst_element_get_static_pad(element, "src");
    if (srcpad) {
      gst_pad_add_probe(srcpad, GST_PAD_PROBE_TYPE_BUFFER,
                        AbrThroughputProbe, self, NULL);
      gst_object_unref(srcpad);
      std::cout << "ABR: attached throughput probe to source: " << name
                << std::endl;
    }
  } else if (name && g_str_has_prefix(name, "multiqueue")) {
    // hlsdemux's downstream multiqueue defaults to max-size-buffers=5,
    // max-size-bytes=2MB, max-size-time=2s — a much smaller window than
    // our 30 s buffering target. On a healthy link the buffering percent
    // plateaus in the mid-teens because the multiqueue is applying
    // backpressure to hlsdemux before hlsdemux has fetched a full 30 s
    // cushion (device log 2026-07-31 09:37 saw pct stuck at ~17 for 25 s
    // on a >4 Mbps average link). Widen the limits to 60 s / 20 MB and
    // remove the buffer-count cap so time/bytes govern instead.
    g_object_set(G_OBJECT(element),
                 "max-size-buffers", (guint)0,          // unlimited count
                 "max-size-bytes",   (guint)(20 * 1024 * 1024),
                 "max-size-time",    (guint64)(60 * GST_SECOND),
                 NULL);
    std::cout << "MULTIQUEUE: widened limits on " << name
              << " (time=60s bytes=20MB buffers=unlimited)" << std::endl;
  }
  g_free(name);
}

// static
GstPadProbeReturn GstVideoPlayer::AbrThroughputProbe(GstPad* /*pad*/,
                                                     GstPadProbeInfo* info,
                                                     gpointer user_data) {
  auto* self = reinterpret_cast<GstVideoPlayer*>(user_data);
  GstBuffer* buf = GST_PAD_PROBE_INFO_BUFFER(info);
  if (!buf) return GST_PAD_PROBE_OK;

  const gsize size = gst_buffer_get_size(buf);
  const auto now = std::chrono::steady_clock::now();

  // Bump the monotonic total FIRST, outside the mutex. The preroll gate reads
  // this without contending for abr_mutex_ so it stays responsive even while
  // the streaming thread is mid-burst-close.
  self->total_bytes_fetched_.fetch_add(size, std::memory_order_relaxed);

  std::lock_guard<std::mutex> lock(self->abr_mutex_);
  // An idle gap separates one segment download from the next.
  if (self->burst_bytes_ > 0 &&
      now - self->burst_last_rx_ > std::chrono::milliseconds(250)) {
    self->CloseBurstLocked();
  }
  if (self->burst_bytes_ == 0) self->burst_start_ = now;
  self->burst_bytes_ += size;
  self->burst_last_rx_ = now;
  return GST_PAD_PROBE_OK;
}

// Caller holds abr_mutex_.
void GstVideoPlayer::CloseBurstLocked() {
  const double secs = std::chrono::duration_cast<std::chrono::duration<double>>(
                          burst_last_rx_ - burst_start_)
                          .count();
  // Skip playlist refreshes and degenerate timings — only segment-sized
  // transfers measured over a meaningful window are useful samples.
  if (burst_bytes_ >= 64 * 1024 && secs > 0.05) {
    const double throughput_bps = static_cast<double>(burst_bytes_) * 8.0 / secs;
    last_segment_bytes_ = burst_bytes_;
    last_segment_download_secs_ = secs;
    last_segment_throughput_bps_ = throughput_bps;
    abr_samples_.push_back(throughput_bps);
    while (abr_samples_.size() > 16) abr_samples_.pop_front();

    std::cout << "NETWORK: Actual measured throughput: throughput = "
              << burst_bytes_ << " bytes / " << secs << " seconds"
              << " = "
              << static_cast<int>((static_cast<double>(burst_bytes_) / secs) / 1024.0)
              << " KB/s (" << static_cast<int>(throughput_bps / 1000.0)
              << " kbps)" << std::endl;
  }
  burst_bytes_ = 0;
}

void GstVideoPlayer::StartAbrEngine() {
  if (abr_running_.exchange(true)) return;
  last_upswitch_time_ = std::chrono::steady_clock::now();
  abr_thread_ = std::thread([this]() {
    while (true) {
      {
        std::unique_lock<std::mutex> lock(abr_mutex_);
        abr_cv_.wait_for(lock, std::chrono::seconds(1));
      }
      if (!abr_running_.load()) break;
      AbrTick();
    }
  });
}

void GstVideoPlayer::StopAbrEngine() {
  abr_running_.store(false);
  abr_cv_.notify_all();
  if (abr_thread_.joinable()) {
    abr_thread_.join();
  }
}

void GstVideoPlayer::AbrTick() {
  std::deque<double> samples;
  GstElement* demux = nullptr;
  {
    std::lock_guard<std::mutex> lock(abr_mutex_);
    const auto now = std::chrono::steady_clock::now();
    if (burst_bytes_ > 0) {
      if (now - burst_last_rx_ > std::chrono::milliseconds(250)) {
        // Transfer finished and no new segment started yet.
        CloseBurstLocked();
      } else if (now - burst_start_ > std::chrono::seconds(3)) {
        // Saturated link: data flows continuously with no idle gaps, so
        // bursts never close on their own — sample the running transfer.
        CloseBurstLocked();
      }
    }
    samples = abr_samples_;
    if (hls_demux_) demux = GST_ELEMENT(gst_object_ref(hls_demux_));
  }

  if (!demux) return;
  // Require more samples before the very first publish so the initial rung
  // pick is based on a stable estimate. The prior 2-sample threshold could
  // land 240p or 360p on a link that could actually sustain 720p — then
  // every 30 s the up-switch dwell would let us climb one rung at a time,
  // producing the visible 240p→360p→480p→720p walk-through. 5 samples is
  // ~5 seconds of measurement on a live stream, still fast enough to keep
  // startup responsive. Subsequent decisions still work off the same 16-
  // sample window; only the "first estimate" branch cares about this.
  const size_t kFirstEstimateMinSamples = 5;
  if (published_kbps_ == 0 && samples.size() < kFirstEstimateMinSamples) {
    gst_object_unref(demux);
    return;
  }
  if (samples.size() < 2) {
    gst_object_unref(demux);
    return;
  }

  // --- Prediction ---
  const size_t n = samples.size();
  const size_t window = std::min<size_t>(n, 8);
  double mean = 0.0, hm_denom = 0.0;
  for (size_t i = n - window; i < n; ++i) {
    mean += samples[i];
    hm_denom += 1.0 / samples[i];
  }
  mean /= window;
  const double harmonic = window / hm_denom;  // robust vs. one fast burst
  double variance = 0.0;
  for (size_t i = n - window; i < n; ++i) {
    variance += (samples[i] - mean) * (samples[i] - mean);
  }
  const double cv = mean > 0 ? std::sqrt(variance / window) / mean : 0.0;
  const double fast = (samples[n - 1] + samples[n - 2]) / 2.0;  // recency

  // Sustainable estimate: the cautious of recent vs. long-run, discounted
  // by jitter — an erratic link earns a lower promise than a steady one.
  double predicted_bps = std::min(fast, harmonic);
  predicted_bps *= std::min(1.0, std::max(0.5, 1.0 - 0.5 * cv));

  // --- Buffer health (percent of the kBufferTargetSecs buffering target) ---
  const int pct = last_buffering_percent_.load();
  const double buffer_secs = pct < 0 ? 20.0 :
      (static_cast<double>(pct) / 100.0) * kBufferTargetSecs;

  // Scaled alongside kBufferTargetSecs 30 -> 60 (task #60). Emergency zone
  // now 0-12 s (was 0-6), cautious 12-30 (was 6-15), comfortable >= 30
  // (was >= 15). Same fractions of the target — same ABR discipline.
  double safety;
  if (buffer_secs < 12.0) {
    safety = 0.50;  // emergency: survival beats quality
  } else if (buffer_secs < 30.0) {
    safety = 0.65;  // cautious: rebuild cushion first
  } else {
    safety = 0.85;  // comfortable: ride quality close to the estimate
  }

  guint64 target_kbps =
      static_cast<guint64>(predicted_bps * safety / 1000.0);
  if (target_kbps < 100) target_kbps = 100;  // keep the lowest rung reachable

  // --- Anti-flap policy (buffer-aware) ---
  // The buffer, not the raw estimate, decides down-switches. A full cushion
  // proves the current rung is sustainable, so we ignore estimate drops until
  // the buffer actually starts draining — this kills the quality pumping seen
  // when every noisy dip at buffer=30s triggered a "bandwidth drop".
  const auto now = std::chrono::steady_clock::now();
  bool publish = false;
  const char* reason = "";
  const bool is_drop = target_kbps * 5 <= published_kbps_ * 4;  // >=20% below

  // Calm-mode global cooldown (task #46, 2026-08-17). After ANY ABR decision,
  // block the next decision for kPostAbrDecisionCooldownSecs. Buffer-emergency
  // is exempt: buffer < 12 s (task #60 rescaled from 6 s) is survival,
  // cooldown does not apply.
  const bool cooldown_served =
      last_abr_decision_time_.time_since_epoch().count() == 0 ||
      now - last_abr_decision_time_ >
          std::chrono::seconds(kPostAbrDecisionCooldownSecs);

  if (published_kbps_ == 0) {
    publish = true;
    reason = "first estimate";
    abr_drop_ticks_ = 0;
    abr_undershoot_ticks_ = 0;
    abr_trapped_ticks_ = 0;
    first_publish_time_ = now;
  } else if (buffer_secs < 12.0 && target_kbps < published_kbps_) {
    // Buffer emergency: cushion nearly gone — any reduction helps NOW. This is
    // the only instant down-switch path AND the only branch exempt from the
    // calm-mode cooldown. Deliberately does NOT stamp last_downswitch_time_:
    // survival beats the anti-thrash dwell. Threshold scaled 6 -> 12 s
    // (task #60) alongside the doubled kBufferTargetSecs.
    publish = true;
    reason = "buffer emergency";
    abr_drop_ticks_ = 0;
    abr_undershoot_ticks_ = 0;
    abr_trapped_ticks_ = 0;
  } else if (!cooldown_served) {
    // Global calm-mode cooldown active: skip all non-emergency decisions.
    // Reset counters so they don't over-accumulate during the quiet window
    // and produce a false "sustained" signal the moment cooldown clears.
    abr_drop_ticks_ = 0;
    abr_undershoot_ticks_ = 0;
    abr_trapped_ticks_ = 0;
  } else if (is_drop && buffer_secs < kHealthyBufferSecs) {
    // The buffer has meaningfully drained AND the estimate is >=20% down —
    // a real decline. Still require it to persist a few ticks so a single
    // dipped sample can't flap the rung.
    if (++abr_drop_ticks_ >= kSustainedDropTicks) {
      publish = true;
      reason = "bandwidth drop";
      abr_drop_ticks_ = 0;
      abr_undershoot_ticks_ = 0;
      abr_trapped_ticks_ = 0;
      last_downswitch_time_ = now;
    } else {
      std::cout << "ABR: drop deferred (" << abr_drop_ticks_ << "/"
                << kSustainedDropTicks << ") buffer="
                << static_cast<int>(buffer_secs) << "s cv="
                << static_cast<int>(cv * 100) << "%" << std::endl;
    }
  } else if (predicted_bps / 1000.0 < static_cast<double>(published_kbps_) * 0.7
             && buffer_secs < kHealthyBufferSecs) {
    // Sustained-undershoot down-switch (task #46 tightened). Two gates:
    //   1. Predicted must be at least 30% below published (was any-below).
    //      A 5% dip on a jittery cv=90% link is noise; only a meaningful
    //      gap justifies rebuilding.
    //   2. Buffer must be below the healthy line. If the buffer is >= 15 s
    //      the current rung is empirically sustainable regardless of what
    //      predicted says — the samples might be off but the buffer is
    //      ground truth for what's actually being served. Suppressing this
    //      here is the biggest ABR-churn reduction in the calm-mode
    //      package. BBC News 2026-08-17 log showed 18 sustained-undershoot
    //      restarts, most with buffer=25-30s: under this rule, zero would
    //      have fired.
    //
    // Compare predicted vs published (NOT target vs published). target =
    // predicted × safety_multiplier, and safety is 0.85 at healthy buffer.
    // So target is always ~15% below predicted, and target < published
    // would fire spuriously whenever predicted is anywhere below ~115% of
    // published — even when the network is actually adequate.
    //
    // Device evidence for the branch working correctly: 2026-07-31
    // 18:22:11-18:23:41 held 1091 kbps published while predicted was
    // 446/517/257/307 kbps — 41-47% below published for four ticks with
    // draining buffer. Both new gates still fire on that scenario.
    abr_drop_ticks_ = 0;
    abr_trapped_ticks_ = 0;  // predicted below published — not trapped-low
    if (++abr_undershoot_ticks_ >= kSustainedUndershootTicks) {
      publish = true;
      reason = "sustained undershoot";
      abr_undershoot_ticks_ = 0;
      last_downswitch_time_ = now;
    } else {
      std::cout << "ABR: undershoot (" << abr_undershoot_ticks_ << "/"
                << kSustainedUndershootTicks << ") predicted="
                << static_cast<int>(predicted_bps / 1000) << "kbps published="
                << published_kbps_ << "kbps buffer="
                << static_cast<int>(buffer_secs) << "s" << std::endl;
    }
  } else {
    // No drop and predicted meets or exceeds 70% of published — reset drop
    // counters and consider an up-switch or trapped-rate escape.
    abr_drop_ticks_ = 0;
    abr_undershoot_ticks_ = 0;

    // TRAPPED-RATE ESCAPE. If predicted is far above published for several
    // consecutive ticks, the currently-published rate is starving the
    // pipeline. The buffer can't reach the healthy line because published is
    // too low; the normal up-switch gate never opens. Force an up-switch
    // regardless of buffer state. Device log 2026-08-03 14:14 held 177 kbps
    // for 6+ minutes while predicted was 6-18 Mbps — this branch would have
    // published a proper rate within ~5 seconds of the network recovering.
    //
    // Publish target_kbps directly (predicted × safety) rather than jumping
    // to full predicted — we want to escape the trap, not overshoot into a
    // rung the buffer can't afford yet.
    if (predicted_bps / 1000.0 >=
        static_cast<double>(published_kbps_) * kTrappedRateMultiplier) {
      if (++abr_trapped_ticks_ >= kTrappedRateTicks) {
        publish = true;
        reason = "trapped-rate escape";
        abr_trapped_ticks_ = 0;
        last_upswitch_time_ = now;
      } else {
        std::cout << "ABR: trapped (" << abr_trapped_ticks_ << "/"
                  << kTrappedRateTicks << ") predicted="
                  << static_cast<int>(predicted_bps / 1000) << "kbps published="
                  << published_kbps_ << "kbps buffer="
                  << static_cast<int>(buffer_secs) << "s" << std::endl;
      }
    } else {
      abr_trapped_ticks_ = 0;
    }
    // Fall through to normal up-switch consideration below. If the trapped
    // escape published, its `publish=true` still holds; the normal up-switch
    // conditions won't re-fire because `publish` is already set.
    // Anti-thrash gate: after a non-emergency down-switch, hold the new rung
    // for kPostDownDwellSecs regardless of estimate before letting it climb
    // again. On device (11:20:29 drop -> 11:20:30 up-switch) the two decisions
    // fired one second apart on the same noisy sample window; the up-switch
    // often resolved to the very rung the drop had just left, producing
    // pipeline-visible churn for zero quality gain.
    const bool post_down_dwell_served =
        last_downswitch_time_.time_since_epoch().count() == 0 ||
        now - last_downswitch_time_ > std::chrono::seconds(kPostDownDwellSecs);
    // A "big jump" up-switch bypasses the up-switch dwell so we don't walk
    // through every intermediate rung (240p → 360p → 480p → 720p) on a
    // first-connect. Even here the calm-mode cooldown still applies (we
    // wouldn't have reached this branch if it hadn't been served).
    const bool big_jump_up = target_kbps >= published_kbps_ * 2;
    const bool up_dwell_served =
        now - last_upswitch_time_ > std::chrono::seconds(kUpSwitchDwellSecs);
    // Calm-mode cv gate: network too jittery to safely commit to a higher
    // rung. Applies only to up-switches; down-switches under high cv are
    // still fine (they move toward safety). BBC News 2026-08-17 10:47:05
    // fired an up-switch at cv=87% and crashed inside libc/libstdc++
    // during the in-place variant switch. cv=60% is the ceiling.
    const bool cv_stable_enough = cv <= kUpSwitchMaxCv;
    if (!publish &&
        target_kbps * 4 >= published_kbps_ * 5 &&
        buffer_secs >= kHealthyBufferSecs &&
        (up_dwell_served || big_jump_up) &&
        post_down_dwell_served &&
        cv_stable_enough) {
      publish = true;  // >=25% headroom, healthy buffer, dwell served (or big jump), stable network
      reason = big_jump_up ? "up-switch (big jump)" : "up-switch";
      last_upswitch_time_ = now;
    } else if (!publish && !cv_stable_enough &&
               target_kbps * 4 >= published_kbps_ * 5 &&
               buffer_secs >= kHealthyBufferSecs &&
               (up_dwell_served || big_jump_up) &&
               post_down_dwell_served &&
               ++abr_heartbeat_counter_ % 10 == 0) {
      std::cout << "ABR: up-switch deferred — cv=" << static_cast<int>(cv * 100)
                << "% exceeds " << static_cast<int>(kUpSwitchMaxCv * 100)
                << "% ceiling (target=" << target_kbps << "kbps published="
                << published_kbps_ << "kbps buffer="
                << static_cast<int>(buffer_secs) << "s)" << std::endl;
    }
  }

  if (publish) {
    // SCOPE 1 (task #37, 2026-08-15): route any DOWN-switch through
    // ABR_RESTART instead of in-place SetConnectionSpeedKbps. The
    // bcm2835-codec V4L2 capture pool cannot shrink in place — hlsdemux
    // switching to a lower rung triggers a v4l2convert S_FMT that the
    // driver rejects with EINVAL. GStreamer's default retry logic then
    // fires 4-8 more failed S_FMT attempts. One clean rebuild replaces
    // the retry-storm entirely.
    //
    // TASK #46 (2026-08-17) extends this to route FRAGILE up-switches
    // through ABR_RESTART too. BBC News 2026-08-17 10:47:05 crashed
    // inside libc/libstdc++ heap primitives during an in-place up-
    // switch that fired on a pipeline barely 55 s old, 4 s after a
    // BUFFER-COLLAPSE. The GStreamer inputselector re-plug during the
    // in-place variant switch corrupted an internal std::string/vector
    // whose length was later handed to memcpy as an unsigned that had
    // been computed as end - start where end < start (register showed
    // x2 = 0xffffffffffffdea2 at crash — negative-cast-to-size_t).
    //
    // A pipeline is "fragile" if any of:
    //   - Age < kFragilePipelineSecs (60 s): cold-start throughput
    //     samples still stabilising, playbin's inner state still
    //     settling from preroll.
    //   - Recent BUFFER-COLLAPSE within kPostCollapseQuietSecs (30 s):
    //     playbin's inputselectors are re-plugging, in-place variant
    //     switches during that window race the re-plug.
    //
    // For fragile up-switches, route through ABR_RESTART. For steady-
    // state up-switches (mature pipeline, no recent collapse), remain
    // in-place — they benefit from playbin's live continuity and pool
    // growth works fine.
    //
    // Down-switches always route through ABR_RESTART regardless of
    // fragility (Scope 1 rule).
    //
    // Exclude first-estimate (published_kbps_ was 0 before this tick's
    // update) because that's the initial-connect publish path, not a
    // switch. Also exclude same-rate republish.
    const bool is_down_switch =
        published_kbps_ > 0 && target_kbps < published_kbps_;
    const bool is_up_switch =
        published_kbps_ > 0 && target_kbps > published_kbps_;
    // Fragile-up-switch check (only computed for up-switches).
    bool up_is_fragile = false;
    std::string fragile_reason;
    if (is_up_switch) {
      // Pipeline age.
      if (first_publish_time_.time_since_epoch().count() > 0 &&
          now - first_publish_time_ <
              std::chrono::seconds(kFragilePipelineSecs)) {
        up_is_fragile = true;
        fragile_reason = "young pipeline";
      }
      // Recent BUFFER-COLLAPSE.
      const int64_t collapse_ns =
          last_buffer_collapse_ns_.load(std::memory_order_relaxed);
      if (collapse_ns > 0) {
        const auto collapse_tp =
            std::chrono::steady_clock::time_point(
                std::chrono::nanoseconds(collapse_ns));
        if (now - collapse_tp <
            std::chrono::seconds(kPostCollapseQuietSecs)) {
          up_is_fragile = true;
          fragile_reason = fragile_reason.empty()
              ? "recent buffer-collapse"
              : fragile_reason + " + recent buffer-collapse";
        }
      }
    }

    published_kbps_ = target_kbps;
    last_abr_decision_time_ = now;

    // VOD safety gate (task #47, 2026-08-20). ABR_RESTART emission is only
    // safe when the consumer (soatv) has a reconnect + snapshot handler.
    // live_tv_player_widget has one; movie_and_tv_shows_player_widget does
    // NOT — it just prints '❌ Movie error' and never rebuilds. If we
    // emitted ABR_RESTART on VOD, playback would die silently on any
    // down-switch or fragile up-switch. Route VOD switches in-place
    // instead: bcm2835-codec's pool-shrink crash class is far less
    // frequent on VOD (CDN-served pre-encoded segments, low cv, stable
    // buffer) than on live, and the VOD user experience of a smooth
    // in-place variant switch is what user tested and validated 2026-
    // 08-20. If we later ship ABR_RESTART handling on the VOD widget
    // (proper snapshot + position preservation), this gate can be
    // relaxed.
    const bool allow_abr_restart = is_live_;

    if (is_down_switch && allow_abr_restart) {
      std::string msg = "ABR_RESTART: down-switch to " +
                        std::to_string(target_kbps) + "kbps (" + reason +
                        ", buffer=" + std::to_string(static_cast<int>(buffer_secs)) +
                        "s)";
      std::cout << "ABR: " << reason
                << " — routing as ABR_RESTART instead of in-place S_FMT: "
                << msg << std::endl;
      bool expected = false;
      if (error_notified_.compare_exchange_strong(expected, true)) {
        last_error_ = msg;
        stream_handler_->OnNotifyError(msg);
      }
    } else if (is_up_switch && up_is_fragile && allow_abr_restart) {
      // Task #46 fragile up-switch redirect (live-only after task #47).
      std::string msg = "ABR_RESTART: up-switch to " +
                        std::to_string(target_kbps) + "kbps (" + reason +
                        ", fragile: " + fragile_reason +
                        ", buffer=" + std::to_string(static_cast<int>(buffer_secs)) +
                        "s cv=" + std::to_string(static_cast<int>(cv * 100)) + "%)";
      std::cout << "ABR: " << reason
                << " — routing as ABR_RESTART (fragile: " << fragile_reason
                << "): " << msg << std::endl;
      bool expected = false;
      if (error_notified_.compare_exchange_strong(expected, true)) {
        last_error_ = msg;
        stream_handler_->OnNotifyError(msg);
      }
    } else {
      // In-place variant switch: safe on VOD (pool-shrink crash class is
      // rare with CDN-stable segments) and used for mature-pipeline steady-
      // state up-switches on live too.
      SetConnectionSpeedKbps(demux, target_kbps);
      std::cout << "ABR: " << reason << " — connection-speed=" << target_kbps
                << "kbps (predicted=" << static_cast<int>(predicted_bps / 1000)
                << "kbps cv=" << static_cast<int>(cv * 100) << "% buffer="
                << static_cast<int>(buffer_secs) << "s safety=" << safety << ")"
                << (allow_abr_restart ? "" : " [VOD in-place]")
                << std::endl;
    }
  } else if (++abr_heartbeat_counter_ % 30 == 0) {
    std::cout << "ABR: holding " << published_kbps_ << "kbps (predicted="
              << static_cast<int>(predicted_bps / 1000) << "kbps cv="
              << static_cast<int>(cv * 100) << "% buffer="
              << static_cast<int>(buffer_secs) << "s samples=" << n << ")"
              << std::endl;
  }

  gst_object_unref(demux);
}

// static
GstBusSyncReply GstVideoPlayer::HandleGstMessage(GstBus* bus,
                                                 GstMessage* message,
                                                 gpointer user_data) {
  // Capture every bus message into the ring buffer for post-collapse dumps.
  // Skip a couple of very-high-frequency types to keep the buffer meaningful —
  // BUFFERING fires many times per second on some streams and would flood the
  // ring, and QOS is nearly-per-frame. Everything else is worth remembering.
  const GstMessageType msg_type = GST_MESSAGE_TYPE(message);
  if (msg_type != GST_MESSAGE_BUFFERING && msg_type != GST_MESSAGE_QOS) {
    auto* outer_self = reinterpret_cast<GstVideoPlayer*>(user_data);
    const gchar* type_name = gst_message_type_get_name(msg_type);
    const gchar* src_name_c = GST_MESSAGE_SRC(message)
        ? GST_OBJECT_NAME(GST_MESSAGE_SRC(message))
        : "(none)";
    std::string extra;
    if (msg_type == GST_MESSAGE_ELEMENT) {
      const GstStructure* s = gst_message_get_structure(message);
      if (s) {
        const gchar* struct_name = gst_structure_get_name(s);
        if (struct_name) extra = std::string("name=") + struct_name;
      }
    } else if (msg_type == GST_MESSAGE_STATE_CHANGED &&
               GST_MESSAGE_SRC(message) == GST_OBJECT(outer_self->gst_.pipeline)) {
      GstState old_s, new_s, pending_s;
      gst_message_parse_state_changed(message, &old_s, &new_s, &pending_s);
      extra = std::string(gst_element_state_get_name(old_s)) + "->" +
              gst_element_state_get_name(new_s);
    }
    outer_self->PushBusMsgRing(type_name ? type_name : "?",
                                src_name_c ? src_name_c : "?", extra);
  }

  switch (msg_type) {
    case GST_MESSAGE_EOS: {
      auto* self = reinterpret_cast<GstVideoPlayer*>(user_data);
      // A LIVE stream has no end and must never "complete". souphttpsrc can
      // emit a spurious EOS on a live source (live edge, or a closed
      // keep-alive=FALSE connection) where curlhttpsrc did not. Marking
      // is_completed_ then makes GetCurrentPosition either SetSeek(0) (auto-
      // repeat → "Failed to seek" on live) or fire a 'completed' event the app
      // restarts from — BOTH replay the buffered window, and a repeating EOS
      // turns that into the "same buffer loops" bug (which appeared anywhere
      // from 10 to 95 min into playback).
      //
      // Gate on is_live_ (set in Preroll from NO_PREROLL) — the authoritative
      // live signal. The earlier duration>0 check was INERT because a live HLS
      // playlist reports a small sliding-window duration that looks like VOD.
      // On live: ignore EOS entirely; a genuine stop is recovered by the
      // frame-arrival watchdog. On VOD: complete normally.
      if (self->is_live_) {
        std::cout << "EOS on LIVE stream — ignoring (no completion/seek-0; "
                     "frame-arrival watchdog handles a real stop)" << std::endl;
        break;
      }
      std::lock_guard<std::mutex> lock(self->mutex_event_completed_);
      self->is_completed_ = true;
      break;
    }
    case GST_MESSAGE_BUFFERING: {
      auto* self = reinterpret_cast<GstVideoPlayer*>(user_data);
      gint percent;
      gst_message_parse_buffering(message, &percent);

      if (percent != self->last_buffering_percent_.load()) {
        self->last_buffering_percent_.store(percent);

        // Reset watchdog stall timer on any percent change (= buffer progress).
        {
          std::lock_guard<std::mutex> lock(self->watchdog_mutex_);
          self->last_buffering_progress_time_ = std::chrono::steady_clock::now();
        }
        self->watchdog_cv_.notify_one();

        // Notify buffering update to Flutter (UI only — never drives pipeline
        // state).
        self->stream_handler_->OnNotifyBufferingUpdate(percent);

        // NOTE: We deliberately do NOT pause/resume the pipeline on buffering
        // percentage. For a LIVE stream that approach is unsafe: when the
        // download starves the percent never climbs back to 100, so the resume
        // condition is unreachable and the pipeline wedges in PAUSED forever.
        // Two unsynchronized detached set_state() threads also raced to a
        // nondeterministic final state. Recovery from a real buffer stall is
        // handled by the watchdog Check 1 below (it fires OnNotifyError so
        // Flutter can re-authenticate and rebuild the pipeline).

        gint64 position = 0;
        const bool has_position = gst_element_query_position(
            self->gst_.pipeline, GST_FORMAT_TIME, &position);
        const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now() - self->buffering_log_start_time_);

        // Extended buffering stats: download rate and ETA (mpv-style).
        GstBufferingMode mode;
        gint avg_in_bps, avg_out_bps;
        gint64 buffering_left_ms;
        gst_message_parse_buffering_stats(message, &mode, &avg_in_bps,
                                          &avg_out_bps, &buffering_left_ms);

        std::cout << "BUFFERING: " << percent << "% elapsed="
                  << elapsed.count() << "s";
        if (has_position) {
          std::cout << " pos=" << (position / GST_SECOND) << "s";
        }
        if (avg_in_bps > 0) {
          std::cout << " dl=" << (avg_in_bps / 1024) << "KB/s";
        }
        if (buffering_left_ms > 0) {
          std::cout << " eta=" << (buffering_left_ms / 1000) << "s";
        }
        std::cout << " cache-target=" << static_cast<int>(kBufferTargetSecs)
                  << "s/10MiB from "
                  << GST_MESSAGE_SRC_NAME(message) << std::endl;
      }
      break;
    }
    case GST_MESSAGE_STATE_CHANGED: {
      // Log pipeline-level state transitions only (element noise is too verbose).
      auto* self = reinterpret_cast<GstVideoPlayer*>(user_data);
      if (GST_MESSAGE_SRC(message) == GST_OBJECT(self->gst_.pipeline)) {
        GstState old_s, new_s, pending;
        gst_message_parse_state_changed(message, &old_s, &new_s, &pending);
        std::cout << "PIPELINE-STATE: "
                  << gst_element_state_get_name(old_s) << " -> "
                  << gst_element_state_get_name(new_s);
        if (pending != GST_STATE_VOID_PENDING) {
          std::cout << " (pending " << gst_element_state_get_name(pending) << ")";
        }
        std::cout << std::endl;
      }
      break;
    }
    case GST_MESSAGE_WARNING: {
      gchar* debug;
      GError* error;
      gst_message_parse_warning(message, &error, &debug);
      std::string error_msg = error->message ? error->message : "unknown warning";
      std::cout << "WARNING from " << GST_OBJECT_NAME(message->src)
                << ": " << error_msg;
      if (debug && debug[0]) std::cout << "\n  debug: " << debug;
      std::cout << std::endl;
      if (IsHttpUnavailable(error, debug)) {
        std::cout << "WARNING: treating as entitlement lapse on live stream"
                  << std::endl;
      }
      g_free(debug);
      g_error_free(error);
      break;
    }
    case GST_MESSAGE_ERROR: {
      auto* self = reinterpret_cast<GstVideoPlayer*>(user_data);
      gchar* debug;
      GError* error;
      gst_message_parse_error(message, &error, &debug);
      std::string error_msg = error->message ? error->message : "unknown error";
      std::cout << "ERROR from " << GST_OBJECT_NAME(message->src)
                << ": " << error_msg;
      if (debug && debug[0]) std::cout << "\n  debug: " << debug;
      std::cout << std::endl;
      if (IsHttpUnavailable(error, debug)) {
        error_msg = std::string(kStreamUnavailablePrefix) + error_msg;
      } else if (IsChannelUnavailable(message, error, debug)) {
        error_msg = std::string(kChannelUnavailablePrefix) + error_msg;
      }
      g_free(debug);
      g_error_free(error);
      // Stop the watchdog then fire OnNotifyError exactly once, even if the
      // watchdog and GST_MESSAGE_ERROR race at the same 30s boundary.
      self->watchdog_running_.store(false);
      self->watchdog_cv_.notify_all();
      bool expected = false;
      if (self->error_notified_.compare_exchange_strong(expected, true)) {
        self->last_error_ = error_msg;
        self->stream_handler_->OnNotifyError(error_msg);
      }
      break;
    }
    default:
      break;
  }
  return GST_BUS_DROP;
}
