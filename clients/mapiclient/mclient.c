/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0.  If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 *
 * Copyright 1997 - July 2008 CWI, August 2008 - 2018 MonetDB B.V.
 */

/* The Mapi Client Interface
 * A textual interface to the Monet server using the Mapi library,
 * providing command-line access for its users. It is the preferred
 * interface for non-DBAs.
 * See mclient.1 for usage information.
 */

#include "monetdb_config.h"
#ifndef HAVE_GETOPT_LONG
#  include "monet_getopt.h"
#else
# ifdef HAVE_GETOPT_H
#  include "getopt.h"
# endif
#endif
#include "mapi.h"
#include <unistd.h>
#include <string.h>
#ifdef HAVE_STRINGS_H
#include <strings.h>		/* strcasecmp */
#endif

#ifdef HAVE_LIBREADLINE
#include <readline/readline.h>
#include <readline/history.h>
#include "ReadlineTools.h"
#endif
#include "stream.h"
#include "msqldump.h"
#include "mprompt.h"
#include "dotmonetdb.h"

#include <locale.h>

#ifdef HAVE_ICONV
#ifdef HAVE_ICONV_H
#include <iconv.h>
#endif
#ifdef HAVE_NL_LANGINFO
#ifdef HAVE_LANGINFO_H
#include <langinfo.h>
#endif
#endif
#endif

#ifndef S_ISCHR
#define S_ISCHR(m)	(((m) & S_IFMT) == S_IFCHR)
#endif
#ifndef S_ISREG
#define S_ISREG(m)	(((m) & S_IFMT) == S_IFREG)
#endif

enum modes {
	MAL,
	SQL
};

static enum modes mode = SQL;
static stream *toConsole;
static stream *stdout_stream;
static stream *stderr_stream;
static stream *fromConsole = NULL;
static char *language = NULL;
static char *logfile = NULL;
static char promptbuf[16];
static bool echoquery = false;
#ifdef HAVE_ICONV
static char *encoding;
#endif
static bool errseen = false;

#define setPrompt() sprintf(promptbuf, "%.*s>", (int) sizeof(promptbuf) - 2, language)
#define debugMode() (strncmp(promptbuf, "mdb", 3) == 0)

/* the internal result set formatters */
enum formatters {
	NOformatter,
	RAWformatter,		// as the data is received
	TABLEformatter,		// render as a bordered table
	CSVformatter,		// render as a comma separate file
	XMLformatter,		// render as a valid XML document
	TESTformatter,		// for testing, escape characters
	TRASHformatter,		// remove the result set
	SAMformatter,		// render a SAM result set
	EXPANDEDformatter	// render as multi-row single record
};
static enum formatters formatter = NOformatter;
char *separator = NULL;		/* column separator for CSV/TAB format */
bool csvheader = false;		/* include header line in CSV format */

#define DEFWIDTH 80

/* use a 64 bit integer for the timer */
typedef int64_t timertype;

static timertype t0, t1;	/* used for timing */

#define UTF8BOM		"\xEF\xBB\xBF"	/* UTF-8 encoding of Unicode BOM */
#define UTF8BOMLENGTH	3	/* length of above */

/* Pagination and simple ASCII-based rendering is provided for SQL
 * sessions. The result set size is limited by the cache size of the
 * Mapi Library. It is sufficiently large to accommodate most result
 * to be browsed manually.
 *
 * The pagewidth determines the maximum space allocated for a single
 * row. If the total space required is larger, then a heuristic
 * routine is called to distribute the available space. Attribute
 * values may then span multiple lines. Setting the pagewidth to 0
 * turns off row size control. */

#ifdef HAVE_POPEN
static char *pager = 0;		/* use external pager */
#include <signal.h>		/* to block SIGPIPE */
#endif
static int rowsperpage = 0;	/* for SQL pagination */
static int pagewidth = 0;	/* -1: take whatever is necessary, >0: limit */
static bool pagewidthset = false; /* whether the user set the width explicitly */
static int croppedfields = 0;	/* whatever got cropped/truncated */
static bool firstcrop = true;	/* first time we see cropping/truncation */

enum modifiers {
	NOmodifier,
	DEBUGmodifier
};
static enum modifiers specials = NOmodifier;
/* set when we see DEBUG (only if mode == SQL).  Also retain these
 * modes until after you have received the answer. */

/* keep these aligned, the MINCOLSIZE should ensure you can always
 * write the NULLSTRING */
#define MINCOLSIZE 4
static char default_nullstring[] = "null";
static char *nullstring = default_nullstring;
/* this is the minimum size (that still makes some sense) for writing
 * variable length columns */
#define MINVARCOLSIZE 10

#include <time.h>
#ifdef HAVE_FTIME
#include <sys/timeb.h>		/* ftime */
#endif
#ifdef HAVE_SYS_TIME_H
#include <sys/time.h>		/* gettimeofday */
#endif
#ifdef HAVE_STROPTS_H
#include <stropts.h>		/* ioctl on Solaris */
#endif
#ifdef HAVE_SYS_IOCTL_H
#include <sys/ioctl.h>
#endif
#ifdef HAVE_TERMIOS_H
#include <termios.h>		/* TIOCGWINSZ/TIOCSWINSZ */
#endif

#if defined(_MSC_VER) && _MSC_VER >= 1400
#define fileno _fileno
#endif

#define my_isspace(c)	((c) == '\f' || (c) == '\n' || (c) == ' ')

#include <ctype.h>
#include "mhelp.h"

static timertype
gettime(void)
{
	/* Return the time in milliseconds since an epoch.  The epoch
	   is roughly the time this program started. */
#ifdef _MSC_VER
	static LARGE_INTEGER freq, start;	/* automatically initialized to 0 */
	LARGE_INTEGER ctr;

	if (start.QuadPart == 0 &&
	    (!QueryPerformanceFrequency(&freq) ||
	     !QueryPerformanceCounter(&start)))
		start.QuadPart = -1;
	if (start.QuadPart > 0) {
		QueryPerformanceCounter(&ctr);
		return (timertype) (((ctr.QuadPart - start.QuadPart) * 1000000) / freq.QuadPart);
	}
#endif
#ifdef HAVE_GETTIMEOFDAY
	{
		static struct timeval tpbase;	/* automatically initialized to 0 */
		struct timeval tp;

		if (tpbase.tv_sec == 0)
			gettimeofday(&tpbase, NULL);
		gettimeofday(&tp, NULL);
		tp.tv_sec -= tpbase.tv_sec;
		return (timertype) tp.tv_sec * 1000000 + (timertype) tp.tv_usec;
	}
#else
#ifdef HAVE_FTIME
	{
		static struct timeb tbbase;	/* automatically initialized to 0 */
		struct timeb tb;

		if (tbbase.time == 0)
			ftime(&tbbase);
		ftime(&tb);
		tb.time -= tbbase.time;
		return (timertype) tb.time * 1000000 + (timertype) tb.millitm * 1000;
	}
#endif	/* HAVE_FTIME */
#endif	/* HAVE_GETTIMEOFDAY */
}

static void
timerStart(void)
{
	t0 = gettime();
}

static void
timerPause(void)
{
	t1 = gettime();
	if (t0 == 0)
		t0 = t1;
}

static void
timerResume(void)
{
	if (t1 == 0)
		t1 = gettime();
	assert(t1 >= t0);
	t0 = gettime() - (t1 - t0);
}

static void
timerEnd(void)
{
	mnstr_flush(toConsole);
	t1 = gettime();
	assert(t1 >= t0);
}

static timertype th = 0;
static void
timerHumanStop(void)
{
	th = gettime();
}

static enum itimers {
	T_NONE = 0,	// don't render the timing information
	T_CLOCK,	// render wallclock time in human readable format
	T_PERF		// return detailed performance
} timermode = T_NONE;

static bool timerHumanCalled = false;
static void
timerHuman(int64_t sqloptimizer, int64_t maloptimizer, int64_t querytime, bool singleinstr, bool total)
{
	timertype t = th - t0;

	timerHumanCalled = true;

	/*
	 * report only the times we do actually measure:
	 * - client-measured wall-clock time per query only when executing individual queries,
	 *   otherwise only the total wall-clock time at the end of a batch;
	 * - server-measured detailed performance measures only per query.
	 */

	/* "(singleinstr != total)" is C for (logical) "(singleinstr XOR total)" */
	if (timermode == T_CLOCK && (singleinstr != total)) {
		/* print wall-clock in "human-friendly" format */
		fflush(stderr);
		mnstr_flush(toConsole);
		if (t / 1000 < 1000) {
			fprintf(stderr, "clk: %" PRId64 ".%03d ms\n", t / 1000, (int) (t % 1000));
			fflush(stderr);
			return;
		}
		t /= 1000;
		if (t / 1000 < 60) {
			fprintf(stderr, "clk: %" PRId64 ".%03d sec\n", t / 1000, (int) (t % 1000));
			fflush(stderr);
			return;
		}
		t /= 1000;
		if (t / 60 < 60) {
			fprintf(stderr, "clk: %" PRId64 ":%02d min\n", t / 60, (int) (t % 60));
			fflush(stderr);
			return;
		}
		t /= 60;
		fprintf(stderr, "clk: %" PRId64 ":%02d h\n", t / 60, (int) (t % 60));
		fflush(stderr);
		return;
	}
	if (timermode == T_PERF && (!total || singleinstr != total)) {
		/* for performance measures we use milliseconds as the base */
		fflush(stderr);
		mnstr_flush(toConsole);
		if (!total)
			fprintf(stderr, "sql:%" PRId64 ".%03d opt:%" PRId64 ".%03d run:%" PRId64 ".%03d ",
				 sqloptimizer / 1000, (int) (sqloptimizer % 1000),
				 maloptimizer / 1000, (int) (maloptimizer % 1000),
				 querytime / 1000, (int) (querytime % 1000));
		if (singleinstr != total)
			fprintf(stderr, "clk:%" PRId64 ".%03d ", t / 1000, (int) (t % 1000));
		fprintf(stderr, "ms\n");
		fflush(stderr);
		return;
	}
	return;
}

/* The Mapi library eats away the comment lines, which we need to
 * detect end of debugging. We overload the routine to our liking. */

static char *
fetch_line(MapiHdl hdl)
{
	char *reply;

	if ((reply = mapi_fetch_line(hdl)) == NULL)
		return NULL;
	if (strncmp(reply, "mdb>#", 5) == 0) {
		if (strncmp(reply, "mdb>#EOD", 8) == 0)
			setPrompt();
		else
			sprintf(promptbuf, "mdb>");
	}
	return reply;
}

static int
fetch_row(MapiHdl hdl)
{
	char *reply;

	do {
		if ((reply = fetch_line(hdl)) == NULL)
			return 0;
	} while (*reply != '[' && *reply != '=');
	return mapi_split_line(hdl);
}

static void
SQLsetSpecial(const char *command)
{
	if (mode == SQL && command && specials == NOmodifier) {
		/* catch the specials for better rendering */
		while (*command == ' ' || *command == '\t')
			command++;
		if (strncmp(command, "debug", 5) == 0)
			specials = DEBUGmodifier;
		else
			specials = NOmodifier;
	}
}

/* return the display length of a UTF-8 string
   if e is not NULL, return length up to e */
