/*
 ============================================================================
 Name        : tetris-together.c
 Description : Multiplayer console tetris for GNU/Linux with btop style graphics
 ============================================================================
 */

#define VERSION "0.1.0"

#define _DEFAULT_SOURCE   // for: getaddrinfo, getifaddrs, clock_gettime
#include <stdio.h>        // defines: printf, snprintf, fprintf
#include <stdlib.h>       // defines: malloc, rand, srand, atoi
#include <string.h>       // defines: memset, memcpy, strcmp
#include <stdarg.h>       // defines: va_list
#include <stdbool.h>      // defines: true, false
#include <stdint.h>       // defines: uint8_t, uint32_t
#include <math.h>         // defines: pow
#include <unistd.h>       // defines: read, write, close
#include <termios.h>      // defines: termios, TCSANOW, ICANON, ECHO
#include <signal.h>       // defines: signal, SIGINT, SIGWINCH
#include <time.h>         // defines: clock_gettime, localtime
#include <poll.h>         // defines: poll
#include <errno.h>        // defines: errno
#include <sys/ioctl.h>    // defines: ioctl, TIOCGWINSZ
#include <sys/socket.h>   // defines: socket, bind, listen, accept
#include <netinet/in.h>   // defines: sockaddr_in
#include <netinet/tcp.h>  // defines: TCP_NODELAY
#include <arpa/inet.h>    // defines: inet_ntop
#include <netdb.h>        // defines: getaddrinfo
#include <ifaddrs.h>      // defines: getifaddrs

#define MAXP 3          // players in one game
#define BW 10           // board width
#define BH 22           // board height (20 visible + 2 hidden)
#define HIDDEN 2        // hidden rows on top of the board
#define NEXTN 5         // pieces in the next queue
#define HISTN 256       // samples kept for the graphs
#define LOCK_MS 500.0   // lock delay
#define CLEAR_MS 260.0  // line clear animation
#define COUNT_MS 1800.0 // countdown before a round
#define SAMPLE_MS 500.0 // graph sample interval
#define KO_MS 5000.0    // a knocked out opponent stays on screen this long
#define DEFAULT_PORT 9471

enum { KIND_NONE, KIND_HUMAN, KIND_BOT, KIND_REMOTE };
enum { ST_LOBBY, ST_COUNTDOWN, ST_PLAYING, ST_OVER };
enum { MODE_OFFLINE, MODE_HOST, MODE_CLIENT };
enum { CELL_EMPTY, CELL_SOLID, CELL_GHOST };
enum { KEY_UP = 1000, KEY_DOWN, KEY_LEFT, KEY_RIGHT };
enum { MSG_HELLO = 1, MSG_WELCOME, MSG_FULL, MSG_ROSTER, MSG_START, MSG_STATE, MSG_GARBAGE, MSG_LEAVE };

// colors are 0xRRGGBB, DEF means "terminal default"
#define DEF 0xFF000000u

// pieces: 1=I 2=O 3=T 4=S 5=Z 6=J 7=L 8=garbage
static const uint32_t pieceColor[9] = {0, 0x3fd0e4, 0xf2d14b, 0xb36cf2, 0x6cd86a, 0xee5a5a, 0x5a7cf2, 0xf29b3a, 0x6a6a78};

// the btop default theme
#define C_MAIN 0xccccccu
#define C_TITLE 0xeeeeeeu
#define C_HI 0xb54040u
#define C_INACTIVE 0x404040u
#define C_DIV 0x303030u
#define C_LABEL 0x8a8a8au
#define C_GRID 0x353535u
#define C_CPU_BOX 0x556d59u
#define C_MEM_BOX 0x6c6c4bu
#define C_NET_BOX 0x5c588du
#define C_PROC_BOX 0x805252u
static const uint32_t gradCpu[3] = {0x50f095, 0xa6d048, 0xdc4c4c};
static const uint32_t gradProc[3] = {0x80d0a3, 0xdcd179, 0xd45454};
static const uint32_t gradNet[3] = {0x6c87d0, 0x8a6cd0, 0xd06cb5};
static const uint32_t gradTemp[3] = {0x4897d4, 0x5474e8, 0xff40b6};

typedef struct
{
	int kind;
	bool present, inRound, alive;
	char name[16];
	uint8_t board[BH][BW];
	// active piece
	int type, rot, x, y;
	bool active;
	int hold;
	bool holdUsed;
	uint8_t queue[32];
	int qlen;
	uint32_t rng, garbRng;
	// scoring
	uint32_t score;
	int lines, level, combo;
	bool b2b;
	int pending;
	// timing
	double gravAcc, lockTimer;
	int lockResets, lowestY;
	bool lastRotate;
	int lastKick;
	// stats
	int pieces, attack, targetRR, samplePieces;
	double koTime;
	double playTime;
	float apm, pps, ppsNow;
	float histPps[HISTN], histHeight[HISTN];
	int histLen;
	// line clear animation
	int clearRows[4], clearCount;
	bool clearing;
	double clearTimer;
	// action text ("TETRIS", "B2B")
	char action[2][24];
	uint32_t actionCol;
	double actionTime;
	// bot
	bool planned;
	int planRot, planDx;
	double botTimer;
} Player;

typedef struct
{
	int fd;
	uint8_t buf[8192];
	int len;
} Conn;

static struct
{
	int mode, state, myId, winner, nBots, startLevel, port;
	double stateTime, botDelay, botNoise;
	bool paused, trueColor;
	char status[64], hostAddr[64];
} G;

static Player players[MAXP];
static Conn conns[MAXP];
static int listenFd = -1;
static volatile sig_atomic_t running = 1, resized = 1;

static int8_t shapes[8][4][4][2];
static const int8_t spawnShape[8][4][2] = {
	{{0}},
	{{0, 1}, {1, 1}, {2, 1}, {3, 1}}, // I
	{{1, 0}, {2, 0}, {1, 1}, {2, 1}}, // O
	{{1, 0}, {0, 1}, {1, 1}, {2, 1}}, // T
	{{1, 0}, {2, 0}, {0, 1}, {1, 1}}, // S
	{{0, 0}, {1, 0}, {1, 1}, {2, 1}}, // Z
	{{0, 0}, {0, 1}, {1, 1}, {2, 1}}, // J
	{{2, 0}, {0, 1}, {1, 1}, {2, 1}}, // L
};

// SRS kick tables (y points up), index is rotation*2 + (cw ? 0 : 1)
static const int8_t kickJLSTZ[8][5][2] = {
	{{0, 0}, {-1, 0}, {-1, 1}, {0, -2}, {-1, -2}}, // 0->R
	{{0, 0}, {1, 0}, {1, 1}, {0, -2}, {1, -2}},	   // 0->L
	{{0, 0}, {1, 0}, {1, -1}, {0, 2}, {1, 2}},	   // R->2
	{{0, 0}, {1, 0}, {1, -1}, {0, 2}, {1, 2}},	   // R->0
	{{0, 0}, {1, 0}, {1, 1}, {0, -2}, {1, -2}},	   // 2->L
	{{0, 0}, {-1, 0}, {-1, 1}, {0, -2}, {-1, -2}}, // 2->R
	{{0, 0}, {-1, 0}, {-1, -1}, {0, 2}, {-1, 2}},  // L->0
	{{0, 0}, {-1, 0}, {-1, -1}, {0, 2}, {-1, 2}},  // L->2
};
static const int8_t kickI[8][5][2] = {
	{{0, 0}, {-2, 0}, {1, 0}, {-2, -1}, {1, 2}}, // 0->R
	{{0, 0}, {-1, 0}, {2, 0}, {-1, 2}, {2, -1}}, // 0->L
	{{0, 0}, {-1, 0}, {2, 0}, {-1, 2}, {2, -1}}, // R->2
	{{0, 0}, {2, 0}, {-1, 0}, {2, 1}, {-1, -2}}, // R->0
	{{0, 0}, {2, 0}, {-1, 0}, {2, 1}, {-1, -2}}, // 2->L
	{{0, 0}, {1, 0}, {-2, 0}, {1, -2}, {-2, 1}}, // 2->R
	{{0, 0}, {1, 0}, {-2, 0}, {1, -2}, {-2, 1}}, // L->0
	{{0, 0}, {-2, 0}, {1, 0}, {-2, -1}, {1, 2}}, // L->2
};
static const int8_t kick180[5][2] = {{0, 0}, {1, 0}, {-1, 0}, {0, 1}, {0, -1}};

static void sendAttack(Player *p, int lines);

/* ------------------------------------------------------------------------ */
/* utilities                                                                */
/* ------------------------------------------------------------------------ */

