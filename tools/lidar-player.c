// SPDX-License-Identifier: GPL-2.0-only
/*
 * lidar-player: plays the web UI's song library on the speaker.
 *
 * Songs are WAV files (PCM S16LE, 48 kHz, stereo, as the web UI's browser
 * side produces them) in the library directory: <id>.wav plus <id>.title.
 * The daemon takes one-line commands on a FIFO and keeps its state as JSON
 * in a file the web UI reads:
 *
 *   play <id> | pause | resume | stop | next | prev | seek <seconds>
 *
 * After a song it continues with the next one in library order. Pause, stop
 * and skips fade out over 20 ms and starts fade in, so nothing clicks; the
 * output is closed while nothing plays.
 *
 * Usage: lidar-player daemon [dir]
 *        lidar-player info <file.wav>   prints the duration, or fails
 */
#include <alsa/asoundlib.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define RATE	48000
#define CHUNK	1024			/* frames per write */
#define FADE	(RATE / 50)		/* 20 ms */
#define PCM	"lidar_speaker"
#define FIFO	"/run/lidar-player.fifo"
#define STATE	"/run/lidar-player.json"

static const char *dir = "/run/lidar-player";

enum { STOPPED, PLAYING, PAUSED };
static const char *const state_name[] = { "stopped", "playing", "paused" };

static int state;
static char cur[64];			/* id of the current song */
static FILE *wav;
static long data_off, frames_total, frame_pos;
static snd_pcm_t *pcm;

/* WAV with a 16-byte "fmt " chunk: PCM, 2 channels, 48 kHz, 16 bit */
static int wav_open(const char *path, FILE **fp, long *off, long *frames)
{
	unsigned char h[12], ck[8], fmt[16];
	FILE *f = fopen(path, "rb");
	long size;

	if (!f)
		return -1;
	if (fread(h, 1, 12, f) != 12 || memcmp(h, "RIFF", 4) || memcmp(h + 8, "WAVE", 4))
		goto bad;
	for (;;) {
		uint32_t len;

		if (fread(ck, 1, 8, f) != 8)
			goto bad;
		len = ck[4] | ck[5] << 8 | ck[6] << 16 | (uint32_t)ck[7] << 24;
		if (!memcmp(ck, "fmt ", 4)) {
			if (len < 16 || fread(fmt, 1, 16, f) != 16 ||
			    fmt[0] != 1 || fmt[1] || fmt[2] != 2 || fmt[3] ||
			    (fmt[4] | fmt[5] << 8 | fmt[6] << 16 | (uint32_t)fmt[7] << 24) != RATE ||
			    fmt[14] != 16)
				goto bad;
			fseek(f, len - 16 + (len & 1), SEEK_CUR);
		} else if (!memcmp(ck, "data", 4)) {
			*off = ftell(f);
			fseek(f, 0, SEEK_END);
			size = ftell(f) - *off;
			if (len < (uint32_t)size)
				size = len;
			*frames = size / 4;
			fseek(f, *off, SEEK_SET);
			*fp = f;
			return 0;
		} else {
			fseek(f, len + (len & 1), SEEK_CUR);
		}
	}
bad:
	fclose(f);
	return -1;
}

static void song_path(char *buf, size_t n, const char *id, const char *ext)
{
	snprintf(buf, n, "%s/%s.%s", dir, id, ext);
}

static int valid_id(const char *id)
{
	if (!*id || strlen(id) >= sizeof(cur))
		return 0;
	for (const char *p = id; *p; p++)
		if (!((*p >= '0' && *p <= '9') || *p == '-'))
			return 0;
	return 1;
}

/* library order = id order (ids are "<epoch>-<n>", zero padded n) */
static int cmp_str(const void *a, const void *b)
{
	return strcmp(*(char *const *)a, *(char *const *)b);
}

/* neighbour of cur in library order: dir = +1 next, -1 previous */
static int neighbour(int step, char *out, size_t n)
{
	DIR *d = opendir(dir);
	struct dirent *e;
	char *ids[1024];
	int count = 0, found = 0;

	if (!d)
		return 0;
	while ((e = readdir(d)) && count < 1024) {
		size_t l = strlen(e->d_name);

		if (l > 4 && !strcmp(e->d_name + l - 4, ".wav")) {
			ids[count] = strndup(e->d_name, l - 4);
			if (ids[count])
				count++;
		}
	}
	closedir(d);
	qsort(ids, count, sizeof(*ids), cmp_str);
	for (int i = 0; i < count; i++)
		if (!strcmp(ids[i], cur) && i + step >= 0 && i + step < count) {
			snprintf(out, n, "%s", ids[i + step]);
			found = 1;
		}
	for (int i = 0; i < count; i++)
		free(ids[i]);
	return found;
}