static size_t
utf8strlenmax(char *s, char *e, size_t max, char **t)
{
	size_t len = 0, len0 = 0;
	int c;
	int n;
	char *t0 = s;

	assert(max == 0 || t != NULL);
	if (s == NULL)
		return 0;
	c = 0;
	n = 0;
	while (*s != 0 && (e == NULL || s < e)) {
		if (*s == '\n') {
			assert(n == 0);
			if (max) {
				*t = s;
				return len;
			}
			len++;
			n = 0;
		} else if ((*s & 0x80) == 0) {
			assert(n == 0);
			len++;
			n = 0;
		} else if ((*s & 0xC0) == 0x80) {
			c = (c << 6) | (*s & 0x3F);
			if (--n == 0) {
				/* last byte of a multi-byte character */
				len++;
				/* this list was created by combining
				 * the code points marked as
				 * Emoji_Presentation in
				 * /usr/share/unicode/emoji/emoji-data.txt
				 * and code points marked either F or
				 * W in EastAsianWidth.txt; this list
				 * is up-to-date with Unicode 9.0 */
				if ((0x1100 <= c && c <= 0x115F) ||
				    (0x231A <= c && c <= 0x231B) ||
				    (0x2329 <= c && c <= 0x232A) ||
				    (0x23E9 <= c && c <= 0x23EC) ||
				    c == 0x23F0 ||
				    c == 0x23F3 ||
				    (0x25FD <= c && c <= 0x25FE) ||
				    (0x2614 <= c && c <= 0x2615) ||
				    (0x2648 <= c && c <= 0x2653) ||
				    c == 0x267F ||
				    c == 0x2693 ||
				    c == 0x26A1 ||
				    (0x26AA <= c && c <= 0x26AB) ||
				    (0x26BD <= c && c <= 0x26BE) ||
				    (0x26C4 <= c && c <= 0x26C5) ||
				    c == 0x26CE ||
				    c == 0x26D4 ||
				    c == 0x26EA ||
				    (0x26F2 <= c && c <= 0x26F3) ||
				    c == 0x26F5 ||
				    c == 0x26FA ||
				    c == 0x26FD ||
				    c == 0x2705 ||
				    (0x270A <= c && c <= 0x270B) ||
				    c == 0x2728 ||
				    c == 0x274C ||
				    c == 0x274E ||
				    (0x2753 <= c && c <= 0x2755) ||
				    c == 0x2757 ||
				    (0x2795 <= c && c <= 0x2797) ||
				    c == 0x27B0 ||
				    c == 0x27BF ||
				    (0x2B1B <= c && c <= 0x2B1C) ||
				    c == 0x2B50 ||
				    c == 0x2B55 ||
				    (0x2E80 <= c && c <= 0x2E99) ||
				    (0x2E9B <= c && c <= 0x2EF3) ||
				    (0x2F00 <= c && c <= 0x2FD5) ||
				    (0x2FF0 <= c && c <= 0x2FFB) ||
				    (0x3000 <= c && c <= 0x303E) ||
				    (0x3041 <= c && c <= 0x3096) ||
				    (0x3099 <= c && c <= 0x30FF) ||
				    (0x3105 <= c && c <= 0x312D) ||
				    (0x3131 <= c && c <= 0x318E) ||
				    (0x3190 <= c && c <= 0x31BA) ||
				    (0x31C0 <= c && c <= 0x31E3) ||
				    (0x31F0 <= c && c <= 0x321E) ||
				    (0x3220 <= c && c <= 0x3247) ||
				    (0x3250 <= c && c <= 0x32FE) ||
				    (0x3300 <= c && c <= 0x4DBF) ||
				    (0x4E00 <= c && c <= 0xA48C) ||
				    (0xA490 <= c && c <= 0xA4C6) ||
				    (0xA960 <= c && c <= 0xA97C) ||
				    (0xAC00 <= c && c <= 0xD7A3) ||
				    (0xF900 <= c && c <= 0xFAFF) ||
				    (0xFE10 <= c && c <= 0xFE19) ||
				    (0xFE30 <= c && c <= 0xFE52) ||
				    (0xFE54 <= c && c <= 0xFE66) ||
				    (0xFE68 <= c && c <= 0xFE6B) ||
				    (0xFF01 <= c && c <= 0xFF60) ||
				    (0xFFE0 <= c && c <= 0xFFE6) ||
				    c == 0x16FE0 ||
				    (0x17000 <= c && c <= 0x187EC) ||
				    (0x18800 <= c && c <= 0x18AF2) ||
				    (0x1B000 <= c && c <= 0x1B001) ||
				    c == 0x1F004 ||
				    c == 0x1F0CF ||
				    c == 0x1F18E ||
				    (0x1F191 <= c && c <= 0x1F19A) ||
				    /* removed 0x1F1E6..0x1F1FF */
				    (0x1F200 <= c && c <= 0x1F202) ||
				    (0x1F210 <= c && c <= 0x1F23B) ||
				    (0x1F240 <= c && c <= 0x1F248) ||
				    (0x1F250 <= c && c <= 0x1F251) ||
				    (0x1F300 <= c && c <= 0x1F320) ||
				    (0x1F32D <= c && c <= 0x1F335) ||
				    (0x1F337 <= c && c <= 0x1F37C) ||
				    (0x1F37E <= c && c <= 0x1F393) ||
				    (0x1F3A0 <= c && c <= 0x1F3CA) ||
				    (0x1F3CF <= c && c <= 0x1F3D3) ||
				    (0x1F3E0 <= c && c <= 0x1F3F0) ||
				    c == 0x1F3F4 ||
				    (0x1F3F8 <= c && c <= 0x1F43E) ||
				    c == 0x1F440 ||
				    (0x1F442 <= c && c <= 0x1F4FC) ||
				    (0x1F4FF <= c && c <= 0x1F53D) ||
				    (0x1F54B <= c && c <= 0x1F54E) ||
				    (0x1F550 <= c && c <= 0x1F567) ||
				    c == 0x1F57A ||
				    (0x1F595 <= c && c <= 0x1F596) ||
				    c == 0x1F5A4 ||
				    (0x1F5FB <= c && c <= 0x1F64F) ||
				    (0x1F680 <= c && c <= 0x1F6C5) ||
				    c == 0x1F6CC ||
				    (0x1F6D0 <= c && c <= 0x1F6D2) ||
				    (0x1F6EB <= c && c <= 0x1F6EC) ||
				    (0x1F6F4 <= c && c <= 0x1F6F6) ||
				    (0x1F910 <= c && c <= 0x1F91E) ||
				    (0x1F920 <= c && c <= 0x1F927) ||
				    c == 0x1F930 ||
				    (0x1F933 <= c && c <= 0x1F93E) ||
				    (0x1F940 <= c && c <= 0x1F94B) ||
				    (0x1F950 <= c && c <= 0x1F95E) ||
				    (0x1F980 <= c && c <= 0x1F991) ||
				    c == 0x1F9C0 ||
				    (0x20000 <= c && c <= 0x2FFFD) ||
				    (0x30000 <= c && c <= 0x3FFFD))
					len++;
			}
		} else if ((*s & 0xE0) == 0xC0) {
			assert(n == 0);
			n = 1;
			c = *s & 0x1F;
		} else if ((*s & 0xF0) == 0xE0) {
			assert(n == 0);
			n = 2;
			c = *s & 0x0F;
		} else if ((*s & 0xF8) == 0xF0) {
			assert(n == 0);
			n = 3;
			c = *s & 0x07;
		} else if ((*s & 0xFC) == 0xF8) {
			assert(n == 0);
			n = 4;
			c = *s & 0x03;
		} else {
			assert(0);
			n = 0;
		}
		s++;
		if (n == 0) {
			if (max != 0) {
				if (len > max) {
					*t = t0;
					return len0;
				}
				if (len == max) {
					*t = s;
					return len;
				}
			}
			t0 = s;
			len0 = len;
		}
	}
	if (max != 0)
		*t = s;
	return len;
}

static size_t
utf8strlen(char *s, char *e)
{
	return utf8strlenmax(s, e, 0, NULL);
}

/* skip the specified number of UTF-8 characters, but stop at a newline */
static char *
utf8skip(char *s, size_t i)
{
	utf8strlenmax(s, NULL, i, &s);
	return s;
}

static int
SQLrow(int *len, int *numeric, char **rest, int fields, int trim, char wm)
{
	int i;
	bool more, first = true;
	char *t;
	int rows = 0;		/* return number of output lines printed */
	size_t ulen;
	int *cutafter = malloc(sizeof(int) * fields);

	if (cutafter == NULL) {
		fprintf(stderr,"Malloc for SQLrow failed");
		exit(2);
	}
	/* trim the text if needed */
	if (trim == 1) {
		for (i = 0; i < fields; i++) {
			if ((t = rest[i]) != NULL &&
			    utf8strlen(t, NULL) > (size_t) len[i]) {
				/* eat leading whitespace */
				while (*t != 0 && my_isspace(*t))
					t++;
				rest[i] = t;
			}
		}
	}

	for (i = 0; i < fields; i++)
		cutafter[i] = -1;

	do {
		more = false;
		for (i = 0; i < fields; i++) {
			if (rest[i] == NULL || *rest[i] == 0) {
				mnstr_printf(toConsole, "%c %*s ",
					     first ? '|' : i > 0 && cutafter[i - 1] == 0 ? '>' : ':',
					     len[i], "");
			} else {
				ulen = utf8strlen(rest[i], NULL);

				if (first && trim == 2) {
					/* calculate the height of
					 * this field according to the
					 * golden ratio, with a
					 * correction for a terminal
					 * screen (1.62 * 2 -> 3 :
					 * 9.72~10) */
					if (ulen > (size_t) len[i]) {
						cutafter[i] = 3 * len[i] / 10;
						if (cutafter[i] == 1)
							cutafter[i]++;
					}
				}

				/* on each cycle we get closer to the limit */
				if (cutafter[i] >= 0)
					cutafter[i]--;

				/* break the string into pieces and
				 * left-adjust them in the column */
				t = strchr(rest[i], '\n');
				if (ulen > (size_t) len[i] || t) {
					char *s;

					t = utf8skip(rest[i], len[i]);
					if (trim == 1) {
						while (t > rest[i] && !my_isspace(*t))
							while ((*--t & 0xC0) == 0x80)
								;
						if (t == rest[i] && !my_isspace(*t))
							t = utf8skip(rest[i], len[i]);
					}
					mnstr_printf(toConsole, "%c",
						     first ? '|' : i > 0 && cutafter[i - 1] == 0 ? '>' : ':');
					if (numeric[i])
						mnstr_printf(toConsole, "%*s",
							     (int) (len[i] - (ulen - utf8strlen(t, NULL))),
							     "");

					if (!numeric[i]) {
						/* replace tabs with a
						 * single space to
						 * avoid screwup the
						 * width
						 * calculations */
						for (s = rest[i]; *s != *t; s++)
							if (*s == '\t')
								*s = ' ';
					}

					s = t;
					if (trim == 1)
						while (my_isspace(*s))
							s++;
					if (trim == 2 && *s == '\n')
						s++;
					if (*s && cutafter[i] == 0) {
						t = utf8skip(rest[i], len[i] - 2);
						s = t;
						if (trim == 1)
							while (my_isspace(*s))
								s++;
						if (trim == 2 && *s == '\n')
							s++;
						mnstr_printf(toConsole, " %.*s...%*s",
							     (int) (t - rest[i]),
							     rest[i],
							     len[i] - 2 - (int) utf8strlen(rest[i], t),
							     "");
						croppedfields++;
					} else {
						mnstr_printf(toConsole, " %.*s ",
							     (int) (t - rest[i]),
							     rest[i]);
						if (!numeric[i])
							mnstr_printf(toConsole, "%*s",
								     (int) (len[i] - (ulen - utf8strlen(t, NULL))),
								     "");
					}
					rest[i] = *s ? s : 0;
					if (rest[i] == NULL) {
						/* avoid > as border
						 * marker if
						 * everything actually
						 * just fits */
						cutafter[i] = -1;
					}
					if (cutafter[i] == 0)
						rest[i] = NULL;
					if (rest[i])
						more = true;
				} else {
					mnstr_printf(toConsole, "%c",
						     first ? '|' : i > 0 && cutafter[i - 1] == 0 ? '>' : ':');
					if (numeric[i]) {
						mnstr_printf(toConsole, "%*s",
							     (int) (len[i] - ulen),
							     "");
						mnstr_printf(toConsole, " %s ",
							     rest[i]);
					}
					if (!numeric[i]) {
						char *p;
						/* replace tabs with a
						 * single space to
						 * avoid screwup the
						 * width
						 * calculations */
						for (p = rest[i]; *p != '\0'; p++)
							if (*p == '\t')
								*p = ' ';
						mnstr_printf(toConsole, " %s ",
							     rest[i]);
						mnstr_printf(toConsole, "%*s",
							     (int) (len[i] - ulen),
							     "");
					}
					rest[i] = 0;
					/* avoid > as border marker if
					 * everything actually just
					 * fits */
					if (cutafter[i] == 0)
						cutafter[i] = -1;
				}
			}
		}
		mnstr_printf(toConsole, "%c%s\n",
			     first ? '|' : i > 0 && cutafter[i - 1] == 0 ? '>' : ':',
			     wm ? ">" : "");
		first = false;
		rows++;
	} while (more);

	free(cutafter);
	return rows;
}

static void
XMLprdata(const char *val)
{
	if (val == NULL)
		return;
	while (*val) {
		if (*val == '&')
			mnstr_printf(toConsole, "&amp;");
		else if (*val == '<')
			mnstr_printf(toConsole, "&lt;");
		else if (*val == '>')
			mnstr_printf(toConsole, "&gt;");
		else if (*val == '"')
			mnstr_printf(toConsole, "&quot;");
		else if (*val == '\'')
			mnstr_printf(toConsole, "&apos;");
		else if ((*val & 0xFF) < 0x20)	/* control character */
			mnstr_printf(toConsole, "&#%d;", *val & 0xFF);
		else if ((*val & 0x80) != 0 /* && encoding != NULL */ ) {
			int n;
			unsigned int m;
			unsigned int c = *val & 0x7F;

			for (n = 0, m = 0x40; c & m; n++, m >>= 1)
				c &= ~m;
			while (--n >= 0)
				c = (c << 6) | (*++val & 0x3F);
			mnstr_printf(toConsole, "&#x%x;", c);
		} else
			mnstr_write(toConsole, val, 1, 1);
		val++;
	}
}

static void
XMLprattr(const char *name, const char *val)
{
	mnstr_printf(toConsole, " %s=\"", name);
	XMLprdata(val);
	mnstr_write(toConsole, "\"", 1, 1);
}

static void
XMLrenderer(MapiHdl hdl)
{
	int i, fields;
	char *name;

	/* we must use toConsole since the XML file is encoded in UTF-8 */
	mnstr_flush(toConsole);
	mnstr_printf(toConsole, "<?xml version='1.0' encoding='UTF-8'?>\n");
	mnstr_printf(toConsole,
		     "<!DOCTYPE table [\n"
		     " <!ELEMENT table (row)*>\n" /* a table consists of zero or more rows */
		     " <!ELEMENT row (column)+>\n"	/* a row consists of one or more columns */
		     " <!ELEMENT column (#PCDATA)>\n"
		     " <!ATTLIST table name CDATA #IMPLIED>\n"	/* a table may have a name */
		     " <!ATTLIST column name CDATA #IMPLIED\n"	/* a column may have a name */
		     "                  isnull (true|false) 'false'>]>\n");
	mnstr_printf(toConsole, "<table");
	name = mapi_get_table(hdl, 0);
	if (name != NULL && *name != 0)
		XMLprattr("name", name);
	mnstr_printf(toConsole, ">\n");
	while (!mnstr_errnr(toConsole) && (fields = fetch_row(hdl)) != 0) {
		mnstr_printf(toConsole, "<row>");
		for (i = 0; i < fields; i++) {
			char *data = mapi_fetch_field(hdl, i);

			mnstr_printf(toConsole, "<column");
			name = mapi_get_name(hdl, i);
			if (name != NULL && *name != 0)
				XMLprattr("name", name);
			if (data == NULL) {
				XMLprattr("isnull", "true");
				mnstr_write(toConsole, "/", 1, 1);
			}
			mnstr_write(toConsole, ">", 1, 1);
			if (data) {
				XMLprdata(data);
				mnstr_printf(toConsole, "</column>");
			}
		}
		mnstr_printf(toConsole, "</row>\n");
	}
	mnstr_printf(toConsole, "</table>\n");
	mnstr_flush(toConsole);
}