static double nowMs(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

static uint32_t xorshift(uint32_t *s)
{
	uint32_t x = *s ? *s : 0x9e3779b9;
	x ^= x << 13;
	x ^= x >> 17;
	x ^= x << 5;
	return *s = x;
}

static uint32_t lerpColor(uint32_t a, uint32_t b, double t)
{
	if (t < 0)
		t = 0;
	if (t > 1)
		t = 1;
	int r = ((a >> 16) & 255) + (((int)((b >> 16) & 255) - (int)((a >> 16) & 255)) * t);
	int g = ((a >> 8) & 255) + (((int)((b >> 8) & 255) - (int)((a >> 8) & 255)) * t);
	int bl = (a & 255) + (((int)(b & 255) - (int)(a & 255)) * t);
	return (uint32_t)(r << 16 | g << 8 | bl);
}

static uint32_t gradient(const uint32_t g[3], double t)
{
	if (t < 0.5)
		return lerpColor(g[0], g[1], t * 2);
	return lerpColor(g[1], g[2], (t - 0.5) * 2);
}

static uint32_t grayOf(uint32_t c)
{
	int l = (((c >> 16) & 255) * 3 + ((c >> 8) & 255) * 6 + (c & 255)) / 10;
	l = 40 + l / 3;
	return (uint32_t)(l << 16 | l << 8 | l);
}

static int utf8len(const char *s)
{
	int n = 0;
	for (; *s; s++)
		if ((*s & 0xC0) != 0x80)
			n++;
	return n;
}

static void put16(uint8_t *d, int v)
{
	d[0] = v >> 8;
	d[1] = v;
}

static void put32(uint8_t *d, uint32_t v)
{
	d[0] = v >> 24;
	d[1] = v >> 16;
	d[2] = v >> 8;
	d[3] = v;
}

static int get16(const uint8_t *d)
{
	return d[0] << 8 | d[1];
}

static uint32_t get32(const uint8_t *d)
{
	return (uint32_t)d[0] << 24 | (uint32_t)d[1] << 16 | (uint32_t)d[2] << 8 | d[3];
}

/* ------------------------------------------------------------------------ */
/* screen buffer, only changed cells are written to the terminal            */
/* ------------------------------------------------------------------------ */

typedef struct
{
	uint32_t ch, fg, bg;
} Cell;

static Cell *scr, *old;
static int scrW, scrH;
static char *out;
static size_t outLen, outCap;

static void emit(const char *s, size_t n)
{
	if (outLen + n > outCap)
	{
		outCap = (outLen + n) * 2;
		out = realloc(out, outCap);
	}
	memcpy(out + outLen, s, n);
	outLen += n;
}

static void emitf(const char *fmt, ...)
{
	char buf[64];
	va_list ap;
	va_start(ap, fmt);
	int n = vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	emit(buf, n);
}

static int to256(uint32_t c)
{
	int r = (c >> 16) & 255, g = (c >> 8) & 255, b = c & 255;
	int mx = r > g ? (r > b ? r : b) : (g > b ? g : b);
	int mn = r < g ? (r < b ? r : b) : (g < b ? g : b);
	if (mx - mn < 12)
	{
		if (r < 8)
			return 16;
		if (r > 238)
			return 231;
		return 232 + (r - 8) * 24 / 231;
	}
	int q[3] = {r, g, b};
	for (int i = 0; i < 3; i++)
		q[i] = q[i] < 48 ? 0 : q[i] < 115 ? 1 : (q[i] - 35) / 40;
	return 16 + 36 * q[0] + 6 * q[1] + q[2];
}

static void emitColor(uint32_t c, bool bg)
{
	if (c == DEF)
		emit(bg ? "\033[49m" : "\033[39m", 5);
	else if (G.trueColor)
		emitf("\033[%d;2;%u;%u;%um", bg ? 48 : 38, (c >> 16) & 255, (c >> 8) & 255, c & 255);
	else
		emitf("\033[%d;5;%dm", bg ? 48 : 38, to256(c));
}

static void emitChar(uint32_t c)
{
	char b[4];
	if (c < 0x80)
	{
		b[0] = c;
		emit(b, 1);
	}
	else if (c < 0x800)
	{
		b[0] = 0xC0 | c >> 6;
		b[1] = 0x80 | (c & 0x3F);
		emit(b, 2);
	}
	else
	{
		b[0] = 0xE0 | c >> 12;
		b[1] = 0x80 | ((c >> 6) & 0x3F);
		b[2] = 0x80 | (c & 0x3F);
		emit(b, 3);
	}
}

static void put(int x, int y, uint32_t ch, uint32_t fg, uint32_t bg)
{
	if (x < 0 || y < 0 || x >= scrW || y >= scrH)
		return;
	Cell *c = &scr[y * scrW + x];
	c->ch = ch;
	c->fg = fg;
	c->bg = bg;
}

static int text(int x, int y, const char *s, uint32_t fg, uint32_t bg)
{
	const unsigned char *p = (const unsigned char *)s;
	int n = 0;
	while (*p)
	{
		uint32_t c;
		if (*p < 0x80)
			c = *p++;
		else if ((*p & 0xE0) == 0xC0)
		{
			c = (p[0] & 0x1F) << 6 | (p[1] & 0x3F);
			p += 2;
		}
		else
		{
			c = (p[0] & 0x0F) << 12 | (p[1] & 0x3F) << 6 | (p[2] & 0x3F);
			p += 3;
		}
		put(x + n++, y, c, fg, bg);
	}
	return n;
}

static void textCenter(int x, int y, int w, const char *s, uint32_t fg)
{
	text(x + (w - utf8len(s)) / 2, y, s, fg, DEF);
}

static void screenResize(void)
{
	struct winsize ws;
	if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) < 0 || ws.ws_col == 0)
	{
		ws.ws_col = 80;
		ws.ws_row = 24;
	}
	scrW = ws.ws_col;
	scrH = ws.ws_row;
	scr = realloc(scr, sizeof(Cell) * scrW * scrH);
	old = realloc(old, sizeof(Cell) * scrW * scrH);
	// invalidate the previous frame so everything is redrawn
	memset(old, 0xFF, sizeof(Cell) * scrW * scrH);
	if (write(STDOUT_FILENO, "\033[m\033[2J", 7) < 0)
		running = 0;
}

static void screenClear(void)
{
	for (int i = 0; i < scrW * scrH; i++)
		scr[i] = (Cell){' ', DEF, DEF};
}

static void screenFlush(void)
{
	uint32_t fg = 0xFFFFFFFF, bg = 0xFFFFFFFF;
	int cx = -1, cy = -1;
	outLen = 0;
	emit("\033[?2026h", 8); // synchronized output, avoids tearing
	for (int y = 0; y < scrH; y++)
		for (int x = 0; x < scrW; x++)
		{
			Cell *c = &scr[y * scrW + x], *o = &old[y * scrW + x];
			if (c->ch == o->ch && c->fg == o->fg && c->bg == o->bg)
				continue;
			if (cx != x || cy != y)
				emitf("\033[%d;%dH", y + 1, x + 1);
			if (c->fg != fg)
				emitColor(fg = c->fg, false);
			if (c->bg != bg)
				emitColor(bg = c->bg, true);
			emitChar(c->ch);
			*o = *c;
			cx = x + 1;
			cy = y;
		}
	emit("\033[?2026l", 8);
	for (size_t done = 0; done < outLen;)
	{
		ssize_t n = write(STDOUT_FILENO, out + done, outLen - done);
		if (n <= 0)
		{
			if (errno == EINTR || errno == EAGAIN)
				continue;
			break;
		}
		done += n;
	}
}

/* ------------------------------------------------------------------------ */
/* btop style widgets                                                       */
/* ------------------------------------------------------------------------ */

// rounded box with the title embedded in the top border: ╭─┐title┌───╮
static void box(int x, int y, int w, int h, uint32_t col, const char *title)
{
	for (int j = y; j < y + h; j++)
		for (int i = x; i < x + w; i++)
		{
			uint32_t ch = ' ';
			if (j == y || j == y + h - 1)
				ch = 0x2500;
			else if (i == x || i == x + w - 1)
				ch = 0x2502;
			put(i, j, ch, col, DEF);
		}
	put(x, y, 0x256D, col, DEF);
	put(x + w - 1, y, 0x256E, col, DEF);
	put(x, y + h - 1, 0x2570, col, DEF);
	put(x + w - 1, y + h - 1, 0x256F, col, DEF);
	if (title && *title)
	{
		put(x + 2, y, 0x2510, col, DEF);
		int n = text(x + 3, y, title, C_TITLE, DEF);
		put(x + 3 + n, y, 0x250C, col, DEF);
	}
}

// secondary title on the right side of a box border
static void boxTitleRight(int x, int y, int w, const char *title, uint32_t fg, uint32_t col)
{
	int n = utf8len(title);
	int tx = x + w - 3 - n;
	put(tx - 1, y, 0x2510, col, DEF);
	text(tx, y, title, fg, DEF);
	put(tx + n, y, 0x250C, col, DEF);
}

// horizontal meter made of ■, colored by a gradient
static void meter(int x, int y, int w, double v, const uint32_t g[3])
{
	int fill = (int)(v * w + 0.5);
	for (int i = 0; i < w; i++)
		put(x + i, y, 0x25A0, i < fill ? gradient(g, w > 1 ? (double)i / (w - 1) : 1) : C_INACTIVE, DEF);
}

// filled braille graph, two samples per character and four dots per row
static void graph(int x, int y, int w, int h, const float *data, int n, const uint32_t g[3])
{
	static const uint8_t left[4] = {0x40, 0x04, 0x02, 0x01}, right[4] = {0x80, 0x20, 0x10, 0x08};
	for (int c = 0; c < w; c++)
	{
		int il = n - 2 * (w - c), ir = il + 1;
		float vl = il >= 0 ? data[il] : -1, vr = ir >= 0 ? data[ir] : -1;
		int dl = vl < 0 ? 0 : (int)(vl * h * 4 + 0.5), dr = vr < 0 ? 0 : (int)(vr * h * 4 + 0.5);
		if (vl > 0.001 && dl == 0)
			dl = 1;
		if (vr > 0.001 && dr == 0)
			dr = 1;
		for (int r = 0; r < h; r++)
		{
			int base = (h - 1 - r) * 4;
			int a = dl - base, b = dr - base;
			uint8_t bits = 0;
			for (int k = 0; k < 4; k++)
			{
				if (k < a)
					bits |= left[k];
				if (k < b)
					bits |= right[k];
			}
			uint32_t col = gradient(g, h > 1 ? 1.0 - (double)r / (h - 1) : 0.5);
			put(x + c, y + r, bits ? 0x2800 + bits : ' ', col, DEF);
		}
	}
}

static void keyValue(int x, int y, int w, const char *label, const char *value, uint32_t col)
{
	text(x, y, label, C_LABEL, DEF);
	text(x + w - utf8len(value), y, value, col, DEF);
}

/* ------------------------------------------------------------------------ */
/* game rules: SRS rotation, 7-bag, hold, lock delay, garbage               */
/* ------------------------------------------------------------------------ */

