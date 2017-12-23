
#line	2	"/sys/src/cmd/pc.y"
#include <u.h>
#include <libc.h>
#include <bio.h>
#include <ctype.h>
#include <mp.h>
#include <thread.h>
#include <libsec.h>

int inbase = 10, outbase, divmode, sep, heads, fail, prompt;
enum { MAXARGS = 16 };

typedef struct Num Num;
struct Num {
	mpint;
	int b;
	Ref;
};
enum { STRONG = 0x100 };

void *
emalloc(int n)
{
	void *v;
	
	v = malloc(n);
	if(v == nil)
		sysfatal("malloc: %r");
	memset(v, 0, n);
	setmalloctag(v, getcallerpc(&n));
	return v;
}

void *
error(char *fmt, ...)
{
	va_list va;
	Fmt f;
	char buf[256];
	
	fmtfdinit(&f, 2, buf, sizeof(buf));
	va_start(va, fmt);
	fmtvprint(&f, fmt, va);
	fmtrune(&f, '\n');
	fmtfdflush(&f);
	va_end(va);
	fail++;
	return nil;
}

Num *
numalloc(void)
{
	Num *r;
	
	r = emalloc(sizeof(Num));
	r->ref = 1;
	r->p = emalloc(0);
	mpassign(mpzero, r);
	return r;
}

Num *
numincref(Num *n)
{
	incref(n);
	return n;
}

Num *
numdecref(Num *n)
{
	if(n == nil) return nil;
	if(decref(n) == 0){
		free(n->p);
		free(n);
		return nil;
	}
	return n;
}

Num *
nummod(Num *n)
{
	Num *m;

	if(n == nil) return nil;
	if(n->ref == 1) return n;
	m = numalloc();
	mpassign(n, m);
	m->b = n->b;
	numdecref(n);
	return m;
}

int
basemax(int a, int b)
{
	if(a == STRONG+10 && b >= STRONG) return b;
	if(b == STRONG+10 && a >= STRONG) return a;
	if(a == 10) return b;
	if(b == 10) return a;
	if(a < b) return b;
	return a;
}

#define	LOEXP	57346
#define	LOLSH	57347
#define	LORSH	57348
#define	LOEQ	57349
#define	LONE	57350
#define	LOLE	57351
#define	LOGE	57352
#define	LOLAND	57353
#define	LOLOR	57354

#line	110	"/sys/src/cmd/pc.y"

Num *
numbin(int op, Num *a, Num *b)
{
	mpint *r;
	
	if(fail || a == nil || b == nil) return nil;
	a = nummod(a);
	a->b = basemax(a->b, b->b);
	switch(op){
	case '+': mpadd(a, b, a); break;
	case '-': mpsub(a, b, a); break;
	case '*': mpmul(a, b, a); break;
	case '/':
		if(mpcmp(b, mpzero) == 0){
			numdecref(a);
			numdecref(b);
			return error("division by zero");
		}
		r = mpnew(0);
		mpdiv(a, b, a, r);
		if(!divmode && r->sign < 0)
			if(b->sign > 0)
				mpsub(a, mpone, a);
			else
				mpadd(a, mpone, a);
		mpfree(r);
		break;
	case '%':
		if(mpcmp(b, mpzero) == 0){
			numdecref(a);
			numdecref(b);
			return error("division by zero");
		}	
		mpdiv(a, b, nil, a);
		if(!divmode && a->sign < 0)
			if(b->sign > 0)
				mpadd(a, b, a);
			else
				mpsub(a, b, a);
		break;
	case '&': mpand(a, b, a); break;
	case '|': mpor(a, b, a); break;
	case '^': mpxor(a, b, a); break;
	case LOEXP:
		if(mpcmp(b, mpzero) < 0){
			numdecref(a);
			numdecref(b);
			return error("negative exponent");
		}
		mpexp(a, b, nil, a);
		break;
	case LOLSH:
		if(mpsignif(b) >= 31){
			if(b->sign > 0)
				error("left shift overflow");
			itomp(-(mpcmp(a, mpzero) < 0), a);
		}else
			mpasr(a, -mptoi(b), a);
		break;	
	case LORSH:
		if(mpsignif(b) >= 31){
			if(b->sign < 0)
				error("right shift overflow");
			itomp(-(mpcmp(a, mpzero) < 0), a);
		}else
			mpasr(a, mptoi(b), a);
		break;
	case '<': itomp(mpcmp(a, b) < 0, a); a->b = 0; break;
	case '>': itomp(mpcmp(a, b) > 0, a); a->b = 0; break;
	case LOLE: itomp(mpcmp(a, b) <= 0, a); a->b = 0; break;
	case LOGE: itomp(mpcmp(a, b) >= 0, a); a->b = 0; break;
	case LOEQ: itomp(mpcmp(a, b) == 0, a); a->b = 0; break;
	case LONE: itomp(mpcmp(a, b) != 0, a); a->b = 0; break;
	case LOLAND:
		a->b = b->b;
		if(mpcmp(a, mpzero) == 0)
			mpassign(mpzero, a);
		else
			mpassign(b, a);
		break;
	case LOLOR:
		a->b = b->b;
		if(mpcmp(a, mpzero) != 0)
			mpassign(mpone, a);
		else
			mpassign(b, a);
		break;
	}
	numdecref(b);
	return a;
}

typedef struct Symbol Symbol;
struct Symbol {
	enum {
		SYMNONE,
		SYMVAR,
		SYMFUNC,
	} t;
	Num *val;
	int nargs;
	Num *(*func)(int, Num **); 
	char *name;
	Symbol *next;
};
Symbol *symtab[64];

Symbol *
getsym(char *n, int mk)
{
	Symbol **p;
	for(p = &symtab[*n&63]; *p != nil; p = &(*p)->next)
		if(strcmp((*p)->name, n) == 0)
			return *p;
	if(!mk) return nil;
	*p = emalloc(sizeof(Symbol));
	(*p)->name = strdup(n);
	return *p;
}

