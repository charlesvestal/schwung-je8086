/*
 * jp8000_live — headless real-time JP-8000 for an embedded Linux box.
 *
 * Drives jeLib::Device in real time and writes raw S16LE stereo to stdout, so
 * the audio backend is whatever you pipe into:
 *
 *   jp8000_live <rom_dir> | aplay -f S16_LE -c 2 -r 88200 -D plughw:0,0
 *
 * That deliberately avoids linking ALSA: nothing new to cross-compile, and it
 * works the same against aplay, pipewire-cat or a file.
 *
 * Use plughw, not hw: the engine runs at 88.2 kHz and most codecs (the Pi's
 * headphone DAC included) do not, so ALSA's plug layer does the conversion.
 *
 * MIDI in is an optional raw ALSA device (/dev/snd/midiC1D0), read non-blocking
 * and parsed with synthLib's own parser. Without one it still runs, which is
 * useful for a first listen on a box with no controller attached.
 *
 * jeLib::Device::getSamplerate() is a hard 88200 and synthLib::Device does not
 * resample -- that lives in the plugin wrapper -- so process() always renders at
 * 88.2 kHz whatever hostSamplerate says. Emitting these frames as 44.1 kHz plays
 * everything an octave low; the rate below has to match the device.
 *
 * Environment:
 *   JE_PIPELINE="1,2,3"        parallel ASIC pipeline (see jePipeline.h)
 *   JE_PIPELINE_CORES="0,1,2,3"
 *   JE_PIPELINE_LATENCY=64     fixed pipeline delay in samples (0.7 ms). Small
 *                              values starve throughput now that stages sleep
 *                              rather than spin: below ~32 a handoff is shorter
 *                              than the sleep that waits for it.
 *   JP_BLOCK=128               frames per process() call
 *   JP_PERIODS=3               ALSA periods; 3 x 884 = 30 ms on a Pi's own DAC
 *
 * LATENCY: on the Raspberry Pi's built-in headphone output the driver fixes the
 * period at 884 frames (~10 ms at 88.2 kHz) -- measured identical through
 * plughw and hw, and unchanged by asking for anything smaller -- so ~30 ms is
 * the floor there and it is the DAC's, not the engine's. The engine renders a
 * 128-frame block in 0.61 ms against a 1.45 ms budget. Use a USB audio
 * interface for low latency.
 */

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <string>
#include <vector>
#include <algorithm>

#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <dlfcn.h>

#include "baseLib/os.h"
#include "jeLib/device.h"
#include "jeLib/je8086.h"
#include "jeLib/romloader.h"
#include "synthLib/midiBufferParser.h"
#include "synthLib/midiTypes.h"


/* Direct ALSA output, dlopen'd.
 *
 * Piping to aplay does not work for live playing: a Unix pipe holds ~64 KB,
 * which is 185 ms of audio here, so the renderer sprints to fill it -- pegging
 * every core -- and then blocks. While it sprints, aplay has to be scheduled
 * promptly on a saturated machine to keep the card fed, and when it is not the
 * ALSA buffer drains. Measured: 12-14 underruns per 15 s at a 20 ms buffer, and
 * it made no difference whether the pipeline stages spun or slept.
 *
 * Writing to the card ourselves fixes the cause rather than the symptom:
 * snd_pcm_writei blocks until the card has room, so the card's own clock paces
 * the renderer and it can never run more than a buffer ahead.
 *
 * dlopen rather than linking: the cross-build image has no ALSA headers, the C
 * ABI here is stable, and this keeps the toolchain untouched. */
namespace alsa
{
	using snd_pcm_t = void;
	using sframes = long;
	using uframes = unsigned long;

	constexpr int StreamPlayback = 0;
	constexpr int FormatS16LE = 2;
	constexpr int AccessRwInterleaved = 3;

