#include "sccs.h"
#define	octal(c)	((c >= '0') && (c <= '7'))

typedef enum {
	T_RPAREN = 1,
	T_EQTWID,
	T_EQUALS,
	T_NOTEQ,
	T_EQ,
	T_NE,
	T_GT,
	T_LT,
	T_GE,
	T_LE,
	T_AND,
	T_OR,
	T_EOF,
} op;

typedef	struct
{
	int	i;		/* index into a lines array typically */
	char	*freeme;	/* last value if it needs to be freed */
	symbol	*sym;		/* symbol link list current state */
} nextln;

/* Globals for dspec code below. */
private struct {
	char	*p;		/* current location in dspec */
	char	*start;		/* start of dspec buffer */
	datum	eachkey;	/* key of current $each */
	char	*eachval;	/* val of eachkey in current iteration */
	FILE	*out;		/* FILE* to receive output */
	char	***buf;		/* lines array to receive output */
	int	line;		/* current line in $each iteration */
	int	tcl;		/* whether we are inside a $tcl construct */
	sccs	*s;
	delta	*d;
} _g[2];
private	int	gindex;
#define	g	_g[gindex]

private void	dollar(int output, FILE *out, char ***buf);
private void	err(char *msg);
private void	evalId(FILE *out, char *** buf);
private void	evalParenid(FILE *out, char ***buf, datum id);
private int	expr(void);
private int	expr2(op *next_tok);
private char	*getnext(datum kw, nextln *state);
private int	getParenid(datum *id);
private void	stmtList(int output);
private char	**string(op *next_tok);

void
dspec_eval(FILE * out, char ***buf, sccs *s, delta *d, char *dspec)
{
	gindex = gindex ? 0 : 1;	// flip flop
	bzero(&g, sizeof(g));
	g.out = out;
	g.buf = buf;
	g.s = s;
	g.d = d;
	g.p = dspec;
	g.start = dspec;
	stmtList(1);
}

/*
 * This is a recursive-descent parser that implements the following
 * grammar for dspecs (where [[...]] indicates an optional clause
 * and {{...}} indicates 0 or more repetitions of):
 *
 * <stmt_list> -> {{ <stmt> }}
 * <stmt>      -> $if(<expr>){<stmt_list>}[[$else{<stmt_list>}]]
 *	       -> $unless(<expr>){<stmt_list>}[[$else{<stmt_list>}]]
 *	       -> $each(:ID:){<stmt_list>}
 *	       -> char
 *	       -> escaped_char
 *	       -> :ID:
 *	       -> (:ID:)
 * <expr>      -> <expr2> {{ <logop> <expr2> }}
 * <expr2>     -> <str> <relop> <str>
 *	       -> <str>
 *	       -> (<expr>)
 *             -> !<expr2>
 * <str>       -> {{ <atom> }}
 * <atom>      -> char
 *	       -> escaped_char
 *	       -> :ID:
 *	       -> (:ID:)
 * <logop>     -> " && " | " || "
 * <relop>     -> "=" | "!=" | "=~" 
 *	       -> " -eq " | " -ne " | " -gt " | " -ge " | " -lt " | " -le "
 *
 * This grammar is ambiguous due to (:ID:) loooking like a
 * parenthesized sub-expression.  The code tries to parse (:ID:) first
 * as an $each variable, then as a regular :ID:, then as regular text.
 *
 * Note that this is broken: $if((:MERGE:)){:REV:}
 *
 * The following procedures can be thought of as implementing an
 * attribute grammar where the output parameters are synthesized
 * attributes which hold the expression values and the next token
 * of lookahead in some cases.	It has been written for speed.
 *
 * Written by Rob Netzer <rob@bolabs.com> with some hacking
 * by wscott & lm.
 */

