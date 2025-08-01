/*
 * Copyright (c) 1989, 1993, 1994
 *	The Regents of the University of California.  All rights reserved.
 *
 * Copyright (C) 2017 Karel Zak <kzak@redhat.com>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. All advertising materials mentioning features or use of this software
 *    must display the following acknowledgement:
 *	This product includes software developed by the University of
 *	California, Berkeley and its contributors.
 * 4. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */
#include <sys/types.h>
#include <sys/ioctl.h>

#include <ctype.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <getopt.h>

#include "nls.h"
#include "c.h"
#include "widechar.h"
#include "xalloc.h"
#include "strutils.h"
#include "closestream.h"
#include "ttyutils.h"
#include "strv.h"
#include "optutils.h"
#include "mbsalign.h"
#include "colors.h"

#include "libsmartcols.h"

#define TABCHAR_CELLS         8

enum {
	COLUMN_MODE_FILLCOLS = 0,
	COLUMN_MODE_FILLROWS,
	COLUMN_MODE_TABLE,
	COLUMN_MODE_SIMPLE
};

struct column_control {
	int	mode;		/* COLUMN_MODE_* */
	size_t	termwidth;	/* -1 uninilialized, 0 unlimited, >0 width (default is 80) */

	struct libscols_table *tab;

	char **tab_colnames;	/* array with column names */
	const char *tab_name;	/* table name */
	const char *tab_order;	/* --table-order */

	char **tab_columns;	/* array from --table-column */

	const char *tab_colright;	/* --table-right */
	const char *tab_coltrunc;	/* --table-trunc */
	const char *tab_colnoextrem;	/* --table-noextreme */
	const char *tab_colwrap;	/* --table-wrap */
	const char *tab_colhide;	/* --table-hide */
	const char *tab_colorscheme;	/* --table-colorscheme */

	const char *tree;
	const char *tree_id;
	const char *tree_parent;

	wchar_t *input_separator;
	const char *output_separator;

	wchar_t	**ents;		/* input entries */
	size_t	nents;		/* number of entries */
	size_t	maxlength;	/* longest input record (line) */
	size_t  maxncols;	/* maximal number of input columns */
	size_t  mincolsep;	/* minimal spaces between columns */

	bool	greedy,
		json,
		header_repeat,
		hide_unnamed,
		maxout : 1,
		keep_empty_lines,	/* --keep-empty-lines */
		tab_noheadings,
		use_spaces;
};

typedef enum {
	ANSI_CHR = 'A',
	ANSI_ESC = 0x1b,
	ANSI_SGR = '[',
	ANSI_OSC = ']',
	ANSI_LNK = '8',
	ANSI_LBL = 0x7,
	ANSI_LSP = ';',
	ANSI_LSG = 'M',
	ANSI_END = '\\'
} ansi_esc_states;

/**
 * Count how many characters are non-printable due to ANSI X3.41 escape codes.
 *
 * It detects and count Fe Escape and OSC 8 links sequences. These sequences contains
 * characters that normally are printable, but due to being part of a escape sequence
 * are ignored when displayed in console terminals.
 */
static inline size_t ansi_esc_width(ansi_esc_states *state, size_t *found, const wchar_t *str, int chw)
{
	switch (*state) {
	case ANSI_CHR:
		// ANSI X3.41 escape sequences begin with ESC ( written as \x1b \033 or ^[ )
		if (*str == 0x1b)
			*state = ANSI_ESC;
		// Ignore 1 byte C1 control codes (0x80–0x9F) due to conflict with UTF-8 and CP-1252
		return 0;
	case ANSI_ESC:
		// Fe escape sequences allows the range 0x40 to 0x5f
		switch (*str) {
		case '[':  // CSI - Control Sequence Introducer
			*state = ANSI_SGR;
			break;
		case ']':  // OSC - Operating System Command
			*state = ANSI_OSC;
			break;
		case '_':  // APC - Application Program Command
		case 'P':  // DCS - Device Control String
		case '^':  // PM  - Privacy Message
			*state = ANSI_END;
			break;
		default:
			*state = ANSI_CHR;
			return 0;
		}
		*found = 1;
		return 0;
	case ANSI_SGR:
		*found += chw;
		// Fe escape sequences allows the range 0x30-0x3f
		// However SGR (Select Graphic Rendition) only uses: 0-9 ';' ':'
		if (*str >= '0' && *str <= '?')
			return 0;
		// Fe ends with the range 0x40-0x7e but SGR ends with 'm'
		if (*str <= '@' && *str >= '~')
			*found = 0;
		break;
	case ANSI_OSC:
		*found += chw;
		if (*str == ANSI_LNK) // OSC8-Link
			*state = ANSI_LNK; 
		else
			*state = ANSI_END; // other command sequences are ignored
		return 0;
	case ANSI_LNK: // OSC8 Terminal Hiperlink Sequence
		switch (*str) {
		case 0x7:  // Separated by BEL
			*state = ANSI_LBL; //#  \e]8;;LINK\aTEXT\e]8;;\a  #
			break;
		case 0x1b: // OSC8-Link separated by ESC-BACKSLASH
			*found += 2;
			*state = ANSI_LBL; //#  \e]8;;LINK\e\\TEXT\e]8;;\e\\  #
			break;
		default:
			*found += chw;
		}
		return 0; // ignore link width
	case ANSI_LBL:
		if (*str == 0x1b) { // Link label goes until ESC BACKSLASH
			*found += chw;
			*state = ANSI_LSP;
		}
		return 0;
	case ANSI_LSP:
		*found += chw;
		if (*str == '[') // SGR FG/BG colors nested inside OSC8-Link sequence
			*state = ANSI_LSG;
		else
			*state = ANSI_END; //# Link label ends with \e[8;;\e\\ #
		return 0;
	case ANSI_LSG: //#  \e]8;;LINK\e\\\e[1;34mTEXT\e[0m\e]8;;\e\\  #
		*found += chw;
		if (*str < '0' || *str > '?') //  SGR color sequence ends with 'm'
			*state = ANSI_LBL;
		return 0;
	case ANSI_END:
		switch (*str) {
		case 0x1b:  // APC/OSC8-Links ends with ESC-BACKSLASH
			*found += chw;
			break;
		case 0x7:  // APC/OSC/OSC8-Links ends with BEL
#ifdef HAVE_WIDECHAR
		case 0x9c:  // APC/DCS/DM ends with ST (String Terminator)
#endif
			break;
		default:
			*found += chw;
		return 0;
	}
	}
	size_t res = *found;
	*state = ANSI_CHR;
	*found = 0;
	return res;
}

