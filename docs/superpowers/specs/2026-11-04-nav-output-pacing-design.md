# Navigation output pacing

User approved option 1: smooth output pacing and add observations. No additional
prefill, PCM removal, resampling, timestamp changes, PLC changes or music policy changes.

For navigation type 101, smooth post-pop ring duration with an 80 ms time-weighted
filter. Target 50 ms; excess microseconds map to speed-up ppm, capped at 50,000
(5%). Slew speed-up by at most 100,000 ppm/s; cancel boost below 15 ms actual
remaining data. Retain fractional microseconds in absolute deadlines. Rebase a
missed deadline to now + one interval rather than emitting an unbounded burst.
Check stop between sleeps. Native type 100 scheduling remains intact.

Use existing validated output hooks; add a guarded relocation at the native
post-send RingBuffer_size call. Its assembly adapter resumes the native path for
unsupported formats/allocation failure, and returns to the native loop after the
new navigation scheduler handles the wait. No mutation of vendor archive input.

Observations use the existing low-priority reporter and output epoch: ring latest,
filtered and min/max duration; boost current/max; interval; deadline rebases and
lateness; send entry/return gap maxima and gap buckets with seq/ts/time; resumed
empty episodes versus initial/terminal/pending empty periods. Running means a
live output thread, not proof that PCM contains audible speech. Log high-water
pressure, retain native push-failure evidence. No logging/allocation per packet.

Alternative considered: output prefill (deferred due added latency). Existing hard
50 ms catch-up is replaced only for supported navigation. Near-full queues retain
the same bounded 5% pace and visible pressure/failure counters, not silent dropping.


## Log15/log16 correction (approved to implement)

Both builds AC8A show output-ring overflow: 23 / 34 failed 40 ms pushes,
respectively. The v1 no-catchup rebase adds a full interval after already late
sends. Merely rebasing to now still loses phase repeatedly; keeping unlimited
phase debt would create a burst. Use a separate media phase with 40 ms debt cap,
minimum send-entry spacing 2/3 of nominal (1.5x transient catch-up), and preserve
the 5% gradual queue boost. Empty/resume resets phase. No added prefill or changes
to PCM, RTP, PLC, type100, or teardown. Record retained debt/clamps and catch-up
spacing limits using the existing periodic reporter.

A 1.25x catch-up ceiling was insufficient under the deterministic recurrent-stall
workload with 1 ms wake rounding; 1.5x passed for both native payload sizes. This
is a device-test candidate, not proof of audible recovery. Validate pressure,
push_drop_pcm, send spacing and speech quality on the same head unit.