static void
EXPANDEDrenderer(MapiHdl hdl)
{
	int i, fields, fieldw, rec = 0;

	fields = mapi_get_field_count(hdl);
	fieldw = 0;
	for (i = 0; i < fields; i++) {
		int w = (int) utf8strlen(mapi_get_name(hdl, i), NULL);
		if (w > fieldw)
			fieldw = w;
	}
	while (!mnstr_errnr(toConsole) && (fields = fetch_row(hdl)) != 0) {
		int valuew = 0, len;
		++rec;
		for (i = 0; i < fields; i++) {
			char *data = mapi_fetch_field(hdl, i);
			char *edata;
			int w;

			if (data == NULL)
				data = nullstring;
			do {
				edata = utf8skip(data, ~(size_t)0);
				w = (int) utf8strlen(data, edata);
				if (w > valuew)
					valuew = w;
				data = edata;
				if (*data)
					data++;
			} while (*edata);
		}
		len = mnstr_printf(toConsole, "-[ RECORD %d ]-", rec);
		while (len++ < fieldw + valuew + 3)
			mnstr_write(toConsole, "-", 1, 1);
		mnstr_write(toConsole, "\n", 1, 1);
		for (i = 0; i < fields; i++) {
			char *data = mapi_fetch_field(hdl, i);
			char *edata;
			const char *name = mapi_get_name(hdl, i);
			if (data == NULL)
				data = nullstring;
			do {
				edata = utf8skip(data, ~(size_t)0);
				mnstr_printf(toConsole, "%-*s | %.*s\n", fieldw, name, (int) (edata - data), data);
				name = "";
				data = edata;
				if (*data)
					data++;
			} while (*edata);
		}
	}
	mnstr_flush(toConsole);
}

static void
CSVrenderer(MapiHdl hdl)
{
	int fields;
	char *s;
	char *sep = separator;
	int i;

	if (csvheader) {
		fields = mapi_get_field_count(hdl);
		for (i = 0; i < fields; i++) {
			s = mapi_get_name(hdl, i);
			if (s == NULL)
				s = "";
			mnstr_printf(toConsole, "%s%s", i == 0 ? "" : sep, s);
		}
		mnstr_printf(toConsole, "\n");
	}
	while (!mnstr_errnr(toConsole) && (fields = fetch_row(hdl)) != 0) {
		for (i = 0; i < fields; i++) {
			s = mapi_fetch_field(hdl, i);
			if (s == NULL)
				s = nullstring == default_nullstring ? "" : nullstring;
			if (strchr(s, *sep) != NULL ||
			    strchr(s, '\n') != NULL ||
			    strchr(s, '"') != NULL) {
				mnstr_printf(toConsole, "%s\"",
					     i == 0 ? "" : sep);
				while (*s) {
					switch (*s) {
					case '\n':
						mnstr_write(toConsole, "\\n", 1, 2);
						break;
					case '\t':
						mnstr_write(toConsole, "\\t", 1, 2);
						break;
					case '\r':
						mnstr_write(toConsole, "\\r", 1, 2);
						break;
					case '\\':
						mnstr_write(toConsole, "\\\\", 1, 2);
						break;
					case '"':
						mnstr_write(toConsole, "\"\"", 1, 2);
						break;
					default:
						mnstr_write(toConsole, s, 1, 1);
						break;
					}
					s++;
				}
				mnstr_write(toConsole, "\"", 1, 1);
			} else
				mnstr_printf(toConsole, "%s%s",
					     i == 0 ? "" : sep, s);
		}
		mnstr_printf(toConsole, "\n");
	}
}

static void
SQLseparator(int *len, int fields, char sep)
{
	int i, j;

	mnstr_printf(toConsole, "+");
	for (i = 0; i < fields; i++) {
		mnstr_printf(toConsole, "%c", sep);
		for (j = 0; j < (len[i] < 0 ? -len[i] : len[i]); j++)
			mnstr_printf(toConsole, "%c", sep);
		mnstr_printf(toConsole, "%c+", sep);
	}
	mnstr_printf(toConsole, "\n");
}

static void
SQLqueryEcho(MapiHdl hdl)
{
	if (echoquery) {
		char *qry;

		qry = mapi_get_query(hdl);
		if (qry) {
			if (formatter != TABLEformatter) {
				char *p = qry;
				char *q = p;
				while ((q = strchr(q, '\n')) != NULL) {
					*q++ = '\0';
					mnstr_printf(toConsole, "#%s\n", p);
					p = q;
				}
				if (*p) {
					/* query does not end in \n */
					mnstr_printf(toConsole, "#%s\n", p);
				}
			} else {
				size_t qrylen = strlen(qry);

				mnstr_printf(toConsole, "%s", qry);
				if (qrylen > 0 && qry[qrylen - 1] != '\n') {
					/* query does not end in \n */
					mnstr_printf(toConsole, "\n");
				}
			}
			free(qry);
		}
	}
}

/* state machine to recognize integers, floating point numbers, OIDs */
static char *
classify(const char *s, size_t l)
{
	/* state is the current state of the state machine:
	 * 0 - initial state, no input seen
	 * 1 - initial sign
	 * 2 - valid integer (optionally preceded by a sign)
	 * 3 - valid integer, followed by a decimal point
	 * 4 - fixed point number of the form [sign] digits period digits
	 * 5 - exponent marker after integer or fixed point number
	 * 6 - sign after exponent marker
	 * 7 - valid floating point number with exponent
	 * 8 - integer followed by single 'L'
	 * 9 - integer followed by 'LL' (lng)
	 * 10 - fixed or floating point number followed by single 'L'
	 * 11 - fixed or floating point number followed by 'LL' (dbl)
	 * 12 - integer followed by '@'
	 * 13 - valid OID (integer followed by '@0')
	 */
	int state = 0;

	if ((l == 4 && strcmp(s, "true") == 0) ||
	    (l == 5 && strcmp(s, "false") == 0))
		return "bit";
	while (l != 0) {
		if (*s == 0)
			return "str";
		switch (*s) {
		case '0':
			if (state == 12) {
				state = 13;	/* int + '@0' (oid) */
				break;
			}
			/* fall through */
		case '1':
		case '2':
		case '3':
		case '4':
		case '5':
		case '6':
		case '7':
		case '8':
		case '9':
			switch (state) {
			case 0:
			case 1:
				state = 2;	/* digit after optional sign */
				break;
			case 3:
				state = 4;	/* digit after decimal point */
				break;
			case 5:
			case 6:
				state = 7;	/* digit after exponent marker and optional sign */
				break;
			case 2:
			case 4:
			case 7:
				break;		/* more digits */
			default:
				return "str";
			}
			break;
		case '.':
			if (state == 2)
				state = 3;	/* decimal point */
			else
				return "str";
			break;
		case 'e':
		case 'E':
			if (state == 2 || state == 4)
				state = 5;	/* exponent marker */
			else
				return "str";
			break;
		case '+':
		case '-':
			if (state == 0)
				state = 1;	/* sign at start */
			else if (state == 5)
				state = 6;	/* sign after exponent marker */
			else
				return "str";
			break;
		case '@':
			if (state == 2)
				state = 12;	/* OID marker */
			else
				return "str";
			break;
		case 'L':
			switch (state) {
			case 2:
				state = 8;	/* int + 'L' */
				break;
			case 8:
				state = 9;	/* int + 'LL' */
				break;
			case 4:
			case 7:
				state = 10;	/* dbl + 'L' */
				break;
			case 10:
				state = 11;	/* dbl + 'LL' */
				break;
			default:
				return "str";
			}
			break;
		default:
			return "str";
		}
		s++;
		l--;
	}
	switch (state) {
	case 13:
		return "oid";
	case 2:
		return "int";
	case 4:
	case 7:
	case 11:
		return "dbl";
	case 9:
		return "lng";
	default:
		return "str";
	}
}

static void
TESTrenderer(MapiHdl hdl)
{
	int fields;
	char *reply;
	char *s;
	size_t l;
	char *tp;
	char *sep;
	int i;

	while (!mnstr_errnr(toConsole) && (reply = fetch_line(hdl)) != 0) {
		if (*reply != '[') {
			if (*reply == '=')
				reply++;
			mnstr_printf(toConsole, "%s\n", reply);
			continue;
		}
		fields = mapi_split_line(hdl);
		sep = "[ ";
		for (i = 0; i < fields; i++) {
			s = mapi_fetch_field(hdl, i);
			l = mapi_fetch_field_len(hdl, i);
			tp = mapi_get_type(hdl, i);
			if (strcmp(tp, "unknown") == 0)
				tp = classify(s, l);
			mnstr_printf(toConsole, "%s", sep);
			sep = ",\t";
			if (s == NULL)
				mnstr_printf(toConsole, "%s", mode == SQL ? "NULL" : "nil");
			else if (strcmp(tp, "varchar") == 0 ||
				 strcmp(tp, "char") == 0 ||
				 strcmp(tp, "clob") == 0 ||
				 strcmp(tp, "str") == 0 ||
				 strcmp(tp, "json") == 0 ||
				 /* NULL byte in string? */
				 strlen(s) < l ||
				 /* start or end with white space? */
				 my_isspace(*s) ||
				 (l > 0 && my_isspace(s[l - 1])) ||
				 /* timezone can have embedded comma */
				 strcmp(tp, "timezone") == 0 ||
				 /* a bunch of geom types */
				 strcmp(tp, "curve") == 0 ||
				 strcmp(tp, "geometry") == 0 ||
				 strcmp(tp, "linestring") == 0 ||
				 strcmp(tp, "mbr") == 0 ||
				 strcmp(tp, "multilinestring") == 0 ||
				 strcmp(tp, "point") == 0 ||
				 strcmp(tp, "polygon") == 0 ||
				 strcmp(tp, "surface") == 0) {
				mnstr_printf(toConsole, "\"");
				while (l != 0) {
					switch (*s) {
					case '\n':
						mnstr_write(toConsole, "\\n", 1, 2);
						break;
					case '\t':
						mnstr_write(toConsole, "\\t", 1, 2);
						break;
					case '\r':
						mnstr_write(toConsole, "\\r", 1, 2);
						break;
					case '\\':
						mnstr_write(toConsole, "\\\\", 1, 2);
						break;
					case '"':
						mnstr_write(toConsole, "\\\"", 1, 2);
						break;
					case '0':
					case '1':
					case '2':
					case '3':
					case '4':
					case '5':
					case '6':
					case '7':
					case '8':
					case '9':
						if (strcmp(tp, "curve") == 0 ||
						    strcmp(tp, "geometry") == 0 ||
						    strcmp(tp, "linestring") == 0 ||
						    strcmp(tp, "mbr") == 0 ||
						    strcmp(tp, "multilinestring") == 0 ||
						    strcmp(tp, "point") == 0 ||
						    strcmp(tp, "polygon") == 0 ||
						    strcmp(tp, "surface") == 0) {
							char *e;
							double d;
							d = strtod(s, &e);
							if (s != e) {
								mnstr_printf(toConsole, "%.10g", d);
								l -= e - s;
								s = e;
								continue;
							}
						}
						/* fall through */
					default:
						if ((unsigned char) *s < ' ')
							mnstr_printf(toConsole,
								     "\\%03o",
								     (int) (unsigned char) *s);
						else
							mnstr_write(toConsole, s, 1, 1);
						break;
					}
					s++;
					l--;
				}
				mnstr_write(toConsole, "\"", 1, 1);
			} else if (strcmp(tp, "double") == 0 ||
				   strcmp(tp, "dbl") == 0) {
				char buf[32];
				int j;
				double v;
				if (strcmp(s, "-0") == 0) /* normalize -0 */
					s = "0";
				v = strtod(s, NULL);
				for (j = 4; j < 11; j++) {
					snprintf(buf, sizeof(buf), "%.*g", j, v);
					if (v == strtod(buf, NULL))
						break;
				}
				mnstr_printf(toConsole, "%s", buf);
			} else if (strcmp(tp, "real") == 0) {
				char buf[32];
				int j;
				float v;
				if (strcmp(s, "-0") == 0) /* normalize -0 */
					s = "0";
				v = strtof(s, NULL);
				for (j = 4; j < 6; j++) {
					snprintf(buf, sizeof(buf), "%.*g", j, v);
					if (v == strtof(buf, NULL))
						break;
				}
				mnstr_printf(toConsole, "%s", buf);
			} else
				mnstr_printf(toConsole, "%s", s);
		}
		mnstr_printf(toConsole, "\t]\n");
	}
}

static void
RAWrenderer(MapiHdl hdl)
{
	char *line;

	while ((line = fetch_line(hdl)) != 0) {
		if (*line == '=')
			line++;
		mnstr_printf(toConsole, "%s\n", line);
	}
}