static void
printhead(int n, int s, int sp, char *t)
{
	char *q;
	int i, j, k;
	
	for(i = 1; i < n; i *= 10)
		;
	while(i /= 10, i != 0){
		q = t;
		*--q = 0;
		for(j = 0, k = 0; j < n; j += s, k++){
			if(k == sep && sep != 0){
				*--q = ' ';
				k = 0;
			}
			if(j >= i || j == 0 && i == 1)
				*--q = '0' + j / i % 10;
			else
				*--q = ' ';
		}
		for(j = 0; j < sp; j++)
			*--q = ' ';
		print("%s\n", q);
	}
}

void
numprint(Num *n)
{
	int b;
	int l, i, st, sp;
	char *s, *t, *p, *q;

	if(n == nil) return;
	if(n->b >= STRONG || n->b != 0 && outbase == 0)
		b = n->b & ~STRONG;
	else if(outbase == 0)
		b = 10;
	else
		b = outbase;
	s = mptoa(n, b, nil, 0);
	l = strlen(s);
	t = emalloc(l * 2 + 4);
	q = t + l * 2 + 4;
	if(heads){
		switch(b){
		case 16: st = 4; sp = 2; break;
		case 8: st = 3; sp = 1; break;
		case 2: st = 1; sp = 2; break;
		default: st = 0; sp = 0;
		}
		if(n->sign < 0)
			sp++;
		if(st != 0)
			printhead(mpsignif(n), st, sp, q);
	}
	*--q = 0;
	for(p = s + l - 1, i = 0; p >= s && *p != '-'; p--, i++){
		if(sep != 0 && i == sep){
			*--q = '_';
			i = 0;
		}
		if(*p >= 'A')
			*--q = *p + ('a' - 'A');
		else
			*--q = *p;
	}
	if(mpcmp(n, mpzero) != 0)
		switch(b){
		case 16: *--q = 'x'; *--q = '0'; break;
		case 10: if(outbase != 0 && outbase != 10 || inbase != 10) {*--q = 'd'; *--q = '0';} break;
		case 8: *--q = '0'; break;
		case 2: *--q = 'b'; *--q = '0'; break;
		}
	if(p >= s)
		*--q = '-';
	print("%s\n", q);
	free(s);
	free(t);
}

void
numdecrefs(int n, Num **x)
{
	int i;
	
	for(i = 0; i < n; i++)
		numdecref(x[i]);
}

Num *
fncall(Symbol *s, int n, Num **x)
{
	int i;

	if(s->t != SYMFUNC){
		numdecrefs(n, x);
		return error("%s: not a function", s->name);
	}
	else if(s->nargs >= 0 && s->nargs != n){
		numdecrefs(n, x);
		return error("%s: wrong number of arguments", s->name);
	}
	for(i = 0; i < n; i++)
		if(x[i] == nil)
			return nil;
	return s->func(n, x);
}

Num *
hexfix(Symbol *s)
{
	char *b, *p, *q;

	if(inbase != 16) return nil;
	if(s->val != nil) return numincref(s->val);
	if(strspn(s->name, "0123456789ABCDEFabcdef_") != strlen(s->name)) return nil;
	b = strdup(s->name);
	for(p = b, q = b; *p != 0; p++)
		if(*p != '_')
			*q++ = *p;
	*q = 0;
	s->val = numalloc();
	strtomp(b, nil, 16, s->val);
	s->val->b = 16;
	free(b);
	return numincref(s->val);
}


#line	363	"/sys/src/cmd/pc.y"
typedef union  {
	Num *n;
	Symbol *sym;
	struct {
		Num *x[MAXARGS];
		int n;
	} args;
} YYSTYPE;
#define	LNUM	57355
#define	LSYMB	57356
#define	unary	57357

#line	394	"/sys/src/cmd/pc.y"
	int save;
	Num *last;
	Num *lastp;
extern	int	yyerrflag;
#ifndef	YYMAXDEPTH
#define	YYMAXDEPTH	150
#endif
YYSTYPE	yylval;
YYSTYPE	yyval;
#define YYEOFCODE 1
#define YYERRCODE 2

#line	555	"/sys/src/cmd/pc.y"


typedef struct Keyword Keyword;
struct Keyword {
	char *name;
	int tok;
};

Keyword ops[] = {
	"**", LOEXP,
	"<<", LOLSH,
	"<=", LOLE,
	">>", LORSH,
	">=", LOGE,
	"==", LOEQ,
	"&&", LOLAND,
	"||", LOLOR,
	"", 0,
};

Keyword *optab[128];


Biobuf *in;
int prompted;

int
yylex(void)
{
	int c, b;
	char buf[512], *p;
	Keyword *kw;
	
	if(prompt && !prompted) {print("; "); prompted = 1;}
	do
		c = Bgetc(in);
	while(c != '\n' && isspace(c));
	if(c == '\n') prompted = 0;
	if(isdigit(c)){
		for(p = buf, *p++ = c; c = Bgetc(in), isalnum(c) || c == '_'; )
			if(p < buf + sizeof(buf) - 1 && c != '_')
				*p++ = c;
		*p = 0;
		Bungetc(in);
		b = inbase;
		p = buf;
		if(*p == '0'){
			p++;
			switch(*p++){
			case 0: p -= 2; break;
			case 'b': case 'B': b = 2; break;
			case 'd': case 'D': b = 10; break;
			case 'x': case 'X': b = 16; break;
			default: p--; b = 8; break;
			}
		}
		yylval.n = numalloc();
		strtomp(p, &p, b, yylval.n);
		if(*p != 0) error("not a number: %s", buf);
		yylval.n->b = b;
		return LNUM;
	}
	if(isalpha(c) || c >= 0x80 || c == '_'){
		for(p = buf, *p++ = c; c = Bgetc(in), isalnum(c) || c >= 0x80 || c == '_'; )
			if(p < buf + sizeof(buf) - 1)
				*p++ = c;
		*p = 0;
		Bungetc(in);
		if(buf[0] == '_' && buf[1] == 0) return '_';
		yylval.sym = getsym(buf, 1);
		return LSYMB;
	}
	if(c < 128 && (kw = optab[c], kw != nil)){
		b = Bgetc(in);
		for(; kw->name[0] == c; kw++)
			if(kw->name[0] == b)
				return kw->tok;
		Bungetc(in);
	}
	return c;
}

