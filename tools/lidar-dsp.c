// SPDX-License-Identifier: GPL-2.0-only
/*
 * lidar-dsp: the speaker tuning of Amazon's firmware, applied to the mixed
 * speaker stream. It reads the owner's own Fire OS configuration (AFE.cfg
 * and the files it names: EQ_*.cfg, ParametricEQ.cfg, MBCL*.cfg) and runs
 * the stock playback chain on it:
 *
 *   volume -> FIR equalizer chosen by volume (loudness, speaker response)
 *          -> parametric EQ -> MBCL (4-band compressor and limiter,
 *          full-band limiter) -> clip -> (L+R)/2 on both channels
 *
 * as reverse engineered from Fire OS's libasp.so. Stages that are a plain
 * delay or unity gain on stock (AVL for music, flat UserEQ, 0 dB crossover
 * gain) are left out.
 *
 * Usage:
 *   lidar-dsp [-c dir] [-i capture] [-o playback] [-v volfile] [-r readyfile]
 *       live: reads S16_LE stereo 48 kHz from the capture PCM (the speaker
 *       mix through snd-aloop), writes the processed stream to the playback
 *       PCM. The volume comes from volfile ("<level dB> <gain dB> <mute>",
 *       written by lidar-volume); readyfile exists while the output runs.
 *   lidar-dsp [-c dir] -k
 *       check the configuration only.
 *   lidar-dsp [-c dir] -f -- <level dB> <gain dB>
 *       offline: float32 stereo 48 kHz from stdin to stdout, fixed volume.
 */
#include <alsa/asoundlib.h>
#include <cjson/cJSON.h>
#include <complex.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <limits.h>
#include <sched.h>
#include <signal.h>
#include <sys/mman.h>
#include <unistd.h>

#define RATE		48000
#define FRAME		384		/* samples per frame, as stock (8 ms) */
#define FFTN		1024		/* FIR partition transform size */
#define MAXTAPS		4096
#define MAXZONES	8
#define MAXBIQ		16
#define FADE_FRAMES	50		/* FIR zone change: 400 ms */
#define MBC_BLOCK	48		/* compressor block, 1 ms */
#define MBC_DELAY	768		/* compressor look-ahead */
#define LIM_N		96		/* limiter look-ahead, 2 ms */
#define LIM_HOLD	20

static const char *cfgdir = "/run/lidar-dsp";

static void die(const char *fmt, const char *arg)
{
	char msg[512];

	snprintf(msg, sizeof(msg), fmt, arg);
	fprintf(stderr, "lidar-dsp: %s\n", msg);	/* one kmsg record */
	exit(1);
}

/* ------------------------------------------------------------ config */

static char *slurp(const char *name)
{
	char path[512];
	FILE *f;
	long n;
	char *buf;

	snprintf(path, sizeof(path), "%s/%s", cfgdir, name);
	f = fopen(path, "rb");
	if (!f)
		die("cannot open %s", path);
	fseek(f, 0, SEEK_END);
	n = ftell(f);
	rewind(f);
	buf = malloc(n + 1);
	if (!buf || fread(buf, 1, n, f) != (size_t)n)
		die("cannot read %s", path);
	buf[n] = 0;
	fclose(f);
	return buf;
}

/* Amazon's .cfg files are JSON with C comments, as cJSON_Minify takes them */
static cJSON *load_json(const char *name)
{
	char *text = slurp(name);
	cJSON *j;

	cJSON_Minify(text);
	j = cJSON_Parse(text);
	if (!j)
		die("%s is not valid JSON", name);
	free(text);
	return j;
}

static double num(const cJSON *o, const char *key, double def)
{
	const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);

	return cJSON_IsNumber(v) ? v->valuedouble : def;
}

static int flag(const cJSON *o, const char *key, int def)
{
	const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);

	return cJSON_IsBool(v) ? cJSON_IsTrue(v) : def;
}

static const cJSON *obj(const cJSON *o, const char *key)
{
	return cJSON_GetObjectItemCaseSensitive(o, key);
}

static double clampd(double v, double lo, double hi)
{
	return v < lo ? lo : v > hi ? hi : v;
}

static double db2lin(double db)
{
	return pow(10.0, db / 20.0);
}

/* ------------------------------------------------------------ biquads */

/*
 * Transposed direct form II, coefficients normalised to a0 = 1:
 * y = b0 x + s1; s1 = b1 x - a1 y + s2; s2 = b2 x - a2 y
 */
struct biquad {
	float b0, b1, b2, a1, a2;
	float s1[2], s2[2];
};

static void bq_set(struct biquad *q, double b0, double b1, double b2,
		   double a1, double a2)
{
	q->b0 = b0; q->b1 = b1; q->b2 = b2; q->a1 = a1; q->a2 = a2;
	q->s1[0] = q->s1[1] = q->s2[0] = q->s2[1] = 0;
}