static size_t width(const wchar_t *str)
{
	size_t count = 0;
	size_t found = 0;
	ansi_esc_states state = ANSI_CHR;

	for (; *str != '\0'; str++) {
#ifdef HAVE_WIDECHAR
		int x = wcwidth(*str);	/* don't use wcswidth(), need to ignore non-printable */
#else
		int x = isprint(*str) ? 1 : 0;
#endif
		int chw = x > 0 ? x : 0;
		size_t nonpr = ansi_esc_width(&state, &found, str, chw);
		count += chw - nonpr;
	}
	return count;
}

static wchar_t *mbs_to_wcs(const char *s)
{
#ifdef HAVE_WIDECHAR
	ssize_t n;
	wchar_t *wcs;

	n = mbstowcs((wchar_t *)0, s, 0);
	if (n < 0)
		return NULL;
	wcs = xcalloc((n + 1) * sizeof(wchar_t), 1);
	n = mbstowcs(wcs, s, n + 1);
	if (n < 0) {
		free(wcs);
		return NULL;
	}
	return wcs;
#else
	return xstrdup(s);
#endif
}

static char *wcs_to_mbs(const wchar_t *s)
{
#ifdef HAVE_WIDECHAR
	size_t n;
	char *str;

	n = wcstombs(NULL, s, 0);
	if (n == (size_t) -1)
		return NULL;

	str = xcalloc(n + 1, 1);
	if (wcstombs(str, s, n) == (size_t) -1) {
		free(str);
		return NULL;
	}
	return str;
#else
	return xstrdup(s);
#endif
}

static wchar_t *local_wcstok(struct column_control const *const ctl, wchar_t *p,
			     wchar_t **state)
{
	wchar_t *result = NULL;

	if (ctl->greedy)
#ifdef HAVE_WIDECHAR
		return wcstok(p, ctl->input_separator, state);
#else
		return strtok_r(p, ctl->input_separator, state);
#endif
	if (!p) {
		if (!*state)
			return NULL;
		p = *state;
	}
	result = p;
#ifdef HAVE_WIDECHAR
	p = wcspbrk(result, ctl->input_separator);
#else
	p = strpbrk(result, ctl->input_separator);
#endif
	if (!p)
		*state = NULL;
	else {
		*p = '\0';
		*state = p + 1;
	}
	return result;
}

static char **split_or_error(const char *str, const char *errmsg)
{
	char **res = ul_strv_split(str, ",");
	if (!res) {
		if (errno == ENOMEM)
			err_oom();
		if (errmsg)
			errx(EXIT_FAILURE, "%s: '%s'", _(errmsg), str);
		else
			return NULL;
	}
	return res;
}

/* @key= is expected in the options string */
static const char *colorseq_from_colorkey(char *opts, const char *key)
{
	char *cs = ul_optstr_get_value(opts, key);
	const char *seq = NULL;

	if (cs) {
		seq = color_scheme_get_sequence(cs, NULL);
		free(cs);
	}

	return seq;
}