private void
stmtList(int output)
{
	char	c;
	int	i;
	FILE	*out = 0;
	char	***buf = 0;
	datum	id;
	static int	depth = 0;

	/*
	 * output==1 means evaluate and output.	 output==0 means
	 * evaluate but throw away.
	 */
	if (output) {
		out = g.out;
		buf = g.buf;
	}

	++depth;
	while (*g.p) {
		switch (*g.p) {
		    case '$':
			dollar(output, out, buf);
			break;
		    case ':':
			evalId(out, buf);
			break;
		    case '}':
			if (depth == 1) {
				show_s(g.s, out, buf, "}", 1);
				++g.p;
			} else {
				--depth;
				return;
			}
			break;
		    case '\\':
			if (octal(g.p[1]) && octal(g.p[2]) && octal(g.p[3])) {
				sscanf(g.p, "\\%03o", &i);
				c = i;
				g.p += 4;
			} else {
				switch (g.p[1]) {
				    case 'b': c = '\b'; break;
				    case 'f': c = '\f'; break;
				    case 'n': c = '\n'; break;
				    case 'r': c = '\r'; break;
				    case 't': c = '\t'; break;
				    default:  c = g.p[1]; break;
				}
				g.p += 2;
			}
			show_s(g.s, out, buf, &c, 1);
			break;
		    default:
			if (getParenid(&id)) {
				evalParenid(out, buf, id);
			} else {
				show_s(g.s, out, buf, g.p, 1);
				++g.p;
			}
			break;
		}
	}
	--depth;
}

private void
dollar(int output, FILE *out, char ***buf)
{
	int	rc;
	static int	in_each = 0;

	if (strneq("$if(", g.p, 4)) {
		g.p += 4;
		rc = expr();
		if (*g.p++ != ')') err("missing )");
		if (*g.p++ != '{') err("missing {");
		stmtList(rc && output);
		if (*g.p++ != '}') err("missing }");
		if (strneq("$else{", g.p, 6)) {
			g.p += 6;
			stmtList(!rc && output);
			if (*g.p++ != '}') err("missing }");
		}
	} else if (strneq("$each(", g.p, 6)) {
		nextln	state;
		char	*bufptr;
		int	savejoin;

		if (in_each++) err("nested each illegal");

		g.p += 6;
		if (*g.p++ != ':') err("missing id");

		/* Extract the id in $each(:id:) */
		g.eachkey.dptr = g.p;
		while (*g.p && (*g.p != ':')) ++g.p;
		unless (*g.p) err("premature eof");
		g.eachkey.dsize = g.p - g.eachkey.dptr;
		++g.p;

		if (*g.p++ != ')') err("missing )");
		if (*g.p++ != '{') err("missing {");

		/*
		 * Re-evaluate the $each body for each
		 * line of the $each variable.
		 */
		bzero(&state, sizeof(state));
		bufptr	  = g.p;
		g.line	  = 1;
		savejoin  = g.s->prs_join;
		g.s->prs_join = 0;
		while (g.eachval = getnext(g.eachkey, &state)) {
			g.p = bufptr;
			stmtList(output);
			++g.line;
		}
		g.eachkey.dptr = 0;
		g.eachkey.dsize = 0;
		g.s->prs_join = savejoin;
		--in_each;
		/* Eat the body if we never parsed it above. */
		if (g.line == 1) stmtList(0);
		if (*g.p++ != '}') err("missing }");
	} else if (strneq("$unless(", g.p, 8)) {
		g.p += 8;
		rc = !expr();
		if (*g.p++ != ')') err("missing )");
		if (*g.p++ != '{') err("missing {");
		stmtList(rc && output);
		if (*g.p++ != '}') err("missing }");
		if (strneq("$else{", g.p, 6)) {
			g.p += 6;
			stmtList(!rc && output);
			if (*g.p++ != '}') err("missing }");
		}
	} else if (strneq("$tcl{", g.p, 5)) {
		g.p += 5;
		rc = g.tcl;
		g.tcl = 0;
		show_s(g.s, out, buf, "{", 1);
		g.tcl = rc + 1;
		stmtList(output);
		g.tcl = 0;
		show_s(g.s, out, buf, "}", 1);
		g.tcl = rc;
		if (*g.p++ != '}') err("missing }");
	} else {
		show_s(g.s, out, buf, "$", 1);
		g.p++;
	}
}