static inline float bq_run(struct biquad *q, int c, float x)
{
	float y = q->b0 * x + q->s1[c];

	q->s1[c] = q->b1 * x - q->a1 * y + q->s2[c];
	q->s2[c] = q->b2 * x - q->a2 * y;
	return y;
}

enum { BYPASS, LOW_PASS, HIGH_PASS, BAND_PASS, NOTCH, PEAK, LOW_SHELF, HIGH_SHELF };

/*
 * Stock's designer: bilinear-transform forms with K = tan(pi fc / fs) (as
 * in Zoelzer, DAFX), not the RBJ cookbook. The shelves ignore Q.
 */
static void bq_design(struct biquad *q, int type, double fc, double Q, double gdb)
{
	double K = tan(M_PI * fc / RATE), K2 = K * K;
	double V = pow(10.0, fabs(gdb) / 20.0), r = sqrt(2.0 * V), s = M_SQRT2;
	double n;

	if (type == BYPASS || fc >= RATE / 2.0) {
		bq_set(q, 1, 0, 0, 0, 0);
		return;
	}
	switch (type) {
	case LOW_PASS:
		n = 1 / (1 + K / Q + K2);
		bq_set(q, K2 * n, 2 * K2 * n, K2 * n, 2 * (K2 - 1) * n, (1 - K / Q + K2) * n);
		break;
	case HIGH_PASS:
		n = 1 / (1 + K / Q + K2);
		bq_set(q, n, -2 * n, n, 2 * (K2 - 1) * n, (1 - K / Q + K2) * n);
		break;
	case BAND_PASS:
		n = 1 / (1 + K / Q + K2);
		bq_set(q, K / Q * n, 0, -K / Q * n, 2 * (K2 - 1) * n, (1 - K / Q + K2) * n);
		break;
	case NOTCH:
		n = 1 / (1 + K / Q + K2);
		bq_set(q, (1 + K2) * n, 2 * (K2 - 1) * n, (1 + K2) * n,
		       2 * (K2 - 1) * n, (1 - K / Q + K2) * n);
		break;
	case PEAK:
		if (gdb >= 0) {
			n = 1 / (1 + K / Q + K2);
			bq_set(q, (1 + V * K / Q + K2) * n, 2 * (K2 - 1) * n,
			       (1 - V * K / Q + K2) * n, 2 * (K2 - 1) * n,
			       (1 - K / Q + K2) * n);
		} else {
			n = 1 / (1 + V * K / Q + K2);
			bq_set(q, (1 + K / Q + K2) * n, 2 * (K2 - 1) * n,
			       (1 - K / Q + K2) * n, 2 * (K2 - 1) * n,
			       (1 - V * K / Q + K2) * n);
		}
		break;
	case LOW_SHELF:
		if (gdb >= 0) {
			n = 1 / (1 + s * K + K2);
			bq_set(q, (1 + r * K + V * K2) * n, 2 * (V * K2 - 1) * n,
			       (1 - r * K + V * K2) * n, 2 * (K2 - 1) * n,
			       (1 - s * K + K2) * n);
		} else {
			n = 1 / (1 + r * K + V * K2);
			bq_set(q, (1 + s * K + K2) * n, 2 * (K2 - 1) * n,
			       (1 - s * K + K2) * n, 2 * (V * K2 - 1) * n,
			       (1 - r * K + V * K2) * n);
		}
		break;
	case HIGH_SHELF:
		if (gdb >= 0) {
			n = 1 / (1 + s * K + K2);
			bq_set(q, (V + r * K + K2) * n, 2 * (K2 - V) * n,
			       (V - r * K + K2) * n, 2 * (K2 - 1) * n,
			       (1 - s * K + K2) * n);
		} else {
			n = 1 / (V + r * K + K2);
			bq_set(q, (1 + s * K + K2) * n, 2 * (K2 - 1) * n,
			       (1 - s * K + K2) * n, 2 * (K2 - V) * n,
			       (V - r * K + K2) * n);
		}
		break;
	}
}

static int bq_type(const char *s)
{
	static const char *names[] = {
		"BYPASS", "LOW_PASS", "HIGH_PASS", "BAND_PASS", "NOTCH", "PEAK",
		"LOW_SHELF", "HIGH_SHELF",
	};

	for (unsigned int i = 0; i < sizeof(names) / sizeof(names[0]); i++)
		if (!strcmp(s, names[i]))
			return i;
	die("unsupported FilterType %s", s);
	return BYPASS;
}

/* ------------------------------------------------------------ FFT */

static float complex twiddle[FFTN / 2];
static unsigned short bitrev[FFTN];

static void fft_init(void)
{
	int bits = 0;

	while ((1 << bits) < FFTN)
		bits++;
	for (int i = 0; i < FFTN / 2; i++)
		twiddle[i] = cexpf(-2.0f * (float)M_PI * I * i / FFTN);
	for (int i = 0; i < FFTN; i++) {
		int r = 0;

		for (int b = 0; b < bits; b++)
			if (i & (1 << b))
				r |= 1 << (bits - 1 - b);
		bitrev[i] = r;
	}
}