static void
SAMrenderer(MapiHdl hdl)
{
	/* Variables keeping track of which result set fields map to
	 * qname, flag etc. (-1 means that it does not occur in result
	 * set) */
	int field_qname = -1;
	int field_flag = -1;
	int field_rname = -1;
	int field_pos = -1;
	int field_mapq = -1;
	int field_cigar = -1;
	int field_rnext = -1;
	int field_pnext = -1;
	int field_tlen = -1;
	int field_seq = -1;
	int field_qual = -1;

	int field_count = mapi_get_field_count(hdl);
	int t_fields;

	int i;

	/* First, initialize field variables properly */
	for (i = 0; i < field_count; i++) {
		char *field_name = mapi_get_name(hdl, i);
		if (strcmp(field_name, "qname") == 0)
			field_qname = i;
		else if (strcmp(field_name, "flag" ) == 0)
			field_flag  = i;
		else if (strcmp(field_name, "rname") == 0)
			field_rname = i;
		else if (strcmp(field_name, "pos"  ) == 0)
			field_pos   = i;
		else if (strcmp(field_name, "mapq" ) == 0)
			field_mapq  = i;
		else if (strcmp(field_name, "cigar") == 0)
			field_cigar = i;
		else if (strcmp(field_name, "rnext") == 0)
			field_rnext = i;
		else if (strcmp(field_name, "pnext") == 0)
			field_pnext = i;
		else if (strcmp(field_name, "tlen" ) == 0)
			field_tlen  = i;
		else if (strcmp(field_name, "seq"  ) == 0)
			field_seq   = i;
		else if (strcmp(field_name, "qual" ) == 0)
			field_qual  = i;
		else
			mnstr_printf(stderr_stream, "Unexpected column name in result set: '%s'. Data in this column is not used.\n", field_name);
	}

	/* Write all alignments */
	while (!mnstr_errnr(toConsole) && (t_fields = fetch_row(hdl)) != 0) {
		if (t_fields != field_count) {
			mnstr_printf(stderr_stream,
				     "invalid tuple received from server, "
				     "got %d columns, expected %d, ignoring\n", t_fields, field_count);
			continue;
		}

		/* Write fields to SAM line */
		mnstr_printf(toConsole, "%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n",
			     (field_qname == -1 ? "*"   : mapi_fetch_field(hdl, field_qname)),
			     (field_flag  == -1 ? "0"   : mapi_fetch_field(hdl, field_flag )),
			     (field_rname == -1 ? "*"   : mapi_fetch_field(hdl, field_rname)),
			     (field_pos   == -1 ? "0"   : mapi_fetch_field(hdl, field_pos  )),
			     (field_mapq  == -1 ? "255" : mapi_fetch_field(hdl, field_mapq )),
			     (field_cigar == -1 ? "*"   : mapi_fetch_field(hdl, field_cigar)),
			     (field_rnext == -1 ? "*"   : mapi_fetch_field(hdl, field_rnext)),
			     (field_pnext == -1 ? "0"   : mapi_fetch_field(hdl, field_pnext)),
			     (field_tlen  == -1 ? "0"   : mapi_fetch_field(hdl, field_tlen )),
			     (field_seq   == -1 ? "*"   : mapi_fetch_field(hdl, field_seq  )),
			     (field_qual  == -1 ? "*"   : mapi_fetch_field(hdl, field_qual)));
	}
}

static void
SQLheader(MapiHdl hdl, int *len, int fields, char more)
{
	SQLseparator(len, fields, '-');
	if (mapi_get_name(hdl, 0)) {
		int i;
		char **names = (char **) malloc(fields * sizeof(char *));
		int *numeric = (int *) malloc(fields * sizeof(int));

		if (names == NULL || numeric == NULL) {
			free(names);
			free(numeric);
			fprintf(stderr,"Malloc for SQLheader failed");
			exit(2);
		}
		for (i = 0; i < fields; i++) {
			names[i] = mapi_get_name(hdl, i);
			numeric[i] = 0;
		}
		SQLrow(len, numeric, names, fields, 1, more);
		SQLseparator(len, fields, '=');
		free(names);
		free(numeric);
	}
}

static void
SQLdebugRendering(MapiHdl hdl)
{
	char *reply;
	int cnt = 0;

	sprintf(promptbuf, "mdb>");
	while ((reply = fetch_line(hdl))) {
		cnt++;
		mnstr_printf(toConsole, "%s\n", reply);
		if (strncmp(reply, "mdb>#EOD", 8) == 0) {
			cnt = 0;
			while ((reply = fetch_line(hdl)))
				mnstr_printf(toConsole, "%s\n", reply);
			break;
		}
	}
	if (cnt == 0) {
		setPrompt();
		specials = NOmodifier;
	}
}

static void
SQLpagemove(int *len, int fields, int *ps, bool *silent)
{
	char buf[512];
	ssize_t sz;

	SQLseparator(len, fields, '-');
	mnstr_printf(toConsole, "next page? (continue,quit,next)");
	mnstr_flush(toConsole);
	sz = mnstr_readline(fromConsole, buf, sizeof(buf));
	if (sz > 0) {
		if (buf[0] == 'c')
			*ps = 0;
		if (buf[0] == 'q')
			*silent = true;
		while (sz > 0 && buf[sz - 1] != '\n')
			sz = mnstr_readline(fromConsole, buf, sizeof(buf));
	}
	if (!*silent)
		SQLseparator(len, fields, '-');
}

static void
SQLrenderer(MapiHdl hdl)
{
	int i, total, lentotal, vartotal, minvartotal;
	int fields, rfields, printfields = 0, max = 1, graphwaste = 0;
	int *len = NULL, *hdr = NULL, *numeric = NULL;
	char **rest = NULL;
	char buf[50];
	int ps = rowsperpage;
	bool silent = false;
	int64_t rows = 0;

	croppedfields = 0;
	fields = mapi_get_field_count(hdl);
	rows = mapi_get_row_count(hdl);

	len = calloc(fields, sizeof(*len));
	hdr = calloc(fields, sizeof(*hdr));
	rest = calloc(fields, sizeof(*rest));
	numeric = calloc(fields, sizeof(*numeric));
	if (len == NULL || hdr == NULL || rest == NULL || numeric == NULL) {
		free(len);
		free(hdr);
		free(rest);
		free(numeric);
		fprintf(stderr,"Malloc for SQLrenderer failed");
		exit(2);
	}

	total = 0;
	lentotal = 0;
	vartotal = 0;
	minvartotal = 0;
	for (i = 0; i < fields; i++) {
		char *s;

		len[i] = mapi_get_len(hdl, i);
		if (len[i] == 0 &&
		    ((s = mapi_get_type(hdl, i)) == NULL ||
		     (strcmp(s, "varchar") != 0 &&
		      strcmp(s, "clob") != 0 &&
		      strcmp(s, "char") != 0 &&
		      strcmp(s, "str") != 0 &&
		      strcmp(s, "json") != 0))) {
			/* no table width known, use maximum, rely on
			 * squeezing later on to fix it to whatever is
			 * available; note that for a column type of
			 * varchar, 0 means the complete column is
			 * NULL or empty string, so MINCOLSIZE (below)
			 * will work great */
			len[i] = pagewidth <= 0 ? DEFWIDTH : pagewidth;
		} else if (len[i] == 0 &&
			   strcmp(mapi_get_type(hdl, i), "uuid") == 0) {
			/* we know how large the UUID representation
			 * is, even if the server doesn't */
			len[i] = 36;
		}
		if (len[i] < MINCOLSIZE)
			len[i] = MINCOLSIZE;
		s = mapi_get_name(hdl, i);
		if (s != NULL) {
			size_t l = strlen(s);
			assert(l <= INT_MAX);
			hdr[i] = (int) l;
		} else {
			hdr[i] = 0;
		}
		/* if no rows, just try to draw headers nicely */
		if (rows == 0)
			len[i] = hdr[i];
		s = mapi_get_type(hdl, i);
		numeric[i] = s != NULL &&
			(strcmp(s, "int") == 0 ||
			 strcmp(s, "tinyint") == 0 ||
			 strcmp(s, "bigint") == 0 ||
			 strcmp(s, "hugeint") == 0 ||
			 strcmp(s, "oid") == 0 ||
			 strcmp(s, "smallint") == 0 ||
			 strcmp(s, "double") == 0 ||
			 strcmp(s, "float") == 0 ||
			 strcmp(s, "decimal") == 0);

		if (rows == 0) {
			minvartotal += len[i]; /* don't wrap column headers if no data */
		} else if (numeric[i]) {
			/* minimum size is equal to maximum size */
			minvartotal += len[i];
		} else {
			/* minimum size for wide columns is MINVARCOLSIZE */
			minvartotal += len[i] > MINVARCOLSIZE ? MINVARCOLSIZE : len[i];
		}
		vartotal += len[i];
		total += len[i];

		/* do a very pessimistic calculation to determine if more
		 * columns would actually fit on the screen */
		if (pagewidth > 0 &&
		    ((((printfields + 1) * 3) - 1) + 2) + /* graphwaste */
		    (total - vartotal) + minvartotal > pagewidth) {
			/* this last column was too much */
			total -= len[i];
			if (!numeric[i])
				vartotal -= len[i];
			break;
		}

		lentotal += (hdr[i] > len[i] ? hdr[i] : len[i]);
		printfields++;
	}

	/* what we waste on space on the display is the column separators '
	 * | ', but the edges lack the edgespace of course */
	graphwaste = ((printfields * 3) - 1) + 2;
	/* make sure we can indicate we dropped columns */
	if (fields != printfields)
		graphwaste++;

	/* punish the column headers first until you cannot squeeze any
	 * further */
	while (pagewidth > 0 && graphwaste + lentotal > pagewidth) {
		/* pick the column where the header is longest compared to its
		 * content */
		max = -1;
		for (i = 0; i < printfields; i++) {
			if (hdr[i] > len[i]) {
				if (max == -1 ||
				    hdr[max] - len[max] < hdr[i] - len[i])
					max = i;
			}
		}
		if (max == -1)
			break;
		hdr[max]--;
		lentotal--;
	}

	/* correct the lengths if the headers are wider than the content,
	 * since the headers are maximally squeezed to the content above, if
	 * a header is larger than its content, it means there was space
	 * enough.  If not, the content will be squeezed below. */
	for (i = 0; i < printfields; i++)
		if (len[i] < hdr[i])
			len[i] = hdr[i];

	/* worst case: lentotal = total, which means it still doesn't fit,
	 * values will be squeezed next */
	while (pagewidth > 0 && graphwaste + total > pagewidth) {
		max = -1;
		for (i = 0; i < printfields; i++) {
			if (!numeric[i] && (max == -1 || len[i] > len[max]))
				max = i;
		}

		/* no varsized fields that we can squeeze */
		if (max == -1)
			break;
		/* penalty for largest field */
		len[max]--;
		total--;
		/* no more squeezing possible */
		if (len[max] == 1)
			break;
	}

	SQLheader(hdl, len, printfields, fields != printfields);

	while ((rfields = fetch_row(hdl)) != 0) {
		if (mnstr_errnr(toConsole))
			continue;
		if (rfields != fields) {
			mnstr_printf(stderr_stream,
				     "invalid tuple received from server, "
				     "got %d columns, expected %d, ignoring\n", rfields, fields);
			continue;
		}
		if (silent)
			continue;
		for (i = 0; i < printfields; i++) {
			rest[i] = mapi_fetch_field(hdl, i);
			if (rest[i] == NULL)
				rest[i] = nullstring;
			else {
				char *p = rest[i];

				while ((p = strchr(p, '\r')) != 0) {
					switch (p[1]) {
					case '\0':
						/* end of string: remove CR */
						*p = 0;
						break;
					case '\n':
						/* followed by LF: remove CR */
						/* note: copy including NUL */
						memmove(p, p + 1, strlen(p));
						break;
					default:
						/* replace with ' ' */
						*p = ' ';
						break;
					}
				}
			}
		}

		if (ps > 0 && rows >= ps && fromConsole != NULL) {
			SQLpagemove(len, printfields, &ps, &silent);
			rows = 0;
			if (silent)
				continue;
		}

		rows += SQLrow(len, numeric, rest, printfields, 2, 0);
	}
	if (fields)
		SQLseparator(len, printfields, '-');
	rows = mapi_get_row_count(hdl);
	snprintf(buf, sizeof(buf), "%" PRId64 " rows", rows);
	mnstr_printf(toConsole, "%" PRId64 " tuple%s", rows, rows != 1 ? "s" : "");

	if (fields != printfields || croppedfields > 0)
		mnstr_printf(toConsole, " !");
	if (fields != printfields) {
		rows = fields - printfields;
		mnstr_printf(toConsole, "%" PRId64 " column%s dropped", rows, rows != 1 ? "s" : "");
	}
	if (fields != printfields && croppedfields > 0)
		mnstr_printf(toConsole, ", ");
	if (croppedfields > 0)
		mnstr_printf(toConsole, "%d field%s truncated",
		       croppedfields, croppedfields != 1 ? "s" : "");
	if (fields != printfields || croppedfields > 0) {
		mnstr_printf(toConsole, "!");
		if (firstcrop) {
			firstcrop = false;
			mnstr_printf(toConsole, "\nnote: to disable dropping columns and/or truncating fields use \\w-1");
		}
	}
	mnstr_printf(toConsole, "\n");

	free(len);
	free(hdr);
	free(rest);
	free(numeric);
}

static void
setFormatter(const char *s)
{
	if (separator)
		free(separator);
	separator = NULL;
	csvheader = false;
#ifdef _TWO_DIGIT_EXPONENT
	if (formatter == TESTformatter)
		_set_output_format(0);
#endif
	if (strcmp(s, "sql") == 0) {
		formatter = TABLEformatter;
	} else if (strcmp(s, "csv") == 0) {
		formatter = CSVformatter;
		separator = strdup(",");
	} else if (strncmp(s, "csv=", 4) == 0) {
		formatter = CSVformatter;
		if (s[4] == '"') {
			separator = strdup(s + 5);
			if (separator[strlen(separator) - 1] == '"')
				separator[strlen(separator) - 1] = 0;
		} else
			separator = strdup(s + 4);
	} else if (strncmp(s, "csv+", 4) == 0) {
		formatter = CSVformatter;
		if (s[4] == '"') {
			separator = strdup(s + 5);
			if (separator[strlen(separator) - 1] == '"')
				separator[strlen(separator) - 1] = 0;
		} else
			separator = strdup(s + 4);
		csvheader = true;
	} else if (strcmp(s, "tab") == 0) {
		formatter = CSVformatter;
		separator = strdup("\t");
	} else if (strcmp(s, "raw") == 0) {
		formatter = RAWformatter;
	} else if (strcmp(s, "xml") == 0) {
		formatter = XMLformatter;
	} else if (strcmp(s, "test") == 0) {
#ifdef _TWO_DIGIT_EXPONENT
		_set_output_format(_TWO_DIGIT_EXPONENT);
#endif
		formatter = TESTformatter;
	} else if (strcmp(s, "trash") == 0) {
		formatter = TRASHformatter;
	} else if (strcmp(s, "sam") == 0) {
		formatter = SAMformatter;
	} else if (strcmp(s, "x") == 0 || strcmp(s, "expanded") == 0) {
		formatter = EXPANDEDformatter;
	} else {
		mnstr_printf(toConsole, "unsupported formatter\n");
	}
}