void
yyerror(char *msg)
{
	error("%s", msg);
}

void
regfunc(char *n, Num *(*f)(int, Num **), int nargs)
{
	Symbol *s;
	
	s = getsym(n, 1);
	s->t = SYMFUNC;
	s->func = f;
	s->nargs = nargs;
}

int
toint(Num *n, int *p, int mustpos)
{
	if(mpsignif(n) > 31 || mustpos && mpcmp(n, mpzero) < 0){
		error("invalid argument");
		return -1;
	}
	if(p != nil)
		*p = mptoi(n);
	return 0;
}

Num *
fnhex(int, Num **a)
{
	Num *r;
	
	r = nummod(a[0]);
	r->b = STRONG | 16;
	return r;
}

Num *
fndec(int, Num **a)
{
	Num *r;
	
	r = nummod(a[0]);
	r->b = STRONG | 10;
	return r;
}

Num *
fnoct(int, Num **a)
{
	Num *r;
	
	r = nummod(a[0]);
	r->b = STRONG | 8;
	return r;
}

Num *
fnbin(int, Num **a)
{
	Num *r;
	
	r = nummod(a[0]);
	r->b = STRONG | 2;
	return r;
}

Num *
fnpb(int, Num **a)
{
	Num *r;
	int b;
	
	if(toint(a[1], &b, 1)){
	out:
		numdecref(a[0]);
		numdecref(a[1]);
		return nil;
	}
	if(b != 0 && b != 2 && b != 8 && b != 10 && b != 16){
		error("unsupported base");
		goto out;
	}
	r = nummod(a[0]);
	if(b == 0)
		r->b = 0;
	else
		r->b = STRONG | b;
	return r;
}

Num *
fnabs(int, Num **a)
{
	Num *r;
	
	r = nummod(a[0]);
	r->sign = 1;
	return r;
}

Num *
fnround(int, Num **a)
{
	mpint *q, *r;
	int i;

	if(mpcmp(a[1], mpzero) <= 0){
		numdecref(a[0]);
		numdecref(a[1]);
		return error("invalid argument");
	}
	q = mpnew(0);
	r = mpnew(0);
	a[0] = nummod(a[0]);
	mpdiv(a[0], a[1], q, r);
	if(r->sign < 0) mpadd(r, a[1], r);
	mpleft(r, 1, r);
	i = mpcmp(r, a[1]);
	mpright(r, 1, r);
	if(i > 0 || i == 0 && (a[0]->sign < 0) ^ (q->top != 0 && (q->p[0] & 1) != 0))
		mpsub(r, a[1], r);
	mpsub(a[0], r, a[0]);
	mpfree(q);
	mpfree(r);
	numdecref(a[1]);
	return a[0];
}

Num *
fnfloor(int, Num **a)
{
	mpint *r;

	if(mpcmp(a[1], mpzero) <= 0){
		numdecref(a[0]);
		numdecref(a[1]);
		return error("invalid argument");
	}
	r = mpnew(0);
	a[0] = nummod(a[0]);
	mpdiv(a[0], a[1], nil, r);
	if(r->sign < 0) mpadd(r, a[1], r);
	mpsub(a[0], r, a[0]);
	mpfree(r);
	numdecref(a[1]);
	return a[0];
}

Num *
fnceil(int, Num **a)
{
	mpint *r;

	if(mpcmp(a[1], mpzero) <= 0){
		numdecref(a[0]);
		numdecref(a[1]);
		return error("invalid argument");
	}
	r = mpnew(0);
	a[0] = nummod(a[0]);
	mpdiv(a[0], a[1], nil, r);
	if(r->sign < 0) mpadd(r, a[1], r);
	if(mpcmp(r, mpzero) != 0){
		mpsub(a[0], r, a[0]);
		mpadd(a[0], a[1], a[0]);
	}
	mpfree(r);
	numdecref(a[1]);
	return a[0];
}

Num *
fntrunc(int, Num **a)
{
	int i;
	
	if(toint(a[1], &i, 1)){
		numdecref(a[0]);
		numdecref(a[1]);
		return nil;
	}
	mptrunc(a[0], i, a[0]);
	return a[0];
}

Num *
fnxtend(int, Num **a)
{
	int i;
	
	if(toint(a[1], &i, 1)) return nil;
	mpxtend(a[0], i, a[0]);
	return a[0];
}

Num *
fnclog(int n, Num **a)
{
	int r;

	if(n != 1 && n != 2){
		numdecrefs(n, a);
		return error("clog: wrong number of arguments");
	}
	if(mpcmp(a[0], mpzero) <= 0 || n == 2 && mpcmp(a[1], mpone) <= 0){
		numdecref(a[0]);
		return error("invalid argument");
	}
	if(n == 1 || mpcmp(a[1], mptwo) == 0){
		a[0] = nummod(a[0]);
		mpsub(a[0], mpone, a[0]);
		itomp(mpsignif(a[0]), a[0]);
		a[0]->b = 0;
		if(n == 2) numdecref(a[1]);
		return a[0];
	}
	a[0] = nummod(a[0]);
	for(r = 0; mpcmp(a[0], mpone) > 0; r++){
		mpadd(a[0], a[1], a[0]);
		mpsub(a[0], mpone, a[0]);
		mpdiv(a[0], a[1], a[0], nil);
	}
	itomp(r, a[0]);
	a[0]->b = 0;
	numdecref(a[1]);
	return a[0];
}