static void init_table(struct column_control *ctl)
{
	scols_init_debug(0);

	ctl->tab = scols_new_table();
	if (!ctl->tab)
		err(EXIT_FAILURE, _("failed to allocate output table"));

	scols_table_set_column_separator(ctl->tab, ctl->output_separator);
	if (ctl->json) {
		scols_table_enable_json(ctl->tab, 1);
		scols_table_set_name(ctl->tab, ctl->tab_name ? : "table");
	} else
		scols_table_enable_noencoding(ctl->tab, 1);

	scols_table_enable_maxout(ctl->tab, ctl->maxout ? 1 : 0);
	scols_table_enable_colors(ctl->tab, colors_wanted() ? 1 : 0);

	if (ctl->tab_columns) {
		char **opts;

		UL_STRV_FOREACH(opts, ctl->tab_columns) {
			struct libscols_column *cl;

			cl = scols_table_new_column(ctl->tab, NULL, 0, 0);
			scols_column_set_properties(cl, *opts);

			if (ctl->tab_colorscheme) {
				const char *seq;

				seq = colorseq_from_colorkey(*opts, "colorkey");
				if (seq)
					scols_column_set_color(cl, seq);

				seq = colorseq_from_colorkey(*opts, "headercolorkey");
				if (seq)
					scols_column_set_headercolor(cl, seq);
			}
		}

	} else if (ctl->tab_colnames) {
		char **name;

		UL_STRV_FOREACH(name, ctl->tab_colnames)
			scols_table_new_column(ctl->tab, *name, 0, 0);
	} else
		scols_table_enable_noheadings(ctl->tab, 1);

	if (ctl->tab_colnames || ctl->tab_columns) {
		if (ctl->header_repeat)
			scols_table_enable_header_repeat(ctl->tab, 1);
		scols_table_enable_noheadings(ctl->tab, !!ctl->tab_noheadings);
	}

}

static struct libscols_column *get_last_visible_column(struct column_control *ctl, int n)
{
	struct libscols_iter *itr;
	struct libscols_column *cl, *res = NULL;

	itr = scols_new_iter(SCOLS_ITER_BACKWARD);
	if (!itr)
		err_oom();

	while (scols_table_next_column(ctl->tab, itr, &cl) == 0) {
		if (scols_column_get_flags(cl) & SCOLS_FL_HIDDEN)
			continue;
		if (n == 0) {
			res = cl;
			break;
		}
		n--;
	}

	scols_free_iter(itr);
	return res;
}

static struct libscols_column *string_to_column(struct column_control *ctl, const char *str)
{
	struct libscols_column *cl;

	if (isdigit_string(str)) {
		uint32_t n = strtou32_or_err(str, _("failed to parse column")) - 1;

		cl = scols_table_get_column(ctl->tab, n);
	} else if (strcmp(str, "-1") == 0)
		cl = get_last_visible_column(ctl, 0);
	else
		cl = scols_table_get_column_by_name(ctl->tab, str);

	if (!cl)
		errx(EXIT_FAILURE, _("undefined column name '%s'"), str);

	return cl;
}

static int column_set_flag(struct libscols_column *cl, int fl)
{
	int cur = scols_column_get_flags(cl);

	return scols_column_set_flags(cl, cur | fl);
}

static int has_unnamed(const char *list)
{
	char **all, **one;
	int rc = 0;

	if (!list)
		return 0;
	if (strcmp(list, "-") == 0)
		return 1;
	if (!strchr(list, ','))
		return 0;

	all = split_or_error(list, NULL);
	if (all) {
		UL_STRV_FOREACH(one, all) {
			if (strcmp(*one, "-") == 0) {
				rc = 1;
				break;
			}
		}
		ul_strv_free(all);
	}

	return rc;
}

static void apply_columnflag_from_list(struct column_control *ctl, const char *list,
					 int flag, const char *errmsg)
{
	char **all;
	char **one;
	int unnamed = 0;
	struct libscols_column *cl;

	/* apply to all */
	if (list && strcmp(list, "0") == 0) {
		struct libscols_iter *itr;

		itr = scols_new_iter(SCOLS_ITER_FORWARD);
		if (!itr)
			err_oom();

		while (scols_table_next_column(ctl->tab, itr, &cl) == 0)
			column_set_flag(cl, flag);
		scols_free_iter(itr);
		return;
	}

	all = split_or_error(list, errmsg);

	/* apply to columns specified by name */
	UL_STRV_FOREACH(one, all) {
		int low = 0, up = 0;

		if (strcmp(*one, "-") == 0) {
			unnamed = 1;
			continue;
		}

		/* parse range (N-M) */
		if (strchr(*one, '-') && ul_parse_range(*one, &low, &up, 0) == 0) {
			for (; low <= up; low++) {
				if (low < 0)
					cl = get_last_visible_column(ctl, (low * -1) -1);
				else
					cl = scols_table_get_column(ctl->tab, low-1);
				if (cl)
					column_set_flag(cl, flag);
			}
			continue;
		}

		/* one item in the list */
		cl = string_to_column(ctl, *one);
		if (cl)
			column_set_flag(cl, flag);
	}
	ul_strv_free(all);

	/* apply flag to all columns without name */
	if (unnamed) {
		struct libscols_iter *itr;

		itr = scols_new_iter(SCOLS_ITER_FORWARD);
		if (!itr)
			err_oom();

		while (scols_table_next_column(ctl->tab, itr, &cl) == 0) {
			if (!scols_column_get_name(cl))
				column_set_flag(cl, flag);
		}
		scols_free_iter(itr);
	}
}