static void initShapes(void)
{
	for (int t = 1; t <= 7; t++)
	{
		int n = t == 1 ? 4 : 3;
		memcpy(shapes[t][0], spawnShape[t], sizeof(spawnShape[t]));
		for (int r = 1; r < 4; r++)
			for (int i = 0; i < 4; i++)
			{
				if (t == 2)
				{
					shapes[t][r][i][0] = shapes[t][0][i][0];
					shapes[t][r][i][1] = shapes[t][0][i][1];
					continue;
				}
				// clockwise rotation inside the n*n box
				shapes[t][r][i][0] = n - 1 - shapes[t][r - 1][i][1];
				shapes[t][r][i][1] = shapes[t][r - 1][i][0];
			}
	}
}

static bool fits(uint8_t b[BH][BW], int t, int r, int x, int y)
{
	for (int i = 0; i < 4; i++)
	{
		int cx = x + shapes[t][r][i][0], cy = y + shapes[t][r][i][1];
		if (cx < 0 || cx >= BW || cy >= BH)
			return false;
		if (cy >= 0 && b[cy][cx])
			return false;
	}
	return true;
}

static bool rotateTest(uint8_t b[BH][BW], int t, int *r, int *x, int *y, int dir, int *kick)
{
	if (t == 2)
		return false;
	int nr = (*r + (dir == 2 ? 2 : dir == 1 ? 1 : 3)) & 3;
	const int8_t(*k)[2] = dir == 2 ? kick180 : t == 1 ? kickI[*r * 2 + (dir == 1 ? 0 : 1)] : kickJLSTZ[*r * 2 + (dir == 1 ? 0 : 1)];
	for (int i = 0; i < 5; i++)
	{
		int nx = *x + k[i][0], ny = *y - k[i][1];
		if (fits(b, t, nr, nx, ny))
		{
			*r = nr;
			*x = nx;
			*y = ny;
			if (kick)
				*kick = i;
			return true;
		}
	}
	return false;
}

static void fillQueue(Player *p)
{
	while (p->qlen <= NEXTN + 1)
	{
		uint8_t bag[7] = {1, 2, 3, 4, 5, 6, 7};
		for (int i = 6; i > 0; i--)
		{
			int j = xorshift(&p->rng) % (i + 1);
			uint8_t tmp = bag[i];
			bag[i] = bag[j];
			bag[j] = tmp;
		}
		memcpy(p->queue + p->qlen, bag, 7);
		p->qlen += 7;
	}
}

static int takeNext(Player *p)
{
	int t = p->queue[0];
	memmove(p->queue, p->queue + 1, --p->qlen);
	fillQueue(p);
	return t;
}

static void setAction(Player *p, const char *a, const char *b, uint32_t col)
{
	snprintf(p->action[0], sizeof(p->action[0]), "%s", a);
	snprintf(p->action[1], sizeof(p->action[1]), "%s", b);
	p->actionCol = col;
	p->actionTime = nowMs();
}

static void die(Player *p)
{
	p->alive = false;
	p->active = false;
	p->clearing = false;
	p->koTime = nowMs();
	setAction(p, "TOP OUT", "", pieceColor[5]);
}

static void spawn(Player *p, int t)
{
	p->type = t;
	p->rot = 0;
	p->x = 3;
	p->y = t == 1 ? 0 : 1;
	p->gravAcc = 0;
	p->lockTimer = 0;
	p->lockResets = 0;
	p->lastRotate = false;
	p->planned = false;
	p->botTimer = 0;
	if (!fits(p->board, t, 0, p->x, p->y))
	{
		die(p);
		return;
	}
	if (fits(p->board, t, 0, p->x, p->y + 1))
		p->y++;
	p->lowestY = p->y;
	p->active = true;
}

static void onMoved(Player *p)
{
	if (p->y > p->lowestY)
	{
		p->lowestY = p->y;
		p->lockResets = 0;
		p->lockTimer = 0;
	}
	else if (p->lockTimer > 0 && p->lockResets < 15)
	{
		p->lockTimer = 0;
		p->lockResets++;
	}
}

static bool doMove(Player *p, int dx)
{
	if (!fits(p->board, p->type, p->rot, p->x + dx, p->y))
		return false;
	p->x += dx;
	p->lastRotate = false;
	onMoved(p);
	return true;
}

static bool doRotate(Player *p, int dir)
{
	if (!rotateTest(p->board, p->type, &p->rot, &p->x, &p->y, dir, &p->lastKick))
		return false;
	p->lastRotate = true;
	onMoved(p);
	return true;
}

static int stackHeight(uint8_t b[BH][BW])
{
	for (int y = HIDDEN; y < BH; y++)
		for (int x = 0; x < BW; x++)
			if (b[y][x])
				return BH - y;
	return 0;
}

static void applyGarbage(Player *p)
{
	int n = p->pending > 10 ? 10 : p->pending;
	if (!n)
		return;
	p->pending -= n;
	bool topOut = false;
	for (int y = 0; y < n; y++)
		for (int x = 0; x < BW; x++)
			if (p->board[y][x])
				topOut = true;
	memmove(p->board[0], p->board[n], (BH - n) * BW);
	int hole = xorshift(&p->garbRng) % BW;
	for (int y = BH - n; y < BH; y++)
		for (int x = 0; x < BW; x++)
			p->board[y][x] = x == hole ? 0 : 8;
	if (topOut)
		die(p);
}

// returns 0 for no t-spin, 1 for a mini and 2 for a full t-spin (3-corner rule)
static int detectTSpin(Player *p)
{
	static const int cx[4] = {0, 2, 0, 2}, cy[4] = {0, 0, 2, 2};
	static const int front[4][2] = {{0, 1}, {1, 3}, {2, 3}, {0, 2}};
	if (p->type != 3 || !p->lastRotate)
		return 0;
	bool occ[4];
	int n = 0;
	for (int i = 0; i < 4; i++)
	{
		int x = p->x + cx[i], y = p->y + cy[i];
		occ[i] = x < 0 || x >= BW || y >= BH || (y >= 0 && p->board[y][x]);
		n += occ[i];
	}
	if (n < 3)
		return 0;
	if (occ[front[p->rot][0]] && occ[front[p->rot][1]])
		return 2;
	return p->lastKick == 4 ? 2 : 1;
}

static void scoreClear(Player *p, int n, int tspin, bool perfect)
{
	static const int base[5] = {0, 100, 300, 500, 800};
	static const int miniBase[3] = {100, 200, 400};
	static const int tBase[4] = {400, 800, 1200, 1600};
	static const int comboAtk[12] = {0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 4, 5};
	static const char *names[5] = {"", "SINGLE", "DOUBLE", "TRIPLE", "TETRIS"};
	int pts, atk;
	if (tspin == 2)
	{
		pts = tBase[n > 3 ? 3 : n];
		atk = n * 2;
	}
	else if (tspin == 1)
	{
		pts = miniBase[n > 2 ? 2 : n];
		atk = n >= 2 ? 1 : 0;
	}
	else
	{
		pts = base[n];
		atk = n == 4 ? 4 : n > 0 ? n - 1 : 0;
	}
	char a0[24] = "", a1[24] = "";
	uint32_t col = C_TITLE;
	if (tspin)
	{
		snprintf(a0, sizeof(a0), "T-SPIN%s %s", tspin == 1 ? " MINI" : "", names[n]);
		col = pieceColor[3];
	}
	else if (n)
	{
		snprintf(a0, sizeof(a0), "%s", names[n]);
		col = n == 4 ? pieceColor[1] : C_TITLE;
	}
	if (n > 0)
	{
		bool difficult = n == 4 || tspin;
		bool chain = difficult && p->b2b;
		p->b2b = difficult;
		if (chain)
		{
			pts = pts * 3 / 2;
			atk += 1;
		}
		p->combo++;
		if (p->combo > 0)
		{
			p->score += 50 * p->combo * p->level;
			atk += comboAtk[p->combo > 11 ? 11 : p->combo];
		}
		if (perfect)
		{
			pts += 2000;
			atk += 10;
			col = pieceColor[2];
		}
		snprintf(a1, sizeof(a1), "%s%s%s", perfect ? "ALL CLEAR " : chain ? "B2B " : "",
				 p->combo > 0 ? "COMBO " : "", "");
		if (p->combo > 0)
			snprintf(a1 + strlen(a1), sizeof(a1) - strlen(a1), "x%d", p->combo);
	}
	else
		p->combo = -1;
	p->score += pts * p->level;
	if (*a0)
		setAction(p, a0, a1, col);
	if (atk > 0)
	{
		p->attack += atk;
		int cancel = atk < p->pending ? atk : p->pending;
		p->pending -= cancel;
		if (atk - cancel > 0)
			sendAttack(p, atk - cancel);
	}
	p->lines += n;
	p->level = G.startLevel + p->lines / 10;
	if (p->level > 20)
		p->level = 20;
}

static void lockPiece(Player *p)
{
	int tspin = detectTSpin(p);
	bool hidden = true, above = false;
	for (int i = 0; i < 4; i++)
	{
		int x = p->x + shapes[p->type][p->rot][i][0], y = p->y + shapes[p->type][p->rot][i][1];
		if (y < 0)
			above = true;
		else
			p->board[y][x] = p->type;
		if (y >= HIDDEN)
			hidden = false;
	}
	p->active = false;
	p->holdUsed = false;
	p->pieces++;
	if (above || hidden)
	{
		die(p);
		return;
	}
	int n = 0;
	bool full[BH], perfect = true;
	for (int y = 0; y < BH; y++)
	{
		full[y] = true;
		bool empty = true;
		for (int x = 0; x < BW; x++)
		{
			if (!p->board[y][x])
				full[y] = false;
			else
				empty = false;
		}
		if (full[y])
			p->clearRows[n++] = y;
		else if (!empty)
			perfect = false;
	}
	scoreClear(p, n, tspin, n > 0 && perfect);
	if (n > 0)
	{
		p->clearCount = n;
		p->clearing = true;
		p->clearTimer = 0;
		return;
	}
	applyGarbage(p);
	if (p->alive)
		spawn(p, takeNext(p));
}