/* in-place radix-2; inverse without the 1/N scaling */
static void fft(float complex *x, int inverse)
{
	for (int i = 0; i < FFTN; i++)
		if (bitrev[i] > i) {
			float complex t = x[i];

			x[i] = x[bitrev[i]];
			x[bitrev[i]] = t;
		}
	for (int len = 2; len <= FFTN; len <<= 1) {
		int step = FFTN / len;

		for (int i = 0; i < FFTN; i += len)
			for (int k = 0; k < len / 2; k++) {
				float complex w = twiddle[k * step];
				float complex a = x[i + k], b;

				if (inverse)
					w = conjf(w);
				b = x[i + k + len / 2] * w;
				x[i + k] = a + b;
				x[i + k + len / 2] = a - b;
			}
	}
}

/* ------------------------------------------------------------ FIR EQ */

/*
 * The FIR zones are given as stock volume indices (0..100). Stock maps an
 * index to dB through this table (libasp.so, music volume curve), entry
 * (int)(index * 0.3 + 0.5); the zone boundaries are compared in dB here,
 * so they stay where stock has them whatever the safety cap.
 */
static double stock_volume_db(int index)
{
	static const double tab[31] = {
		-200, -75, -60, -51, -48, -45, -42, -39, -37, -34, -31, -29,
		-27, -26, -24.5, -23.5, -21, -19, -17, -15, -13.5, -12.5,
		-11.5, -10, -8, -6, -5, -4, -2, -1, 0,
	};
	int i = (int)(clampd(index, 0, 100) * 0.3 + 0.5);

	return tab[i];
}

/*
 * Uniformly partitioned convolution: partition j of the taps (FRAME long,
 * zero padded to FFTN) multiplies the spectrum of the input window that
 * ended j frames ago. The window holds the last FFTN input samples; the
 * last FRAME outputs of each inverse transform are free of wrap-around
 * because FFTN - FRAME + 1 >= FRAME. Left and right travel as real and
 * imaginary part of one complex transform: the taps are real and the same
 * for both channels, so the two outputs separate again.
 */
struct fir {
	int enabled, nzones, nparts;
	double bound_db[MAXZONES];	/* zone z up to this level */
	float complex *H[MAXZONES];	/* nparts * FFTN per zone */
	float complex *Hfrom, *Hmix;	/* fade: from-set and current set */
	float complex *X;		/* spectra of the last nparts windows */
	float complex win[FFTN];	/* time window, L + iR */
	int head;			/* newest spectrum in X */
	int zone, fade;			/* fade = frames into a fade, 0 = none */
};

static float *read_taps(const char *name, int *n)
{
	char *text = slurp(name), *p = text, *end;
	float *t = malloc(MAXTAPS * sizeof(*t));

	*n = 0;
	while (*p) {
		float v = strtof(p, &end);

		if (end == p) {
			p++;
			continue;
		}
		if (*n == MAXTAPS)
			die("too many taps in %s", name);
		t[(*n)++] = v;
		p = end;
	}
	free(text);
	return t;
}

static void fir_load(struct fir *f, const cJSON *def)
{
	const cJSON *files = obj(def, "External Coefficients");
	const cJSON *bounds = obj(def, "Volume Boundary");
	int ntaps = 0;

	f->nzones = cJSON_GetArraySize(files);
	if (f->nzones < 1 || f->nzones > MAXZONES ||
	    cJSON_GetArraySize(bounds) != f->nzones)
		die("%s", "Equalizer FIR: bad External Coefficients / Volume Boundary");
	for (int z = 0; z < f->nzones; z++) {
		const cJSON *fn = cJSON_GetArrayItem(files, z);
		float *taps;
		int n;

		if (!cJSON_IsString(fn))
			die("%s", "Equalizer FIR: file name expected");
		f->bound_db[z] = stock_volume_db(cJSON_GetArrayItem(bounds, z)->valueint);
		taps = read_taps(fn->valuestring, &n);
		if (n < 1)
			die("%s: no taps", fn->valuestring);
		if (z == 0) {
			ntaps = n;
			f->nparts = (n + FRAME - 1) / FRAME;
		} else if (n != ntaps) {
			die("%s", "Equalizer FIR: all files need the same length");
		}
		f->H[z] = calloc(f->nparts * FFTN, sizeof(float complex));
		for (int j = 0; j < f->nparts; j++) {
			float complex *h = f->H[z] + j * FFTN;

			for (int k = 0; k < FRAME && j * FRAME + k < n; k++)
				h[k] = taps[j * FRAME + k] / FFTN;
			fft(h, 0);
		}
		free(taps);
	}
	f->Hfrom = calloc(f->nparts * FFTN, sizeof(float complex));
	f->Hmix = calloc(f->nparts * FFTN, sizeof(float complex));
	f->X = calloc(f->nparts * FFTN, sizeof(float complex));
	f->zone = -1;
	f->enabled = 1;
}