static void reorder_table(struct column_control *ctl)
{
	struct libscols_column **wanted, *last = NULL;
	size_t i, count = 0;
	size_t ncols = scols_table_get_ncols(ctl->tab);
	char **order = split_or_error(ctl->tab_order, N_("failed to parse --table-order list"));
	char **one;

	wanted = xcalloc(ncols, sizeof(struct libscols_column *));

	UL_STRV_FOREACH(one, order) {
		struct libscols_column *cl = string_to_column(ctl, *one);
		if (cl)
			wanted[count++] = cl;
	}

	for (i = 0; i < count; i++) {
		scols_table_move_column(ctl->tab, last, wanted[i]);
		last = wanted[i];
	}

	free(wanted);
	ul_strv_free(order);
}

static void create_tree(struct column_control *ctl)
{
	struct libscols_column *cl_tree = string_to_column(ctl, ctl->tree);
	struct libscols_column *cl_p = string_to_column(ctl, ctl->tree_parent);
	struct libscols_column *cl_i = string_to_column(ctl, ctl->tree_id);
	struct libscols_iter *itr_p, *itr_i;
	struct libscols_line *ln_i;

	if (!cl_p || !cl_i || !cl_tree)
		return;			/* silently ignore the tree request */

	column_set_flag(cl_tree, SCOLS_FL_TREE);

	itr_p = scols_new_iter(SCOLS_ITER_FORWARD);
	itr_i = scols_new_iter(SCOLS_ITER_FORWARD);
	if (!itr_p || !itr_i)
		err_oom();

	/* scan all lines for ID */
	while (scols_table_next_line(ctl->tab, itr_i, &ln_i) == 0) {
		struct libscols_line *ln;
		struct libscols_cell *ce = scols_line_get_column_cell(ln_i, cl_i);
		const char *id = ce ? scols_cell_get_data(ce) : NULL;

		if (!id)
			continue;

		/* see if the ID is somewhere used in parent column */
		scols_reset_iter(itr_p, SCOLS_ITER_FORWARD);
		while (scols_table_next_line(ctl->tab, itr_p, &ln) == 0) {
			const char *parent;

			ce = scols_line_get_column_cell(ln, cl_p);
			parent = ce ? scols_cell_get_data(ce) : NULL;

			if (!parent)
				continue;
			if (strcmp(id, parent) != 0)
				continue;
			if (scols_line_is_ancestor(ln, ln_i))
				continue;
			scols_line_add_child(ln_i, ln);
		}
	}

	scols_free_iter(itr_p);
	scols_free_iter(itr_i);
}

static void modify_table(struct column_control *ctl)
{
	if (ctl->termwidth > 0) {
		scols_table_set_termwidth(ctl->tab, ctl->termwidth);
		scols_table_set_termforce(ctl->tab, SCOLS_TERMFORCE_ALWAYS);
	}

	if (ctl->tab_colhide)
		apply_columnflag_from_list(ctl, ctl->tab_colhide,
				SCOLS_FL_HIDDEN , N_("failed to parse --table-hide list"));

	if (ctl->tab_colright)
		apply_columnflag_from_list(ctl, ctl->tab_colright,
				SCOLS_FL_RIGHT, N_("failed to parse --table-right list"));

	if (ctl->tab_coltrunc)
		apply_columnflag_from_list(ctl, ctl->tab_coltrunc,
				SCOLS_FL_TRUNC , N_("failed to parse --table-trunc list"));

	if (ctl->tab_colnoextrem)
		apply_columnflag_from_list(ctl, ctl->tab_colnoextrem,
				SCOLS_FL_NOEXTREMES , N_("failed to parse --table-noextreme list"));

	if (ctl->tab_colwrap)
		apply_columnflag_from_list(ctl, ctl->tab_colwrap,
				SCOLS_FL_WRAP , N_("failed to parse --table-wrap list"));

	if (!ctl->tab_colnoextrem) {
		struct libscols_column *cl = get_last_visible_column(ctl, 0);
		if (cl)
			column_set_flag(cl, SCOLS_FL_NOEXTREMES);
	}

	if (ctl->tree)
		create_tree(ctl);

	/* This must be the last step! */
	if (ctl->tab_order)
		reorder_table(ctl);
}