static void finishClear(Player *p)
{
	int dst = BH - 1;
	for (int y = BH - 1; y >= 0; y--)
	{
		bool cleared = false;
		for (int i = 0; i < p->clearCount; i++)
			if (p->clearRows[i] == y)
				cleared = true;
		if (!cleared)
			memmove(p->board[dst--], p->board[y], BW);
	}
	while (dst >= 0)
		memset(p->board[dst--], 0, BW);
	p->clearing = false;
	p->clearCount = 0;
	spawn(p, takeNext(p));
}

static void hardDrop(Player *p)
{
	int n = 0;
	while (fits(p->board, p->type, p->rot, p->x, p->y + 1))
	{
		p->y++;
		n++;
	}
	if (n)
		p->lastRotate = false;
	p->score += 2 * n;
	lockPiece(p);
}

static void softDrop(Player *p)
{
	if (!fits(p->board, p->type, p->rot, p->x, p->y + 1))
		return;
	p->y++;
	p->score += 1;
	p->gravAcc = 0;
	p->lastRotate = false;
	onMoved(p);
}

static void holdPiece(Player *p)
{
	if (p->holdUsed)
		return;
	int t = p->type;
	if (p->hold)
		spawn(p, p->hold);
	else
		spawn(p, takeNext(p));
	p->hold = t;
	p->holdUsed = true;
}

static double gravityMs(int level)
{
	return pow(0.8 - (level - 1) * 0.007, level - 1) * 1000.0;
}

static void resetPlayer(Player *p, uint32_t seed, int id)
{
	memset(p->board, 0, sizeof(p->board));
	p->rng = seed ? seed : 1;
	p->garbRng = seed ^ (0x9e3779b9u * (id + 1));
	p->qlen = 0;
	fillQueue(p);
	p->hold = 0;
	p->holdUsed = false;
	p->score = 0;
	p->lines = 0;
	p->level = G.startLevel;
	p->combo = -1;
	p->b2b = false;
	p->pending = 0;
	p->pieces = p->attack = p->samplePieces = 0;
	p->playTime = 0;
	p->apm = p->pps = p->ppsNow = 0;
	p->histLen = 0;
	p->clearing = false;
	p->alive = true;
	p->action[0][0] = p->action[1][0] = 0;
	p->actionTime = 0;
	spawn(p, takeNext(p));
}

/* ------------------------------------------------------------------------ */
/* bots: place every reachable rotation/column and pick the best board      */
/* ------------------------------------------------------------------------ */

static double botEvaluate(uint8_t b[BH][BW])
{
	uint8_t t[BH][BW];
	int lines = 0, dst = BH - 1;
	for (int y = BH - 1; y >= 0; y--)
	{
		bool full = true;
		for (int x = 0; x < BW; x++)
			if (!b[y][x])
				full = false;
		if (full)
			lines++;
		else
			memcpy(t[dst--], b[y], BW);
	}
	while (dst >= 0)
		memset(t[dst--], 0, BW);
	int h[BW], agg = 0, holes = 0, bump = 0;
	for (int x = 0; x < BW; x++)
	{
		h[x] = 0;
		bool found = false;
		for (int y = 0; y < BH; y++)
		{
			if (t[y][x])
			{
				if (!found)
					h[x] = BH - y;
				found = true;
			}
			else if (found)
				holes++;
		}
		agg += h[x];
		if (x > 0)
			bump += abs(h[x] - h[x - 1]);
	}
	return -0.510066 * agg + 0.760666 * lines - 0.35663 * holes - 0.184483 * bump;
}

static void botPlan(Player *p)
{
	double best = -1e18;
	p->planRot = p->planDx = 0;
	for (int rc = 0; rc < 4; rc++)
	{
		int r = p->rot, x = p->x, y = p->y;
		bool ok = true;
		for (int i = 0; i < rc && ok; i++)
			ok = rotateTest(p->board, p->type, &r, &x, &y, 1, NULL);
		if (!ok)
			continue;
		for (int dx = -BW; dx <= BW; dx++)
		{
			int xx = x, st = dx < 0 ? -1 : 1;
			for (int k = 0; k < abs(dx) && ok; k++)
			{
				if (fits(p->board, p->type, r, xx + st, y))
					xx += st;
				else
					ok = false;
			}
			if (!ok)
			{
				ok = true;
				continue;
			}
			int yy = y;
			while (fits(p->board, p->type, r, xx, yy + 1))
				yy++;
			uint8_t tmp[BH][BW];
			memcpy(tmp, p->board, sizeof(tmp));
			for (int i = 0; i < 4; i++)
			{
				int cy = yy + shapes[p->type][r][i][1];
				if (cy >= 0)
					tmp[cy][xx + shapes[p->type][r][i][0]] = p->type;
			}
			double sc = botEvaluate(tmp) + G.botNoise * ((double)rand() / RAND_MAX - 0.5);
			if (sc > best)
			{
				best = sc;
				p->planRot = rc;
				p->planDx = dx;
			}
		}
	}
	p->planned = true;
}

static void botStep(Player *p, double dt)
{
	p->botTimer += dt;
	if (p->botTimer < G.botDelay)
		return;
	p->botTimer = 0;
	if (!p->planned)
		botPlan(p);
	if (p->planRot > 0)
	{
		if (doRotate(p, 1))
			p->planRot--;
		else
			p->planRot = 0;
	}
	else if (p->planDx)
	{
		int st = p->planDx < 0 ? -1 : 1;
		if (doMove(p, st))
			p->planDx -= st;
		else
			p->planDx = 0;
	}
	else
		hardDrop(p);
}

static void updatePlayer(Player *p, double dt)
{
	if (!p->alive || !p->inRound)
		return;
	p->playTime += dt;
	if (p->playTime > 1000)
	{
		p->pps = p->pieces / (p->playTime / 1000.0);
		p->apm = p->attack / (p->playTime / 60000.0);
	}
	if (p->clearing)
	{
		p->clearTimer += dt;
		if (p->clearTimer >= CLEAR_MS)
			finishClear(p);
		return;
	}
	if (!p->active)
		return;
	if (p->kind == KIND_BOT)
	{
		botStep(p, dt);
		if (!p->active)
			return;
	}
	double interval = gravityMs(p->level);
	p->gravAcc += dt;
	while (p->gravAcc >= interval)
	{
		p->gravAcc -= interval;
		if (!fits(p->board, p->type, p->rot, p->x, p->y + 1))
		{
			p->gravAcc = 0;
			break;
		}
		p->y++;
		p->lastRotate = false;
		if (p->y > p->lowestY)
		{
			p->lowestY = p->y;
			p->lockResets = 0;
		}
	}
	if (!fits(p->board, p->type, p->rot, p->x, p->y + 1))
	{
		p->lockTimer += dt;
		if (p->lockTimer >= LOCK_MS)
			lockPiece(p);
	}
	else
		p->lockTimer = 0;
}

// board with the active piece baked in, used for the small boards and the network
static void composeBoard(Player *p, uint8_t b[BH][BW])
{
	memcpy(b, p->board, sizeof(p->board));
	if (!p->active || p->kind == KIND_REMOTE)
		return;
	for (int i = 0; i < 4; i++)
	{
		int x = p->x + shapes[p->type][p->rot][i][0], y = p->y + shapes[p->type][p->rot][i][1];
		if (y >= 0)
			b[y][x] = p->type;
	}
}

/* ------------------------------------------------------------------------ */
/* rounds                                                                   */
/* ------------------------------------------------------------------------ */

static void pushSample(Player *p, float pps, float height)
{
	if (p->histLen == HISTN)
	{
		memmove(p->histPps, p->histPps + 1, sizeof(float) * (HISTN - 1));
		memmove(p->histHeight, p->histHeight + 1, sizeof(float) * (HISTN - 1));
		p->histLen--;
	}
	p->histPps[p->histLen] = pps > 1 ? 1 : pps;
	p->histHeight[p->histLen++] = height;
}

static void startRound(uint32_t seed)
{
	for (int i = 0; i < MAXP; i++)
	{
		Player *p = &players[i];
		p->inRound = p->present;
		if (!p->present)
			continue;
		if (p->kind == KIND_REMOTE)
		{
			memset(p->board, 0, sizeof(p->board));
			p->alive = true;
			p->score = p->lines = p->pending = 0;
			p->level = G.startLevel;
			p->apm = p->pps = 0;
			p->histLen = 0;
		}
		else
			resetPlayer(p, seed, i);
	}
	G.state = ST_COUNTDOWN;
	G.stateTime = nowMs();
	G.winner = -1;
	G.paused = false;
}

static void checkRoundOver(void)
{
	int in = 0, alive = 0, last = -1;
	for (int i = 0; i < MAXP; i++)
		if (players[i].present && players[i].inRound)
		{
			in++;
			if (players[i].alive)
			{
				alive++;
				last = i;
			}
		}
	if ((in >= 2 && alive <= 1) || (in == 1 && alive == 0) || (G.mode == MODE_OFFLINE && !players[0].alive))
	{
		G.state = ST_OVER;
		G.stateTime = nowMs();
		G.winner = in >= 2 && alive == 1 ? last : -1;
	}
}

/* ------------------------------------------------------------------------ */
/* network: the host relays every packet, the clients only talk to the host */
/* packet = [len:16][type:8][from:8][payload]                               */
/* ------------------------------------------------------------------------ */

static void sendPacket(int fd, int type, int from, const uint8_t *d, int len)
{
	uint8_t pkt[4 + 512];
	put16(pkt, len);
	pkt[2] = type;
	pkt[3] = from;
	if (len > 0)
		memcpy(pkt + 4, d, len);
	send(fd, pkt, 4 + len, MSG_NOSIGNAL);
}

// send to every other player
static void netSend(int type, const uint8_t *d, int len)
{
	if (G.mode == MODE_HOST)
	{
		for (int i = 1; i < MAXP; i++)
			if (conns[i].fd >= 0)
				sendPacket(conns[i].fd, type, 0, d, len);
	}
	else if (G.mode == MODE_CLIENT && conns[0].fd >= 0)
		sendPacket(conns[0].fd, type, G.myId, d, len);
}