static int fir_zone(const struct fir *f, double db)
{
	for (int z = 0; z < f->nzones; z++)
		if (db <= f->bound_db[z])
			return z;
	return f->nzones - 1;
}

/* Stock crossfades the coefficients linearly over 50 frames */
static void fir_volume(struct fir *f, double db)
{
	int z = fir_zone(f, db);
	size_t sz = f->nparts * FFTN * sizeof(float complex);

	if (z == f->zone)
		return;
	if (f->zone < 0) {
		memcpy(f->Hmix, f->H[z], sz);
		f->zone = z;
		return;
	}
	memcpy(f->Hfrom, f->Hmix, sz);	/* mid-fade: start from where we are */
	f->zone = z;
	f->fade = 1;
}

static void fir_run(struct fir *f, float *l, float *r)
{
	float complex Y[FFTN] = { 0 };
	float complex *X;

	if (f->fade > FADE_FRAMES) {
		memcpy(f->Hmix, f->H[f->zone], f->nparts * FFTN * sizeof(float complex));
		f->fade = 0;
	} else if (f->fade) {
		/* frame k of the fade (k = 0..49) uses (1 - k/50) old + k/50 new */
		float a = (float)(f->fade - 1) / FADE_FRAMES;
		const float complex *to = f->H[f->zone];

		for (int i = 0; i < f->nparts * FFTN; i++)
			f->Hmix[i] = (1 - a) * f->Hfrom[i] + a * to[i];
		f->fade++;
	}
	memmove(f->win, f->win + FRAME, (FFTN - FRAME) * sizeof(float complex));
	for (int n = 0; n < FRAME; n++)
		f->win[FFTN - FRAME + n] = l[n] + I * r[n];
	f->head = (f->head + 1) % f->nparts;
	X = f->X + f->head * FFTN;
	memcpy(X, f->win, sizeof(f->win));
	fft(X, 0);
	for (int j = 0; j < f->nparts; j++) {
		const float complex *Xj = f->X + ((f->head - j + f->nparts) % f->nparts) * FFTN;
		const float complex *Hj = f->Hmix + j * FFTN;

		for (int k = 0; k < FFTN; k++)
			Y[k] += Xj[k] * Hj[k];
	}
	fft(Y, 1);
	for (int n = 0; n < FRAME; n++) {
		l[n] = crealf(Y[FFTN - FRAME + n]);
		r[n] = cimagf(Y[FFTN - FRAME + n]);
	}
}

/* ------------------------------------------------------------ PEQ */

struct peq {
	int n;
	struct biquad q[MAXBIQ];
};

static void peq_load(struct peq *p, const char *file)
{
	cJSON *j = load_json(file);
	const cJSON *defs = obj(j, "Biquad Definitions"), *d;

	p->n = 0;
	if (flag(j, "Bypass", 0)) {
		cJSON_Delete(j);
		return;
	}
	cJSON_ArrayForEach(d, defs) {
		const cJSON *t = obj(d, "FilterType");
		int type;

		if (!cJSON_IsString(t))
			die("%s: FilterType expected", file);
		type = bq_type(t->valuestring);
		if (type == BYPASS)
			continue;
		if (p->n == MAXBIQ)
			die("%s: too many biquads", file);
		bq_design(&p->q[p->n++], type, num(d, "Fc", 1000),
			  num(d, "Q", M_SQRT1_2), num(d, "GaindB", 0));
	}
	cJSON_Delete(j);
}

/* ------------------------------------------------------------ MBCL */

/*
 * Peak limiter with 2 ms look-ahead, L/R linked. When the input exceeds
 * the threshold the gain drops at once and the delayed samples already in
 * the look-ahead line are scaled by a ramp, so the output never exceeds
 * it; after a 20-sample hold the gain returns linearly over the release.
 */
struct limiter {
	float T, inV, G, step;
	int R, hold, rel, i;
	float dl[2][LIM_N];
};

static void lim_init(struct limiter *m, const cJSON *d)
{
	memset(m, 0, sizeof(*m));
	m->T = db2lin(clampd(num(d, "lim_thresh", 0), -15, 0));
	m->inV = db2lin(clampd(num(d, "lim_inVol", 0), -10, 10));
	m->R = clampd(num(d, "lim_release", 180), 180, 400) * RATE / 1000;
	m->G = 1;
}

static inline void lim_run(struct limiter *m, float *l, float *r)
{
	float v0 = *l * m->G * m->inV, v1 = *r * m->G * m->inV;
	float pk = fmaxf(fabsf(v0), fabsf(v1));

	*l = m->dl[0][m->i];
	*r = m->dl[1][m->i];
	m->dl[0][m->i] = v0;
	m->dl[1][m->i] = v1;
	if (pk > m->T) {
		float g = m->T / pk;

		m->G *= g;
		for (int k = 0; k < LIM_N; k++) {
			int idx = (m->i - k + LIM_N) % LIM_N;
			float s = g + k * (1 - g) / LIM_N;

			m->dl[0][idx] *= s;
			m->dl[1][idx] *= s;
		}
		m->hold = 1;
		m->rel = 0;
		m->step = (1 - m->G) / m->R;
	} else if (m->hold >= 1) {
		if (m->hold++ >= LIM_HOLD) {
			m->hold = 0;
			m->rel = 1;
			m->G += m->step;
		}
	} else if (m->rel >= 1) {
		m->G += m->step;
		if (m->rel++ >= m->R) {
			m->rel = 0;
			m->G = 1;
			m->step = 0;
		}
	}
	m->i = (m->i + 1) % LIM_N;
}