	int (*open_)(snd_pcm_t**, const char*, int, int) = nullptr;
	int (*set_params)(snd_pcm_t*, int, int, unsigned, unsigned, int, unsigned) = nullptr;
	/* Explicit hw_params. snd_pcm_set_params only takes a total latency and picks
	 * the period itself: on the Pi's bcm2835 it chose 884 frames and, for any
	 * latency under ~20 ms, a buffer of exactly ONE period -- no double buffering,
	 * so the card starved every time we were writing. That produced an identical
	 * 682 xruns at 10, 6 and 4 ms, which is what a systematic misconfiguration
	 * looks like next to genuine jitter. The driver itself advertises periods down
	 * to 80 frames. */
	int (*hw_params_malloc)(void**) = nullptr;
	void (*hw_params_free)(void*) = nullptr;
	int (*hw_params_any)(snd_pcm_t*, void*) = nullptr;
	int (*hw_set_access)(snd_pcm_t*, void*, int) = nullptr;
	int (*hw_set_format)(snd_pcm_t*, void*, int) = nullptr;
	int (*hw_set_channels)(snd_pcm_t*, void*, unsigned) = nullptr;
	int (*hw_set_rate_near)(snd_pcm_t*, void*, unsigned*, int*) = nullptr;
	int (*hw_set_period_size_near)(snd_pcm_t*, void*, uframes*, int*) = nullptr;
	int (*hw_set_periods_near)(snd_pcm_t*, void*, unsigned*, int*) = nullptr;
	int (*hw_params_)(snd_pcm_t*, void*) = nullptr;
	int (*prepare)(snd_pcm_t*) = nullptr;
	sframes (*writei)(snd_pcm_t*, const void*, uframes) = nullptr;
	int (*recover)(snd_pcm_t*, int, int) = nullptr;
	int (*close_)(snd_pcm_t*) = nullptr;
	int (*drain)(snd_pcm_t*) = nullptr;
	const char* (*strerror_)(int) = nullptr;

	bool load()
	{
		void* h = dlopen("libasound.so.2", RTLD_NOW);
		if (!h) return false;
		auto sym = [h](const char* n) { return dlsym(h, n); };
		open_       = reinterpret_cast<decltype(open_)>(sym("snd_pcm_open"));
		set_params  = reinterpret_cast<decltype(set_params)>(sym("snd_pcm_set_params"));
		writei      = reinterpret_cast<decltype(writei)>(sym("snd_pcm_writei"));
		recover     = reinterpret_cast<decltype(recover)>(sym("snd_pcm_recover"));
		close_      = reinterpret_cast<decltype(close_)>(sym("snd_pcm_close"));
		drain       = reinterpret_cast<decltype(drain)>(sym("snd_pcm_drain"));
		strerror_   = reinterpret_cast<decltype(strerror_)>(sym("snd_strerror"));
		hw_params_malloc = reinterpret_cast<decltype(hw_params_malloc)>(sym("snd_pcm_hw_params_malloc"));
		hw_params_free   = reinterpret_cast<decltype(hw_params_free)>(sym("snd_pcm_hw_params_free"));
		hw_params_any    = reinterpret_cast<decltype(hw_params_any)>(sym("snd_pcm_hw_params_any"));
		hw_set_access    = reinterpret_cast<decltype(hw_set_access)>(sym("snd_pcm_hw_params_set_access"));
		hw_set_format    = reinterpret_cast<decltype(hw_set_format)>(sym("snd_pcm_hw_params_set_format"));
		hw_set_channels  = reinterpret_cast<decltype(hw_set_channels)>(sym("snd_pcm_hw_params_set_channels"));
		hw_set_rate_near = reinterpret_cast<decltype(hw_set_rate_near)>(sym("snd_pcm_hw_params_set_rate_near"));
		hw_set_period_size_near = reinterpret_cast<decltype(hw_set_period_size_near)>(sym("snd_pcm_hw_params_set_period_size_near"));
		hw_set_periods_near = reinterpret_cast<decltype(hw_set_periods_near)>(sym("snd_pcm_hw_params_set_periods_near"));
		hw_params_       = reinterpret_cast<decltype(hw_params_)>(sym("snd_pcm_hw_params"));
		prepare          = reinterpret_cast<decltype(prepare)>(sym("snd_pcm_prepare"));
		return open_ && set_params && writei && recover && close_;
	}
}

using namespace jeLib;

namespace
{
	volatile sig_atomic_t g_stop = 0;
	void onSignal(int) { g_stop = 1; }

	std::vector<int> parseList(const char* _s)
	{
		std::vector<int> v;
		if (!_s) return v;
		int n = 0; bool any = false;
		for (const char* p = _s;; ++p)
		{
			if (*p >= '0' && *p <= '9') { n = n * 10 + (*p - '0'); any = true; }
			else { if (any) v.push_back(n); n = 0; any = false; if (!*p) break; }
		}
		return v;
	}
}