static void sendRoster(void)
{
	uint8_t d[MAXP * 17];
	for (int i = 0; i < MAXP; i++)
	{
		d[i * 17] = players[i].present;
		memcpy(d + i * 17 + 1, players[i].name, 16);
	}
	netSend(MSG_ROSTER, d, sizeof(d));
}

static void sendState(void)
{
	Player *p = &players[G.myId];
	uint8_t d[213], b[BH][BW];
	composeBoard(p, b);
	memcpy(d, b[HIDDEN], BW * (BH - HIDDEN));
	d[200] = p->alive;
	d[201] = p->pending > 255 ? 255 : p->pending;
	d[202] = p->level;
	put32(d + 203, p->score);
	put16(d + 207, p->lines);
	put16(d + 209, (int)(p->apm * 10));
	put16(d + 211, (int)(p->pps * 100));
	netSend(MSG_STATE, d, sizeof(d));
}

static int pickTarget(int from)
{
	int cand[MAXP], n = 0;
	for (int i = 0; i < MAXP; i++)
		if (i != from && players[i].present && players[i].inRound && players[i].alive)
			cand[n++] = i;
	if (!n)
		return -1;
	return cand[players[from].targetRR++ % n];
}

static void sendAttack(Player *p, int lines)
{
	int from = (int)(p - players);
	int target = pickTarget(from);
	if (target < 0)
		return;
	if (G.mode == MODE_OFFLINE)
		players[target].pending += lines;
	else
	{
		uint8_t d[2] = {(uint8_t)target, (uint8_t)(lines > 255 ? 255 : lines)};
		netSend(MSG_GARBAGE, d, 2);
	}
}

static void hostStart(void)
{
	uint32_t seed = (uint32_t)time(NULL) ^ (uint32_t)rand();
	uint8_t d[4];
	put32(d, seed);
	netSend(MSG_START, d, 4);
	startRound(seed);
}

static void handleMessage(int type, int from, const uint8_t *d, int len)
{
	if (from < 0 || from >= MAXP)
		return;
	if (type == MSG_WELCOME && len >= 1 && d[0] < MAXP)
	{
		G.myId = d[0];
		players[G.myId].kind = KIND_HUMAN;
		players[G.myId].present = true;
	}
	else if (type == MSG_FULL)
	{
		snprintf(G.status, sizeof(G.status), "game is full");
		running = 0;
	}
	else if (type == MSG_ROSTER && len >= MAXP * 17)
	{
		for (int i = 0; i < MAXP; i++)
		{
			if (i == G.myId)
				continue;
			Player *p = &players[i];
			p->present = d[i * 17];
			p->kind = p->present ? KIND_REMOTE : KIND_NONE;
			memcpy(p->name, d + i * 17 + 1, 16);
			p->name[15] = 0;
			if (!p->present)
				p->inRound = false;
		}
	}
	else if (type == MSG_START && len >= 4)
		startRound(get32(d));
	else if (type == MSG_STATE && len >= 213 && from != G.myId)
	{
		Player *p = &players[from];
		for (int i = 0; i < BW * (BH - HIDDEN); i++)
			p->board[HIDDEN + i / BW][i % BW] = d[i] <= 8 ? d[i] : 8;
		memset(p->board, 0, HIDDEN * BW);
		bool wasAlive = p->alive;
		p->alive = d[200];
		p->pending = d[201];
		p->level = d[202];
		p->score = get32(d + 203);
		p->lines = get16(d + 207);
		p->apm = get16(d + 209) / 10.0;
		p->pps = get16(d + 211) / 100.0;
		if (wasAlive && !p->alive)
		{
			p->koTime = nowMs();
			setAction(p, "K.O.", "", pieceColor[5]);
		}
	}
	else if (type == MSG_GARBAGE && len >= 2 && d[0] == G.myId)
	{
		Player *p = &players[G.myId];
		if (p->alive && p->inRound)
			p->pending += d[1];
	}
	else if (type == MSG_LEAVE && len >= 1 && d[0] < MAXP && d[0] != G.myId)
	{
		players[d[0]].present = false;
		players[d[0]].inRound = false;
		players[d[0]].kind = KIND_NONE;
	}
}

static void hostPacket(int conn, int type, const uint8_t *d, int len)
{
	if (type == MSG_HELLO)
	{
		Player *p = &players[conn];
		memset(p, 0, sizeof(*p));
		p->kind = KIND_REMOTE;
		p->present = true;
		memcpy(p->name, d, len < 15 ? len : 15);
		uint8_t w[1] = {(uint8_t)conn};
		sendPacket(conns[conn].fd, MSG_WELCOME, 0, w, 1);
		sendRoster();
		return;
	}
	// relay to the other clients, the sender id is set by the host
	for (int i = 1; i < MAXP; i++)
		if (i != conn && conns[i].fd >= 0)
			sendPacket(conns[i].fd, type, conn, d, len);
	handleMessage(type, conn, d, len);
}

static void dropConn(int i)
{
	close(conns[i].fd);
	conns[i].fd = -1;
	conns[i].len = 0;
	if (G.mode == MODE_HOST)
	{
		players[i].present = false;
		players[i].inRound = false;
		players[i].kind = KIND_NONE;
		uint8_t d[1] = {(uint8_t)i};
		netSend(MSG_LEAVE, d, 1);
		sendRoster();
	}
	else
	{
		snprintf(G.status, sizeof(G.status), "connection to host lost");
		G.state = ST_OVER;
		G.winner = -1;
	}
}

static void readConn(int i)
{
	Conn *c = &conns[i];
	ssize_t n = recv(c->fd, c->buf + c->len, sizeof(c->buf) - c->len, 0);
	if (n <= 0)
	{
		if (n < 0 && (errno == EINTR || errno == EAGAIN))
			return;
		dropConn(i);
		return;
	}
	c->len += n;
	int off = 0;
	while (c->len - off >= 4)
	{
		int plen = get16(c->buf + off);
		if (c->len - off < 4 + plen)
			break;
		uint8_t *pk = c->buf + off;
		if (G.mode == MODE_HOST)
			hostPacket(i, pk[2], pk + 4, plen);
		else
			handleMessage(pk[2], pk[3], pk + 4, plen);
		off += 4 + plen;
	}
	memmove(c->buf, c->buf + off, c->len - off);
	c->len -= off;
}

static void acceptConn(void)
{
	int fd = accept(listenFd, NULL, NULL);
	if (fd < 0)
		return;
	int one = 1;
	setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
	for (int i = 1; i < MAXP; i++)
		if (conns[i].fd < 0 && !players[i].present)
		{
			conns[i].fd = fd;
			conns[i].len = 0;
			return;
		}
	sendPacket(fd, MSG_FULL, 0, NULL, 0);
	close(fd);
}