static void
setWidth(void)
{
	if (!pagewidthset) {
#ifdef TIOCGWINSZ
		struct winsize ws;

		if (ioctl(fileno(stdout), TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0)
			pagewidth = ws.ws_col;
		else
#endif
		{
#ifdef WIN32
			pagewidth = 79;	 /* 80 columns minus 1 for the edge */
#else
			pagewidth = -1;
#endif
		}
	}
}

#ifdef HAVE_POPEN
static void
start_pager(stream **saveFD)
{
	*saveFD = NULL;

	if (pager) {
		FILE *p;
		struct sigaction act;

		/* ignore SIGPIPE so that we get an error instead of signal */
		act.sa_handler = SIG_IGN;
		(void) sigemptyset(&act.sa_mask);
		act.sa_flags = 0;
		if(sigaction(SIGPIPE, &act, NULL) == -1) {
			fprintf(stderr, "Starting '%s' failed\n", pager);
		} else {
			p = popen(pager, "w");
			if (p == NULL)
				fprintf(stderr, "Starting '%s' failed\n", pager);
			else {
				*saveFD = toConsole;
				/* put | in name to indicate that file should be closed with pclose */
				if ((toConsole = file_wastream(p, "|pager")) == NULL) {
					toConsole = *saveFD;
					*saveFD = NULL;
					fprintf(stderr, "Starting '%s' failed\n", pager);
				}
#ifdef HAVE_ICONV
				if (encoding != NULL) {
					if ((toConsole = iconv_wstream(toConsole, encoding, "pager")) == NULL) {
						toConsole = *saveFD;
						*saveFD = NULL;
						fprintf(stderr, "Starting '%s' failed\n", pager);
					}
				}
#endif
			}
		}
	}
}

static void
end_pager(stream *saveFD)
{
	if (saveFD) {
		close_stream(toConsole);
		toConsole = saveFD;
	}
}
#endif

static int
format_result(Mapi mid, MapiHdl hdl, bool singleinstr)
{
	MapiMsg rc = MERROR;
	int64_t aff, lid;
	char *reply;
	int64_t sqloptimizer = 0;
	int64_t maloptimizer = 0;
	int64_t querytime = 0;
#ifdef HAVE_POPEN
	stream *saveFD;

	start_pager(&saveFD);
#endif

	setWidth();

	timerHumanCalled = false;

	do {
		/* handle errors first */
		if (mapi_result_error(hdl) != NULL) {
			mnstr_flush(toConsole);
			if (formatter == TABLEformatter) {
				mapi_noexplain(mid, "");
			} else {
				mapi_noexplain(mid, NULL);
			}
			mapi_explain_result(hdl, stderr);
			errseen = true;
			/* don't need to print something like '0
			 * tuples' if we got an error */
			continue;
		}

		// get the timings as reported by the backend
		sqloptimizer = 0;
		maloptimizer = mapi_get_maloptimizertime(hdl);
		querytime = mapi_get_querytime(hdl);
		timerHumanStop();
		switch (mapi_get_querytype(hdl)) {
		case Q_BLOCK:
		case Q_PARSE:
			/* should never see these */
			continue;
		case Q_UPDATE:
			SQLqueryEcho(hdl);
			if (formatter == RAWformatter ||
			    formatter == TESTformatter) {
				mnstr_printf(toConsole, "[ %" PRId64 "\t]\n", mapi_rows_affected(hdl));
			} else if (formatter != TRASHformatter) {
				aff = mapi_rows_affected(hdl);
				lid = mapi_get_last_id(hdl);
				mnstr_printf(toConsole,
					     "%" PRId64 " affected row%s",
					     aff,
					     aff != 1 ? "s" : "");
				if (lid != -1) {
					mnstr_printf(toConsole,
						     ", last generated key: "
						     "%" PRId64,
						     lid);
				}
				mnstr_printf(toConsole, "\n");
			}
			timerHuman(sqloptimizer, maloptimizer, querytime, singleinstr, false);
			continue;
		case Q_SCHEMA:
			SQLqueryEcho(hdl);
			if (formatter == TABLEformatter) {
				mnstr_printf(toConsole, "operation successful\n");
			}
			timerHuman(sqloptimizer, maloptimizer, querytime, singleinstr, false);
			continue;
		case Q_TRANS:
			SQLqueryEcho(hdl);
			if (formatter == TABLEformatter)
				mnstr_printf(toConsole,
					     "auto commit mode: %s\n",
					     mapi_get_autocommit(mid) ? "on" : "off");
			timerHuman(sqloptimizer, maloptimizer, querytime, singleinstr, false);
			continue;
		case Q_PREPARE:
			SQLqueryEcho(hdl);
			if (formatter == TABLEformatter)
				mnstr_printf(toConsole,
					     "execute prepared statement "
					     "using: EXEC %d(...)\n",
					     mapi_get_tableid(hdl));
			timerHuman(sqloptimizer, maloptimizer, querytime, singleinstr, false);
			break;
		case Q_TABLE:
			break;
		default:
			if (formatter == TABLEformatter && specials != DEBUGmodifier) {
				int i;
				mnstr_printf(stderr_stream,
					     "invalid/unknown response from server, "
					     "ignoring output\n");
				for (i = 0; i < 5 && (reply = fetch_line(hdl)) != 0; i++)
					mnstr_printf(stderr_stream, "? %s\n", reply);
				if (i == 5 && fetch_line(hdl) != 0) {
					mnstr_printf(stderr_stream,
						     "(remaining output omitted, "
						     "use \\fraw to examine in detail)\n");
					/* skip over the
					 * unknown/invalid stuff,
					 * otherwise mapi_next_result
					 * call will assert in
					 * close_result because the
					 * logic there doesn't expect
					 * random unread garbage
					 * somehow */
					while (fetch_line(hdl) != 0)
						;
				}
				continue;
			}
		}

		/* note: specials != NOmodifier implies mode == SQL */
		if (specials != NOmodifier && debugMode()) {
			SQLdebugRendering(hdl);
			continue;
		}
		if (debugMode())
			RAWrenderer(hdl);
		else {
			SQLqueryEcho(hdl);

			switch (formatter) {
			case TRASHformatter:
				break;
			case XMLformatter:
				XMLrenderer(hdl);
				break;
			case CSVformatter:
				CSVrenderer(hdl);
				break;
			case TESTformatter:
				TESTrenderer(hdl);
				break;
			case TABLEformatter:
				switch (specials) {
				case DEBUGmodifier:
					SQLdebugRendering(hdl);
					break;
				default:
					SQLrenderer(hdl);
					break;
				}
				break;
			case SAMformatter:
				SAMrenderer(hdl);
				break;
			case EXPANDEDformatter:
				EXPANDEDrenderer(hdl);
				break;
			default:
				RAWrenderer(hdl);
				break;
			}

			timerHuman(sqloptimizer, maloptimizer, querytime, singleinstr, false);
		}
	} while (!mnstr_errnr(toConsole) && (rc = mapi_next_result(hdl)) == 1);
	/*
	 * in case we called timerHuman() in the loop above with "total == false",
	 * call it again with "total == true" to get the total wall-clock time
	 * in case "singleinstr == false".
	 */
	if (timerHumanCalled)
		timerHuman(sqloptimizer, maloptimizer, querytime, singleinstr, true);
	if (mnstr_errnr(toConsole)) {
		mnstr_clearerr(toConsole);
		fprintf(stderr, "write error\n");
		errseen = true;
	}
#ifdef HAVE_POPEN
	end_pager(saveFD);
#endif

	return rc;
}

static int
doRequest(Mapi mid, const char *buf)
{
	MapiHdl hdl;

	if (mode == SQL)
		SQLsetSpecial(buf);

	hdl = mapi_query(mid, buf);
	if (hdl == NULL) {
		if (formatter == TABLEformatter) {
			mapi_noexplain(mid, "");
		} else {
			mapi_noexplain(mid, NULL);
		}
		mapi_explain(mid, stderr);
		errseen = true;
		return 1;
	}

	if (mapi_needmore(hdl) == MMORE)
		return 0;

	format_result(mid, hdl, false);

	if (mapi_get_active(mid) == NULL)
		mapi_close_handle(hdl);
	return 0;
}

#define CHECK_RESULT(mid, hdl, break_or_continue, buf, fp)	\
	switch (mapi_error(mid)) {				\
	case MOK:						\
		/* everything A OK */				\
		break;						\
	case MERROR:						\
		/* some error, but try to continue */		\
		if (formatter == TABLEformatter) {		\
			mapi_noexplain(mid, "");		\
		} else {					\
			mapi_noexplain(mid, NULL);		\
		}						\
		if (hdl) {					\
			mapi_explain_query(hdl, stderr);	\
			mapi_close_handle(hdl);			\
			hdl = NULL;				\
		} else						\
			mapi_explain(mid, stderr);		\
		errseen = true;					\
		break_or_continue;				\
	case MTIMEOUT:						\
		/* lost contact with the server */		\
		if (formatter == TABLEformatter) {		\
			mapi_noexplain(mid, "");		\
		} else {					\
			mapi_noexplain(mid, NULL);		\
		}						\
		if (hdl) {					\
			mapi_explain_query(hdl, stderr);	\
			mapi_close_handle(hdl);			\
			hdl = NULL;				\
		} else						\
			mapi_explain(mid, stderr);		\
		errseen = true;					\
		timerEnd();					\
		if (buf)					\
			free(buf);				\
		if (fp)						\
			close_stream(fp);			\
		return 1;					\
	}

static bool
doFileBulk(Mapi mid, stream *fp)
{
	char *buf = NULL;
	ssize_t length;
	MapiHdl hdl = mapi_get_active(mid);
	MapiMsg rc = MOK;
	size_t bufsize = 0;

	bufsize = 10240;
	buf = malloc(bufsize + 1);
	if (!buf) {
		fprintf(stderr, "cannot allocate memory for send buffer\n");
		if (fp)
			close_stream(fp);
		return true;
	}

	timerStart();
	do {
		timerPause();
		if (fp == NULL) {
			if (hdl == NULL)
				break;
			length = 0;
			buf[0] = 0;
		} else if ((length = mnstr_read(fp, buf, 1, bufsize)) <= 0) {
			/* end of file or error */
			if (hdl == NULL)
				break;	/* nothing more to do */
			buf[0] = 0;
			length = 0; /* handle error like EOF */
		} else {
			buf[length] = 0;
			if (strlen(buf) < (size_t) length) {
				fprintf(stderr, "NULL byte in input\n");
				errseen = true;
				break;
			}
		}
		timerResume();
		if (hdl == NULL) {
			hdl = mapi_query_prep(mid);
			CHECK_RESULT(mid, hdl, continue, buf, fp);
		}

		assert(hdl != NULL);
		mapi_query_part(hdl, buf, (size_t) length);
		CHECK_RESULT(mid, hdl, continue, buf, fp);

		/* if not at EOF, make sure there is a newline in the
		 * buffer */
		if (length > 0 && strchr(buf, '\n') == NULL)
			continue;

		assert(hdl != NULL);
		/* If the server wants more but we're at the end of
		 * file (length == 0), notify the server that we
		 * don't have anything more.  If the server still
		 * wants more (shouldn't happen according to the
		 * protocol) we break out of the loop (via the
		 * continue).  The assertion at the end will then go
		 * off. */
		if (mapi_query_done(hdl) == MMORE &&
		    (length > 0 || mapi_query_done(hdl) == MMORE))
			continue;	/* get more data */

		CHECK_RESULT(mid, hdl, continue, buf, fp);

		rc = format_result(mid, hdl, false);

		if (rc == MMORE && (length > 0 || mapi_query_done(hdl) != MOK))
			continue;	/* get more data */

		CHECK_RESULT(mid, hdl, continue, buf, fp);

		mapi_close_handle(hdl);
		hdl = NULL;

	} while (length > 0);
	/* reached on end of file */
	if (hdl)
		mapi_close_handle(hdl);
	timerEnd();

	free(buf);
	mnstr_flush(toConsole);
	if (fp)
		close_stream(fp);
	return errseen;
}

/* The options available for controlling input and rendering depends
 * on the language mode. */

static void
showCommands(void)
{
	/* shared control options */
	mnstr_printf(toConsole, "\\?       - show this message\n");
	if (mode == MAL)
		mnstr_printf(toConsole, "?pat  - MAL function help. pat=[modnme[.fcnnme][(][)]] wildcard *\n");
	mnstr_printf(toConsole, "\\<file   - read input from file\n");
	mnstr_printf(toConsole, "\\>file   - save response in file, or stdout if no file is given\n");
#ifdef HAVE_POPEN
	mnstr_printf(toConsole, "\\|cmd    - pipe result to process, or stop when no command is given\n");
#endif
#ifdef HAVE_LIBREADLINE
	mnstr_printf(toConsole, "\\history - show the readline history\n");
#endif
	mnstr_printf(toConsole, "\\help    - synopsis of the SQL syntax\n");
	if (mode == SQL) {
		mnstr_printf(toConsole, "\\D table- dumps the table, or the complete database if none given.\n");
		mnstr_printf(toConsole, "\\d[Stvsfn]+ [obj] - list database objects, or describe if obj given\n");
		mnstr_printf(toConsole, "\\A      - enable auto commit\n");
		mnstr_printf(toConsole, "\\a      - disable auto commit\n");
	}
	mnstr_printf(toConsole, "\\e      - echo the query in sql formatting mode\n");
	mnstr_printf(toConsole, "\\t      - set the timer {none,clock,performance} (none is default)\n");
	mnstr_printf(toConsole, "\\f      - format using a built-in renderer {csv,tab,raw,sql,xml,trash}\n");
	mnstr_printf(toConsole, "\\w#     - set maximal page width (-1=unlimited, 0=terminal width, >0=limit to num)\n");
	mnstr_printf(toConsole, "\\r#     - set maximum rows per page (-1=raw)\n");
	mnstr_printf(toConsole, "\\L file - save client/server interaction\n");
	mnstr_printf(toConsole, "\\X      - trace mclient code\n");
	mnstr_printf(toConsole, "\\q      - terminate session\n");
}

#define MD_TABLE    1
#define MD_VIEW     2
#define MD_SEQ      4
#define MD_FUNC     8
#define MD_SCHEMA  16

#define READBLOCK 8192

#ifdef HAVE_LIBREADLINE
struct myread_t {
	stream *s;
	const char *prompt;
	char *buf;
	size_t read;
	size_t len;
};

static ssize_t
myread(void *restrict private, void *restrict buf, size_t elmsize, size_t cnt)
{
	struct myread_t *p = private;
	size_t size = elmsize * cnt;
	size_t cpsize = size;

	assert(elmsize == 1);
	if (size == 0)
		return cnt;
	if (p->buf == NULL) {
		rl_completion_func_t *func = NULL;

		if (strcmp(p->prompt, "more>") == 0)
			func = suspend_completion();
		p->buf = readline(p->prompt);
		if (func)
			continue_completion(func);
		if (p->buf == NULL)
			return 0;
		p->len = strlen(p->buf);
		p->read = 0;
		if (p->len > 1)
			save_line(p->buf);
	}
	if (p->read < p->len) {
		if (p->len - p->read < size)
			cpsize = p->len - p->read;
		memcpy(buf, p->buf + p->read, cpsize);
		p->read += cpsize;
	} else {
		cpsize = 0;
	}
	if (p->read == p->len && cpsize < size) {
		((char *) buf)[cpsize++] = '\n';
		free(p->buf);
		p->buf = NULL;
	}
	return cpsize / elmsize;
}

static void
mydestroy(void *private)
{
	struct myread_t *p = private;

	if (p->buf)
		free(p->buf);
}
#endif

static bool
doFile(Mapi mid, stream *fp, bool useinserts, bool interactive, int save_history)
{
	char *line = NULL;
	char *buf = NULL;
	size_t length;
	size_t bufsiz = 0;
	MapiHdl hdl;
	MapiMsg rc = MOK;
	int lineno = 1;
	char *prompt = NULL;
	int prepno = 0;
#ifdef HAVE_LIBREADLINE
	struct myread_t rl;
#endif
	int fd;

	(void) save_history;	/* not used if no readline */
	if ((fd = getFileNo(fp)) >= 0 && isatty(fd)
#ifdef WIN32			/* isatty may not give expected result */
	    && formatter != TESTformatter
#endif
		) {
		interactive = true;
		setPrompt();
		prompt = promptbuf;
		fromConsole = fp;
#ifdef HAVE_LIBREADLINE
		init_readline(mid, language, save_history);
		rl.s = fp;
		rl.buf = NULL;
		if ((fp = callback_stream(&rl, myread, NULL, mydestroy, mnstr_name(fp))) == NULL) {
			fprintf(stderr,"Malloc for doFile failed");
			exit(2);
		}
#endif
	}
#ifdef HAVE_ICONV
	if (encoding) {
		if ((fp = iconv_rstream(fp, encoding, mnstr_name(fp))) == NULL) {
			fprintf(stderr,"Malloc failure");
			exit(2);
		}
	}
#endif

	if (!interactive && !echoquery)
		return doFileBulk(mid, fp);

	hdl = mapi_get_active(mid);

	bufsiz = READBLOCK;
	buf = malloc(bufsiz);

	if (buf == NULL) {
		fprintf(stderr,"Malloc for doFile failed");
		exit(2);
	}
	do {
		bool seen_null_byte = false;

		if (prompt) {
			char *p = hdl ? "more>" : prompt;
			/* clear errors when interactive */
			errseen = false;
#ifdef HAVE_LIBREADLINE
			rl.prompt = p;
#else
			mnstr_write(toConsole, p, 1, strlen(p));
#endif
		}
		mnstr_flush(toConsole);
		timerPause();
		/* read a line */
		length = 0;
		for (;;) {
			ssize_t l;
			char *newbuf;
			l = mnstr_readline(fp, buf + length, bufsiz - length);
			if (l <= 0)
				break;
			if (!seen_null_byte && strlen(buf + length) < (size_t) l) {
				fprintf(stderr, "NULL byte in input on line %d of input\n", lineno);
				seen_null_byte = true;
				errseen = true;
				if (hdl) {
					mapi_close_handle(hdl);
					hdl = NULL;
				}
			}
			length += l;
			if (buf[length - 1] == '\n')
				break;
			newbuf = realloc(buf, bufsiz += READBLOCK);
			if (newbuf) {
				buf = newbuf;
			} else {
				fprintf(stderr,"Malloc failure");
				length = 0;
				errseen = true;
				if (hdl) {
					mapi_close_handle(hdl);
					hdl = NULL;
				}
				break;
			}
		}
		line = buf;
		lineno++;
		if (seen_null_byte)
			continue;
		if (length == 0) {
			/* end of file */
			if (hdl == NULL) {
				/* nothing more to do */
				free(buf);
				goto bailout;
			}

			/* hdl != NULL, we should finish the current query */
		}
		if (hdl == NULL && length > 0 && interactive) {
			/* test for special commands */
			if (mode != MAL)
				while (length > 0 &&
				       (*line == '\f' ||
					*line == '\n' ||
					*line == ' ')) {
					line++;
					length--;
				}
			/* in the switch, use continue if the line was
			 * processed, use break to send to server */
			switch (*line) {
			case '\n':
			case '\0':
				break;
			case 'e':
				/* a bit of a hack for prepare/exec
				 * tests: replace "exec **" with the
				 * ID of the last prepared
				 * statement */
				if (mode == SQL &&
				    formatter == TESTformatter &&
				    strncmp(line, "exec **", 7) == 0) {
					line[5] = prepno < 10 ? ' ' : prepno / 10 + '0';
					line[6] = prepno % 10 + '0';
				}
				break;
			case '\\':
				switch (line[1]) {
				case 'q':
					free(buf);
					goto bailout;
				case 'X':
					/* toggle interaction trace */
					mapi_trace(mid, !mapi_get_trace(mid));
					continue;
				case 'A':
					if (mode != SQL)
						break;
					mapi_setAutocommit(mid, 1);
					continue;
				case 'a':
					if (mode != SQL)
						break;
					mapi_setAutocommit(mid, 0);
					continue;
				case 'w':
					pagewidth = atoi(line + 2);
					pagewidthset = pagewidth != 0;
					continue;
				case 'r':
					rowsperpage = atoi(line + 2);
					continue;
				case 'd': {
					bool hasWildcard = false;
					bool hasSchema = false;
					bool wantsSystem = false;
					unsigned int x = 0;
					char *p, *q;
					bool escaped = false;
					if (mode != SQL)
						break;
					while (my_isspace(line[length - 1]))
						line[--length] = 0;
					for (line += 2;
					     *line && !my_isspace(*line);
					     line++) {
						switch (*line) {
						case 't':
							x |= MD_TABLE;
							break;
						case 'v':
							x |= MD_VIEW;
							break;
						case 's':
							x |= MD_SEQ;
							break;
						case 'f':
							x |= MD_FUNC;
							break;
						case 'n':
							x |= MD_SCHEMA;
							break;
						case 'S':
							wantsSystem = true;
							break;
						default:
							fprintf(stderr, "unknown sub-command for \\d: %c\n", *line);
							length = 0;
							line[1] = '\0';
							break;
						}
					}
					if (length == 0)
						continue;
					if (x == 0) /* default to tables and views */
						x = MD_TABLE | MD_VIEW;
					for ( ; *line && my_isspace(*line); line++)
						;

					/* lowercase the object, except for quoted parts */
					q = line;
					for (p = line; *p != '\0'; p++) {
						if (*p == '"') {
							if (escaped) {
								if (*(p + 1) == '"') {
									/* SQL escape */
									*q++ = *p++;
								} else {
									escaped = false;
								}
							} else {
								escaped = true;
							}
						} else {
							if (!escaped) {
								*q++ = tolower((int) *p);
								if (*p == '*') {
									*p = '%';
									hasWildcard = true;
								} else if (*p == '?') {
									*p = '_';
									hasWildcard = true;
								} else if (*p == '.') {
									hasSchema = true;
								}
							} else {
								*q++ = *p;
							}
						}
					}
					*q = '\0';
					if (escaped) {
						fprintf(stderr, "unexpected end of string while "
							"looking for matching \"\n");
						continue;
					}

					if (*line && !hasWildcard) {
#ifdef HAVE_POPEN
						stream *saveFD;

						start_pager(&saveFD);
#endif
						if (x & MD_TABLE || x & MD_VIEW)
							describe_table(mid, NULL, line, toConsole, 1);
						if (x & MD_SEQ)
							describe_sequence(mid, NULL, line, toConsole);
						if (x & MD_FUNC)
							dump_functions(mid, toConsole, 0, NULL, line, NULL);
						if (x & MD_SCHEMA)
							describe_schema(mid, line, toConsole);
#ifdef HAVE_POPEN
						end_pager(saveFD);
#endif
					} else {
						/* get all object names in current schema */
						char *with_clause = 
							", describe_all_objects AS (\n"
							"  SELECT s.name AS sname,\n"
							"      t.name,\n"
							"      s.name || '.' || t.name AS fullname,\n"
							"      CAST(CASE t.type\n"
							"      WHEN 1 THEN 2\n" /* ntype for views */
							"      ELSE 1\n" /* ntype for tables */
							"      END AS SMALLINT) AS ntype,\n"
							"      (CASE WHEN t.system THEN 'SYSTEM ' ELSE '' END) || tt.table_type_name AS type,\n"
							"      t.system,\n"
							"      c.remark AS remark\n"
							"    FROM sys._tables t\n"
							"    LEFT OUTER JOIN comments c ON t.id = c.id\n"
							"    LEFT OUTER JOIN sys.schemas s ON t.schema_id = s.id\n"
							"    LEFT OUTER JOIN sys.table_types tt ON t.type = tt.table_type_id\n"
							"  UNION ALL\n"
							"  SELECT s.name AS sname,\n"
							"      sq.name,\n"
							"      s.name || '.' || sq.name AS fullname,\n"
							"      CAST(4 AS SMALLINT) AS ntype,\n"
							"      'SEQUENCE' AS type,\n"
							"      false AS system,\n"
							"      c.remark AS remark\n"
							"    FROM sys.sequences sq\n"
							"    LEFT OUTER JOIN comments c ON sq.id = c.id\n"
							"    LEFT OUTER JOIN sys.schemas s ON sq.schema_id = s.id\n"
							"  UNION ALL\n"
							"  SELECT DISTINCT s.name AS sname,\n" /* DISTINCT is needed to filter out duplicate overloaded function/procedure names */
							"      f.name,\n"
							"      s.name || '.' || f.name AS fullname,\n"
							"      CAST(8 AS SMALLINT) AS ntype,\n"
							"      (CASE WHEN sf.function_id IS NOT NULL THEN 'SYSTEM ' ELSE '' END) || function_type_keyword AS type,\n"
							"      sf.function_id IS NOT NULL AS system,\n"
							"      c.remark AS remark\n"
							"    FROM sys.functions f\n"
							"    LEFT OUTER JOIN comments c ON f.id = c.id\n"
							"    LEFT OUTER JOIN function_types ft ON f.type = ft.function_type_id\n"
							"    LEFT OUTER JOIN sys.schemas s ON f.schema_id = s.id\n"
							"    LEFT OUTER JOIN sys.systemfunctions sf ON f.id = sf.function_id\n"
							"  UNION ALL\n"
							"  SELECT NULL AS sname,\n"
							"      s.name,\n"
							"      s.name AS fullname,\n"
							"      CAST(16 AS SMALLINT) AS ntype,\n"
							"      (CASE WHEN s.system THEN 'SYSTEM SCHEMA' ELSE 'SCHEMA' END) AS type,\n"
							"      s.system,\n"
							"      c.remark AS remark\n"
							"    FROM sys.schemas s\n"
							"    LEFT OUTER JOIN comments c ON s.id = c.id\n"
							"  ORDER BY system, name, sname, ntype)\n"
							;
						size_t len = strlen(with_clause) + 1500 + strlen(line);
						char *query = malloc(len);
						char *q = query, *endq = query + len;
						char *name_column = hasSchema ? "fullname" : "name";

						if (!query)
							return true;

						/*
						 * | LINE            | SCHEMA FILTER | NAME FILTER                   |
						 * |-----------------+---------------+-------------------------------|
						 * | ""              | yes           | -                             |
						 * | "my_table"      | yes           | name LIKE 'my_table'          |
						 * | "my*"           | yes           | name LIKE 'my%'               |
						 * | "data.my_table" | no            | fullname LIKE 'data.my_table' |
						 * | "data.my*"      | no            | fullname LIKE 'data.my%'      |
						 * | "*a.my*"        | no            | fullname LIKE '%a.my%'        |
						 */
						q += snprintf(q, endq - q, "%s", get_with_comments_as_clause(mid));
						q += snprintf(q, endq - q, "%s", with_clause);
						q += snprintf(q, endq - q, "SELECT type, fullname, remark FROM describe_all_objects\n");
						q += snprintf(q, endq - q, "WHERE (ntype & %u) > 0\n", x);
						if (!wantsSystem) {
							q += snprintf(q, endq - q, "AND NOT system\n");
						}
						if (!hasSchema) {
							q += snprintf(q, endq - q, "AND (sname IS NULL OR sname = current_schema)\n");
						}
						if (*line) {
							q += snprintf(q, endq - q, "AND (%s LIKE '%s')\n", name_column, line);
						}
						q += snprintf(q, endq - q, "ORDER BY fullname, type, remark\n");
						q += snprintf(q, endq - q, ";\n");

						hdl = mapi_query(mid, query);
						free(query);
						CHECK_RESULT(mid, hdl, continue, buf, fp);
						while (fetch_row(hdl) == 3) {
							char *type = mapi_fetch_field(hdl, 0);
							char *name = mapi_fetch_field(hdl, 1);
							char *remark = mapi_fetch_field(hdl, 2);
							int type_width = mapi_get_len(hdl, 0);
							int name_width = mapi_get_len(hdl, 1);
							mnstr_printf(toConsole,
								     "%-*s  %-*s",
								     type_width, type,
								     name_width * (remark != NULL), name);
							if (remark) {
								char *c;
								mnstr_printf(toConsole, "  '");
								for (c = remark; *c; c++) {
									switch (*c) {
									case '\'':
										mnstr_printf(toConsole, "''");
										break;
									default:
										mnstr_writeChr(toConsole, *c);
									}
								}
								mnstr_printf(toConsole, "'");
							}
							mnstr_printf(toConsole, "\n");

						}
						mapi_close_handle(hdl);
						hdl = NULL;
					}
					continue;
				}
				case 'D':{
#ifdef HAVE_POPEN
					stream *saveFD;
#endif

					if (mode != SQL)
						break;
					while (my_isspace(line[length - 1]))
						line[--length] = 0;
					if (line[2] && !my_isspace(line[2])) {
						fprintf(stderr, "space required after \\D\n");
						continue;
					}
					for (line += 2; *line && my_isspace(*line); line++)
						;
#ifdef HAVE_POPEN
					start_pager(&saveFD);
#endif
					if (*line) {
						mnstr_printf(toConsole, "START TRANSACTION;\n");
						dump_table(mid, NULL, line, toConsole, 0, 1, useinserts);
						mnstr_printf(toConsole, "COMMIT;\n");
					} else
						dump_database(mid, toConsole, 0, useinserts);
#ifdef HAVE_POPEN
					end_pager(saveFD);
#endif
					continue;
				}
				case '<': {
					stream *s;
					/* read commands from file */
					while (my_isspace(line[length - 1]))
						line[--length] = 0;
					for (line += 2; *line && my_isspace(*line); line++)
						;
					/* use open_rastream to
					 * convert filename from UTF-8
					 * to locale */
					if ((s = open_rastream(line)) == NULL ||
					    mnstr_errnr(s)) {
						if (s)
							close_stream(s);
						fprintf(stderr, "%s: cannot open\n", line);
					} else
						doFile(mid, s, 0, 0, 0);
					continue;
				}
				case '>':
					/* redirect output to file */
					while (my_isspace(line[length - 1]))
						line[--length] = 0;
					for (line += 2; *line && my_isspace(*line); line++)
						;
					if (toConsole != stdout_stream &&
					    toConsole != stderr_stream) {
						close_stream(toConsole);
					}
					if (*line == 0 ||
					    strcmp(line, "stdout") == 0)
						toConsole = stdout_stream;
					else if (strcmp(line, "stderr") == 0)
						toConsole = stderr_stream;
					else if ((toConsole = open_wastream(line)) == NULL ||
						 mnstr_errnr(toConsole)) {
						if (toConsole != NULL) {
							close_stream(toConsole);
						}
						toConsole = stdout_stream;
						fprintf(stderr, "Cannot open %s\n", line);
					}
					continue;
				case 'L':
					free(logfile);
					logfile = NULL;
					while (my_isspace(line[length - 1]))
						line[--length] = 0;
					for (line += 2; *line && my_isspace(*line); line++)
						;
					if (*line == 0) {
						/* turn of logging */
						mapi_log(mid, NULL);
					} else {
						logfile = strdup(line);
						mapi_log(mid, logfile);
					}
					continue;
				case '?':
					showCommands();
					continue;
#ifdef HAVE_POPEN
				case '|':
					if (pager)
						free(pager);
					pager = NULL;
					setWidth();	/* reset to system default */

					while (my_isspace(line[length - 1]))
						line[--length] = 0;
					for (line += 2; *line && my_isspace(*line); line++)
						;
					if (*line == 0)
						continue;
					pager = strdup(line);
					continue;
#endif
				case 'h':
				{
#ifdef HAVE_LIBREADLINE
					int h;
					char *nl;

					if (strcmp(line,"\\history") == 0) {
						for (h = 0; h < history_length; h++) {
							nl = history_get(h) ? history_get(h)->line : 0;
							if (nl)
								mnstr_printf(toConsole, "%d %s\n", h, nl);
						}
					} else
#endif
					{
						setWidth();
						sql_help(line, toConsole, pagewidth <= 0 ? DEFWIDTH : pagewidth);
					}
					continue;
				}
#if 0 /* for later */
#ifdef HAVE_LIBREADLINE
				case '!':
				{
					char *nl;

					nl = strchr(line, '\n');
					if (nl)
						*nl = 0;
					if (history_expand(line + 2, &nl)) {
						mnstr_printf(toConsole, "%s\n", nl);
					}
					mnstr_printf(toConsole, "Expansion needs work\n");
					continue;
				}
#endif
#endif	/* 0 */
				case 'e':
					echoquery = true;
					continue;
				case 'f':
					while (my_isspace(line[length - 1]))
						line[--length] = 0;
					for (line += 2; *line && my_isspace(*line); line++)
						;
					if (*line == 0) {
						mnstr_printf(toConsole, "Current formatter: ");
						switch (formatter) {
						case RAWformatter:
							mnstr_printf(toConsole, "raw\n");
							break;
						case TABLEformatter:
							mnstr_printf(toConsole, "sql\n");
							break;
						case CSVformatter:
							mnstr_printf(toConsole, "%s\n", separator[0] == '\t' ? "tab" : "csv");
							break;
						case TRASHformatter:
							mnstr_printf(toConsole, "trash\n");
							break;
						case XMLformatter:
							mnstr_printf(toConsole, "xml\n");
							break;
						case EXPANDEDformatter:
							mnstr_printf(toConsole, "expanded\n");
							break;
						default:
							mnstr_printf(toConsole, "none\n");
							break;
						}
					} else
						setFormatter(line);
					continue;
				case 't':
					while (my_isspace(line[length - 1]))
						line[--length] = 0;
					for (line += 2; *line && my_isspace(*line); line++)
						;
					if (*line == 0) {
						mnstr_printf(toConsole, "Current time formatter: ");
						if (timermode == T_NONE)
							mnstr_printf(toConsole,"none\n");
						if (timermode == T_CLOCK)
							mnstr_printf(toConsole,"clock\n");
						if (timermode == T_PERF)
							mnstr_printf(toConsole,"performance\n");
					} else if (strcmp(line,"none") == 0) {
						timermode = T_NONE;
					} else if (strcmp(line,"clock") == 0) {
						timermode = T_CLOCK;
					} else if (strncmp(line,"perf",4) == 0 || strcmp(line,"performance") == 0) {
						timermode = T_PERF;
					} else if (*line != '\0') {
						fprintf(stderr, "warning: invalid argument to -t: %s\n",
							line);
					}
					continue;
				default:
					showCommands();
					continue;
				}
			}
		}

		if (hdl == NULL) {
			timerStart();
			hdl = mapi_query_prep(mid);
			CHECK_RESULT(mid, hdl, continue, buf, fp);
		} else
			timerResume();

		assert(hdl != NULL);

		if (length > 0) {
			SQLsetSpecial(line);
			mapi_query_part(hdl, line, length);
			CHECK_RESULT(mid, hdl, continue, buf, fp);
		}

		/* If the server wants more but we're at the
		 * end of file (line == NULL), notify the
		 * server that we don't have anything more.
		 * If the server still wants more (shouldn't
		 * happen according to the protocol) we break
		 * out of the loop (via the continue).  The
		 * assertion at the end will then go off. */
		if (mapi_query_done(hdl) == MMORE) {
			if (line != NULL) {
				continue;	/* get more data */
			} else if (mapi_query_done(hdl) == MMORE) {
				hdl = NULL;
				continue;	/* done */
			}
		}
		CHECK_RESULT(mid, hdl, continue, buf, fp);

		if (mapi_get_querytype(hdl) == Q_PREPARE) {
			prepno = mapi_get_tableid(hdl);
			assert(prepno < 100);
		}

		rc = format_result(mid, hdl, interactive || echoquery);

		if (rc == MMORE && (line != NULL || mapi_query_done(hdl) != MOK))
			continue;	/* get more data */

		CHECK_RESULT(mid, hdl, continue, buf, fp);

		timerEnd();
		mapi_close_handle(hdl);
		hdl = NULL;
	} while (line != NULL);
	/* reached on end of file */
	assert(hdl == NULL);
  bailout:
#ifdef HAVE_LIBREADLINE
	if (prompt)
		deinit_readline();
#endif
	close_stream(fp);
	return errseen;
}

static void
set_timezone(Mapi mid)
{
	char buf[128];
	int tzone;
	MapiHdl hdl;

	/* figure out our current timezone */
#if defined HAVE_GETDYNAMICTIMEZONEINFORMATION
	DYNAMIC_TIME_ZONE_INFORMATION tzinf;

	/* documentation says: UTC = localtime + Bias (in minutes),
	 * but experimentation during DST period says, UTC = localtime
	 * + Bias + DaylightBias, and presumably during non DST
	 * period, UTC = localtime + Bias */
	switch (GetDynamicTimeZoneInformation(&tzinf)) {
	case TIME_ZONE_ID_STANDARD:
	case TIME_ZONE_ID_UNKNOWN:
		tzone = (int) tzinf.Bias * 60;
		break;
	case TIME_ZONE_ID_DAYLIGHT:
		tzone = (int) (tzinf.Bias + tzinf.DaylightBias) * 60;
		break;
	default:
		/* call failed, we don't know the time zone */
		tzone = 0;
		break;
	}
#elif defined HAVE_STRUCT_TM_TM_ZONE
	time_t t;
	struct tm *tmp;

	t = time(NULL);
	tmp = localtime(&t);
	tzone = (int) -tmp->tm_gmtoff;
#else
	time_t t, lt, gt;
	struct tm *tmp;

	t = time(NULL);
	tmp = gmtime(&t);
	gt = mktime(tmp);
	tmp = localtime(&t);
	tmp->tm_isdst=0; /* We need the difference without dst */
	lt = mktime(tmp);
	assert((int64_t) gt - (int64_t) lt >= (int64_t) INT_MIN && (int64_t) gt - (int64_t) lt <= (int64_t) INT_MAX);
	tzone = (int) (gt - lt);
#endif
	if (tzone < 0)
		snprintf(buf, sizeof(buf),
			 "SET TIME ZONE INTERVAL '+%02d:%02d' HOUR TO MINUTE",
			 -tzone / 3600, (-tzone % 3600) / 60);
	else
		snprintf(buf, sizeof(buf),
			 "SET TIME ZONE INTERVAL '-%02d:%02d' HOUR TO MINUTE",
			 tzone / 3600, (tzone % 3600) / 60);
	if ((hdl = mapi_query(mid, buf)) == NULL) {
		if (formatter == TABLEformatter) {
			mapi_noexplain(mid, "");
		} else {
			mapi_noexplain(mid, NULL);
		}
		mapi_explain(mid, stderr);
		errseen = true;
		return;
	}
	mapi_close_handle(hdl);
}

__declspec(noreturn) static void usage(const char *prog, int xit)
	__attribute__((__noreturn__));

static void
usage(const char *prog, int xit)
{
	fprintf(stderr, "Usage: %s [ options ] [ file or database [ file ... ] ]\n", prog);
	fprintf(stderr, "\nOptions are:\n");
#ifdef HAVE_SYS_UN_H
	fprintf(stderr, " -h hostname | --host=hostname    host or UNIX domain socket to connect to\n");
#else
	fprintf(stderr, " -h hostname | --host=hostname    host to connect to\n");
#endif
	fprintf(stderr, " -p portnr   | --port=portnr      port to connect to\n");
	fprintf(stderr, " -u user     | --user=user        user id\n");
	fprintf(stderr, " -d database | --database=database  database to connect to (may be URI)\n");

	fprintf(stderr, " -e          | --echo             echo the query\n");
#ifdef HAVE_ICONV
	fprintf(stderr, " -E charset  | --encoding=charset specify encoding (character set) of the terminal\n");
#endif
	fprintf(stderr, " -f kind     | --format=kind      specify output format {csv,tab,raw,sql,xml,trash}\n");
	fprintf(stderr, " -H          | --history          load/save cmdline history (default off)\n");
	fprintf(stderr, " -i          | --interactive      interpret `\\' commands on stdin\n");
	fprintf(stderr, " -t          | --timer=format     use time formatting {none,clock,performance} (none is default)\n");
	fprintf(stderr, " -l language | --language=lang    {sql,mal}\n");
	fprintf(stderr, " -L logfile  | --log=logfile      save client/server interaction\n");
	fprintf(stderr, " -s stmt     | --statement=stmt   run single statement\n");
	fprintf(stderr, " -X          | --Xdebug           trace mapi network interaction\n");
	fprintf(stderr, " -z          | --timezone         do not tell server our timezone\n");
#ifdef HAVE_POPEN
	fprintf(stderr, " -| cmd      | --pager=cmd        for pagination\n");
#endif
	fprintf(stderr, " -v          | --version          show version information and exit\n");
	fprintf(stderr, " -?          | --help             show this usage message\n");

	fprintf(stderr, "\nSQL specific opions \n");
	fprintf(stderr, " -n nullstr  | --null=nullstr     change NULL representation for sql, csv and tab output modes\n");
	fprintf(stderr, " -a          | --autocommit       turn off autocommit mode\n");
	fprintf(stderr, " -r nr       | --rows=nr          for pagination\n");
	fprintf(stderr, " -w nr       | --width=nr         for pagination\n");
	fprintf(stderr, " -D          | --dump             create an SQL dump\n");
	fprintf(stderr, " -N          | --inserts          use INSERT INTO statements when dumping\n");
	fprintf(stderr, "The file argument can be - for stdin\n");
	exit(xit);
}

/* hardwired defaults, only used if monet environment cannot be found */
#define defaultPort 50000

int
main(int argc, char **argv)
{
	int port = 0;
	char *user = NULL;
	char *passwd = NULL;
	char *host = NULL;
	char *command = NULL;
	char *dbname = NULL;
	char *output = NULL;	/* output format as string */
	FILE *fp = NULL;
	int trace = 0;
	bool dump = false;
	bool useinserts = false;
	int c = 0;
	Mapi mid;
	int save_history = 0;
	bool interactive = false;
	bool has_fileargs = false;
	int option_index = 0;
	bool settz = true;
	bool autocommit = true;	/* autocommit mode default on */
	bool user_set_as_flag = false;
	bool passwd_set_as_flag = false;
	static struct option long_options[] = {
		{"autocommit", 0, 0, 'a'},
		{"database", 1, 0, 'd'},
		{"dump", 0, 0, 'D'},
		{"inserts", 0, 0, 'N'},
		{"echo", 0, 0, 'e'},
#ifdef HAVE_ICONV
		{"encoding", 1, 0, 'E'},
#endif
		{"format", 1, 0, 'f'},
		{"help", 0, 0, '?'},
		{"history", 0, 0, 'H'},
		{"host", 1, 0, 'h'},
		{"interactive", 0, 0, 'i'},
		{"timer", 1, 0, 't'},
		{"language", 1, 0, 'l'},
		{"log", 1, 0, 'L'},
		{"null", 1, 0, 'n'},
#ifdef HAVE_POPEN
		{"pager", 1, 0, '|'},
#endif
		{"port", 1, 0, 'p'},
		{"rows", 1, 0, 'r'},
		{"statement", 1, 0, 's'},
		{"user", 1, 0, 'u'},
		{"version", 0, 0, 'v'},
		{"width", 1, 0, 'w'},
		{"Xdebug", 0, 0, 'X'},
		{"timezone", 0, 0, 'z'},
		{0, 0, 0, 0}
	};

#ifndef WIN32
	/* don't set locale on Windows: setting the locale like this
	 * causes the output to be converted (we could set it to
	 * ".OCP" if we knew for sure that we were running in a cmd
	 * window) */
	if(setlocale(LC_CTYPE, "") == NULL) {
		fprintf(stderr, "error: could not set locale\n");
		exit(2);
	}
#endif
	toConsole = stdout_stream = file_wastream(stdout, "stdout");
	stderr_stream = file_wastream(stderr, "stderr");
	if(!stdout_stream || !stderr_stream) {
		if(stdout_stream)
			close_stream(stdout_stream);
		if(stderr_stream)
			close_stream(stderr_stream);
		fprintf(stderr, "error: could not open an output stream\n");
		exit(2);
	}

	/* parse config file first, command line options override */
	parse_dotmonetdb(&user, &passwd, &dbname, &language, &save_history, &output, &pagewidth);
	pagewidthset = pagewidth != 0;
	if (language) {
		if (strcmp(language, "sql") == 0) {
			mode = SQL;
		} else if (strcmp(language, "mal") == 0) {
			mode = MAL;
		}
	} else {
		language = strdup("sql");
		mode = SQL;
	}

	while ((c = getopt_long(argc, argv, "ad:De"
#ifdef HAVE_ICONV
				"E:"
#endif
				"f:h:Hil:L:n:Np:P:r:s:t:u:vw:Xz"
#ifdef HAVE_POPEN
				"|:"
#endif
				"?",
				long_options, &option_index)) != -1) {
		switch (c) {
		case 0:
#ifdef HAVE_POPEN
			if (strcmp(long_options[option_index].name, "pager") == 0) {
				pager = optarg;
				(void) pager;	/* will be further used later */
			}
#endif
			break;
		case 'a':
			autocommit = false;
			break;
		case 'd':
			assert(optarg);
			if (dbname)
				free(dbname);
			dbname = strdup(optarg);
			break;
		case 'D':
			dump = true;
			break;
		case 'e':
			echoquery = true;
			break;
#ifdef HAVE_ICONV
		case 'E':
			assert(optarg);
			encoding = optarg;
			break;
#endif
		case 'f':
			assert(optarg);
			if (output != NULL)
				free(output);
			output = strdup(optarg);	/* output format */
			break;
		case 'h':
			assert(optarg);
			host = optarg;
			break;
		case 'H':
			save_history = 1;
			break;
		case 'i':
			interactive = true;
			break;
		case 'l':
			assert(optarg);
			/* accept unambiguous prefix of language */
			if (strcmp(optarg, "sql") == 0 ||
			    strcmp(optarg, "sq") == 0 ||
			    strcmp(optarg, "s") == 0) {
				free(language);
				language = strdup(optarg);
				mode = SQL;
			} else if (strcmp(optarg, "mal") == 0 ||
				   strcmp(optarg, "ma") == 0) {
				free(language);
				language = strdup("mal");
				mode = MAL;
			} else if (strcmp(optarg, "msql") == 0) {
				free(language);
				language = strdup("msql");
				mode = MAL;
			} else {
				fprintf(stderr, "language option needs to be sql or mal\n");
				exit(-1);
			}
			break;
		case 'L':
			assert(optarg);
			logfile = strdup(optarg);
			break;
		case 'n':
			assert(optarg);
			nullstring = optarg;
			break;
		case 'N':
			useinserts = true;
			break;
		case 'p':
			assert(optarg);
			port = atoi(optarg);
			break;
		case 'P':
			assert(optarg);
			if (passwd)
				free(passwd);
			passwd = strdup(optarg);
			passwd_set_as_flag = true;
			break;
		case 'r':
			assert(optarg);
			rowsperpage = atoi(optarg);
			break;
		case 's':
			assert(optarg);
			command = optarg;
			break;
		case 't':
			if (optarg != NULL) {
				if (strcmp(optarg,"none") == 0) {
					timermode = T_NONE;
				} else if (strcmp(optarg,"clock") == 0) {
					timermode = T_CLOCK;
				} else if (strcmp(optarg, "perf") == 0 || strcmp(optarg, "performance") == 0) {
					timermode = T_PERF;
				} else if (*optarg != '\0') {
					fprintf(stderr, "warning: invalid argument to -t: %s\n",
						optarg);
				}
			}
			break;
		case 'u':
			assert(optarg);
			if (user)
				free(user);
			user = strdup(optarg);
			user_set_as_flag = true;
			break;
		case 'v':
			mnstr_printf(toConsole,
				     "mclient, the MonetDB interactive terminal (%s)\n",
				     MONETDB_RELEASE);
#ifdef HAVE_LIBREADLINE
			mnstr_printf(toConsole,
				     "support for command-line editing compiled-in\n");
#endif
#ifdef HAVE_ICONV
#ifdef HAVE_NL_LANGINFO
			if (encoding == NULL)
				encoding = nl_langinfo(CODESET);
#endif
			mnstr_printf(toConsole,
				     "character encoding: %s\n", encoding ? encoding : "utf-8 (default)");
#endif
			return 0;
		case 'w':
			assert(optarg);
			pagewidth = atoi(optarg);
			pagewidthset = pagewidth != 0;
			break;
		case 'X':
			trace = MAPI_TRACE;
			break;
		case 'z':
			settz = false;
			break;
#ifdef HAVE_POPEN
		case '|':
			assert(optarg);
			pager = optarg;
			break;
#endif
		case '?':
			/* a bit of a hack: look at the option that the
			 * current `c' is based on and see if we recognize
			 * it: if -? or --help, exit with 0, else with -1 */
			usage(argv[0], strcmp(argv[optind - 1], "-?") == 0 || strcmp(argv[optind - 1], "--help") == 0 ? 0 : -1);
			/* not reached */
		default:
			usage(argv[0], -1);
			/* not reached */
		}
	}
	if (passwd_set_as_flag &&
	    (output == NULL || strcmp(output, "test") != 0)) {
		usage(argv[0], -1);
		/* not reached */
	}

#ifdef HAVE_ICONV
#ifdef HAVE_NL_LANGINFO
	if (encoding == NULL)
		encoding = nl_langinfo(CODESET);
#endif
	if (encoding != NULL && strcasecmp(encoding, "utf-8") == 0)
		encoding = NULL;
	if (encoding != NULL) {
		stream *s = iconv_wstream(toConsole, encoding, "stdout");
		if (s == NULL || mnstr_errnr(s)) {
			fprintf(stderr, "warning: cannot convert local character set %s to UTF-8\n", encoding);
			close_stream(s);
		} else
			toConsole = s;
		stdout_stream = toConsole;
	}
#endif /* HAVE_ICONV */

	/* when config file would provide defaults */
	if (user_set_as_flag) {
		if (passwd && !passwd_set_as_flag) {
			free(passwd);
			passwd = NULL;
		}
	}

	if (user == NULL)
		user = simple_prompt("user", BUFSIZ, 1, prompt_getlogin());
	if (passwd == NULL)
		passwd = simple_prompt("password", BUFSIZ, 0, NULL);

	c = 0;
	has_fileargs = optind != argc;

	if (dbname == NULL && has_fileargs &&
	    (fp = fopen(argv[optind], "r")) == NULL) {
		dbname = strdup(argv[optind]);
		optind++;
		has_fileargs = optind != argc;
	}

	if (dbname != NULL && strncmp(dbname, "mapi:monetdb://", 15) == 0) {
		mid = mapi_mapiuri(dbname, user, passwd, language);
	} else {
		mid = mapi_mapi(host, port, user, passwd, language, dbname);
	}
	if (user)
		free(user);
	user = NULL;
	if (passwd)
		free(passwd);
	passwd = NULL;
	if (dbname)
		free(dbname);
	dbname = NULL;
	if (mid && mapi_error(mid) == MOK)
		mapi_reconnect(mid);	/* actually, initial connect */

	if (mid == NULL) {
		fprintf(stderr, "failed to allocate Mapi structure\n");
		exit(2);
	}

	if (mapi_error(mid)) {
		if (trace)
			mapi_explain(mid, stderr);
		else
			fprintf(stderr, "%s\n", mapi_error_str(mid));
		exit(2);
	}
	mapi_cache_limit(mid, -1);
	if (dump) {
		if (mode == SQL) {
			exit(dump_database(mid, toConsole, 0, useinserts));
		} else {
			fprintf(stderr, "Dump only supported for SQL\n");
			exit(1);
		}
	}

	if (!autocommit)
		mapi_setAutocommit(mid, autocommit);

	if (logfile)
		mapi_log(mid, logfile);

	mapi_trace(mid, trace);
	if (output) {
		setFormatter(output);
		free(output);
	} else {
		if (mode == SQL) {
			setFormatter("sql");
		} else {
			setFormatter("raw");
		}
	}
	/* give the user a welcome message with some general info */
	if (!has_fileargs && command == NULL && isatty(fileno(stdin))) {
		char *lang;

		if (mode == SQL) {
			lang = "/SQL";
		} else {
			lang = "";
		}

		mnstr_printf(toConsole,
			     "Welcome to mclient, the MonetDB%s "
			     "interactive terminal (%s)\n",
			     lang, MONETDB_RELEASE);

		if (mode == SQL)
			dump_version(mid, toConsole, "Database:");

		mnstr_printf(toConsole, "Type \\q to quit, \\? for a list of available commands\n");
		if (mode == SQL)
			mnstr_printf(toConsole, "auto commit mode: %s\n",
				     mapi_get_autocommit(mid) ? "on" : "off");
	}

	if (mode == SQL && settz)
		set_timezone(mid);

	if (command != NULL) {
#ifdef HAVE_ICONV
		iconv_t cd_in;
		int free_command = 0;

		if (encoding != NULL &&
		    (cd_in = iconv_open("utf-8", encoding)) != (iconv_t) -1) {
			char *savecommand = command;
			ICONV_CONST char *from = command;
			size_t fromlen = strlen(from);
			int factor = 4;
			size_t tolen = factor * fromlen + 1;
			char *to = malloc(tolen);

			if (to == NULL) {
				fprintf(stderr,"Malloc in main failed");
				exit(2);
			}
			free_command = 1;

		  try_again:
			command = to;
			if (iconv(cd_in, &from, &fromlen, &to, &tolen) == (size_t) -1) {
				switch (errno) {
				case EILSEQ:
					/* invalid multibyte sequence */
					fprintf(stderr, "Illegal input sequence in command line\n");
					exit(-1);
				case E2BIG:
					/* output buffer too small */
					from = savecommand;
					fromlen = strlen(from);
					factor *= 2;
					tolen = factor * fromlen + 1;
					free(command);
					to = malloc(tolen);
					if (to == NULL) {
						fprintf(stderr,"Malloc in main failed");
						exit(2);
					}
					goto try_again;
				case EINVAL:
					/* incomplete multibyte sequence */
					fprintf(stderr, "Incomplete input sequence on command line\n");
					exit(-1);
				default:
					break;
				}
			}
			*to = 0;
			iconv_close(cd_in);
		} else if (encoding)
			fprintf(stderr, "warning: cannot convert local character set %s to UTF-8\n", encoding);
#endif
		/* execute from command-line, need interactive to know whether
		 * to keep the mapi handle open */
		timerStart();
		c = doRequest(mid, command);
		timerEnd();
#ifdef HAVE_ICONV
		if (free_command)
			free(command);
#endif
	}

	if (optind < argc) {
		/* execute from file(s) */
		while (optind < argc) {
			stream *s;

			if (fp == NULL &&
			    (fp = (strcmp(argv[optind], "-") == 0 ?
				   stdin :
				   fopen(argv[optind], "r"))) == NULL) {
				fprintf(stderr, "%s: cannot open\n", argv[optind]);
				c |= 1;
			} else if ((s = file_rastream(fp, argv[optind])) == NULL) {
				fclose(fp);
				c |= 1;
			} else {
				c |= doFile(mid, s, useinserts, interactive, save_history);
			}
			fp = NULL;
			optind++;
		}
	} else if (command && mapi_get_active(mid))
		c = doFileBulk(mid, NULL);

	if (!has_fileargs && command == NULL) {
		stream *s = file_rastream(stdin, "<stdin>");
		if(!s) {
			mapi_destroy(mid);
			mnstr_destroy(stdout_stream);
			mnstr_destroy(stderr_stream);
			fprintf(stderr,"Failed to open stream for stdin\n");
			exit(2);
		}
		c = doFile(mid, s, useinserts, interactive, save_history);
	}

	mapi_destroy(mid);
	mnstr_destroy(stdout_stream);
	mnstr_destroy(stderr_stream);
	return c;
}
