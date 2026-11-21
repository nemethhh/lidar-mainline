// SPDX-License-Identifier: GPL-2.0-only
/*
 * lidar-aec: the Echo's microphones with the speaker's echo removed.
 *
 * Captures the microphone array and the playback reference (the AFE's
 * loopback of what goes to the DAC) at 48 kHz, both from the same clock,
 * mixes mics 1-6 to mono (0.5 each, like the lidar_mics device) and runs
 * them through WebRTC's audio processing: echo cancellation (AEC3), noise
 * suppression and a high-pass filter. Writes 16 bit mono at 48 kHz to
 * stdout until stdout or a device goes away.
 *
 * Usage: lidar-aec [-m mic-device] [-r reference-device] [-n 0-3] [-g] [-v]
 *   -n  noise suppression level (0 off, 1 low, 2 moderate, 3 high),
 *       default 2
 *   -g  adaptive digital gain (AGC2)
 *   -v  levels and echo canceller statistics on stderr once a second
 */
#include <alsa/asoundlib.h>
#include <api/audio/audio_processing.h>

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <unistd.h>
#include <vector>

namespace {

constexpr int kRate = 48000;
constexpr snd_pcm_uframes_t kFrames = kRate / 100;	// 10 ms, what APM takes
constexpr int kMicChannels = 8;		// TDM capture: mics 1-7 and line-in
constexpr int kMixedMics = 6;		// mic 7 picks up interference
constexpr int kRefChannels = 2;
/*
 * The echo reaches the mics only 2.4 ms after the reference, less than
 * AEC3's delay estimator expects: without a margin it locks onto a wrong
 * delay and its linear filter never converges (measured: 5-13 dB of
 * suppression instead of ~40 dB). Running the mic 40 ms behind fixes that.
 */
constexpr int kMicDelayPeriods = 4;

snd_pcm_t *open_capture(const char *name, snd_pcm_format_t format,
			unsigned int channels)
{
	snd_pcm_t *pcm;
	int err = snd_pcm_open(&pcm, name, SND_PCM_STREAM_CAPTURE, 0);

	if (err < 0) {
		fprintf(stderr, "lidar-aec: %s: %s\n", name, snd_strerror(err));
		return nullptr;
	}
	/* 4 periods of 10 ms; resampling is never wanted here */
	err = snd_pcm_set_params(pcm, format, SND_PCM_ACCESS_RW_INTERLEAVED,
				 channels, kRate, 0, 40000);
	if (err < 0) {
		fprintf(stderr, "lidar-aec: %s: %s\n", name, snd_strerror(err));
		snd_pcm_close(pcm);
		return nullptr;
	}
	return pcm;
}

enum class Read { ok, xrun, gone };

/* Reads exactly one 10 ms period. */
Read read_period(snd_pcm_t *pcm, void *buf, size_t frame_bytes)
{
	auto *p = static_cast<char *>(buf);
	snd_pcm_uframes_t left = kFrames;

	while (left) {
		snd_pcm_sframes_t n = snd_pcm_readi(pcm, p, left);

		if (n == -EAGAIN)
			continue;
		if (n == -EPIPE || n == -ESTRPIPE)
			return Read::xrun;
		if (n < 0) {
			fprintf(stderr, "lidar-aec: capture: %s\n", snd_strerror(n));
			return Read::gone;
		}
		p += n * frame_bytes;
		left -= n;
	}
	return Read::ok;
}

/*
 * (Re)starts both streams at once. The start threshold is the whole buffer
 * and reads are one period, so neither stream starts on its own.
 */
bool start(snd_pcm_t *mic, snd_pcm_t *ref, bool linked)
{
	if (snd_pcm_start(mic) < 0)
		return false;
	return linked || snd_pcm_start(ref) >= 0;
}

/* After an xrun on either stream: stop both and start them together again. */
bool restart(snd_pcm_t *mic, snd_pcm_t *ref, bool linked)
{
	fprintf(stderr, "lidar-aec: overrun, restarting capture\n");
	snd_pcm_drop(mic);
	if (!linked)
		snd_pcm_drop(ref);
	if (snd_pcm_prepare(mic) < 0 || (!linked && snd_pcm_prepare(ref) < 0))
		return false;
	return start(mic, ref, linked);
}

bool write_all(const int16_t *buf, size_t n)
{
	auto *p = reinterpret_cast<const char *>(buf);
	size_t left = n * sizeof(*buf);

	while (left) {
		ssize_t w = write(STDOUT_FILENO, p, left);

		if (w < 0) {
			if (errno == EINTR)
				continue;
			return false;
		}
		p += w;
		left -= w;
	}
	return true;
}

double dbfs(double power, double samples)
{
	return 10 * std::log10(power / samples + 1e-12);
}

}  // namespace

