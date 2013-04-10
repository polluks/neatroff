#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "xroff.h"

#define ADJ_LL		(n_l - n_i)	/* effective line length */
#define ADJ_MODE	(n_u ? n_j : ADJ_N)
#define adj		env_adj()	/* line buffer */

/* diversions */
struct div {
	struct sbuf sbuf;	/* diversion output */
	int reg;		/* diversion register */
	int dl;			/* diversion width */
	int prev_d;		/* previous \(.d value */
	int tpos;		/* diversion trap position */
	int treg;		/* diversion trap register */
};
static struct div divs[NPREV];	/* diversion stack */
static struct div *cdiv;	/* current diversion */
static int ren_f;		/* last rendered n_f */
static int ren_s;		/* last rendered n_s */
static int ren_div;		/* rendering a diversion */

static int ren_backed = -1;	/* pushed back character */

static int bp_next;		/* next page number */
static int bp_force;		/* execute the traps until the next page */

static int ren_next(void)
{
	int c = ren_backed >= 0 ? ren_backed : tr_next();
	ren_backed = -1;
	return c;
}

static void ren_back(int c)
{
	ren_backed = c;
}

static int nextchar(char *s)
{
	int c = ren_next();
	int l = utf8len(c);
	int i;
	if (c < 0)
		return 0;
	s[0] = c;
	for (i = 1; i < l; i++)
		s[i] = ren_next();
	s[l] = '\0';
	return l;
}

void tr_di(char **args)
{
	if (args[1]) {
		cdiv = cdiv ? cdiv + 1 : divs;
		memset(cdiv, 0, sizeof(*cdiv));
		sbuf_init(&cdiv->sbuf);
		cdiv->reg = REG(args[1][0], args[1][1]);
		cdiv->prev_d = n_d;
		cdiv->treg = -1;
		if (args[0][2] == 'a' && str_get(cdiv->reg))	/* .da */
			sbuf_append(&cdiv->sbuf, str_get(cdiv->reg));
		n_d = 0;
		sbuf_append(&cdiv->sbuf, DIV_BEG "\n");
		ren_f = 0;
		ren_s = 0;
	} else if (cdiv) {
		sbuf_putnl(&cdiv->sbuf);
		sbuf_append(&cdiv->sbuf, DIV_END "\n");
		str_set(cdiv->reg, sbuf_buf(&cdiv->sbuf));
		sbuf_done(&cdiv->sbuf);
		n_dl = cdiv->dl;
		n_dn = n_d;
		n_d = cdiv->prev_d;
		cdiv = cdiv > divs ? cdiv - 1 : NULL;
		ren_f = 0;
		ren_s = 0;
	}
}

void tr_divbeg(char **args)
{
	odiv_beg();
	ren_div++;
}

void tr_divend(char **args)
{
	odiv_end();
	ren_div--;
}

static void ren_sp(int n)
{
	char cmd[32];
	if (!n && ren_div && !n_u)
		return;
	n_d += n ? n : n_v;
	if (cdiv) {
		sbuf_putnl(&cdiv->sbuf);
		sprintf(cmd, ".sp %du\n", n ? n : n_v);
		sbuf_append(&cdiv->sbuf, cmd);
	} else {
		n_nl = n_d;
		if (n_nl <= n_p)
			OUT("v%d\n", n ? n : n_v);
	}
}

static int trap_reg(int pos);
static int trap_pos(int pos);

static void push_ne(int dobr)
{
	char buf[32];
	sprintf(buf, "%cne %du\n", dobr ? '.' : '\'', n_p);
	in_push(buf, NULL);
}

static void trap_exec(int reg)
{
	if (bp_force)
		push_ne(0);
	if (str_get(reg))
		in_push(str_get(reg), NULL);
}

/* return 1 if executed a trap */
static int ren_traps(int beg, int end, int dosp)
{
	int pos = trap_pos(beg);
	if (pos >= 0 && pos < n_p && pos <= end) {
		if (dosp && pos > beg)
			ren_sp(pos - beg);
		trap_exec(trap_reg(beg));
		return 1;
	}
	return 0;
}

/* start a new page if needed */
static int ren_pagelimit(int ne)
{
	if (n_nl + ne >= n_p && !cdiv) {
		ren_page(bp_next);
		bp_force = 0;
		if (trap_pos(-1) == 0)
			trap_exec(trap_reg(-1));
		return 1;
	}
	return 0;
}