Num *
fnubits(int, Num **a)
{
	if(a[0]->sign < 0){
		numdecref(a[0]);
		return error("invalid argument");
	}
	a[0] = nummod(a[0]);
	itomp(mpsignif(a[0]), a[0]);
	a[0]->b = 0;
	return a[0];
}

Num *
fnsbits(int, Num **a)
{
	a[0] = nummod(a[0]);
	if(a[0]->sign < 0) mpadd(a[0], mpone, a[0]);
	itomp(mpsignif(a[0]) + 1, a[0]);
	a[0]->b = 0;
	return a[0];
}

Num *
fnnsa(int, Num **a)
{
	int n, i;
	mpdigit d;

	a[0] = nummod(a[0]);
	if(a[0]->sign < 0){
		numdecref(a[0]);
		return error("invalid argument");
	}
	n = 0;
	for(i = 0; i < a[0]->top; i++){
		d = a[0]->p[i];
		for(; d != 0; d &= d-1)
			n++;
	}
	itomp(n, a[0]);
	a[0]->b = 0;
	return a[0];
}

Num *
fngcd(int, Num **a)
{
	a[0] = nummod(a[0]);
	a[0]->b = basemax(a[0]->b, a[1]->b);
	mpextendedgcd(a[0], a[1], a[0], nil, nil);
	return a[0];
}

Num *
fnrand(int, Num **a)
{
	Num *n;

	n = numalloc();
	n->b = a[0]->b;
	mpnrand(a[0], genrandom, n);
	numdecref(a[0]);
	return n;
}

Num *
fnminv(int, Num **a)
{
	mpint *x;

	a[0] = nummod(a[0]);
	x = mpnew(0);
	mpextendedgcd(a[0], a[1], x, a[0], nil);
	if(mpcmp(x, mpone) != 0)
		error("no modular inverse");
	else
		mpmod(a[0], a[1], a[0]);
	mpfree(x);
	numdecref(a[1]);
	return a[0];
}

Num *
fnrev(int, Num **a)
{
	mpdigit v, m;
	int i, j, n;
	
	if(toint(a[1], &n, 1)){
		numdecref(a[0]);
		numdecref(a[1]);
		return nil;
	}
	a[0] = nummod(a[0]);
	mptrunc(a[0], n, a[0]);
	for(i = 0; i < a[0]->top; i++){
		v = a[0]->p[i];
		m = -1;
		for(j = sizeof(mpdigit) * 8; j >>= 1; ){
			m ^= m << j;
			v = v >> j & m | v << j & ~m;
		}
		a[0]->p[i] = v;
	}
	for(i = 0; i < a[0]->top / 2; i++){
		v = a[0]->p[i];
		a[0]->p[i] = a[0]->p[a[0]->top - 1 - i];
		a[0]->p[a[0]->top - 1 - i] = v;
	}
	mpleft(a[0], n - a[0]->top * sizeof(mpdigit) * 8, a[0]);
	numdecref(a[1]);
	return a[0];
}

Num *
fncat(int n, Num **a)
{
	int i, w;
	Num *r;

	if(n % 2 != 0){
		error("cat: odd number of arguments");
		i = 0;
	fail:
		for(; i < n; i++)
			numdecref(a[i]);
		return nil;
	}
	r = numalloc();
	for(i = 0; i < n; i += 2){
		if(toint(a[i+1], &w, 1)) goto fail;
		mpleft(r, w, r);
		if(a[i]->sign < 0 || mpsignif(a[i]) > w){
			a[i] = nummod(a[i]);
			mptrunc(a[i], w, a[i]);
		}
		r->b = basemax(r->b, a[i]->b);
		mpor(r, a[i], r);
		numdecref(a[i]);
		numdecref(a[i+1]);
	}
	return r;
}