static int netHost(int port)
{
	listenFd = socket(AF_INET, SOCK_STREAM, 0);
	int one = 1;
	setsockopt(listenFd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
	struct sockaddr_in a = {0};
	a.sin_family = AF_INET;
	a.sin_addr.s_addr = htonl(INADDR_ANY);
	a.sin_port = htons(port);
	if (bind(listenFd, (struct sockaddr *)&a, sizeof(a)) < 0 || listen(listenFd, 4) < 0)
	{
		perror("tetris-together: cannot listen");
		return -1;
	}
	// find an address to show in the lobby
	struct ifaddrs *ifa, *i;
	snprintf(G.hostAddr, sizeof(G.hostAddr), "localhost");
	if (getifaddrs(&ifa) == 0)
	{
		for (i = ifa; i; i = i->ifa_next)
			if (i->ifa_addr && i->ifa_addr->sa_family == AF_INET && strcmp(i->ifa_name, "lo") != 0)
			{
				inet_ntop(AF_INET, &((struct sockaddr_in *)i->ifa_addr)->sin_addr, G.hostAddr, sizeof(G.hostAddr));
				break;
			}
		freeifaddrs(ifa);
	}
	return 0;
}

static int netJoin(const char *addr)
{
	char host[256];
	snprintf(host, sizeof(host), "%s", addr);
	char port[16];
	snprintf(port, sizeof(port), "%d", DEFAULT_PORT);
	char *colon = strrchr(host, ':');
	if (colon)
	{
		*colon = 0;
		snprintf(port, sizeof(port), "%s", colon + 1);
	}
	struct addrinfo hints = {0}, *res, *r;
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	if (getaddrinfo(host, port, &hints, &res) != 0)
	{
		fprintf(stderr, "tetris-together: cannot resolve %s\n", host);
		return -1;
	}
	int fd = -1;
	for (r = res; r; r = r->ai_next)
	{
		fd = socket(r->ai_family, r->ai_socktype, r->ai_protocol);
		if (fd >= 0 && connect(fd, r->ai_addr, r->ai_addrlen) == 0)
			break;
		if (fd >= 0)
			close(fd);
		fd = -1;
	}
	freeaddrinfo(res);
	if (fd < 0)
	{
		fprintf(stderr, "tetris-together: cannot connect to %s:%s\n", host, port);
		return -1;
	}
	int one = 1;
	setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
	conns[0].fd = fd;
	sendPacket(fd, MSG_HELLO, G.myId, (const uint8_t *)players[0].name, 16);
	// wait for our player id
	double deadline = nowMs() + 5000;
	G.myId = -1;
	while (G.myId < 0 && running && nowMs() < deadline)
	{
		struct pollfd pf = {fd, POLLIN, 0};
		if (poll(&pf, 1, 100) > 0)
		{
			readConn(0);
			if (conns[0].fd < 0)
				break;
		}
	}
	if (G.myId < 0)
	{
		fprintf(stderr, "tetris-together: %s\n", G.status[0] ? G.status : "no answer from host");
		return -1;
	}
	return 0;
}

/* ------------------------------------------------------------------------ */
/* drawing                                                                  */
/* ------------------------------------------------------------------------ */

// one board cell, 2*s columns wide and s rows high
static void drawCell(int sx, int sy, int s, uint32_t col, int kind)
{
	for (int r = 0; r < s; r++)
		for (int c = 0; c < 2 * s; c++)
		{
			if (kind == CELL_SOLID)
			{
				if (r == 0)
					put(sx + c, sy + r, 0x2594, lerpColor(col, 0xffffff, 0.45), col); // ▔ highlight
				else
					put(sx + c, sy + r, 0x2581, lerpColor(col, 0, 0.35), col); // ▁ shadow
			}
			else if (kind == CELL_GHOST)
				put(sx + c, sy + r, 0x2591, lerpColor(col, 0, 0.45), DEF); // ░
			else
				put(sx + c, sy + r, (c == 2 * s - 1 && r == s - 1) ? 0x00B7 : ' ', C_GRID, DEF);
		}
}

// piece preview at scale 1, centered in a w columns wide area
static void drawPiece(int x, int y, int w, int t, bool dim)
{
	if (!t)
		return;
	int minX = 4, maxX = 0, minY = 4;
	for (int i = 0; i < 4; i++)
	{
		int px = shapes[t][0][i][0], py = shapes[t][0][i][1];
		minX = px < minX ? px : minX;
		maxX = px > maxX ? px : maxX;
		minY = py < minY ? py : minY;
	}
	int ox = x + (w - (maxX - minX + 1) * 2) / 2;
	uint32_t col = dim ? grayOf(pieceColor[t]) : pieceColor[t];
	for (int i = 0; i < 4; i++)
		drawCell(ox + (shapes[t][0][i][0] - minX) * 2, y + shapes[t][0][i][1] - minY, 1, col, CELL_SOLID);
}

// dialog in the middle of an area
static void overlay(int ax, int ay, int aw, int ah, const char **lines, const uint32_t *cols, int n, uint32_t border)
{
	int w = 0;
	for (int i = 0; i < n; i++)
		w = utf8len(lines[i]) > w ? utf8len(lines[i]) : w;
	w += 4;
	int h = n + 2, x = ax + (aw - w) / 2, y = ay + (ah - h) / 2;
	box(x, y, w, h, border, NULL);
	for (int i = 0; i < n; i++)
		textCenter(x, y + 1 + i, w, lines[i], cols[i]);
}

static void drawBigDigit(int bx, int by, int s, int digit)
{
	static const char *font[3][5] = {
		{".#.", "##.", ".#.", ".#.", "###"},
		{"###", "..#", "###", "#..", "###"},
		{"###", "..#", "###", "..#", "###"},
	};
	static const int col[3] = {4, 2, 5};
	for (int r = 0; r < 5; r++)
		for (int c = 0; c < 3; c++)
			if (font[digit - 1][r][c] == '#')
				drawCell(bx + (3 + c) * 2 * s, by + (6 + r) * s, s, pieceColor[col[digit - 1]], CELL_SOLID);
}

static const char *playerLabel(int id)
{
	Player *p = &players[id];
	if (!p->present)
		return NULL;
	if (G.state == ST_LOBBY || !p->inRound)
		return G.state == ST_LOBBY ? "ready" : "spectating";
	if (G.state == ST_OVER && G.winner == id)
		return "WINNER";
	if (!p->alive)
		return "K.O.";
	return NULL;
}

static void drawOpponent(int px, int top, int pw, int mh, int s, int id)
{
	int bc = s == 1 ? 10 : 20, br = s == 1 ? 10 : 20, bh = br + 2;
	Player *p = id >= 0 ? &players[id] : NULL;
	int bx = px + (pw - bc) / 2, by = top + 1;
	box(px, top, pw, bh, C_NET_BOX, p ? p->name : "empty");
	box(px, top + bh, pw, mh - bh, C_NET_BOX, "stats");
	if (!p)
	{
		textCenter(px, top + bh / 2 - 1, pw, "no player", C_LABEL);
		textCenter(px, top + bh / 2 + 1, pw, G.mode == MODE_OFFLINE ? "try --bots 2" : "waiting...", C_INACTIVE);
		return;
	}
	uint8_t b[BH][BW];
	composeBoard(p, b);
	bool gray = !p->alive || !p->inRound;
	for (int r = 0; r < br; r++)
		for (int c = 0; c < BW; c++)
		{
			if (s == 2)
			{
				int v = b[HIDDEN + r][c];
				drawCell(bx + c * 2, by + r, 1, v ? (gray ? grayOf(pieceColor[v]) : pieceColor[v]) : 0, v ? CELL_SOLID : CELL_EMPTY);
				continue;
			}
			// half blocks: two board rows per terminal row
			int a = b[HIDDEN + r * 2][c], d = b[HIDDEN + r * 2 + 1][c];
			uint32_t ca = a ? (gray ? grayOf(pieceColor[a]) : pieceColor[a]) : DEF;
			uint32_t cd = d ? (gray ? grayOf(pieceColor[d]) : pieceColor[d]) : DEF;
			if (a && d)
				put(bx + c, by + r, 0x2580, ca, cd);
			else if (a)
				put(bx + c, by + r, 0x2580, ca, DEF);
			else if (d)
				put(bx + c, by + r, 0x2584, cd, DEF);
		}
	// incoming garbage
	int g = s == 1 ? (p->pending + 1) / 2 : p->pending;
	for (int i = 0; i < g && i < br; i++)
		put(bx - 1, by + br - 1 - i, 0x2590, gradient(gradCpu, (double)i / br), DEF);
	const char *label = playerLabel(id);
	if (label)
	{
		uint32_t lc = !strcmp(label, "WINNER") ? gradCpu[0] : !strcmp(label, "K.O.") ? C_HI : C_LABEL;
		textCenter(px, top + bh / 2, pw, label, lc);
	}
	// stats
	char v[32];
	int ix = px + 2, iw = pw - 4, iy = top + bh + 1, ih = mh - bh - 2;
	snprintf(v, sizeof(v), "%u", p->score);
	keyValue(ix, iy, iw, "score", v, C_MAIN);
	snprintf(v, sizeof(v), "%d", p->lines);
	keyValue(ix, iy + 1, iw, "lines", v, C_MAIN);
	snprintf(v, sizeof(v), "%.1f", p->apm);
	keyValue(ix, iy + 2, iw, "apm", v, C_MAIN);
	int h = stackHeight(p->board);
	text(ix, iy + 3, "stk", C_LABEL, DEF);
	meter(ix + 4, iy + 3, iw - 4, h / 20.0, gradCpu);
	if (ih > 4)
		graph(px + 1, iy + 4, pw - 2, ih - 4, p->histHeight, p->histLen, gradCpu);
}

static void drawHelp(int x, int y)
{
	static const char *keys[][2] = {{"←→", "move"}, {"↓", "soft"}, {"↑x", "cw"}, {"z", "ccw"}, {"a", "180"}, {"spc", "drop"}, {"c", "hold"}, {"p", "pause"}, {"q", "quit"}};
	for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++)
	{
		if (G.mode != MODE_OFFLINE && keys[i][0][0] == 'p')
			continue;
		x += text(x, y, keys[i][0], C_HI, DEF);
		x += text(x + 1, y, keys[i][1], C_LABEL, DEF) + 3;
	}
}