static void down(int n)
{
	if (!ren_traps(n_d, n_d + (n ? n : n_v), 1)) {
		ren_sp(n);
		ren_pagelimit(0);
	}
}

static void out_line(char *out, int w)
{
	int prev_d = n_d;
	ren_sp(0);
	n_n = w;
	if (cdiv) {
		if (cdiv->dl < w)
			cdiv->dl = w;
		sbuf_append(&cdiv->sbuf, out);
	} else {
		OUT("H%d\n", n_o + n_i);
		output(out);
	}
	if (!ren_traps(prev_d, n_d, 0))
		ren_pagelimit(0);
}

static void ren_br(int force)
{
	char out[LNLEN];
	int w;
	if (!adj_empty(adj, ADJ_MODE)) {
		w = adj_fill(adj, force ? ADJ_N : ADJ_MODE, ADJ_LL, out);
		out_line(out, w);
	}
}

void tr_br(char **args)
{
	if (args[0][0] == '.')
		ren_br(1);
}

void tr_sp(char **args)
{
	if (args[0][0] == '.')
		ren_br(1);
	down(args[1] ? tr_int(args[1], 0, 'v') : n_v);
}

void ren_page(int pg)
{
	n_nl = -1;
	n_d = 0;
	n_pg = pg;
	bp_next = n_pg + 1;
	OUT("p%d\n", pg);
	OUT("V%d\n", 0);
}

void tr_ne(char **args)
{
	int n = args[1] ? tr_int(args[1], 0, 'v') : n_v;
	ren_br(1);
	if (!ren_traps(n_d, n_d + n, 1))
		ren_pagelimit(n);
}

void tr_bp(char **args)
{
	if (!cdiv) {
		bp_force = 1;
		if (args[1])
			bp_next = tr_int(args[1], n_pg, '\0');
		push_ne(args[0][0] == '.');
	}
}

static void ren_ps(char *s)
{
	int ps = !s || !*s || !strcmp("0", s) ? n_s0 : tr_int(s, n_s, '\0');
	n_s0 = n_s;
	n_s = ps;
}

void tr_ps(char **args)
{
	ren_ps(args[1]);
}

void tr_in(char **args)
{
	if (args[0][0] == '.')
		ren_br(1);
	if (args[1])
		n_i = tr_int(args[1], n_i, 'm');
}

static void ren_ft(char *s)
{
	int fn = !*s || !strcmp("P", s) ? n_f0 : dev_font(s);
	if (fn >= 0) {
		n_f0 = n_f;
		n_f = fn;
	}
}

void tr_ft(char **args)
{
	if (args[1])
		ren_ft(args[1]);
}

void tr_fp(char **args)
{
	if (!args[2])
		return;
	if (dev_mnt(atoi(args[1]), args[2], args[3] ? args[3] : args[2]) < 0)
		errmsg("troff: failed to mount %s\n", args[2]);
}

void tr_nf(char **args)
{
	if (args[0][0] == '.')
		ren_br(1);
	n_u = 0;
}

void tr_fi(char **args)
{
	if (args[0][0] == '.')
		ren_br(1);
	n_u = 1;
}

static void escarg_ren(char *d, int cmd)
{
	int c, q;
	if (strchr(ESC_P, cmd)) {
		c = ren_next();
		if (cmd == 's' && (c == '-' || c == '+')) {
			*d++ = c;
			c = ren_next();
		}
		if (c == '(') {
			*d++ = ren_next();
			*d++ = ren_next();
		} else {
			*d++ = c;
			if (cmd == 's' && c >= '1' && c <= '3') {
				c = ren_next();
				if (isdigit(c))
					*d++ = c;
				else
					ren_back(c);
			}
		}
	}
	if (strchr(ESC_Q, cmd)) {
		q = ren_next();
		while (1) {
			c = ren_next();
			if (c == q || c < 0)
				break;
			*d++ = c;
		}
	}
	if (cmd == 'z')
		*d++ = ren_next();
	*d = '\0';
}