static void json_str(FILE *f, const char *s)
{
	fputc('"', f);
	for (; *s; s++) {
		if (*s == '"' || *s == '\\')
			fprintf(f, "\\%c", *s);
		else if ((unsigned char)*s < 0x20)
			fprintf(f, "\\u%04x", *s);
		else
			fputc(*s, f);
	}
	fputc('"', f);
}

static void write_state(void)
{
	char path[256], title[512] = "";
	FILE *f;
	double elapsed = (double)frame_pos / RATE;

	if (state != STOPPED) {
		FILE *t;

		song_path(path, sizeof(path), cur, "title");
		t = fopen(path, "r");
		if (t) {
			if (!fgets(title, sizeof(title), t))
				title[0] = 0;
			title[strcspn(title, "\n")] = 0;
			fclose(t);
		}
		if (pcm && state == PLAYING) {
			snd_pcm_sframes_t delay;

			if (snd_pcm_delay(pcm, &delay) == 0 && delay > 0 && delay < frame_pos)
				elapsed = (double)(frame_pos - delay) / RATE;
		}
	}
	f = fopen(STATE ".new", "w");
	if (!f)
		return;
	fprintf(f, "{\"state\": \"%s\", \"id\": ", state_name[state]);
	if (state == STOPPED) {
		fprintf(f, "null, \"title\": null, \"elapsed\": 0, \"duration\": 0}\n");
	} else {
		json_str(f, cur);
		fprintf(f, ", \"title\": ");
		json_str(f, title);
		fprintf(f, ", \"elapsed\": %.1f, \"duration\": %.1f}\n", elapsed,
			(double)frames_total / RATE);
	}
	fclose(f);
	rename(STATE ".new", STATE);
}

static void pcm_close(void)
{
	if (pcm) {
		snd_pcm_drain(pcm);
		snd_pcm_close(pcm);
		pcm = NULL;
	}
}

static int pcm_open(void)
{
	int err;

	if (pcm)
		return 0;
	if ((err = snd_pcm_open(&pcm, PCM, SND_PCM_STREAM_PLAYBACK, 0)) < 0 ||
	    (err = snd_pcm_set_params(pcm, SND_PCM_FORMAT_S16_LE,
				      SND_PCM_ACCESS_RW_INTERLEAVED, 2, RATE, 1, 200000)) < 0) {
		fprintf(stderr, "lidar-player: %s: %s\n", PCM, snd_strerror(err));
		if (pcm)
			snd_pcm_close(pcm);
		pcm = NULL;
		return -1;
	}
	return 0;
}

static void close_song(void)
{
	if (wav)
		fclose(wav);
	wav = NULL;
}

static int open_song(const char *id)
{
	char path[256];

	if (!valid_id(id))
		return -1;
	close_song();
	song_path(path, sizeof(path), id, "wav");
	if (wav_open(path, &wav, &data_off, &frames_total) < 0)
		return -1;
	snprintf(cur, sizeof(cur), "%s", id);
	frame_pos = 0;
	return 0;
}

/*
 * Plays up to n frames from the current song with a gain ramp from g0 to
 * g1 across them; returns the frames played, 0 at the end of the song,
 * -1 if the output failed (then everything is stopped).
 */
static int play_frames(int n, float g0, float g1)
{
	short buf[2 * CHUNK];
	size_t got;

	if (n > frames_total - frame_pos)	/* chunks after "data" are not audio */
		n = frames_total - frame_pos;
	got = n > 0 ? fread(buf, 4, n, wav) : 0;
	for (size_t i = 0; i < got; i++) {
		float g = g0 + (g1 - g0) * i / n;

		buf[2 * i] = buf[2 * i] * g;
		buf[2 * i + 1] = buf[2 * i + 1] * g;
	}
	if (got) {
		snd_pcm_sframes_t w = snd_pcm_writei(pcm, buf, got);

		if (w < 0 && (snd_pcm_recover(pcm, w, 1) < 0 ||
			      snd_pcm_writei(pcm, buf, got) < 0)) {
			fprintf(stderr, "lidar-player: output failed: %s\n", snd_strerror(w));
			snd_pcm_close(pcm);
			pcm = NULL;
			close_song();
			state = STOPPED;
			write_state();
			return -1;
		}
	}
	frame_pos += got;
	return got;
}