private int
expr(void)
{
	op	op;
	int	ret;

	ret = expr2(&op);
	while (*g.p) {
		switch (op) {
		    case T_AND:
			/*
			 * You might be tempted to write this as
			 *	(ret && expr2(&op))
			 * since that reads more like what the user
			 * wrote.  But C will short circuit that and
			 * you won't run expr2() in some cases.  Oops.
			 *
			 * You might also think you can do a return here
			 * and you can't, this handles a || b && c.
			 */
			ret = expr2(&op) && ret;
			break;
		    case T_OR:
			ret = expr2(&op) || ret;
			break;
		    case T_RPAREN:
			return (ret);
		    default:
			err("expected &&, ||, or )");
		}
	}
	return (ret);
}

private int
expr2(op *next_tok)
{
	op	op;
	datum	id;
	int	ret;
	char	*lhs, *rhs;

	while (g.p[0] == ' ') ++g.p; /* skip whitespace */
	if (g.p[0] == '!') {
		g.p++;
		return (!expr2(next_tok));
	}
	if ((g.p[0] == '(') && !getParenid(&id)) {
		/* Parenthesized sub-expression. */
		++g.p;	/* eat ( */
		ret = expr();
		++g.p;	/* eat ) */
		while (g.p[0] == ' ') ++g.p; /* skip whitespace */
		if (g.p[0] == ')') {
			*next_tok = T_RPAREN;
		} else if ((g.p[0] == '&') && (g.p[1] == '&')) {
			*next_tok = T_AND;
			g.p += 2;
		} else if ((g.p[0] == '|') && (g.p[1] == '|')) {
			*next_tok = T_OR;
			g.p += 2;
		} else {
			err("expected &&, ||, or )");
		}
		return (ret);
	} else {
		lhs = str_pullup(0, string(&op));
		switch (op) {
		    case T_RPAREN:
		    case T_AND:
		    case T_OR:
			ret = *lhs;
			free(lhs);
			*next_tok = op;
			return (ret);
		    case T_EOF:
			err("expected operator or )");
		    default:
			break;
		}
		rhs = str_pullup(0, string(next_tok));
		switch (op) {
		    case T_EQUALS:	ret =  streq(lhs, rhs); break;
		    case T_NOTEQ:	ret = !streq(lhs, rhs); break;
		    case T_EQ:		ret = atof(lhs) == atof(rhs); break;
		    case T_NE:		ret = atof(lhs) != atof(rhs); break;
		    case T_GT:		ret = atof(lhs) >  atof(rhs); break;
		    case T_GE:		ret = atof(lhs) >= atof(rhs); break;
		    case T_LT:		ret = atof(lhs) <  atof(rhs); break;
		    case T_LE:		ret = atof(lhs) <= atof(rhs); break;
		    case T_EQTWID:	ret =  match_one(lhs, rhs, 1); break;
		    default: assert(0); ret = 0; break;
		}
		free(lhs);
		free(rhs);
		return (ret);
	}
}