static int add_line_to_table(struct column_control *ctl, wchar_t *wcs0)
{
	wchar_t *sv = NULL, *wcs = wcs0, *all = NULL;
	size_t n = 0;
	struct libscols_line *ln = NULL;


	if (!ctl->tab)
		init_table(ctl);

	if (ctl->maxncols) {
		all = wcsdup(wcs0);
		if (!all)
			err(EXIT_FAILURE, _("failed to allocate input line"));
	}

	do {
		char *data;
		wchar_t *wcdata = local_wcstok(ctl, wcs, &sv);

		if (!wcdata)
			break;

		if (ctl->maxncols && n + 1 == ctl->maxncols) {
			/* Use rest of the string as column data */
			size_t skip = wcdata - wcs0;
			wcdata = all + skip;
		}

		if (scols_table_get_ncols(ctl->tab) < n + 1) {
			if (scols_table_is_json(ctl->tab) && !ctl->hide_unnamed)
				errx(EXIT_FAILURE, _("line %zu: for JSON the name of the "
					"column %zu is required"),
					scols_table_get_nlines(ctl->tab) + 1,
					n + 1);
			scols_table_new_column(ctl->tab, NULL, 0,
					ctl->hide_unnamed ? SCOLS_FL_HIDDEN : 0);
		}
		if (!ln) {
			ln = scols_table_new_line(ctl->tab, NULL);
			if (!ln)
				err(EXIT_FAILURE, _("failed to allocate output line"));
		}

		data = wcs_to_mbs(wcdata);
		if (!data)
			err(EXIT_FAILURE, _("failed to allocate output data"));
		if (scols_line_refer_data(ln, n, data))
			err(EXIT_FAILURE, _("failed to add output data"));
		n++;
		wcs = NULL;
		if (ctl->maxncols && n == ctl->maxncols)
			break;
	} while (1);

	free(all);
	return 0;
}

static int add_emptyline_to_table(struct column_control *ctl)
{
	if (!ctl->tab)
		init_table(ctl);

	if (!scols_table_new_line(ctl->tab, NULL))
		err(EXIT_FAILURE, _("failed to allocate output line"));

	return 0;
}

static void add_entry(struct column_control *ctl, size_t *maxents, wchar_t *wcs)
{
	if (ctl->nents <= *maxents) {
		*maxents += 1000;
		ctl->ents = xreallocarray(ctl->ents, *maxents, sizeof(wchar_t *));
	}
	ctl->ents[ctl->nents] = wcs;
	ctl->nents++;
}

static int read_input(struct column_control *ctl, FILE *fp)
{
	wchar_t *empty = NULL;
	char *buf = NULL;
	size_t bufsz = 0;
	size_t maxents = 0;
	int rc = 0;

	/* Read input */
	do {
		char *str, *p;
		wchar_t *wcs = NULL;
		size_t len;

		if (getline(&buf, &bufsz, fp) < 0) {
			if (feof(fp))
				break;
			err(EXIT_FAILURE, _("read failed"));
		}
		str = (char *) skip_space(buf);
		if (str) {
			p = strchr(str, '\n');
			if (p)
				*p = '\0';
		}
		if (!str || !*str) {
			if (ctl->keep_empty_lines) {
				if (ctl->mode == COLUMN_MODE_TABLE) {
					add_emptyline_to_table(ctl);
				} else {
					if (!empty)
						empty = mbs_to_wcs("");
					add_entry(ctl, &maxents, empty);
				}
			}
			continue;
		}

		wcs = mbs_to_wcs(buf);
		if (!wcs) {
			/*
			 * Convert broken sequences to \x<hex> and continue.
			 */
			size_t tmpsz = 0;
			char *tmp = mbs_invalid_encode(buf, &tmpsz);

			if (!tmp)
				err(EXIT_FAILURE, _("read failed"));
			wcs = mbs_to_wcs(tmp);
			free(tmp);
		}

		switch (ctl->mode) {
		case COLUMN_MODE_TABLE:
			rc = add_line_to_table(ctl, wcs);
			free(wcs);
			break;

		case COLUMN_MODE_FILLCOLS:
		case COLUMN_MODE_FILLROWS:
			add_entry(ctl, &maxents, wcs);
			len = width(wcs);
			if (ctl->maxlength < len)
				ctl->maxlength = len;
			break;
		default:
			free(wcs);
			break;
		}
	} while (rc == 0);

	free(buf);

	return rc;
}