/*
 * Compressor on 1 ms blocks: mean-square level per channel, a slow
 * envelope (attack ~44 ms, release ~436 ms) frozen when the level drops
 * suddenly by more than 38 dB, hard knee on the linked level, gain
 * smoothed per block (~0.65 s), applied to the signal 768 samples later.
 */
struct comp {
	float drive, thr_pow, ratio, gmin_db, g;
	float env[2], f[2], s[2], acc[2];
	int nacc, di;
	float dl[2][MBC_DELAY];
};

static void comp_init(struct comp *c, const cJSON *d)
{
	memset(c, 0, sizeof(*c));
	c->drive = db2lin(clampd(num(d, "comp_inVol", 0), -20, 15));
	c->thr_pow = pow(10.0, clampd(num(d, "comp_thresh", 0), -90, 0) / 10.0);
	c->ratio = clampd(num(d, "comp_ratio", 1), 1, 20);
	c->gmin_db = clampd(num(d, "comp_gainMin", -40), -40, 0);
	c->g = 1;
	c->env[0] = c->env[1] = 0.01f;
}

static inline void comp_run(struct comp *c, float *l, float *r)
{
	float u0 = *l * c->drive, u1 = *r * c->drive;

	*l = c->g * c->dl[0][c->di];
	*r = c->g * c->dl[1][c->di];
	c->dl[0][c->di] = u0;
	c->dl[1][c->di] = u1;
	c->di = (c->di + 1) % MBC_DELAY;
	c->acc[0] += u0 * u0;
	c->acc[1] += u1 * u1;
	if (++c->nacc < MBC_BLOCK)
		return;
	for (int ch = 0; ch < 2; ch++) {
		float p = c->acc[ch] / MBC_BLOCK;

		c->f[ch] += (p - c->f[ch]) / 96;
		c->s[ch] += (c->f[ch] < c->s[ch] ? 4.1667e-4f : 4.1667e-5f) *
			    (c->f[ch] - c->s[ch]);
		if (c->f[ch] > 1.44e-4f * c->s[ch])
			c->env[ch] += (p > c->env[ch] ? 0.0229167f : 0.00229167f) *
				      (p - c->env[ch]);
		c->acc[ch] = 0;
	}
	c->nacc = 0;
	{
		float L = 0.5f * (c->env[0] + c->env[1]), target = 1;

		if (L > c->thr_pow) {
			float gdb = (1 - 1 / c->ratio) * 10 * log10f(c->thr_pow / L);

			target = db2lin(fmaxf(gdb, c->gmin_db));
		}
		c->g += 0.00152778f * (target - c->g);
	}
}

struct mbcl {
	int enabled;
	float inVol;
	/* filterbank: split k has LP and HP pairs of Butterworth sections */
	struct biquad lp[3][2], hp[3][2];
	struct biquad ap1[2], ap2;	/* band 1: AP(fc2), AP(fc3); band 2: AP(fc3) */
	struct comp comp[4];
	struct limiter lim[4], full;
};

static void ap_design(struct biquad *q, double fc)
{
	struct biquad b;

	bq_design(&b, LOW_PASS, fc, M_SQRT1_2, 0);
	bq_set(q, b.a2, b.a1, 1, b.a1, b.a2);
}

static void mbcl_load(struct mbcl *m, const char *file)
{
	cJSON *j = load_json(file);
	const cJSON *fc = obj(j, "FilterBank FC"), *bands = obj(j, "Bands Definition");
	double f[3];

	if (flag(j, "Bypass", 0)) {
		cJSON_Delete(j);
		return;
	}
	if (!flag(j, "PreFilterBypass", 1))
		fprintf(stderr, "lidar-dsp: %s: PreFilter not supported, left out\n", file);
	if (cJSON_GetArraySize(fc) != 3 || cJSON_GetArraySize(bands) != 4)
		die("%s: 4 bands and 3 cut-off frequencies expected", file);
	for (int k = 0; k < 3; k++) {
		f[k] = cJSON_GetArrayItem(fc, k)->valuedouble;
		for (int s = 0; s < 2; s++) {
			bq_design(&m->lp[k][s], LOW_PASS, f[k], M_SQRT1_2, 0);
			bq_design(&m->hp[k][s], HIGH_PASS, f[k], M_SQRT1_2, 0);
		}
	}
	ap_design(&m->ap1[0], f[1]);
	ap_design(&m->ap1[1], f[2]);
	ap_design(&m->ap2, f[2]);
	m->inVol = db2lin(clampd(num(j, "inVol", 0), -15, 20));
	for (int b = 0; b < 4; b++) {
		comp_init(&m->comp[b], cJSON_GetArrayItem(bands, b));
		lim_init(&m->lim[b], cJSON_GetArrayItem(bands, b));
	}
	lim_init(&m->full, obj(j, "Full-band limiter"));
	m->enabled = 1;
	cJSON_Delete(j);
}