private char **
string(op *next_tok)
{
	char	**s = 0;
	datum	id;

	while (*g.p) {
		if (g.p[0] == ':') {
			evalId(0, &s);
			continue;
		} else if (getParenid(&id)) {
			evalParenid(0, &s, id);
			continue;
		} else if (g.p[0] == ')') {
			*next_tok = T_RPAREN;
			return (s);
		} else if ((g.p[0] == '=') && (g.p[1] == '~')) {
			*next_tok = T_EQTWID;
			g.p += 2;
			return (s);
		} else if (g.p[0] == '=') {
			*next_tok = T_EQUALS;
			++g.p;
			return (s);
		} else if ((g.p[0] == '!') && (g.p[1] == '=')) {
			*next_tok = T_NOTEQ;
			g.p += 2;
			return (s);
		} else if ((g.p[0] == ' ') &&
		    (g.p[1] == '-') && (g.p[4] == ' ')) {
			if ((g.p[2] == 'e') && (g.p[3] == 'q')) {
				*next_tok = T_EQ;
				g.p += 5;
				return (s);
			} else if ((g.p[2] == 'n') && (g.p[3] == 'e')) {
				*next_tok = T_NE;
				g.p += 5;
				return (s);
			} else if ((g.p[2] == 'g') && (g.p[3] == 't')) {
				*next_tok = T_GT;
				g.p += 5;
				return (s);
			} else if ((g.p[2] == 'g') && (g.p[3] == 'e')) {
				*next_tok = T_GE;
				g.p += 5;
				return (s);
			} else if ((g.p[2] == 'l') && (g.p[3] == 't')) {
				*next_tok = T_LT;
				g.p += 5;
				return (s);
			} else if ((g.p[2] == 'l') && (g.p[3] == 'e')) {
				*next_tok = T_LE;
				g.p += 5;
				return (s);
			}
		} else if ((g.p[0] == '&') && (g.p[1] == '&')) {
			*next_tok = T_AND;
			g.p += 2;
			return (s);
		} else if ((g.p[0] == '|') && (g.p[1] == '|')) {
			*next_tok = T_OR;
			g.p += 2;
			return (s);
		} else if (g.p[0] == ' ') {
			++g.p;
			continue;
		}
		s = data_append(s, g.p++, 1, 0);
	}
	*next_tok = T_EOF;
	return (s);
}

private int
getParenid(datum *id)
{
	/*
	 * Find out whether g.p points to a (:ID:) construct.  If so,
	 * return 1 and set *id.
	 */
	char *c;

	unless ((g.p[0] == '(') && (g.p[1] == ':')) return (0);
	id->dptr = c = g.p + 2;
	while (*c && ((c[0] != ':') || (c[1] != ')'))) {
	       if ((*c != '%') && (*c != '_') &&
		   (*c != '-') && (*c != '#') &&
		   !isalpha(*c)) {
		       return (0);
	       }
	       ++c;
	}
	unless (*c) return (0);
	id->dsize = c - id->dptr;
	return (1);
}

private void
evalParenid(FILE *out, char ***buf, datum id)
{
	/*
	 * Expand a (:ID:).  If the eachkey has a value for if id,
	 * use that.  Otherwise output the parentheses and try
	 * expanding ID as a regular keyword.  If it's not a keyword,
	 * treat it as a regular string.
	 */
	if ((id.dsize == g.eachkey.dsize) &&
	    strneq(g.eachkey.dptr, id.dptr, id.dsize)) {
		show_s(g.s, out, buf, g.eachval, strlen(g.eachval));
	} else {
		show_s(g.s, out, buf, "(", 1);
		if (kw2val(out, buf, id.dptr, id.dsize, g.s, g.d) < 0) {
			show_s(g.s, out, buf, ":", 1);
			show_s(g.s, out, buf, id.dptr, id.dsize);
			show_s(g.s, out, buf, ":", 1);
		}
		show_s(g.s, out, buf, ")", 1);
	}
	g.p += id.dsize + 4;  /* move past ending ':)' */
}

private void
evalId(FILE *out, char *** buf)
{
	/*
	 * Call with g.p pointing to a ':'.  If what comes after is
	 * ":ID:" and a keyword, expand it into out/buf.  If it's not
	 * a keyword or no ending colon is there, output just the ':'.
	 */
	char	*c, *id;

	id = c = g.p + 1;
	while (*c && (*c != ':')) ++c;

	if (*c) {
		if (kw2val(out, buf, id, c - id, g.s, g.d) >= 0) {
			g.p = c + 1;  /* move past ending ':' */
			return;
		}
	}
	show_s(g.s, out, buf, ":", 1);
	++g.p;
}

private void
err(char *msg)
{
	int	i, n;

	fprintf(stderr, "syntax error: %s\n", msg);
	fprintf(stderr, "%s\n", g.start);
	n = g.p - g.start - 1;
	for (i = 0; i < n; ++i) fputc(' ', stderr);
	fprintf(stderr, "^\n");
	exit(1);
}