void
main(int argc, char **argv)
{
	Keyword *kw;
	
	fmtinstall('B', mpfmt);
	
	for(kw = ops; kw->name[0] != 0; kw++)
		if(optab[kw->name[0]] == nil)
			optab[kw->name[0]] = kw;
	
	regfunc("hex", fnhex, 1);
	regfunc("dec", fndec, 1);
	regfunc("oct", fnoct, 1);
	regfunc("bin", fnbin, 1);
	regfunc("pb", fnpb, 2);
	regfunc("abs", fnabs, 1);
	regfunc("round", fnround, 2);
	regfunc("floor", fnfloor, 2);
	regfunc("ceil", fnceil, 2);
	regfunc("trunc", fntrunc, 2);
	regfunc("xtend", fnxtend, 2);
	regfunc("clog", fnclog, -1);
	regfunc("ubits", fnubits, 1);
	regfunc("sbits", fnsbits, 1);
	regfunc("nsa", fnnsa, 1);
	regfunc("gcd", fngcd, 2);
	regfunc("minv", fnminv, 2);
	regfunc("rand", fnrand, 1);
	regfunc("rev", fnrev, 2);
	regfunc("cat", fncat, -1);

	prompt = 1;
	ARGBEGIN{
	case 'n': prompt = 0; break;
	}ARGEND;
	
	in = Bfdopen(0, OREAD);
	if(in == nil) sysfatal("Bfdopen: %r");
	extern void yyparse(void);
	yyparse();
}
short	yyexca[] =
{-1, 1,
	1, -1,
	28, 5,
	29, 5,
	-2, 0,
-1, 20,
	28, 5,
	29, 5,
	-2, 0,
};
#define	YYNPROD	52
#define	YYPRIVATE 57344
#define	YYLAST	321
short	yyact[] =
{
   4,  86,  85,  52,  19,  20,  29,  45,  44,  43,
  42,  41,   2,  46,  47,  48,  49,  50,   1,  81,
  51,  80,  54,  55,  56,  57,  58,  59,  60,  61,
  62,  63,  64,  65,  66,  67,  68,  69,  70,  71,
  72,  73,  74,  75,  76,  77,  78,   0,   0,   0,
   0,   0,  82,  83,  29,  30,  31,  32,  33,  36,
  37,  38,  39,   3,   0,   0,  40,  27,  28,  26,
  34,  35,  21,  22,   0,  23,  24,  25,   0,   0,
   0,   0,   0,   0,  53,  87,  84,  88,  29,  30,
  31,  32,  33,  36,  37,  38,  39,   0,   0,   0,
  40,  27,  28,  26,  34,  35,  21,  22,  10,  23,
  24,  25,  29,  30,  31,   0,   0,  79,   0,  11,
  17,   0,   0,   0,   0,   0,   6,   7,  13,  14,
  21,  22,   8,  23,  24,  25,   5,   9,  12,   0,
  15,  16,   0,  18,  29,  30,  31,  32,  33,  36,
  37,  38,  39,  29,   0,   0,  40,  27,  28,  26,
  34,  35,  21,  22,   0,  23,  24,  25,  11,  17,
   0,  21,  22,   0,  23,  24,  25,  13,  14,   0,
  29,   0,   0,   0,   0,   0,   0,  12,   0,  15,
  16,   0,  18,  29,  30,  31,  32,  33,  36,  37,
  38,  23,  24,  25,   0,   0,  27,  28,  26,  34,
  35,  21,  22,   0,  23,  24,  25,  29,  30,  31,
  32,  33,  36,  37,   0,   0,   0,   0,   0,   0,
  27,  28,  26,  34,  35,  21,  22,   0,  23,  24,
  25,  29,  30,  31,  32,  33,  36,  37,   0,   0,
   0,   0,   0,   0,   0,  28,  26,  34,  35,  21,
  22,   0,  23,  24,  25,  29,  30,  31,  32,  33,
  36,  37,   0,  29,  30,  31,  32,  33,  36,  37,
  26,  34,  35,  21,  22,   0,  23,  24,  25,  34,
  35,  21,  22,   0,  23,  24,  25,  29,  30,  31,
   0,   0,  36,  37,   0,   0,   0,   0,   0,   0,
   0,   0,   0,  34,  35,  21,  22,   0,  23,  24,
  25
};
short	yypact[] =
{
-1000, 106, -24,-1000, 140,-1000,-1000,-1000,-1000,-1000,
-1000,-1000, 155, 155, 155, 155, 155, -12,-1000,-1000,
 106, 155, 155, 155, 155, 155, 155, 155, 155, 155,
 155, 155, 155, 155, 155, 155, 155, 155, 155, 155,
 155, 155, 155, 155, 155, 155,  84, 176, 176, 176,
 176, 155, 155,-1000, 176, 176,   2,   2,   2, 269,
 237, 261,   2, 149, 149, 293, 293, 108, 108, 108,
 108, 213, 189,  50, 140, 140, 140, 140, 140,-1000,
 -31, -37, 140, 140, 155,-1000, 155, 140, 140
};
short	yypgo[] =
{
   0,   0,  21,  19,  18,  12,  63,  11,  10,   9,
   8,   7
};
short	yyr1[] =
{
   0,   4,   4,   5,   5,   6,   6,   7,   6,   8,
   6,   9,   6,  10,   6,  11,   6,   6,   1,   1,
   1,   1,   1,   1,   1,   1,   1,   1,   1,   1,
   1,   1,   1,   1,   1,   1,   1,   1,   1,   1,
   1,   1,   1,   1,   1,   1,   1,   1,   2,   2,
   3,   3
};
short	yyr2[] =
{
   0,   0,   3,   1,   3,   0,   1,   0,   3,   0,
   3,   0,   3,   0,   3,   0,   3,   1,   1,   3,
   3,   3,   3,   3,   3,   3,   3,   3,   3,   3,
   3,   3,   3,   3,   3,   3,   3,   3,   3,   2,
   2,   2,   2,   5,   4,   1,   3,   1,   0,   1,
   1,   3
};
short	yychk[] =
{
-1000,  -4,  -5,  -6,  -1,  30,  20,  21,  26,  31,
   2,  13,  32,  22,  23,  34,  35,  14,  37,  28,
  29,  22,  23,  25,  26,  27,  19,  17,  18,   4,
   5,   6,   7,   8,  20,  21,   9,  10,  11,  12,
  16,  -7,  -8,  -9, -10, -11,  -1,  -1,  -1,  -1,
  -1,  32,  15,  -6,  -1,  -1,  -1,  -1,  -1,  -1,
  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,
  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  -1,  33,
  -2,  -3,  -1,  -1,  36,  33,  38,  -1,  -1
};
short	yydef[] =
{
   1,  -2,   0,   3,   6,   7,   9,  11,  13,  15,
  17,  18,   0,   0,   0,   0,   0,  45,  47,   2,
  -2,   0,   0,   0,   0,   0,   0,   0,   0,   0,
   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
   0,   0,   0,   0,   0,   0,   0,  39,  40,  41,
  42,  48,   0,   4,  20,  21,  22,  23,  24,  25,
  26,  27,  28,  29,  30,  31,  32,  33,  34,  35,
  36,  37,  38,   0,   8,  10,  12,  14,  16,  19,
   0,  49,  50,  46,   0,  44,   0,  43,  51
};
short	yytok1[] =
{
   1,   0,   0,   0,   0,   0,   0,   0,   0,   0,
  28,   0,   0,   0,   0,   0,   0,   0,   0,   0,
   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
   0,   0,   0,  35,   0,   0,   0,  27,  19,  31,
  32,  33,  25,  22,  38,  23,   0,  26,   0,   0,
   0,   0,   0,   0,   0,   0,   0,   0,  36,  29,
  20,  15,  21,  16,  37,   0,   0,   0,   0,   0,
   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
   0,   0,   0,   0,  18,  30,   0,   0,   0,   0,
   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
   0,   0,   0,   0,  17,   0,  34
};
short	yytok2[] =
{
   2,   3,   4,   5,   6,   7,   8,   9,  10,  11,
  12,  13,  14,  24
};
long	yytok3[] =
{
   0
};