static inline float split(struct biquad q[2], int c, float x)
{
	return bq_run(&q[1], c, bq_run(&q[0], c, x));
}

static void mbcl_run(struct mbcl *m, float *l, float *r)
{
	for (int n = 0; n < FRAME; n++) {
		float b[4][2], x[2] = { l[n] * m->inVol, r[n] * m->inVol };
		float out[2] = { 0, 0 };

		for (int c = 0; c < 2; c++) {
			float hi;

			b[0][c] = split(m->lp[0], c, x[c]);
			hi = split(m->hp[0], c, x[c]);
			b[1][c] = split(m->lp[1], c, hi);
			hi = split(m->hp[1], c, hi);
			b[2][c] = split(m->lp[2], c, hi);
			b[3][c] = split(m->hp[2], c, hi);
			b[0][c] = bq_run(&m->ap1[1], c, bq_run(&m->ap1[0], c, b[0][c]));
			b[1][c] = bq_run(&m->ap2, c, b[1][c]);
		}
		for (int k = 0; k < 4; k++) {
			comp_run(&m->comp[k], &b[k][0], &b[k][1]);
			lim_run(&m->lim[k], &b[k][0], &b[k][1]);
			out[0] += b[k][0];
			out[1] += b[k][1];
		}
		lim_run(&m->full, &out[0], &out[1]);
		l[n] = out[0];
		r[n] = out[1];
	}
}

/* ------------------------------------------------------------ chain */

static struct fir fir;
static struct peq peq;
static struct mbcl mbcl;
static float gain = 1, gain_target = 1;

static void chain_load(void)
{
	cJSON *afe = load_json("AFE.cfg");
	const cJSON *algs = obj(obj(obj(afe, "Path Definition"), "Playback"), "Algorithms");
	const cJSON *defs = obj(afe, "Algorithm Definition"), *a;

	if (!algs || !defs)
		die("%s", "AFE.cfg: no Playback path");
	fft_init();
	cJSON_ArrayForEach(a, algs) {
		const cJSON *def;

		if (!cJSON_IsString(a))
			continue;
		def = obj(defs, a->valuestring);
		if (!strcmp(a->string, "EQ") && def && !flag(def, "Bypass", 0)) {
			fir_load(&fir, def);
		} else if (!strcmp(a->string, "ParametricEQ") && def) {
			const cJSON *f = obj(def, "External Config");

			if (cJSON_IsString(f))
				peq_load(&peq, f->valuestring);
		} else if (!strcmp(a->string, "MBCL") && def) {
			/* stock starts in the last (highest) speaker power mode */
			const cJSON *f = obj(def, "External Config");
			const cJSON *last = cJSON_GetArrayItem(f, cJSON_GetArraySize(f) - 1);

			if (cJSON_IsString(f))
				last = f;
			if (cJSON_IsString(last))
				mbcl_load(&mbcl, last->valuestring);
		}
	}
	cJSON_Delete(afe);
	fprintf(stderr, "lidar-dsp: FIR %s (%d zones), %d PEQ biquads, MBCL %s\n",
		fir.enabled ? "on" : "off", fir.nzones, peq.n, mbcl.enabled ? "on" : "off");
}

/*
 * Volume, from lidar-volume: the level in dB re the codec's 0 dB (stock's
 * full scale), the digital gain to apply (the codec sits at the safety
 * cap) and the mute flag.
 */
static void set_volume(double abs_db, double gain_db, int mute)
{
	if (fir.enabled)
		fir_volume(&fir, abs_db);
	/* never above the codec level (the safety cap) */
	gain_target = mute ? 0 : db2lin(fmin(gain_db, 0));
}

/* in/out: FRAME stereo float samples, interleaved */
static void chain_run(float *io)
{
	float l[FRAME], r[FRAME];
	/* 5 ms volume ramp per sample */
	const float k = 1 - expf(-1.0f / (0.005f * RATE));

	for (int n = 0; n < FRAME; n++) {
		gain += k * (gain_target - gain);
		l[n] = io[2 * n] * gain;
		r[n] = io[2 * n + 1] * gain;
	}
	if (fir.enabled)
		fir_run(&fir, l, r);
	for (int i = 0; i < peq.n; i++)
		for (int n = 0; n < FRAME; n++) {
			l[n] = bq_run(&peq.q[i], 0, l[n]);
			r[n] = bq_run(&peq.q[i], 1, r[n]);
		}
	if (mbcl.enabled)
		mbcl_run(&mbcl, l, r);
	for (int n = 0; n < FRAME; n++) {
		/* OutputTrim clips; one internal speaker gets (L+R)/2 */
		float m = 0.5f * (clampd(l[n], -1, 0.99997) + clampd(r[n], -1, 0.99997));

		io[2 * n] = io[2 * n + 1] = m;
	}
}