int main(int argc, char **argv)
{
	const char *mic_dev = "hw:amazonlidar,1";
	const char *ref_dev = "hw:amazonlidar,2";
	int ns_level = 2;
	bool agc = false, verbose = false;
	int opt;

	while ((opt = getopt(argc, argv, "m:r:n:gv")) != -1) {
		switch (opt) {
		case 'm': mic_dev = optarg; break;
		case 'r': ref_dev = optarg; break;
		case 'n': ns_level = atoi(optarg); break;
		case 'g': agc = true; break;
		case 'v': verbose = true; break;
		default:
			fprintf(stderr, "usage: lidar-aec [-m mic] [-r reference] [-n 0-3] [-g] [-v]\n");
			return 2;
		}
	}
	if (ns_level < 0 || ns_level > 3) {
		fprintf(stderr, "lidar-aec: -n takes 0-3\n");
		return 2;
	}
	signal(SIGPIPE, SIG_IGN);	/* a closed stdout ends the loop */

	snd_pcm_t *mic = open_capture(mic_dev, SND_PCM_FORMAT_S32_LE, kMicChannels);
	snd_pcm_t *ref = open_capture(ref_dev, SND_PCM_FORMAT_S16_LE, kRefChannels);

	if (!mic || !ref)
		return 1;
	/* Start both on the same trigger, so the echo delay stays constant. */
	bool linked = snd_pcm_link(mic, ref) == 0;

	if (!linked)
		fprintf(stderr, "lidar-aec: could not link the devices, starting separately\n");

	webrtc::AudioProcessing::Config config;

	config.echo_canceller.enabled = true;
	config.high_pass_filter.enabled = true;
	config.noise_suppression.enabled = ns_level > 0;
	if (ns_level > 0)
		config.noise_suppression.level =
			static_cast<webrtc::AudioProcessing::Config::NoiseSuppression::Level>(ns_level - 1);
	config.gain_controller2.enabled = agc;
	config.gain_controller2.adaptive_digital.enabled = agc;

	rtc::scoped_refptr<webrtc::AudioProcessing> apm =
		webrtc::AudioProcessingBuilder().Create();
	apm->ApplyConfig(config);

	/*
	 * The speaker plays the mono mix of the stream (DAC Mono Mix), so that
	 * is the reference: a stereo reference makes AEC3 model two echo paths
	 * that are really one.
	 */
	const webrtc::StreamConfig mic_cfg(kRate, 1), ref_cfg(kRate, 1);
	std::vector<int32_t> mic_buf(kFrames * kMicChannels);
	std::vector<int16_t> ref_buf(kFrames * kRefChannels), ref_mono(kFrames), out(kFrames);
	std::vector<float> mono(kFrames);
	std::vector<float> delay_line(kMicDelayPeriods * kFrames);
	float *mono_ch[] = { mono.data() };

	delay_line.reserve((kMicDelayPeriods + 1) * kFrames);
	double ref_pow = 0, mic_pow = 0, out_pow = 0;

	if (!start(mic, ref, linked))
		return 1;
	for (unsigned long period = 1;; period++) {
		Read r = read_period(ref, ref_buf.data(), kRefChannels * sizeof(int16_t));

		if (r == Read::ok)
			r = read_period(mic, mic_buf.data(), kMicChannels * sizeof(int32_t));
		if (r == Read::xrun) {
			/* both restart aligned; the delay line held the old stream */
			std::fill(delay_line.begin(), delay_line.end(), 0.0f);
			if (!restart(mic, ref, linked))
				return 1;
			continue;
		}
		if (r == Read::gone)
			return 1;

		for (snd_pcm_uframes_t i = 0; i < kFrames; i++) {
			int v = (ref_buf[2 * i] + ref_buf[2 * i + 1]) / 2;

			ref_mono[i] = static_cast<int16_t>(v);
			ref_pow += static_cast<double>(v) * v / (32768.0 * 32768.0);
		}
		apm->ProcessReverseStream(ref_mono.data(), ref_cfg, ref_cfg,
					  ref_mono.data());

		for (snd_pcm_uframes_t i = 0; i < kFrames; i++) {
			const int32_t *f = &mic_buf[i * kMicChannels];
			float sum = 0;

			for (int c = 0; c < kMixedMics; c++)
				sum += f[c] / 2147483648.0f;
			delay_line.push_back(std::clamp(sum * 0.5f, -1.0f, 1.0f));
		}
		std::copy_n(delay_line.begin(), kFrames, mono.begin());
		delay_line.erase(delay_line.begin(), delay_line.begin() + kFrames);
		for (float v : mono)
			mic_pow += v * v;

		apm->ProcessStream(mono_ch, mic_cfg, mic_cfg, mono_ch);

		for (snd_pcm_uframes_t i = 0; i < kFrames; i++) {
			out[i] = static_cast<int16_t>(std::clamp(mono[i], -1.0f, 1.0f) * 32767.0f);
			out_pow += mono[i] * mono[i];
		}
		if (!write_all(out.data(), out.size()))
			break;

		if (verbose && period % 100 == 0) {
			webrtc::AudioProcessingStats s = apm->GetStatistics();
			const double n = 100.0 * kFrames;

			fprintf(stderr, "%lus ref %.1f mic %.1f out %.1f dBFS, delay %d ms, ERL %.1f dB, ERLE %.1f dB\n",
				period / 100, dbfs(ref_pow, n), dbfs(mic_pow, n),
				dbfs(out_pow, n), s.delay_ms.value_or(-1),
				s.echo_return_loss.value_or(0),
				s.echo_return_loss_enhancement.value_or(0));
			ref_pow = mic_pow = out_pow = 0;
		}
	}

	snd_pcm_close(mic);
	snd_pcm_close(ref);
	return 0;
}