#line	1	"/sys/lib/yaccpar"
#define YYFLAG 		-1000
#define	yyclearin	yychar = -1
#define	yyerrok		yyerrflag = 0

#ifdef	yydebug
#include	"y.debug"
#else
#define	yydebug		0
char*	yytoknames[1];		/* for debugging */
char*	yystates[1];		/* for debugging */
#endif

/*	parser for yacc output	*/

int	yynerrs = 0;		/* number of errors */
int	yyerrflag = 0;		/* error recovery flag */

extern	int	fprint(int, char*, ...);
extern	int	sprint(char*, char*, ...);

char*
yytokname(int yyc)
{
	static char x[16];

	if(yyc > 0 && yyc <= sizeof(yytoknames)/sizeof(yytoknames[0]))
	if(yytoknames[yyc-1])
		return yytoknames[yyc-1];
	sprint(x, "<%d>", yyc);
	return x;
}

char*
yystatname(int yys)
{
	static char x[16];

	if(yys >= 0 && yys < sizeof(yystates)/sizeof(yystates[0]))
	if(yystates[yys])
		return yystates[yys];
	sprint(x, "<%d>\n", yys);
	return x;
}

long
yylex1(void)
{
	long yychar;
	long *t3p;
	int c;

	yychar = yylex();
	if(yychar <= 0) {
		c = yytok1[0];
		goto out;
	}
	if(yychar < sizeof(yytok1)/sizeof(yytok1[0])) {
		c = yytok1[yychar];
		goto out;
	}
	if(yychar >= YYPRIVATE)
		if(yychar < YYPRIVATE+sizeof(yytok2)/sizeof(yytok2[0])) {
			c = yytok2[yychar-YYPRIVATE];
			goto out;
		}
	for(t3p=yytok3;; t3p+=2) {
		c = t3p[0];
		if(c == yychar) {
			c = t3p[1];
			goto out;
		}
		if(c == 0)
			break;
	}
	c = 0;

out:
	if(c == 0)
		c = yytok2[1];	/* unknown char */
	if(yydebug >= 3)
		fprint(2, "lex %.4lux %s\n", yychar, yytokname(c));
	return c;
}