static void render(void)
{
	double t = nowMs();
	screenClear();
	int s = scrW >= 130 && scrH >= 46 ? 2 : 1;
	int pw = (s == 1 ? 10 : 20) + 6, sw = 16, gap = 1, mw = 20 * s + 2, mh = 20 * s + 2;
	int tw = pw * 2 + sw * 2 + mw + 4 * gap, th = mh + 2;
	if (scrW < tw && s == 1)
	{
		// compact layout for 80 column terminals: narrower panels that touch
		pw = sw = 14;
		gap = 0;
		tw = pw * 2 + sw * 2 + mw;
	}
	if (scrW < tw || scrH < th)
	{
		int y = scrH / 2 - 2;
		textCenter(0, y, scrW, "Terminal size too small", C_TITLE);
		char b[64];
		snprintf(b, sizeof(b), "Width = %d Height = %d", scrW, scrH);
		textCenter(0, y + 1, scrW, b, C_HI);
		textCenter(0, y + 3, scrW, "Needed for current config:", C_TITLE);
		snprintf(b, sizeof(b), "Width = %d Height = %d", tw, th);
		textCenter(0, y + 4, scrW, b, gradCpu[0]);
		screenFlush();
		return;
	}
	int x0 = (scrW - tw) / 2, y0 = (scrH - th) / 2, top = y0 + 1;
	int lpx = x0, lsx = x0 + pw + gap, mx = lsx + sw + gap, rsx = mx + mw + gap, rpx = rsx + sw + gap;
	Player *me = &players[G.myId < 0 ? 0 : G.myId];

	// header: name, mode and clock like btop's top bar
	const char *title = "tetris-together";
	for (int i = 0; title[i]; i++)
		put(x0 + i, y0, title[i], i < 6 ? gradient(gradNet, i / 5.0) : C_LABEL, DEF);
	char mode[64];
	int nIn = 0;
	for (int i = 0; i < MAXP; i++)
		nIn += players[i].present;
	if (G.mode == MODE_OFFLINE)
		snprintf(mode, sizeof(mode), G.nBots ? "versus %d bot%s" : "marathon", G.nBots, G.nBots > 1 ? "s" : "");
	else
		snprintf(mode, sizeof(mode), "battle · %d player%s · %s", nIn, nIn > 1 ? "s" : "", G.mode == MODE_HOST ? "host" : "client");
	textCenter(x0, y0, tw, mode, C_MAIN);
	char clk[16];
	time_t now = time(NULL);
	strftime(clk, sizeof(clk), "%H:%M:%S", localtime(&now));
	text(x0 + tw - 8, y0, clk, C_TITLE, DEF);

	// opponents: with one opponent it is shown on both sides, knocked out players
	// disappear after KO_MS unless nobody else is left
	int opp[2] = {-1, -1}, no = 0;
	for (int pass = 0; pass < 2 && !no; pass++)
		for (int i = 0; i < MAXP && no < 2; i++)
		{
			Player *p = &players[i];
			bool gone = G.state != ST_LOBBY && p->inRound && !p->alive && t - p->koTime >= KO_MS;
			if (i != G.myId && p->present && (pass || !gone))
				opp[no++] = i;
		}
	if (no == 1)
		opp[1] = opp[0];
	drawOpponent(lpx, top, pw, mh, s, opp[0]);
	drawOpponent(rpx, top, pw, mh, s, opp[1]);

	// hold and stats
	box(lsx, top, sw, 6, C_MEM_BOX, "hold");
	drawPiece(lsx + 1, top + 2, sw - 2, me->hold, me->holdUsed);
	int sh = mh - 6, ix = lsx + 2, iw = sw - 4, iy = top + 7;
	box(lsx, top + 6, sw, sh, C_CPU_BOX, "stats");
	char v[32];
	snprintf(v, sizeof(v), "%u", me->score);
	keyValue(ix, iy, iw, "score", v, C_TITLE);
	snprintf(v, sizeof(v), "%d", me->level);
	keyValue(ix, iy + 1, iw, "level", v, C_MAIN);
	snprintf(v, sizeof(v), "%d", me->lines);
	keyValue(ix, iy + 2, iw, "lines", v, C_MAIN);
	int secs = (int)(me->playTime / 1000);
	snprintf(v, sizeof(v), "%02d:%02d", secs / 60, secs % 60);
	keyValue(ix, iy + 3, iw, "time", v, C_MAIN);
	snprintf(v, sizeof(v), "%.2f", me->pps);
	keyValue(ix, iy + 4, iw, "pps", v, C_MAIN);
	snprintf(v, sizeof(v), "%.1f", me->apm);
	keyValue(ix, iy + 5, iw, "apm", v, C_MAIN);
	for (int i = 0; i < sw - 2; i++)
		put(lsx + 1 + i, iy + 6, 0x2500, C_DIV, DEF);
	text(ix, iy + 7, "lvl", C_LABEL, DEF);
	meter(ix + 4, iy + 7, iw - 4, (me->lines % 10) / 10.0, gradProc);
	text(ix, iy + 8, "stk", C_LABEL, DEF);
	meter(ix + 4, iy + 8, iw - 4, stackHeight(me->board) / 20.0, gradCpu);
	double age = t - me->actionTime;
	if (me->action[0][0] && age < 2500)
	{
		double fade = age < 1500 ? 0 : (age - 1500) / 1000.0;
		uint32_t col = lerpColor(me->actionCol, C_INACTIVE, fade);
		textCenter(lsx + 1, iy + 10, sw - 2, me->action[0], col);
		textCenter(lsx + 1, iy + 11, sw - 2, me->action[1], lerpColor(C_MAIN, C_INACTIVE, fade));
	}
	if (sh - 2 > 18)
	{
		text(ix, iy + 13, "stack", C_LABEL, DEF);
		graph(lsx + 1, iy + 14, sw - 2, sh - 2 - 14, me->histHeight, me->histLen, gradCpu);
	}

	// main board, the border turns red when the stack gets high
	int bx = mx + 1, by = top + 1;
	int height = stackHeight(me->board);
	uint32_t border = C_PROC_BOX;
	if (height >= 15 && me->alive)
		border = lerpColor(C_PROC_BOX, 0xff3030, 0.5 + 0.5 * sin(t / 150.0));
	box(mx, top, mw, mh, border, me->name);
	snprintf(v, sizeof(v), "lvl %d", me->level);
	boxTitleRight(mx, top, mw, v, C_MAIN, border);
	bool gray = !me->alive && me->inRound;
	int ghostY = me->y;
	if (me->active)
		while (fits(me->board, me->type, me->rot, me->x, ghostY + 1))
			ghostY++;
	double ct = me->clearing ? me->clearTimer / CLEAR_MS : 0;
	for (int r = HIDDEN; r < BH; r++)
	{
		bool clearing = false;
		for (int i = 0; i < me->clearCount && me->clearing; i++)
			if (me->clearRows[i] == r)
				clearing = true;
		for (int c = 0; c < BW; c++)
		{
			int sx = bx + c * 2 * s, sy = by + (r - HIDDEN) * s, v = me->board[r][c];
			if (clearing)
			{
				// the row dissolves from the middle outwards
				if (fabs(c - 4.5) > (1 - ct) * 5.5)
					drawCell(sx, sy, s, 0, CELL_EMPTY);
				else
					drawCell(sx, sy, s, lerpColor(0xffffff, pieceColor[v], ct), CELL_SOLID);
			}
			else if (v)
				drawCell(sx, sy, s, gray ? grayOf(pieceColor[v]) : pieceColor[v], CELL_SOLID);
			else
				drawCell(sx, sy, s, 0, CELL_EMPTY);
		}
	}
	if (me->active && G.state != ST_LOBBY)
	{
		for (int pass = 0; pass < 2; pass++)
			for (int i = 0; i < 4; i++)
			{
				int c = me->x + shapes[me->type][me->rot][i][0];
				int r = (pass ? me->y : ghostY) + shapes[me->type][me->rot][i][1];
				if (r >= HIDDEN)
					drawCell(bx + c * 2 * s, by + (r - HIDDEN) * s, s, pieceColor[me->type], pass ? CELL_SOLID : CELL_GHOST);
			}
	}
	// incoming garbage meter on the left of the board
	for (int i = 0; i < me->pending * s && i < 20 * s; i++)
		put(gap ? mx - 1 : mx, by + 20 * s - 1 - i, gap ? 0x2590 : 0x2503, gradient(gradCpu, 0.4 + 0.6 * i / (20.0 * s)), DEF);

	// next queue and pps graph
	box(rsx, top, sw, 17, C_MEM_BOX, "next");
	for (int i = 0; i < NEXTN && i < me->qlen; i++)
		drawPiece(rsx + 1, top + 2 + i * 3, sw - 2, G.state == ST_LOBBY ? 0 : me->queue[i], false);
	box(rsx, top + 17, sw, mh - 17, C_CPU_BOX, "pps");
	snprintf(v, sizeof(v), "%.2f", me->ppsNow);
	if (sw >= 16)
		boxTitleRight(rsx, top + 17, sw, v, C_MAIN, C_CPU_BOX);
	graph(rsx + 1, top + 18, sw - 2, mh - 19, me->histPps, me->histLen, gradTemp);

	// dialogs
	const char *l[10];
	uint32_t lc[10];
	int n = 0;
	if (G.status[0])
	{
		l[n] = G.status, lc[n++] = C_HI;
		l[n] = "q quit", lc[n++] = C_LABEL;
	}
	else if (G.state == ST_LOBBY)
	{
		char addr[96], names[MAXP][40];
		l[n] = "LOBBY", lc[n++] = C_TITLE;
		if (G.mode == MODE_HOST)
		{
			snprintf(addr, sizeof(addr), "%s:%d", G.hostAddr, G.port);
			l[n] = "join with -j", lc[n++] = C_LABEL;
			l[n] = addr, lc[n++] = gradNet[0];
		}
		for (int i = 0; i < MAXP; i++)
		{
			snprintf(names[i], sizeof(names[i]), "%d %s", i + 1, players[i].present ? players[i].name : "-");
			l[n] = names[i], lc[n++] = players[i].present ? (i == G.myId ? gradCpu[0] : C_MAIN) : C_INACTIVE;
		}
		l[n] = G.mode == MODE_HOST ? "enter: start" : "waiting for host", lc[n++] = C_LABEL;
	}
	else if (G.state == ST_COUNTDOWN)
	{
		int d = 3 - (int)((t - G.stateTime) / (COUNT_MS / 3));
		if (d >= 1 && d <= 3)
			drawBigDigit(bx, by, s, d);
	}
	else if (G.state == ST_OVER)
	{
		static char line[48];
		bool versus = G.mode != MODE_OFFLINE || G.nBots > 0;
		if (!versus)
			l[n] = "GAME OVER", lc[n++] = C_HI;
		else if (G.winner == G.myId)
			l[n] = "YOU WIN", lc[n++] = gradCpu[0];
		else
		{
			l[n] = "K.O.", lc[n++] = C_HI;
			if (G.winner >= 0)
			{
				snprintf(line, sizeof(line), "%s wins", players[G.winner].name);
				l[n] = line, lc[n++] = C_MAIN;
			}
		}
		static char sc[32];
		snprintf(sc, sizeof(sc), "score %u", me->score);
		l[n] = sc, lc[n++] = C_TITLE;
		l[n] = G.mode == MODE_CLIENT ? "waiting for host" : G.mode == MODE_HOST ? "enter: next round" : "r: restart", lc[n++] = C_LABEL;
	}
	else if (G.paused)
	{
		l[n] = "PAUSED", lc[n++] = C_TITLE;
		l[n] = "p: resume", lc[n++] = C_LABEL;
	}
	else if (!me->alive && me->inRound)
	{
		l[n] = "K.O.", lc[n++] = C_HI;
		l[n] = "spectating", lc[n++] = C_LABEL;
	}
	else if (!me->inRound && G.state == ST_PLAYING)
	{
		l[n] = "round in progress", lc[n++] = C_TITLE;
		l[n] = "you join the next one", lc[n++] = C_LABEL;
	}
	if (n)
		overlay(bx, by, 20 * s, 20 * s, l, lc, n, C_TITLE);

	drawHelp(x0, y0 + mh + 1);
	screenFlush();
}

/* ------------------------------------------------------------------------ */
/* terminal and input                                                       */
/* ------------------------------------------------------------------------ */

static struct termios oldTerm;

static void termSetup(void)
{
	struct termios t;
	tcgetattr(STDIN_FILENO, &oldTerm);
	t = oldTerm;
	t.c_lflag &= ~(ICANON | ECHO);
	t.c_cc[VMIN] = 0;
	t.c_cc[VTIME] = 0;
	tcsetattr(STDIN_FILENO, TCSANOW, &t);
	// alternate screen, hide cursor, no line wrap
	printf("\033[?1049h\033[?25l\033[?7l");
	fflush(stdout);
}