/* fade out what is playing, then leave the output closed */
static void fade_out(void)
{
	if (state == PLAYING && pcm && wav)
		play_frames(FADE, 1, 0);
	pcm_close();
}

static void start(void)
{
	if (pcm_open() == 0) {
		state = PLAYING;
		play_frames(FADE, 0, 1);
	} else {
		close_song();
		state = STOPPED;
	}
}

static void command(char *line)
{
	char *arg = strchr(line, ' ');
	char id[64];

	if (arg)
		*arg++ = 0;
	if (!strcmp(line, "play") && arg) {
		fade_out();
		if (open_song(arg) == 0)
			start();
		else
			state = STOPPED;
	} else if (!strcmp(line, "pause") && state == PLAYING) {
		fade_out();
		state = wav ? PAUSED : STOPPED;	/* the output may have failed */
	} else if (!strcmp(line, "resume") && state == PAUSED) {
		start();
	} else if (!strcmp(line, "stop")) {
		fade_out();
		close_song();
		state = STOPPED;
	} else if ((!strcmp(line, "next") || !strcmp(line, "prev")) && state != STOPPED) {
		int was = state;

		fade_out();
		if (neighbour(line[0] == 'n' ? 1 : -1, id, sizeof(id)) && open_song(id) == 0) {
			if (was == PLAYING)
				start();
		} else if (line[0] == 'p' && wav) {
			frame_pos = 0;	/* first song: back to its start */
			fseek(wav, data_off, SEEK_SET);
			if (was == PLAYING)
				start();
		} else {
			close_song();
			state = STOPPED;
		}
	} else if (!strcmp(line, "seek") && arg && state != STOPPED) {
		double sec = atof(arg);
		long f = sec <= 0 ? 0 : sec >= (double)frames_total / RATE ? frames_total : (long)(sec * RATE);
		int was = state;

		fade_out();
		if (!wav) {	/* the output failed during the fade */
			state = STOPPED;
		} else {
			frame_pos = f;
			fseek(wav, data_off + f * 4, SEEK_SET);
			if (was == PLAYING)
				start();
		}
	}
	write_state();
}

static int daemon_main(void)
{
	char line[256];
	size_t used = 0;
	int fd;
	unsigned int n = 0;

	if ((mkdir(dir, 0755) < 0 && errno != EEXIST) ||
	    (mkfifo(FIFO, 0600) < 0 && errno != EEXIST))
		return 1;
	/* read-write: never sees EOF when a writer goes */
	fd = open(FIFO, O_RDWR | O_NONBLOCK);
	if (fd < 0)
		return 1;
	write_state();
	for (;;) {
		ssize_t r = read(fd, line + used, sizeof(line) - 1 - used);

		if (r > 0) {
			char *nl;

			used += r;
			line[used] = 0;
			while ((nl = strchr(line, '\n'))) {
				*nl = 0;
				command(line);
				memmove(line, nl + 1, used - (nl + 1 - line) + 1);
				used -= nl + 1 - line;
			}
			if (used == sizeof(line) - 1)
				used = 0;	/* overlong line */
		}
		if (state != PLAYING) {
			usleep(50000);
			continue;
		}
		if (play_frames(CHUNK, 1, 1) == 0) {	/* end of song */
			char id[64];

			if (neighbour(1, id, sizeof(id)) && open_song(id) == 0) {
				play_frames(CHUNK, 1, 1);
			} else {
				pcm_close();
				close_song();
				state = STOPPED;
			}
			write_state();
		}
		if (++n % 12 == 0)	/* ~250 ms */
			write_state();
	}
}

int main(int argc, char **argv)
{
	if (argc >= 2 && !strcmp(argv[1], "daemon")) {
		if (argc >= 3)
			dir = argv[2];
		return daemon_main();
	}
	if (argc == 3 && !strcmp(argv[1], "info")) {
		FILE *f;
		long off, frames;

		if (wav_open(argv[2], &f, &off, &frames) < 0) {
			fprintf(stderr, "not a 48 kHz 16-bit stereo PCM WAV\n");
			return 1;
		}
		fclose(f);
		printf("%.1f\n", (double)frames / RATE);
		return 0;
	}
	fprintf(stderr, "usage: lidar-player daemon [dir] | info <file.wav>\n");
	return 2;
}
