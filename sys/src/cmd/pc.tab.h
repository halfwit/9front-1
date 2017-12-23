#define	LOEXP	57346
#define	LOLSH	57347
#define	LORSH	57348
#define	LOEQ	57349
#define	LONE	57350
#define	LOLE	57351
#define	LOGE	57352
#define	LOLAND	57353
#define	LOLOR	57354

typedef union  {
	Num *n;
	Symbol *sym;
	struct {
		Num *x[MAXARGS];
		int n;
	} args;
}	YYSTYPE;
extern	YYSTYPE	yylval;
#define	LNUM	57355
#define	LSYMB	57356
#define	unary	57357