/*
 * Given a keyword with a multi-line value, return each line successively.
 * A nextln * is passed in to store the state
 * of where we are in the list of lines to return.
 * Call this function with all of state cleared the first time.
 * In particular, state->i=0 to get the first line, then pass the
 * return value back in to get subsquent lines.
 * Return a pointer to the data or 0 meaning EOF.
 * If data is malloced, save the pointer in freeme, which will get freed
 * on next call.  Nothing outside this routine knows internals of state.
 */
private char *
getnext(datum kw, nextln *state)
{
	if (state->freeme) {
		free(state->freeme);
		state->freeme = 0;
	}
again:
	++state->i;	/* first call has it set to 0, so now 1 */
	if (strneq(kw.dptr, "C", kw.dsize)) {
		comments_load(g.s, g.d);
		unless (g.d && g.d->cmnts &&
		    (state->i < LSIZ(g.d->cmnts)) &&
		    g.d->cmnts[state->i]) {
			return (0);
		}
		if (g.d->cmnts[state->i][0] == '\001') goto again;
		return(g.d->cmnts[state->i]);
	}

	/* XXX FD depracated */
	if (strneq(kw.dptr, "FD", kw.dsize)) {
		unless (g.s && g.s->text &&
		    (state->i < LSIZ(g.s->text)) && g.s->text[state->i]) {
			return (0);
		}
		/* XXX Is this needed for title, or only comments? */
		if (g.s->text[state->i][0] == '\001') goto again;
		return (g.s->text[state->i]);
	}

	if (strneq(kw.dptr, "SYMBOL", kw.dsize) ||
	    strneq(kw.dptr, "TAG", kw.dsize)) {
		if (state->i == 1) {
			unless (g.d && (g.d->flags & D_SYMBOLS)) return (0);
			state->sym = g.s->symbols;
		} else {
			state->sym = state->sym->next;
		}
		while (state->sym && (g.d !=
		    (g.s->prs_all ? state->sym->metad : state->sym->d))) {
			state->sym = state->sym->next;
		}
		unless (state->sym) return (0);
		return (state->sym->symname);
	}

	/* Handle all single-line keywords. */
	if (state->i == 1) {
		char	**ret = 0;

		/* First time in, get the keyword value. */
		kw2val(0, &ret, kw.dptr, kw.dsize, g.s, g.d);
		return (state->freeme = str_pullup(0, ret));
	} else {
		/* Second time in, bail out. */
		return (0);
	}
	/* not reached */
}


void
dspec_printeach(sccs *s, FILE *out, char ***vbuf)
{
	show_s(s, out, vbuf, g.eachval, strlen(g.eachval));
}

void
dspec_printline(sccs *s, FILE *out, char ***vbuf)
{
	show_d(s, out, vbuf, "%d", g.line);
}

private void
tclQuote(char *s, int len, FILE *f)
{
	unless (s && s[0] && len) return;
	for (; len; --len, ++s) {
		switch (*s) {
		    case '\b':	fputs("\\b", f); continue;
		    case '\f':	fputs("\\f", f); continue;
		    case '\n':	fputs("\\n", f); continue;
		    case '\r':  fputs("\\r", f); continue;
		    case '\t':  fputs("\\t", f); continue;

		    case ']':
		    case '[':
		    case '$':
		    case ';':
		    case '\\':
		    case '"':
		    case ' ':
		    case '{':
		    case '}':	fputc('\\', f); /* Fallthrough */
		    default:	fputc(*s, f);
		}
	}
}

void
show_d(sccs *s, FILE *out, char ***vbuf, char *format, int num)
{
	if (out) {
		fprintf(out, format, num);
		s->prs_output = 1;
	}
	if (vbuf) *vbuf = str_append(*vbuf, aprintf(format, num), 1);
}

void
show_s(sccs *s, FILE *out, char ***vbuf, char *data, int len)
{
	if (len == -1) len = strlen(data); /* special interface */
	if (out) {
		if (g.tcl) {
			tclQuote(data, len, out);
		} else {
			fwrite(data, 1, len, out);
		}
		s->prs_output = 1;
	}
	if (vbuf) *vbuf = data_append(*vbuf, data, len, 0);
}