int
yyparse(void)
{
	struct
	{
		YYSTYPE	yyv;
		int	yys;
	} yys[YYMAXDEPTH], *yyp, *yypt;
	short *yyxi;
	int yyj, yym, yystate, yyn, yyg;
	long yychar;
	YYSTYPE save1, save2;
	int save3, save4;

	save1 = yylval;
	save2 = yyval;
	save3 = yynerrs;
	save4 = yyerrflag;

	yystate = 0;
	yychar = -1;
	yynerrs = 0;
	yyerrflag = 0;
	yyp = &yys[-1];
	goto yystack;

ret0:
	yyn = 0;
	goto ret;

ret1:
	yyn = 1;
	goto ret;

ret:
	yylval = save1;
	yyval = save2;
	yynerrs = save3;
	yyerrflag = save4;
	return yyn;

yystack:
	/* put a state and value onto the stack */
	if(yydebug >= 4)
		fprint(2, "char %s in %s", yytokname(yychar), yystatname(yystate));

	yyp++;
	if(yyp >= &yys[YYMAXDEPTH]) {
		yyerror("yacc stack overflow");
		goto ret1;
	}
	yyp->yys = yystate;
	yyp->yyv = yyval;

yynewstate:
	yyn = yypact[yystate];
	if(yyn <= YYFLAG)
		goto yydefault; /* simple state */
	if(yychar < 0)
		yychar = yylex1();
	yyn += yychar;
	if(yyn < 0 || yyn >= YYLAST)
		goto yydefault;
	yyn = yyact[yyn];
	if(yychk[yyn] == yychar) { /* valid shift */
		yychar = -1;
		yyval = yylval;
		yystate = yyn;
		if(yyerrflag > 0)
			yyerrflag--;
		goto yystack;
	}

yydefault:
	/* default state action */
	yyn = yydef[yystate];
	if(yyn == -2) {
		if(yychar < 0)
			yychar = yylex1();

		/* look through exception table */
		for(yyxi=yyexca;; yyxi+=2)
			if(yyxi[0] == -1 && yyxi[1] == yystate)
				break;
		for(yyxi += 2;; yyxi += 2) {
			yyn = yyxi[0];
			if(yyn < 0 || yyn == yychar)
				break;
		}
		yyn = yyxi[1];
		if(yyn < 0)
			goto ret0;
	}
	if(yyn == 0) {
		/* error ... attempt to resume parsing */
		switch(yyerrflag) {
		case 0:   /* brand new error */
			yyerror("syntax error");
			yynerrs++;
			if(yydebug >= 1) {
				fprint(2, "%s", yystatname(yystate));
				fprint(2, "saw %s\n", yytokname(yychar));
			}

		case 1:
		case 2: /* incompletely recovered error ... try again */
			yyerrflag = 3;

			/* find a state where "error" is a legal shift action */
			while(yyp >= yys) {
				yyn = yypact[yyp->yys] + YYERRCODE;
				if(yyn >= 0 && yyn < YYLAST) {
					yystate = yyact[yyn];  /* simulate a shift of "error" */
					if(yychk[yystate] == YYERRCODE)
						goto yystack;
				}

				/* the current yyp has no shift onn "error", pop stack */
				if(yydebug >= 2)
					fprint(2, "error recovery pops state %d, uncovers %d\n",
						yyp->yys, (yyp-1)->yys );
				yyp--;
			}
			/* there is no state on the stack with an error shift ... abort */
			goto ret1;

		case 3:  /* no shift yet; clobber input char */
			if(yydebug >= 2)
				fprint(2, "error recovery discards %s\n", yytokname(yychar));
			if(yychar == YYEOFCODE)
				goto ret1;
			yychar = -1;
			goto yynewstate;   /* try again in the same state */
		}
	}

	/* reduction by production yyn */
	if(yydebug >= 2)
		fprint(2, "reduce %d in:\n\t%s", yyn, yystatname(yystate));

	yypt = yyp;
	yyp -= yyr2[yyn];
	yyval = (yyp+1)->yyv;
	yym = yyn;

	/* consult goto table to find next state */
	yyn = yyr1[yyn];
	yyg = yypgo[yyn];
	yyj = yyg + yyp->yys + 1;

	if(yyj >= YYLAST || yychk[yystate=yyact[yyj]] != -yyn)
		yystate = yyact[yyg];
	switch(yym) {
		
case 2:
#line	401	"/sys/src/cmd/pc.y"
{
		if(!fail && last != nil) {
			numprint(last);
			numdecref(lastp);
			lastp = last;
		}
		fail = 0;
		last = nil;
	} break;
case 5:
#line	414	"/sys/src/cmd/pc.y"
{ last = nil; } break;
case 6:
#line	415	"/sys/src/cmd/pc.y"
{ last = yypt[-0].yyv.n; } break;
case 7:
#line	416	"/sys/src/cmd/pc.y"
{ save = inbase; inbase = 10; } break;
case 8:
#line	416	"/sys/src/cmd/pc.y"
{
		inbase = save;
		if(mpcmp(yypt[-0].yyv.n, mpzero) < 0)
			error("no.");
		if(!fail) 
			sep = mptoi(yypt[-0].yyv.n);
		numdecref(yypt[-0].yyv.n);
		numdecref(last);
		last = nil;
	} break;
case 9:
#line	426	"/sys/src/cmd/pc.y"
{ save = inbase; inbase = 10; } break;
case 10:
#line	426	"/sys/src/cmd/pc.y"
{
		inbase = save;
		if(!fail) 
			inbase = mptoi(yypt[-0].yyv.n);
		if(inbase != 2 && inbase != 8 && inbase != 10 && inbase != 16){
			error("no.");
			inbase = save;
		}
		numdecref(yypt[-0].yyv.n);
		numdecref(last);
		last = nil;
	} break;
case 11:
#line	438	"/sys/src/cmd/pc.y"
{ save = inbase; inbase = 10; } break;
case 12:
#line	438	"/sys/src/cmd/pc.y"
{
		inbase = save;
		save = outbase;
		if(!fail) 
			outbase = mptoi(yypt[-0].yyv.n);
		if(outbase != 0 && outbase != 2 && outbase != 8 && outbase != 10 && outbase != 16){
			error("no.");
			outbase = save;
		}
		numdecref(yypt[-0].yyv.n);
		numdecref(last);
		last = nil;
	} break;
case 13:
#line	451	"/sys/src/cmd/pc.y"
{ save = inbase; inbase = 10; } break;
case 14:
#line	451	"/sys/src/cmd/pc.y"
{
		inbase = save;
		save = divmode;
		if(!fail) 
			divmode = mptoi(yypt[-0].yyv.n);
		if(divmode != 0 && divmode != 1){
			error("no.");
			divmode = save;
		}
		numdecref(yypt[-0].yyv.n);
		numdecref(last);
		last = nil;
	} break;
case 15:
#line	464	"/sys/src/cmd/pc.y"
{ save = inbase; inbase = 10; } break;
case 16:
#line	464	"/sys/src/cmd/pc.y"
{
		inbase = save;
		save = heads;
		if(!fail) 
			heads = mptoi(yypt[-0].yyv.n);
		if(heads != 0 && heads != 1){
			error("no.");
			heads = save;
		}
		numdecref(yypt[-0].yyv.n);
		numdecref(last);
		last = nil;
	} break;
case 19:
#line	480	"/sys/src/cmd/pc.y"
{ yyval.n = yypt[-1].yyv.n; } break;
case 20:
#line	481	"/sys/src/cmd/pc.y"
{ yyval.n = numbin('+', yypt[-2].yyv.n, yypt[-0].yyv.n); } break;
case 21:
#line	482	"/sys/src/cmd/pc.y"
{ yyval.n = numbin('-', yypt[-2].yyv.n, yypt[-0].yyv.n); } break;
case 22:
#line	483	"/sys/src/cmd/pc.y"
{ yyval.n = numbin('*', yypt[-2].yyv.n, yypt[-0].yyv.n); } break;
case 23:
#line	484	"/sys/src/cmd/pc.y"
{ yyval.n = numbin('/', yypt[-2].yyv.n, yypt[-0].yyv.n); } break;
case 24:
#line	485	"/sys/src/cmd/pc.y"
{ yyval.n = numbin('%', yypt[-2].yyv.n, yypt[-0].yyv.n); } break;
case 25:
#line	486	"/sys/src/cmd/pc.y"
{ yyval.n = numbin('&', yypt[-2].yyv.n, yypt[-0].yyv.n); } break;
case 26:
#line	487	"/sys/src/cmd/pc.y"
{ yyval.n = numbin('|', yypt[-2].yyv.n, yypt[-0].yyv.n); } break;
case 27:
#line	488	"/sys/src/cmd/pc.y"
{ yyval.n = numbin('^', yypt[-2].yyv.n, yypt[-0].yyv.n); } break;
case 28:
#line	489	"/sys/src/cmd/pc.y"
{ yyval.n = numbin(LOEXP, yypt[-2].yyv.n, yypt[-0].yyv.n); } break;
case 29:
#line	490	"/sys/src/cmd/pc.y"
{ yyval.n = numbin(LOLSH, yypt[-2].yyv.n, yypt[-0].yyv.n); } break;
case 30:
#line	491	"/sys/src/cmd/pc.y"
{ yyval.n = numbin(LORSH, yypt[-2].yyv.n, yypt[-0].yyv.n); } break;
case 31:
#line	492	"/sys/src/cmd/pc.y"
{ yyval.n = numbin(LOEQ, yypt[-2].yyv.n, yypt[-0].yyv.n); } break;
case 32:
#line	493	"/sys/src/cmd/pc.y"
{ yyval.n = numbin(LONE, yypt[-2].yyv.n, yypt[-0].yyv.n); } break;
case 33:
#line	494	"/sys/src/cmd/pc.y"
{ yyval.n = numbin('<', yypt[-2].yyv.n, yypt[-0].yyv.n); } break;
case 34:
#line	495	"/sys/src/cmd/pc.y"
{ yyval.n = numbin('>', yypt[-2].yyv.n, yypt[-0].yyv.n); } break;
case 35:
#line	496	"/sys/src/cmd/pc.y"
{ yyval.n = numbin(LOLE, yypt[-2].yyv.n, yypt[-0].yyv.n); } break;
case 36:
#line	497	"/sys/src/cmd/pc.y"
{ yyval.n = numbin(LOGE, yypt[-2].yyv.n, yypt[-0].yyv.n); } break;
case 37:
#line	498	"/sys/src/cmd/pc.y"
{ yyval.n = numbin(LOLAND, yypt[-2].yyv.n, yypt[-0].yyv.n); } break;
case 38:
#line	499	"/sys/src/cmd/pc.y"
{ yyval.n = numbin(LOLOR, yypt[-2].yyv.n, yypt[-0].yyv.n); } break;
case 39:
#line	500	"/sys/src/cmd/pc.y"
{ yyval.n = yypt[-0].yyv.n; } break;
case 40:
#line	501	"/sys/src/cmd/pc.y"
{ yyval.n = nummod(yypt[-0].yyv.n); if(yyval.n != nil) mpsub(mpzero, yyval.n, yyval.n); } break;
case 41:
#line	502	"/sys/src/cmd/pc.y"
{ yyval.n = nummod(yypt[-0].yyv.n); if(yyval.n != nil) mpnot(yyval.n, yyval.n); } break;
case 42:
#line	503	"/sys/src/cmd/pc.y"
{ yyval.n = nummod(yypt[-0].yyv.n); if(yyval.n != nil) {itomp(mpcmp(yyval.n, mpzero) == 0, yyval.n); yyval.n->b = 0; } } break;
case 43:
#line	504	"/sys/src/cmd/pc.y"
{
		if(yypt[-4].yyv.n == nil || mpcmp(yypt[-4].yyv.n, mpzero) != 0){
			yyval.n = yypt[-2].yyv.n;
			numdecref(yypt[-0].yyv.n);
		}else{
			yyval.n = yypt[-0].yyv.n;
			numdecref(yypt[-2].yyv.n);
		}
		numdecref(yypt[-4].yyv.n);
	} break;
case 44:
#line	514	"/sys/src/cmd/pc.y"
{ yyval.n = fncall(yypt[-3].yyv.sym, yypt[-1].yyv.args.n, yypt[-1].yyv.args.x); } break;
case 45:
#line	515	"/sys/src/cmd/pc.y"
{
		Num *n;
		yyval.n = nil;
		switch(yypt[-0].yyv.sym->t){
		case SYMVAR: yyval.n = numincref(yypt[-0].yyv.sym->val); break;
		case SYMNONE:
			n = hexfix(yypt[-0].yyv.sym);
			if(n != nil) yyval.n = n;
			else error("%s undefined", yypt[-0].yyv.sym->name);
			break;
		case SYMFUNC: error("%s is a function", yypt[-0].yyv.sym->name); break;
		default: error("%s invalid here", yypt[-0].yyv.sym->name);
		}
	} break;
case 46:
#line	529	"/sys/src/cmd/pc.y"
{
		if(yypt[-2].yyv.sym->t != SYMNONE && yypt[-2].yyv.sym->t != SYMVAR)
			error("%s redefined", yypt[-2].yyv.sym->name);
		else if(!fail){
			yypt[-2].yyv.sym->t = SYMVAR;
			numdecref(yypt[-2].yyv.sym->val);
			yypt[-2].yyv.sym->val = numincref(yypt[-0].yyv.n);
		}
		yyval.n = yypt[-0].yyv.n;
	} break;
case 47:
#line	539	"/sys/src/cmd/pc.y"
{
		yyval.n = lastp;
		if(yyval.n == nil) error("no last result");
		else numincref(yyval.n);
	} break;
case 48:
#line	545	"/sys/src/cmd/pc.y"
{ yyval.args.n = 0; } break;
case 50:
#line	546	"/sys/src/cmd/pc.y"
{ yyval.args.x[0] = yypt[-0].yyv.n; yyval.args.n = 1; } break;
case 51:
#line	547	"/sys/src/cmd/pc.y"
{
		yyval.args = yypt[-2].yyv.args;
		if(yyval.args.n >= MAXARGS)
			error("too many arguments");
		else
			yyval.args.x[yyval.args.n++] = yypt[-0].yyv.n;
	} break;
	}
	goto yystack;  /* stack new state and value */
}