static void columnate_fillrows(struct column_control *ctl)
{
	size_t chcnt, col, cnt, endcol, numcols, remains;
	wchar_t **lp;

	if (ctl->use_spaces)
		ctl->maxlength += ctl->mincolsep;
	else
		ctl->maxlength = (ctl->maxlength + TABCHAR_CELLS) & ~(TABCHAR_CELLS - 1);
	numcols = ctl->termwidth / ctl->maxlength;
	remains = ctl->termwidth % ctl->maxlength;
	if (ctl->use_spaces && remains + ctl->mincolsep >= ctl->maxlength)
		numcols++;
	endcol = ctl->maxlength;
	for (chcnt = col = 0, lp = ctl->ents; /* nothing */; ++lp) {
		fputws(*lp, stdout);
		chcnt += width(*lp);
		if (!--ctl->nents)
			break;
		if (++col == numcols) {
			chcnt = col = 0;
			endcol = ctl->maxlength;
			putwchar('\n');
		} else {
			if (ctl->use_spaces) {
				while (chcnt < endcol) {
					putwchar(' ');
					chcnt++;
				}
			} else {
				while ((cnt = ((chcnt + TABCHAR_CELLS) & ~(TABCHAR_CELLS - 1))) <= endcol) {
					putwchar('\t');
					chcnt = cnt;
				}
			}
			endcol += ctl->maxlength;
		}
	}
	if (chcnt)
		putwchar('\n');
}

static void columnate_fillcols(struct column_control *ctl)
{
	size_t base, chcnt, cnt, col, endcol, numcols, numrows, row, remains;

	if (ctl->use_spaces)
		ctl->maxlength += ctl->mincolsep;
	else
		ctl->maxlength = (ctl->maxlength + TABCHAR_CELLS) & ~(TABCHAR_CELLS - 1);
	numcols = ctl->termwidth / ctl->maxlength;
	remains = ctl->termwidth % ctl->maxlength;
	if (!numcols)
		numcols = 1;
	if (ctl->use_spaces && remains + ctl->mincolsep >= ctl->maxlength)
		numcols++;
	numrows = ctl->nents / numcols;
	if (ctl->nents % numcols)
		++numrows;

	for (row = 0; row < numrows; ++row) {
		endcol = ctl->maxlength;
		for (base = row, chcnt = col = 0; col < numcols; ++col) {
			fputws(ctl->ents[base], stdout);
			chcnt += width(ctl->ents[base]);
			if ((base += numrows) >= ctl->nents)
				break;
			if (ctl->use_spaces) {
				while (chcnt < endcol) {
					putwchar(' ');
					chcnt++;
				}
			} else {
				while ((cnt = ((chcnt + TABCHAR_CELLS) & ~(TABCHAR_CELLS - 1))) <= endcol) {
					putwchar('\t');
					chcnt = cnt;
				}
			}
			endcol += ctl->maxlength;
		}
		putwchar('\n');
	}
}

static void simple_print(struct column_control *ctl)
{
	int cnt;
	wchar_t **lp;

	for (cnt = ctl->nents, lp = ctl->ents; cnt--; ++lp) {
		fputws(*lp, stdout);
		putwchar('\n');
	}
}

static void __attribute__((__noreturn__)) usage(void)
{
	FILE *out = stdout;

	fputs(USAGE_HEADER, out);
	fprintf(out, _(" %s [options] [<file>...]\n"), program_invocation_short_name);

	fputs(USAGE_SEPARATOR, out);
	fputs(_("Columnate lists.\n"), out);

	fputs(USAGE_OPTIONS, out);
	fputs(_(" -t, --table                      create a table\n"), out);
	fputs(_(" -n, --table-name <name>          table name for JSON output\n"), out);
	fputs(_(" -O, --table-order <columns>      specify order of output columns\n"), out);
	fputs(_("     --table-colorscheme <name>   specify color scheme name\n"), out);
	fputs(_(" -C, --table-column <properties>  define column\n"), out);
	fputs(_(" -N, --table-columns <names>      comma separated columns names\n"), out);
	fputs(_(" -l, --table-columns-limit <num>  maximal number of input columns\n"), out);
	fputs(_(" -E, --table-noextreme <columns>  don't count long text from the columns to column width\n"), out);
	fputs(_(" -d, --table-noheadings           don't print header\n"), out);
	fputs(_(" -m, --table-maxout               fill all available space\n"), out);
	fputs(_(" -e, --table-header-repeat        repeat header for each page\n"), out);
	fputs(_(" -H, --table-hide <columns>       don't print the columns\n"), out);
	fputs(_(" -R, --table-right <columns>      right align text in these columns\n"), out);
	fputs(_(" -T, --table-truncate <columns>   truncate text in the columns when necessary\n"), out);
	fputs(_(" -W, --table-wrap <columns>       wrap text in the columns when necessary\n"), out);
	fputs(_(" -L, --keep-empty-lines           don't ignore empty lines\n"), out);
	fputs(_(" -J, --json                       use JSON output format for table\n"), out);

	fputs(USAGE_SEPARATOR, out);
	fputs(_(" -r, --tree <column>              column to use tree-like output for the table\n"), out);
	fputs(_(" -i, --tree-id <column>           line ID to specify child-parent relation\n"), out);
	fputs(_(" -p, --tree-parent <column>       parent to specify child-parent relation\n"), out);

	fputs(USAGE_SEPARATOR, out);
	fputs(_(" -c, --output-width <width>       width of output in number of characters\n"), out);
	fputs(_(" -o, --output-separator <string>  columns separator for table output (default is two spaces)\n"), out);
	fputs(_(" -s, --separator <string>         possible table delimiters\n"), out);
	fputs(_(" -x, --fillrows                   fill rows before columns\n"), out);
	fputs(_(" -S, --use-spaces <number>        minimal whitespaces between columns (no tabs)\n"), out);

	fputs(USAGE_SEPARATOR, out);
	fprintf(out, _("     --color[=<when>]             colorize output (%s, %s or %s)\n"), "auto", "always", "never");
	fprintf(out,
	        "                                    %s\n", USAGE_COLORS_DEFAULT);
	fputs(USAGE_SEPARATOR, out);
	fprintf(out, USAGE_HELP_OPTIONS(34));
	fprintf(out, USAGE_MAN_TAIL("column(1)"));

	exit(EXIT_SUCCESS);
}

