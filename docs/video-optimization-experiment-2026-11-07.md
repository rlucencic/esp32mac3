# Mac3 LC II / LC III video conversion

Date: 2026-11-07. Reference: esp32mac3 `6f7e3fc`, esp32emu `7d6db01`.
Follows the [Mac2 LC converter trial](../../esp32mac2/docs/lc-video-experiment-2026-11-07.md).
Tab5 ESP32-P4 at 360 MHz, ESP-IDF 5.5.6 / GCC 14.2.0, converter at -O2,
32 MB / 200 MHz PSRAM, instructions and rodata in PSRAM.

Decision: retain paired indexed reads and masked RGB555 conversion for
lower converter cost and higher dirty-screen observation rates, with
observed adaptive CPU-speed tradeoffs. Indexed kernels cost 23–29% less,
RGB555 about 19% less. Primary full-firmware signature rates rise
9.35%/7.67%, while separate clock windows show mean adaptive CPU speed
0.58%/1.77% lower. These are graphics results, not panel-FPS or general
application/CPU-speed gains.

## Mac3-specific behavior

Both models draw through `video_render_line`. The LC II's V8 has 1024-byte
indexed rows; Sonora on the LC III packs rows to width × depth / 8.
RGB555 uses two bytes per pixel. Mac3 already checks the complete row
against VRAM bounds and blanks it if any of that row is outside VRAM.
That differs from Mac2's per-pixel RGB555 fallback. Sonora also blanks
for its mode register's blank bit or a mode/monitor mismatch. Depth codes
above four are rejected. These rules remain unchanged in every candidate.

Test paired native-word reads for 8-bit pixels and the equivalent masked
RGB555 expression from Mac2, separately and together. Do not transfer
Mac2's extra row-bounds branch: Mac3 already has one. Supported widths
are even (512 and 640), and source/output retain uint16_t alignment.
No table, allocation or ISA-specific assembly is introduced.

## Native correctness

Reference, paired-only, masked-RGB555-only and combined snapshots compile
under ASan plus UBSan at -O2. All 1,246,912 complete row/output-canary
comparisons per candidate pass. Coverage includes both machines, both
monitor sizes, modes 0..15, invalid/valid rows, two source/output alignment
offsets, null VRAM, boundaries around row starts/ends, all 256 display-mode
register values, and every 16-bit input word. The latter enumerates all
8-bit index pairs and every RGB555 value, including the unused top bit,
in both pixel positions. All blanking and whole-row bounds behavior must
match the original converter.

Four diskless native LC II/III ROM boots (both models and monitors,
4 MB / 1,000 frames) produce byte-identical final pixels between reference
and combined candidate. Test source snapshots match the production converter.

The full P4 converter grows from 608 to 618 text bytes (+10), outside
IRAM. The smaller isolated test fixture grows six bytes; use the real
firmware's ten-byte cost for this integration.

## Hardware method

Back up the entire 16 MB Tab5 flash before replacing the Mac2 partition
layout and ROMs with Mac3's. Backups and private ROMs stay outside git.
Test apps use an isolated disk-selection namespace; original guest disks
are never mounted. The production source will retain its normal namespace.

Use 4 MB, injected changing VRAM fills and masked guest interrupts.
The injected program disables the 68030 MMU before using physical video
addresses; this exercises graphics and physical writes, not Mac OS MMU
performance. LC II writes 512 KB per fill; LC III writes 1 MB. Each fresh
boot gets the same wait, initial code and registers. Capture frame and
screen-signature counters at three host-monotonic timestamps about 30
seconds apart for LC II 8-bit and LC III RGB555. The other two model/depth
combinations receive profile and pixel checks without throughput windows.

After each workload, request three 32,000-sample core-0 profiles at
20 kHz (50 microseconds), resolving against the exact flashed ELF. A CPU status row
read just before profiles records the adaptive controller's current speed;
it is a coarse observation, not a timed CPU benchmark. Mac3 adjusts CPU
speed to maintain the frame cap, so near-60 FPS does not establish faster
CPU execution. The screen signature counts dirty-screen observations,
not completed panel frames or smoothness.

For pixel comparison, use a deterministic single fill followed by a guest
self-loop and capture the entire 1280×720 LCD buffer. This checks converter,
scaling, blending and output placement, not tearing or physical panel timing.

## Results

Four source variants run in one minimal P4 firmware. VRAM is in PSRAM;
palette/output are internal RAM. Each timed pass draws four frames, with a
volatile output checksum. Seven ABBA repetitions compare reference,
candidate, candidate, reference. Interrupts remain enabled; no guest,
LCD, network or audio tasks run.

Median combined converter cycle-cost changes:

| Model / source | 1 bit | 2 bits | 4 bits | 8 bits | RGB555 |
| --- | ---: | ---: | ---: | ---: | ---: |
| LC II / 640×480 | −0.000% | +0.011% | +0.046% | −23.340% | −18.757% |
| LC II / 512×384 | +0.010% | +0.008% | −0.008% | −23.170% | −18.923% |
| LC III / 640×480 | +0.011% | +0.016% | +0.008% | −23.812% | −18.821% |
| LC III / 512×384 | +0.036% | +0.027% | −0.001% | −29.139% | −18.870% |