void render(void)
{
	char c[GNLEN * 2];
	char arg[ILNLEN];
	struct glyph *g;
	int esc = 0;
	ren_br(1);
	while (nextchar(c) > 0) {
		if (c[0] == ' ' || c[0] == '\n')
			adj_put(adj, charwid(dev_spacewid(), n_s), c);
		while (adj_full(adj, ADJ_MODE, ADJ_LL))
			ren_br(0);
		if (c[0] == ' ' || c[0] == '\n')
			continue;
		esc = 0;
		if (c[0] == '\\') {
			esc = 1;
			nextchar(c);
			if (c[0] == '(') {
				int l = nextchar(c);
				l += nextchar(c + l);
				c[l] = '\0';
			} else if (strchr("sf", c[0])) {
				escarg_ren(arg, c[0]);
				if (c[0] == 'f')
					ren_ft(arg);
				if (c[0] == 's')
					ren_ps(arg);
				continue;
			}
		}
		if (ren_s != n_s) {
			adj_swid(adj, charwid(dev_spacewid(), n_s));
			adj_put(adj, 0, "\\s(%02d", n_s);
			ren_s = n_s;
		}
		if (ren_f != n_f) {
			adj_put(adj, 0, "\\f(%02d", n_f);
			ren_f = n_f;
		}
		if (utf8len(c[0]) == strlen(c))
			sprintf(arg, "%s%s", esc ? "\\" : "", c);
		else
			sprintf(arg, "\\(%s", c);
		g = dev_glyph(c, n_f);
		adj_put(adj, charwid(g ? g->wid : dev_spacewid(), n_s), arg);
	}
	ren_br(1);
}

static int tpos[NTRAPS];	/* trap positions */
static int treg[NTRAPS];	/* trap registers */
static int ntraps;

static int trap_first(int pos)
{
	int best = -1;
	int i;
	for (i = 0; i < ntraps; i++)
		if (treg[i] >= 0 && tpos[i] > pos)
			if (best < 0 || tpos[i] <= tpos[best])
				best = i;
	return best;
}

static int trap_byreg(int reg)
{
	int i;
	for (i = 0; i < ntraps; i++)
		if (treg[i] == reg)
			return i;
	return -1;
}

static int trap_bypos(int reg, int pos)
{
	int i;
	for (i = 0; i < ntraps; i++)
		if (treg[i] >= 0 && tpos[i] == pos)
			if (reg == -1 || treg[i] == reg)
				return i;
	return -1;
}

static int tpos_parse(char *s)
{
	int pos = tr_int(s, 0, 'v');
	return pos >= 0 ? pos : n_p + pos;
}

void tr_wh(char **args)
{
	int reg, pos, id;
	if (!args[1])
		return;
	pos = tpos_parse(args[1]);
	id = trap_bypos(-1, pos);
	if (!args[2]) {
		if (id >= 0)
			treg[id] = -1;
		return;
	}
	reg = REG(args[2][0], args[2][1]);
	if (id < 0)
		id = trap_byreg(-1);
	if (id < 0)
		id = ntraps++;
	tpos[id] = pos;
	treg[id] = reg;
}

void tr_ch(char **args)
{
	int reg;
	int id;
	if (!args[1])
		return;
	reg = REG(args[1][0], args[1][1]);
	id = trap_byreg(reg);
	if (id)
		tpos[id] = args[2] ? tpos_parse(args[2]) : -1;
}

void tr_dt(char **args)
{
	if (!cdiv)
		return;
	if (args[2]) {
		cdiv->tpos = tr_int(args[1], 0, 'v');
		cdiv->treg = REG(args[2][0], args[2][1]);
	} else {
		cdiv->treg = -1;
	}
}

static int trap_pos(int pos)
{
	int ret = trap_first(pos);
	if (cdiv)
		return cdiv->treg && cdiv->tpos > pos ? cdiv->tpos : -1;
	return ret >= 0 ? tpos[ret] : -1;
}

static int trap_reg(int pos)
{
	int ret = trap_first(pos);
	if (cdiv)
		return cdiv->treg && cdiv->tpos > pos ? cdiv->treg : -1;
	return ret >= 0 ? treg[ret] : -1;
}

int trap_next(void)
{
	int pos = trap_pos(n_d);
	if (cdiv)
		return pos >= 0 ? pos : 0x7fffffff;
	return pos >= 0 && pos < n_p ? pos : n_p - n_d;
}