static void termRestore(void)
{
	tcsetattr(STDIN_FILENO, TCSANOW, &oldTerm);
	printf("\033[m\033[?7h\033[?25h\033[?1049l");
	fflush(stdout);
}

static void onSignal(int sig)
{
	if (sig == SIGWINCH)
		resized = 1;
	else
		running = 0;
}

static void onKey(int k)
{
	Player *me = &players[G.myId];
	if (k == 'q' || k == 'Q')
	{
		running = 0;
		return;
	}
	if (G.status[0])
		return;
	bool enter = k == '\n' || k == '\r';
	if ((G.state == ST_LOBBY || G.state == ST_OVER) && G.mode == MODE_HOST && (enter || k == 'r'))
	{
		hostStart();
		return;
	}
	if (G.state == ST_OVER && G.mode == MODE_OFFLINE && (enter || k == 'r'))
	{
		startRound((uint32_t)time(NULL) ^ (uint32_t)rand());
		return;
	}
	if (k == 'p' && G.mode == MODE_OFFLINE && G.state == ST_PLAYING)
		G.paused = !G.paused;
	if (G.state != ST_PLAYING || G.paused || !me->active)
		return;
	switch (k)
	{
	case KEY_LEFT:
		doMove(me, -1);
		break;
	case KEY_RIGHT:
		doMove(me, 1);
		break;
	case KEY_DOWN:
		softDrop(me);
		break;
	case KEY_UP:
	case 'x':
	case 'X':
		doRotate(me, 1);
		break;
	case 'z':
	case 'Z':
		doRotate(me, -1);
		break;
	case 'a':
	case 'A':
		doRotate(me, 2);
		break;
	case ' ':
		hardDrop(me);
		break;
	case 'c':
	case 'C':
		holdPiece(me);
		break;
	}
}

static void readInput(void)
{
	unsigned char b[64];
	ssize_t n = read(STDIN_FILENO, b, sizeof(b));
	for (ssize_t i = 0; i < n; i++)
	{
		if (b[i] == 27 && i + 2 < n && (b[i + 1] == '[' || b[i + 1] == 'O'))
		{
			int k = 0;
			switch (b[i + 2])
			{
			case 'A':
				k = KEY_UP;
				break;
			case 'B':
				k = KEY_DOWN;
				break;
			case 'C':
				k = KEY_RIGHT;
				break;
			case 'D':
				k = KEY_LEFT;
				break;
			}
			i += 2;
			if (k)
				onKey(k);
		}
		else if (b[i] != 27)
			onKey(b[i]);
	}
}

static void usage(void)
{
	printf("tetris-together %s\n\n", VERSION);
	printf("usage: tetris-together [options]\n\n");
	printf("  -n, --name NAME        your player name\n");
	printf("  -b, --bots N           play against 1 or 2 bots\n");
	printf("  -d, --difficulty D     bot difficulty: easy, normal, hard\n");
	printf("  -s, --host [PORT]      host a game for up to 3 players (default port %d)\n", DEFAULT_PORT);
	printf("  -j, --join HOST[:PORT] join a hosted game\n");
	printf("  -l, --level N          start level (1-20)\n");
	printf("      --256              use 256 colors instead of truecolor\n");
	printf("  -h, --help             show this help\n");
	printf("  -v, --version          show the version\n");
}

int main(int argc, char *argv[])
{
	const char *join = NULL, *name = getenv("USER");
	const char *colorterm = getenv("COLORTERM");
	G.trueColor = colorterm && (strstr(colorterm, "truecolor") || strstr(colorterm, "24bit"));
	G.port = DEFAULT_PORT;
	G.startLevel = 1;
	G.botDelay = 140;
	G.botNoise = 0.5;
	for (int i = 1; i < argc; i++)
	{
		const char *a = argv[i];
		bool more = i + 1 < argc;
		if ((!strcmp(a, "-n") || !strcmp(a, "--name")) && more)
			name = argv[++i];
		else if ((!strcmp(a, "-b") || !strcmp(a, "--bots")) && more)
			G.nBots = atoi(argv[++i]);
		else if ((!strcmp(a, "-d") || !strcmp(a, "--difficulty")) && more)
		{
			const char *d = argv[++i];
			if (!strcmp(d, "easy"))
				G.botDelay = 300, G.botNoise = 0.8;
			else if (!strcmp(d, "hard"))
				G.botDelay = 45, G.botNoise = 0.3;
		}
		else if (!strcmp(a, "-s") || !strcmp(a, "--host"))
		{
			G.mode = MODE_HOST;
			if (more && argv[i + 1][0] != '-')
				G.port = atoi(argv[++i]);
		}
		else if ((!strcmp(a, "-j") || !strcmp(a, "--join")) && more)
		{
			G.mode = MODE_CLIENT;
			join = argv[++i];
		}
		else if ((!strcmp(a, "-l") || !strcmp(a, "--level")) && more)
			G.startLevel = atoi(argv[++i]);
		else if (!strcmp(a, "--256"))
			G.trueColor = false;
		else if (!strcmp(a, "-v") || !strcmp(a, "--version"))
		{
			printf("tetris-together %s\n", VERSION);
			return EXIT_SUCCESS;
		}
		else
		{
			usage();
			return !strcmp(a, "-h") || !strcmp(a, "--help") ? EXIT_SUCCESS : EXIT_FAILURE;
		}
	}
	if (G.nBots < 0)
		G.nBots = 0;
	if (G.nBots > MAXP - 1)
		G.nBots = MAXP - 1;
	if (G.startLevel < 1)
		G.startLevel = 1;
	if (G.startLevel > 20)
		G.startLevel = 20;
	if (G.mode != MODE_OFFLINE)
		G.nBots = 0;

	srand(time(NULL) ^ getpid());
	signal(SIGPIPE, SIG_IGN);
	initShapes();
	for (int i = 0; i < MAXP; i++)
	{
		conns[i].fd = -1;
		players[i].level = G.startLevel;
	}
	Player *me = &players[0];
	me->kind = KIND_HUMAN;
	me->present = true;
	snprintf(me->name, sizeof(me->name), "%s", name && *name ? name : "player");
	for (int i = 1; i <= G.nBots; i++)
	{
		players[i].kind = KIND_BOT;
		players[i].present = true;
		snprintf(players[i].name, sizeof(players[i].name), "bot-%d", i);
	}

	if (G.mode == MODE_HOST && netHost(G.port) < 0)
		return EXIT_FAILURE;
	if (G.mode == MODE_CLIENT)
	{
		char myName[16];
		memcpy(myName, me->name, 16);
		memset(me, 0, sizeof(*me));
		memcpy(me->name, myName, 16);
		if (netJoin(join) < 0)
			return EXIT_FAILURE;
		memcpy(players[G.myId].name, myName, 16);
		players[G.myId].level = G.startLevel;
	}
	G.state = ST_LOBBY;
	if (G.mode == MODE_OFFLINE)
		startRound((uint32_t)time(NULL) ^ (uint32_t)rand());

	signal(SIGINT, onSignal);
	signal(SIGTERM, onSignal);
	signal(SIGWINCH, onSignal);
	termSetup();

	double last = nowMs(), lastSample = last, lastSend = 0, lastRender = 0;
	while (running)
	{
		struct pollfd pf[2 + MAXP];
		int map[2 + MAXP], n = 0;
		pf[n] = (struct pollfd){STDIN_FILENO, POLLIN, 0};
		map[n++] = -2;
		if (listenFd >= 0)
		{
			pf[n] = (struct pollfd){listenFd, POLLIN, 0};
			map[n++] = -1;
		}
		for (int i = 0; i < MAXP; i++)
			if (conns[i].fd >= 0)
			{
				pf[n] = (struct pollfd){conns[i].fd, POLLIN, 0};
				map[n++] = i;
			}
		if (poll(pf, n, 5) > 0)
			for (int i = 0; i < n; i++)
			{
				if (!(pf[i].revents & (POLLIN | POLLHUP | POLLERR)))
					continue;
				if (map[i] == -2)
					readInput();
				else if (map[i] == -1)
					acceptConn();
				else if (conns[map[i]].fd >= 0)
					readConn(map[i]);
			}
		if (resized)
		{
			resized = 0;
			screenResize();
		}

		double t = nowMs(), dt = t - last;
		last = t;
		if (dt > 100)
			dt = 100;
		if (G.state == ST_COUNTDOWN && t - G.stateTime >= COUNT_MS)
		{
			G.state = ST_PLAYING;
			G.stateTime = t;
			lastSample = t;
			setAction(&players[G.myId], "GO!", "", gradCpu[0]);
		}
		if (G.state == ST_PLAYING && !G.paused)
		{
			for (int i = 0; i < MAXP; i++)
				if (players[i].kind == KIND_HUMAN || players[i].kind == KIND_BOT)
					updatePlayer(&players[i], dt);
			if (t - lastSample >= SAMPLE_MS)
			{
				lastSample = t;
				for (int i = 0; i < MAXP; i++)
				{
					Player *p = &players[i];
					if (!p->present || !p->inRound)
						continue;
					if (p->kind != KIND_REMOTE)
					{
						float inst = (p->pieces - p->samplePieces) * (1000.0 / SAMPLE_MS);
						p->samplePieces = p->pieces;
						p->ppsNow = p->alive ? 0.6 * p->ppsNow + 0.4 * inst : 0;
					}
					else
						p->ppsNow = p->pps;
					pushSample(p, p->ppsNow / 4.0, stackHeight(p->board) / 20.0);
				}
			}
			checkRoundOver();
		}
		if (G.mode != MODE_OFFLINE && G.myId >= 0 && players[G.myId].inRound && t - lastSend >= 66)
		{
			lastSend = t;
			sendState();
		}
		if (t - lastRender >= 16)
		{
			lastRender = t;
			render();
		}
	}

	termRestore();
	for (int i = 0; i < MAXP; i++)
		if (conns[i].fd >= 0)
			close(conns[i].fd);
	if (listenFd >= 0)
		close(listenFd);
	if (G.status[0])
		printf("%s\n", G.status);
	return EXIT_SUCCESS;
}