Paired-only and RGB555-only variants show essentially the corresponding
single-path saving; combining them preserves both. Combined other-depth
controls stay within 0.05%; these tiny differences are not claimed as
gains or regressions. The LC II 640×480 RGB555 case deliberately includes
rows outside its 512 KB VRAM, which still blank as complete rows. Its ROM
only exposes RGB555 at 512×384. These results describe converter cost,
not whole-emulator or panel FPS.

Baseline exclusive converter residency is 14.1–14.5% at LC II 8-bit,
13.0–14.0% at LC II RGB555, 18.7–18.8% at LC III 8-bit, and 26.9–27.2%
at LC III RGB555. One LC III 8-bit capture returned only 8,000 samples and
core-1 functions, indicating the console's default request rather than
the requested profile. It is discarded; that range uses the remaining
two valid captures. Other ranges use three. Candidate captures validate
the 32,000-sample count and retry incomplete requests.

Candidate test and normal production P4 builds pass. Shared runtime source
is unchanged; no shared 24-build matrix is needed for this local Mac3 change.
Mac3 is P4-only.

Full-firmware exclusive converter sample shares:

| Workload | Reference | Combined |
| --- | --- | --- |
| LC II 8-bit, 640×480 | 14.1–14.5% | 11.1–11.2% |
| LC II RGB555, 512×384 | 13.0–14.0% | 11.9–12.2% |
| LC III 8-bit, 640×480 | 18.7–18.8% | 15.0–15.3% |
| LC III RGB555, 640×480 | 26.9–27.2% | 23.8–24.6% |

These are sample shares in separate captures, not per-call timings.
The four complete 1280×720 reference/candidate LCD-buffer pairs are
byte-identical. Native ROM-boot pixels also match on all four model/monitor
combinations. No representative OS/application gain is established.

Profiler-free primary workloads, pooled over about 60 seconds per build:

| Workload | Reference/candidate emulated FPS | Reference/candidate signature increments/s | Signature change |
| --- | --- | --- | ---: |
| LC II 8-bit | 60.146 / 60.139 | 7.437 / 8.133 | +9.354% |
| LC III RGB555 | 60.155 / 60.151 | 11.485 / 12.366 | +7.674% |

Frame-rate differences are about 0.01%, near the controller's cap; they
are not CPU-performance comparisons. These are one fresh run per build
and workload, without broad statistical confidence.

Pre-profile CPU status readings (coarse single observations):

| Workload | Reference MHz | Candidate MHz |
| --- | ---: | ---: |
| LC II indexed | 11.7 | 11.2 |
| LC II RGB555 | 11.3 | 11.4 |
| LC III indexed | 5.7 | 5.9 |
| LC III RGB555 | 5.6 | 5.3 |

These observations vary with the adaptive controller. Lower candidate
readings in the two primary workloads warrant additional matched CPU-speed
sampling windows before making the retention decision.

To follow up those coarse readings, repeat fresh boots of the primary
workloads in reference/candidate order. For each, collect fifteen CPU
status samples at two-second intervals across 30 seconds, plus start/end
frame/signature counters. HTTP responses are reduced to CPU fields;
private board details are discarded. No PC profiling is active.

| Workload | Reference mean MHz (range) | Candidate mean MHz (range) | Mean change | Reference/candidate signature increments/s |
| --- | --- | --- | ---: | --- |
| LC II indexed | 11.520 (10.9–12.1) | 11.453 (11.1–12.1) | −0.579% | 7.431 / 8.122 |
| LC III RGB555 | 5.647 (5.4–5.9) | 5.547 (5.4–5.7) | −1.771% | 11.490 / 12.362 |

The CPU status rounds individual readings to 0.1 MHz. Fifteen correlated
samples in one window per build are not a broad confidence interval;
report the observed means and ranges without claiming an established
causal CPU regression or improvement. Signature observations rise 9.30%
and 7.59% in these separate windows, consistent with the longer tests.
This follow-up confirms that the frame cap alone would hide a CPU-speed
tradeoff. Retain the graphics change with that limitation documented.

Production source/build use the normal disk-selection namespace and have
no temporary probes. The board is left on the uninstrumented candidate
test app, LC III 4 MB / 13-inch monitor, at its ordinary ROM disk-wait
screen. No synthetic guest code is running and no guest disks are mounted.
The original Mac2 flash backup remains outside git for restoration if needed.

The profiling interval in earlier Mac2/shared reports was corrected from
25 to 50 microseconds: `PROFILE_HZ` is 20,000; the Python profile call's
`25` argument is the number of output symbols. Sample counts, measured
sample shares and the underlying conclusions are unchanged. The audited
earlier repeat-capture logs all have the requested 32,000 samples.

## Next target

Move to an Amiga bitplane/blitter baseline, profiling the guest CPU and
presenter before choosing a kernel. Mac3 CPU/MMU work remains a separate
candidate that needs representative software with the MMU enabled.
Real software on copied disks, physical input and audio-by-ear coverage
remain open.

Scratch fixtures, full logs, firmware and backups stay outside the repos
under `/tmp/mac3-video-2026-11-07`.
