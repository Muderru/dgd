extern void	 c_init		P((char*, char*, char*, char**, bool));
extern object	*c_compile	P((char*));
extern int	 c_autodriver	P((void));

extern bool  c_inherit		P((char*, node*));
extern void  c_global		P((unsigned short, unsigned short, node*));
extern void  c_function		P((unsigned short, unsigned short, node*));
extern void  c_funcbody		P((node*));
extern void  c_local		P((unsigned short, unsigned short, node*));
extern node *c_concat		P((node*, node*));
extern node *c_list_exp		P((node*));
extern node *c_exp_stmt		P((node*));
extern node *c_quest		P((node*, node*, node*));
extern node *c_if		P((node*, node*, node*));
extern void  c_loop		P((void));
extern node *c_do		P((node*, node*));
extern node *c_while		P((node*, node*));
extern node *c_for		P((node*, node*, node*, node*));
extern void  c_startswitch	P((node*, bool));
extern node *c_endswitch	P((node*, node*));
extern node *c_case		P((node*, node*));
extern node *c_default		P((void));
extern node *c_break		P((void));
extern node *c_continue		P((void));
extern short c_ftype		P((void));
extern short c_vtype		P((int));
extern void  c_startcompound	P((void));
extern node *c_endcompound	P((node*));
extern node *c_flookup		P((node*, bool));
extern node *c_iflookup		P((node*, node*));
extern node *c_variable		P((node*));
extern node *c_funcall		P((node*, node*));
extern node *c_arrow		P((node*, node*, node*));
extern node *c_checklval	P((node*));
extern node *c_tst		P((node*));
extern node *c_not		P((node*));
extern node *c_lvalue		P((node*, char*));
extern char *c_typename		P((unsigned short));
extern unsigned short c_tmatch	P((unsigned short, unsigned short));