/* ------------------------------------------------------------ offline */

static int run_offline(double abs_db, double gain_db)
{
	float buf[2 * FRAME];
	size_t n;

	set_volume(abs_db, gain_db, 0);
	gain = gain_target;
	while ((n = fread(buf, sizeof(float) * 2, FRAME, stdin)) > 0) {
		if (n < FRAME)
			memset(buf + 2 * n, 0, (FRAME - n) * 2 * sizeof(float));
		chain_run(buf);
		if (fwrite(buf, sizeof(float) * 2, FRAME, stdout) != FRAME)
			return 1;
	}
	return 0;
}

/* ------------------------------------------------------------ live */

static int set_hw(snd_pcm_t *pcm, snd_pcm_format_t fmt, snd_pcm_uframes_t buffer)
{
	snd_pcm_hw_params_t *hw;
	snd_pcm_sw_params_t *sw;
	snd_pcm_uframes_t period = FRAME;
	int err;

	snd_pcm_hw_params_alloca(&hw);
	snd_pcm_sw_params_alloca(&sw);
	if ((err = snd_pcm_hw_params_any(pcm, hw)) < 0 ||
	    (err = snd_pcm_hw_params_set_access(pcm, hw, SND_PCM_ACCESS_RW_INTERLEAVED)) < 0 ||
	    (err = snd_pcm_hw_params_set_format(pcm, hw, fmt)) < 0 ||
	    (err = snd_pcm_hw_params_set_channels(pcm, hw, 2)) < 0 ||
	    (err = snd_pcm_hw_params_set_rate(pcm, hw, RATE, 0)) < 0 ||
	    (err = snd_pcm_hw_params_set_period_size(pcm, hw, period, 0)) < 0 ||
	    (err = snd_pcm_hw_params_set_buffer_size_near(pcm, hw, &buffer)) < 0 ||
	    (err = snd_pcm_hw_params(pcm, hw)) < 0)
		return err;
	/* started explicitly, once the playback side is primed */
	if ((err = snd_pcm_sw_params_current(pcm, sw)) < 0 ||
	    (err = snd_pcm_sw_params_set_start_threshold(pcm, sw, LONG_MAX)) < 0 ||
	    (err = snd_pcm_sw_params(pcm, sw)) < 0)
		return err;
	return 0;
}

static int read_volume(const char *file)
{
	FILE *f = fopen(file, "r");
	double abs_db, gain_db;
	int mute, ok;

	if (!f)
		return 0;
	ok = fscanf(f, "%lf %lf %d", &abs_db, &gain_db, &mute) == 3;
	if (ok)
		set_volume(abs_db, gain_db, mute);
	fclose(f);
	return ok;
}

/* lidar-volume moves the codec to the safety cap once the DSP runs */
static void announce(const char *ready, int up)
{
	int fd;

	if (up) {
		fd = open(ready, O_WRONLY | O_CREAT, 0644);
		if (fd >= 0)
			close(fd);
	} else {
		unlink(ready);
	}
	fd = open("/run/lidar-volume.fifo", O_WRONLY | O_NONBLOCK);
	if (fd >= 0) {
		if (write(fd, "refresh\n", 8) != 8)
			fprintf(stderr, "lidar-dsp: cannot notify lidar-volume\n");
		close(fd);
	}
}

static volatile sig_atomic_t stop;

static void on_signal(int sig)
{
	(void)sig;
	stop = 1;
}

/*
 * The loopback only advances on the codec's period ticks (snd-aloop's
 * timer_source), so the codec write paces the loop: a blocking write of one
 * frame, then whatever the loopback has, topped up with silence. A blocking
 * read could wait forever if the codec stalled.
 */