int main(int argc, char **argv)
{
	struct column_control ctl = {
		.mode = COLUMN_MODE_FILLCOLS,
		.greedy = 1,
		.termwidth = (size_t) -1
	};

	int c;
	unsigned int eval = 0;		/* exit value */
	int colormode = UL_COLORMODE_UNDEF;
	enum {
		OPT_COLOR	= CHAR_MAX + 1,
		OPT_COLORSCHEME,
	};

	static const struct option longopts[] =
	{
		{ "columns",             required_argument, NULL, 'c' }, /* deprecated */
		{ "color",               optional_argument, NULL, OPT_COLOR },
		{ "fillrows",            no_argument,       NULL, 'x' },
		{ "help",                no_argument,       NULL, 'h' },
		{ "json",                no_argument,       NULL, 'J' },
		{ "keep-empty-lines",    no_argument,       NULL, 'L' },
		{ "output-separator",    required_argument, NULL, 'o' },
		{ "output-width",        required_argument, NULL, 'c' },
		{ "separator",           required_argument, NULL, 's' },
		{ "table",               no_argument,       NULL, 't' },
		{ "table-colorscheme",   required_argument, NULL,  OPT_COLORSCHEME },
		{ "table-columns",       required_argument, NULL, 'N' },
		{ "table-column",        required_argument, NULL, 'C' },
		{ "table-columns-limit", required_argument, NULL, 'l' },
		{ "table-hide",          required_argument, NULL, 'H' },
		{ "table-name",          required_argument, NULL, 'n' },
		{ "table-maxout",        no_argument,       NULL, 'm' },
		{ "table-noextreme",     required_argument, NULL, 'E' },
		{ "table-noheadings",    no_argument,       NULL, 'd' },
		{ "table-order",         required_argument, NULL, 'O' },
		{ "table-right",         required_argument, NULL, 'R' },
		{ "table-truncate",      required_argument, NULL, 'T' },
		{ "table-wrap",          required_argument, NULL, 'W' },
		{ "table-empty-lines",   no_argument,       NULL, 'L' }, /* deprecated */
		{ "table-header-repeat", no_argument,       NULL, 'e' },
		{ "tree",                required_argument, NULL, 'r' },
		{ "tree-id",             required_argument, NULL, 'i' },
		{ "tree-parent",         required_argument, NULL, 'p' },
		{ "use-spaces",          required_argument, NULL, 'S' },
		{ "version",             no_argument,       NULL, 'V' },
		{ NULL,	0, NULL, 0 },
	};
	static const ul_excl_t excl[] = {       /* rows and cols in ASCII order */
		{ 'C','N' },
		{ 'J','x' },
		{ 't','x' },
		{ 0 }
	};
	int excl_st[ARRAY_SIZE(excl)] = UL_EXCL_STATUS_INIT;

	setlocale(LC_ALL, "");
	bindtextdomain(PACKAGE, LOCALEDIR);
	textdomain(PACKAGE);
	close_stdout_atexit();

	ctl.output_separator = "  ";
	ctl.input_separator = mbs_to_wcs("\t ");

	while ((c = getopt_long(argc, argv, "C:c:dE:eH:hi:Jl:LN:n:mO:o:p:R:r:S:s:T:tVW:x", longopts, NULL)) != -1) {

		err_exclusive_options(c, longopts, excl, excl_st);

		switch(c) {
		case 'C':
			if (ul_strv_extend(&ctl.tab_columns, optarg))
				err_oom();
			break;
		case 'c':
			if (strcmp(optarg, "unlimited") == 0)
				ctl.termwidth = 0;
			else
				ctl.termwidth = strtou32_or_err(optarg, _("invalid columns argument"));
			break;
		case 'd':
			ctl.tab_noheadings = 1;
			break;
		case 'E':
			ctl.tab_colnoextrem = optarg;
			break;
		case 'e':
			ctl.header_repeat = 1;
			break;
		case 'H':
			ctl.tab_colhide = optarg;
			ctl.hide_unnamed = has_unnamed(ctl.tab_colhide);
			break;
		case 'i':
			ctl.tree_id = optarg;
			break;
		case 'J':
			ctl.json = 1;
			ctl.mode = COLUMN_MODE_TABLE;
			break;
		case 'L':
			ctl.keep_empty_lines = 1;
			break;
		case 'l':
			ctl.maxncols = strtou32_or_err(optarg, _("invalid columns limit argument"));
			if (ctl.maxncols == 0)
				errx(EXIT_FAILURE, _("columns limit must be greater than zero"));
			break;
		case 'N':
			ctl.tab_colnames = split_or_error(optarg, N_("failed to parse column names"));
			break;
		case 'n':
			ctl.tab_name = optarg;
			break;
		case 'm':
			ctl.maxout = 1;
			break;
		case 'O':
			ctl.tab_order = optarg;
			break;
		case 'o':
			ctl.output_separator = optarg;
			break;
		case 'p':
			ctl.tree_parent = optarg;
			break;
		case 'R':
			ctl.tab_colright = optarg;
			break;
		case 'r':
			ctl.tree = optarg;
			break;
		case 'S':
			ctl.use_spaces = 1;
			ctl.mincolsep = strtou32_or_err(optarg, _("invalid spaces argument"));
			break;
		case 's':
			free(ctl.input_separator);
			ctl.input_separator = mbs_to_wcs(optarg);
			if (!ctl.input_separator)
				err(EXIT_FAILURE, _("failed to parse input separator"));
			ctl.greedy = 0;
			break;
		case 'T':
			ctl.tab_coltrunc = optarg;
			break;
		case 't':
			ctl.mode = COLUMN_MODE_TABLE;
			break;
		case 'W':
			ctl.tab_colwrap = optarg;
			break;
		case 'x':
			ctl.mode = COLUMN_MODE_FILLROWS;
			break;
		case OPT_COLOR:
			colormode = UL_COLORMODE_AUTO;
			if (optarg)
				colormode = colormode_or_err(optarg);
			break;
		case OPT_COLORSCHEME:
			ctl.tab_colorscheme = optarg;
			break;

		case 'h':
			usage();
		case 'V':
			print_version(EXIT_SUCCESS);
		default:
			errtryhelp(EXIT_FAILURE);
		}
	}
	argc -= optind;
	argv += optind;

	if (ctl.termwidth == (size_t) -1)
		ctl.termwidth = get_terminal_width(80);

	if (ctl.tree) {
		ctl.mode = COLUMN_MODE_TABLE;
		if (!ctl.tree_parent || !ctl.tree_id)
			errx(EXIT_FAILURE, _("options --tree-id and --tree-parent are "
					     "required for tree formatting"));
	}

	if (ctl.mode != COLUMN_MODE_TABLE
	    && (ctl.tab_order || ctl.tab_name || ctl.tab_colwrap ||
		ctl.tab_colhide || ctl.tab_coltrunc || ctl.tab_colnoextrem ||
		ctl.tab_colright || ctl.tab_colnames || ctl.tab_columns))
		errx(EXIT_FAILURE, _("option --table required for all --table-*"));

	if (!ctl.tab_colnames && !ctl.tab_columns && ctl.json)
		errx(EXIT_FAILURE, _("option --table-columns or --table-column required for --json"));

	if (ctl.mode == COLUMN_MODE_TABLE)
		colors_init(colormode, ctl.tab_colorscheme ? : "column");

	if (!*argv)
		eval += read_input(&ctl, stdin);
	else
		for (; *argv; ++argv) {
			FILE *fp;

			if ((fp = fopen(*argv, "r")) != NULL) {
				eval += read_input(&ctl, fp);
				fclose(fp);
			} else {
				warn("%s", *argv);
				eval += EXIT_FAILURE;
			}
		}

	if (ctl.mode != COLUMN_MODE_TABLE) {
		if (!ctl.nents)
			exit(eval);
		if (ctl.maxlength >= ctl.termwidth)
			ctl.mode = COLUMN_MODE_SIMPLE;
	}

	switch (ctl.mode) {
	case COLUMN_MODE_TABLE:
		if (ctl.tab && scols_table_get_nlines(ctl.tab)) {
			modify_table(&ctl);
			eval = scols_print_table(ctl.tab);

			scols_unref_table(ctl.tab);
			if (ctl.tab_colnames)
				ul_strv_free(ctl.tab_colnames);
			if (ctl.tab_columns)
				ul_strv_free(ctl.tab_columns);
		}
		break;
	case COLUMN_MODE_FILLCOLS:
		columnate_fillcols(&ctl);
		break;
	case COLUMN_MODE_FILLROWS:
		columnate_fillrows(&ctl);
		break;
	case COLUMN_MODE_SIMPLE:
		simple_print(&ctl);
		break;
	}

	free(ctl.input_separator);

	return eval == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