int main(int _argc, char* _argv[])
{
	if (_argc < 2)
	{
		fprintf(stderr, "usage: %s <rom_dir> [midi_device]   (audio: raw S16LE stereo on stdout)\n", _argv[0]);
		return 1;
	}

	signal(SIGINT, onSignal);
	signal(SIGTERM, onSignal);

	char abspath[4096];
	const std::string romDir = realpath(_argv[1], abspath) ? abspath : _argv[1];

	baseLib::setFlushDenormalsToZero();

	synthLib::RomLoader::addSearchPath(romDir);
	auto rom = RomLoader::findROM();
	if (!rom.isValid()) { fprintf(stderr, "no valid ROM in %s\n", romDir.c_str()); return 1; }

	// Must match jeLib::Device::getSamplerate(); it does not resample for us.
	constexpr uint32_t EngineSR = 88200;
	constexpr uint32_t HostSR = EngineSR;

	synthLib::DeviceCreateParams params;
	params.romData = rom.getData();
	params.romName = rom.getName();
	std::string homeDir = romDir;
	if (const size_t slash = homeDir.find_last_of('/'); slash != std::string::npos && slash > 0)
		homeDir = homeDir.substr(0, slash);
	params.homePath = homeDir;
	params.hostSamplerate = HostSR;
	params.preferredSamplerate = EngineSR;

	Device device(params);
	if (!device.isValid()) { fprintf(stderr, "device init failed\n"); return 1; }
	device.setMasterVolume(7.0f);

	if (const char* pb = getenv("JE_PIPELINE"))
	{
		const auto bounds = parseList(pb);
		const auto cores = parseList(getenv("JE_PIPELINE_CORES"));
		const int64_t latency = getenv("JE_PIPELINE_LATENCY") ? atoll(getenv("JE_PIPELINE_LATENCY")) : 64;
		device.getJe8086().requestParallelPipeline(bounds, cores, latency);
		fprintf(stderr, "[live] parallel pipeline: bounds=%s cores=%s latency=%lld\n",
		        pb, getenv("JE_PIPELINE_CORES") ? getenv("JE_PIPELINE_CORES") : "(none)", (long long)latency);
	}

	const std::string snapPath = romDir + "/boot.snap";
	const bool snap = device.getJe8086().loadSnapshot(snapPath.c_str());
	fprintf(stderr, "[live] snapshot %s\n", snap ? "loaded" : "MISSING — cold boot, this takes a while");

	int midiFd = -1;
	if (_argc > 2)
	{
		midiFd = open(_argv[2], O_RDONLY | O_NONBLOCK);
		fprintf(stderr, "[live] midi in: %s%s\n", _argv[2], midiFd < 0 ? " (FAILED to open)" : "");
	}
	else
	{
		fprintf(stderr, "[live] no midi device given; running silent. Pass e.g. /dev/snd/midiC1D0\n");
	}

	synthLib::MidiBufferParser midiParser(synthLib::MidiEventSource::Host);

	const uint32_t block = getenv("JP_BLOCK") ? static_cast<uint32_t>(atoi(getenv("JP_BLOCK"))) : 256;
	std::vector<float> outL(block), outR(block);
	std::vector<int16_t> interleaved(block * 2);

	synthLib::TAudioInputs inputs{};
	synthLib::TAudioOutputs outputs{};
	outputs[0] = outL.data();
	outputs[1] = outR.data();

	/* JP_ALSA=plughw:0,0 writes to the card directly (paced by it); unset falls
	 * back to raw stdout for piping. */
	alsa::snd_pcm_t* pcm = nullptr;
	const char* alsaDev = getenv("JP_ALSA");
	const unsigned bufUs = getenv("JP_BUFFER_US") ? static_cast<unsigned>(atoi(getenv("JP_BUFFER_US"))) : 20000;
	if (alsaDev)
	{
		if (!alsa::load()) { fprintf(stderr, "[live] libasound.so.2 not loadable\n"); return 1; }
		if (const int e = alsa::open_(&pcm, alsaDev, alsa::StreamPlayback, 0); e < 0)
		{
			fprintf(stderr, "[live] cannot open %s: %s\n", alsaDev, alsa::strerror_ ? alsa::strerror_(e) : "?");
			return 1;
		}
		bool configured = false;
		if (alsa::hw_params_malloc && alsa::hw_params_any && alsa::hw_params_)
		{
			void* hw = nullptr;
			if (alsa::hw_params_malloc(&hw) >= 0)
			{
				alsa::uframes period = block;		// one period per process() call
				unsigned periods = getenv("JP_PERIODS") ? static_cast<unsigned>(atoi(getenv("JP_PERIODS"))) : 3;
				// 3 is the floor that runs clean on a Pi: the bcm2835 output pins the
				// period at 884 frames (~10 ms at 88.2 kHz) whatever we ask for, and
				// two periods xrun. A USB interface will accept far smaller ones.
				unsigned rate = HostSR;
				int dir = 0;
				alsa::hw_params_any(pcm, hw);
				alsa::hw_set_access(pcm, hw, alsa::AccessRwInterleaved);
				alsa::hw_set_format(pcm, hw, alsa::FormatS16LE);
				alsa::hw_set_channels(pcm, hw, 2);
				alsa::hw_set_rate_near(pcm, hw, &rate, &dir);
				dir = 0;
				alsa::hw_set_period_size_near(pcm, hw, &period, &dir);
				dir = 0;
				alsa::hw_set_periods_near(pcm, hw, &periods, &dir);
				if (alsa::hw_params_(pcm, hw) >= 0)
				{
					configured = true;
					fprintf(stderr, "[live] audio: %s direct, period %lu frames x %u = %.1f ms buffer, %u Hz\n",
					        alsaDev, static_cast<unsigned long>(period), periods,
					        1000.0 * period * periods / rate, rate);
				}
				alsa::hw_params_free(hw);
				if (configured && alsa::prepare) alsa::prepare(pcm);
			}
		}
		if (!configured)
		{
			if (const int e = alsa::set_params(pcm, alsa::FormatS16LE, alsa::AccessRwInterleaved, 2, HostSR, 1, bufUs); e < 0)
			{
				fprintf(stderr, "[live] set_params failed: %s\n", alsa::strerror_ ? alsa::strerror_(e) : "?");
				return 1;
			}
			fprintf(stderr, "[live] audio: %s direct, %u us buffer (fallback path)\n", alsaDev, bufUs);
		}
	}
	else
	{
		fprintf(stderr, "[live] audio: raw S16LE on stdout (pipe to aplay -r %u)\n", HostSR);
	}

	uint64_t xruns = 0;
	std::vector<synthLib::SMidiEvent> midiIn, midiOut;
	uint8_t midiBuf[512];

	fprintf(stderr, "[live] running: %u Hz out, %u frames/block. Ctrl-C to stop.\n", HostSR, block);

	/* Per-block render time. An occasional long block is invisible in an average
	 * and is exactly what forces a large buffer, so keep the tail. */
	std::vector<double> blockMs;
	blockMs.reserve(200000);
	uint64_t frames = 0;
	while (!g_stop)
	{
		if (midiFd >= 0)
		{
			const ssize_t n = read(midiFd, midiBuf, sizeof(midiBuf));
			for (ssize_t i = 0; i < n; ++i)
				midiParser.write(midiBuf[i]);
			midiParser.getEvents(midiIn);
		}

		timespec t0{}, t1{};
		clock_gettime(CLOCK_MONOTONIC, &t0);
		device.process(inputs, outputs, block, midiIn, midiOut);
		clock_gettime(CLOCK_MONOTONIC, &t1);
		blockMs.push_back((t1.tv_sec - t0.tv_sec) * 1e3 + (t1.tv_nsec - t0.tv_nsec) / 1e6);
		midiIn.clear();
		midiOut.clear();

		for (uint32_t i = 0; i < block; ++i)
		{
			const auto cvt = [](const float _v)
			{
				const float c = std::clamp(_v, -1.0f, 1.0f);
				return static_cast<int16_t>(c * 32767.0f);
			};
			interleaved[i * 2] = cvt(outL[i]);
			interleaved[i * 2 + 1] = cvt(outR[i]);
		}

		if (pcm)
		{
			// blocks until the card has room: this is what paces the renderer
			alsa::sframes n = alsa::writei(pcm, interleaved.data(), block);
			if (n < 0)
			{
				++xruns;
				n = alsa::recover(pcm, static_cast<int>(n), 1);
				if (n < 0) { fprintf(stderr, "[live] unrecoverable: %s\n", alsa::strerror_ ? alsa::strerror_(static_cast<int>(n)) : "?"); break; }
			}
		}
		else
		{
			const size_t bytes = interleaved.size() * sizeof(int16_t);
			if (fwrite(interleaved.data(), 1, bytes, stdout) != bytes)
				break;	// downstream went away
		}

		frames += block;
	}

	if (pcm) { if (alsa::drain) alsa::drain(pcm); alsa::close_(pcm); }
	fflush(stdout);
	fprintf(stderr, "\n[live] stopped after %.1f s of audio, %llu xruns\n",
	        static_cast<double>(frames) / HostSR, static_cast<unsigned long long>(xruns));
	if (!blockMs.empty())
	{
		std::sort(blockMs.begin(), blockMs.end());
		const auto pct = [&](const double p) { return blockMs[static_cast<size_t>(p * (blockMs.size() - 1))]; };
		const double budget = 1000.0 * block / HostSR;
		size_t over = 0;
		for (const double v : blockMs) if (v > budget) ++over;
		fprintf(stderr, "[live] block time ms: p50 %.2f  p99 %.2f  p99.9 %.2f  max %.2f   (budget %.2f, over budget %zu/%zu)\n",
		        pct(0.5), pct(0.99), pct(0.999), blockMs.back(), budget, over, blockMs.size());
	}
	if (midiFd >= 0) close(midiFd);
	return 0;
}