static int run_live(const char *cap, const char *play, const char *volfile,
		    const char *ready)
{
	snd_pcm_t *in = NULL, *out = NULL;
	snd_pcm_format_t fmt = SND_PCM_FORMAT_S32_LE;
	static short acc[2 * 4 * FRAME];
	static int o32[2 * FRAME];
	static short o16[2 * FRAME];
	float buf[2 * FRAME];
	int have = 0, err, announced = 0;
	unsigned int frames = 0;
	struct sched_param sp = { .sched_priority = 50 };

	signal(SIGTERM, on_signal);
	signal(SIGINT, on_signal);
	unlink(ready);
	/* never a sample before the volume is known */
	while (!read_volume(volfile)) {
		if (stop)
			return 0;
		usleep(100000);
	}
	gain = 0;	/* fade in */
	if ((err = snd_pcm_open(&out, play, SND_PCM_STREAM_PLAYBACK, 0)) < 0)
		goto fail;
	if (set_hw(out, fmt, 4 * FRAME) < 0) {
		fmt = SND_PCM_FORMAT_S16_LE;
		if ((err = set_hw(out, fmt, 4 * FRAME)) < 0)
			goto fail;
	}
	/* the first opener of the cable fixes its parameters: dmix may hold it */
	while ((err = snd_pcm_open(&in, cap, SND_PCM_STREAM_CAPTURE, SND_PCM_NONBLOCK)) < 0 ||
	       (err = set_hw(in, SND_PCM_FORMAT_S16_LE, 8 * FRAME)) < 0) {
		if (in)
			snd_pcm_close(in);
		in = NULL;
		if (stop)
			return 0;
		fprintf(stderr, "lidar-dsp: %s: %s, retrying\n", cap, snd_strerror(err));
		sleep(1);
	}
	mlockall(MCL_CURRENT | MCL_FUTURE);
	if (sched_setscheduler(0, SCHED_FIFO, &sp) < 0)
		fprintf(stderr, "lidar-dsp: no SCHED_FIFO\n");
	memset(o32, 0, sizeof(o32));
	memset(o16, 0, sizeof(o16));
restart:
	for (int i = 0; i < 2; i++)
		snd_pcm_writei(out, fmt == SND_PCM_FORMAT_S32_LE ? (void *)o32 : (void *)o16, FRAME);
	snd_pcm_start(out);
	snd_pcm_drop(in);
	snd_pcm_prepare(in);
	snd_pcm_start(in);
	have = 0;
	while (!stop) {
		snd_pcm_sframes_t n;

		for (int tries = 0; tries < 2 && have < FRAME; tries++) {
			n = snd_pcm_readi(in, acc + 2 * have, 4 * FRAME - have);
			if (n == -EAGAIN) {
				if (tries == 0)
					snd_pcm_wait(in, 4);
				continue;
			}
			if (n < 0) {	/* capture trouble: silence meanwhile */
				snd_pcm_drop(in);
				snd_pcm_prepare(in);
				snd_pcm_start(in);
				break;
			}
			have += n;
		}
		if (have < FRAME)
			memset(acc + 2 * have, 0, (FRAME - have) * 2 * sizeof(short));
		for (int i = 0; i < 2 * FRAME; i++)
			buf[i] = acc[i] / 32768.0f;
		have = have > FRAME ? have - FRAME : 0;
		memmove(acc, acc + 2 * FRAME, have * 2 * sizeof(short));
		if (++frames % 2 == 0)	/* 16 ms: volume and mute act at once */
			read_volume(volfile);
		chain_run(buf);
		if (fmt == SND_PCM_FORMAT_S32_LE)
			for (int i = 0; i < 2 * FRAME; i++)
				o32[i] = lrintf(buf[i] * 2147483392.0f);
		else
			for (int i = 0; i < 2 * FRAME; i++)
				o16[i] = lrintf(buf[i] * 32767.0f);
		n = snd_pcm_writei(out, fmt == SND_PCM_FORMAT_S32_LE ? (void *)o32 : (void *)o16, FRAME);
		if (n < 0) {
			fprintf(stderr, "lidar-dsp: playback: %s\n", snd_strerror(n));
			if (snd_pcm_prepare(out) < 0)
				break;
			memset(o32, 0, sizeof(o32));
			memset(o16, 0, sizeof(o16));
			goto restart;
		}
		if (!announced) {
			announce(ready, 1);
			announced = 1;
		}
	}
	announce(ready, 0);
	snd_pcm_close(in);
	snd_pcm_close(out);
	return 0;
fail:
	fprintf(stderr, "lidar-dsp: %s: %s\n", play, snd_strerror(err));
	return 1;
}

int main(int argc, char **argv)
{
	const char *cap = "hw:Loopback,1,0", *play = "hw:amazonlidar,0";
	const char *volfile = "/run/lidar-dsp/volume", *ready = "/run/lidar-dsp/ready";
	int opt, offline = 0, check = 0;

	while ((opt = getopt(argc, argv, "c:i:o:v:r:fk")) != -1) {
		switch (opt) {
		case 'c': cfgdir = optarg; break;
		case 'i': cap = optarg; break;
		case 'o': play = optarg; break;
		case 'v': volfile = optarg; break;
		case 'r': ready = optarg; break;
		case 'f': offline = 1; break;
		case 'k': check = 1; break;
		default: return 2;
		}
	}
	/* flush denormals to zero: decaying filter tails on silence */
	{
		unsigned long fpcr;

		__asm__ volatile("mrs %0, fpcr" : "=r"(fpcr));
		__asm__ volatile("msr fpcr, %0" : : "r"(fpcr | (1UL << 24)));
	}
	chain_load();
	if (check)
		return 0;
	if (offline) {
		if (argc - optind != 2)
			return 2;
		return run_offline(atof(argv[optind]), atof(argv[optind + 1]));
	}
	return run_live(cap, play, volfile, ready);
}
