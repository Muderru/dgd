/*
 * This file is part of DGD, https://github.com/dworkin/dgd
 * Copyright (C) 1993-2010 Dworkin B.V.
 * Copyright (C) 2010-2018 DGD Authors (see the commit log for details)
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as
 * published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

# include "comp.h"
# include "str.h"
# include "array.h"
# include "object.h"
# include "xfloat.h"
# include "interpret.h"
# include "data.h"
# include "path.h"
# include "macro.h"
# include "token.h"
# include "ppcontrol.h"
# include "node.h"
# include "control.h"
# include "optimize.h"
# include "codegen.h"
# include "compile.h"
# include <stdarg.h>

# define COND_CHUNK	16
# define COND_BMAP	BMAP(MAX_LOCALS)

struct cond : public ChunkAllocated {
    cond *prev;			/* surrounding conditional */
    Uint init[COND_BMAP];	/* initialize variable bitmap */
};

# define BLOCK_CHUNK	16

struct block : public ChunkAllocated {
    int vindex;			/* variable index */
    int nvars;			/* # variables in this block */
    block *prev;		/* surrounding block */
    node *gotos;		/* gotos in this block */
    node *labels;		/* labels in this block */
};

struct var {
    const char *name;		/* variable name */
    short type;			/* variable type */
    short unset;		/* used before set? */
    String *cvstr;		/* class name */
};

static Chunk<cond, COND_CHUNK> cchunk;
static Chunk<block, BLOCK_CHUNK> bchunk;

static cond *thiscond;			/* current condition */
static block *thisblock;		/* current statement block */
static int vindex;			/* variable index */
static int nvars;			/* number of local variables */
static int nparams;			/* number of parameters */
static var variables[MAX_LOCALS];	/* variables */

/*
 * NAME:	cond->new()
 * DESCRIPTION:	create a new condition
 */
static void cond_new(cond *c2)
{
    cond *c;

    c = chunknew (cchunk) cond;
    c->prev = thiscond;
    if (c2 != (cond *) NULL) {
	memcpy(c->init, c2->init, COND_BMAP * sizeof(Uint));
    } else {
	memset(c->init, '\0', COND_BMAP * sizeof(Uint));
    }
    thiscond = c;
}

/*
 * NAME:	cond->del()
 * DESCRIPTION:	delete the current condition
 */
static void cond_del()
{
    cond *c;

    c = thiscond;
    thiscond = c->prev;
    delete c;
}

/*
 * NAME:	cond->match()
 * DESCRIPTION:	match two init bitmaps
 */
static void cond_match(cond *c1, cond *c2, cond *c3)
{
    Uint *p, *q, *r;
    int i;

    p = c1->init;
    q = c2->init;
    r = c3->init;
    for (i = COND_BMAP; i > 0; --i) {
	*p++ = *q++ & *r++;
    }
}

/*
 * NAME:	cond->clear()
 * DESCRIPTION:	clean up conditions
 */
static void cond_clear()
{
    cchunk.clean();
    thiscond = (cond *) NULL;
}

/*
 * NAME:	block->new()
 * DESCRIPTION:	start a new block
 */
static void block_new()
{
    block *b;

    b = chunknew (bchunk) block;
    if (thisblock == (block *) NULL) {
	cond_new((cond *) NULL);
	b->vindex = 0;
	b->nvars = nparams;
    } else {
	b->vindex = vindex;
	b->nvars = 0;
    }
    b->prev = thisblock;
    b->gotos = (node *) NULL;
    b->labels = (node *) NULL;
    thisblock = b;
}

/*
 * NAME:	block->goto()
 * DESCRIPTION:	resolve gotos in this block
 */
static void block_goto(node *g)
{
    block *b;
    node *l;

    for (b = thisblock; b != (block *) NULL; b = b->prev) {
	for (l = b->labels; l != (node *) NULL; l = l->r.right) {
	    if (l->l.string->cmp(g->l.string) == 0) {
		g->mod -= l->mod;
		g->r.right = l;
		return;
	    }
	}
    }

    c_error("unknown label: %s", g->l.string->text);
}

/*
 * NAME:	block->del()
 * DESCRIPTION:	finish the current block
 */
static void block_del(bool keep)
{
    node *g, *r;
    block *f;
    int i;

    for (g = thisblock->gotos; g != (node *) NULL; g = r) {
	r = g->r.right;
	block_goto(g);
    }

    f = thisblock;
    if (keep) {
	for (i = f->vindex; i < f->vindex + f->nvars; i++) {
	    /*
	     * Make sure that variables declared in the closing block can no
	     * longer be used.
	     */
	    variables[i].name = "-";
	}
    } else {
	vindex = f->vindex;
    }
    thisblock = f->prev;
    if (thisblock == (block *) NULL) {
	cond_del();
    }
    delete f;
}

/*
 * NAME:	block->var()
 * DESCRIPTION:	return the index of the local var if found, or -1
 */
static int block_var(char *name)
{
    int i;

    for (i = vindex; i > 0; ) {
	if (strcmp(variables[--i].name, name) == 0) {
	    return i;
	}
    }
    return -1;
}

/*
 * NAME:	block->pdef()
 * DESCRIPTION:	declare a function parameter
 */
static void block_pdef(char *name, short type, String *cvstr)
{
    if (block_var(name) >= 0) {
	c_error("redeclaration of parameter %s", name);
    } else {
	/* "too many parameters" is checked for elsewhere */
	variables[nparams].name = name;
	variables[nparams].type = type;
	variables[nparams].unset = 0;
	variables[nparams++].cvstr = cvstr;
	vindex++;
	nvars++;
    }
}

/*
 * NAME:	block->vdef()
 * DESCRIPTION:	declare a local variable
 */
static void block_vdef(char *name, short type, String *cvstr)
{
    if (block_var(name) >= thisblock->vindex) {
	c_error("redeclaration of local variable %s", name);
    } else if (vindex == MAX_LOCALS) {
	c_error("too many local variables");
    } else {
	BCLR(thiscond->init, vindex);
	thisblock->nvars++;
	variables[vindex].name = name;
	variables[vindex].type = type;
	variables[vindex].unset = 0;
	variables[vindex++].cvstr = cvstr;
	if (vindex > nvars) {
	    nvars++;
	}
    }
}

/*
 * NAME:	block->clear()
 * DESCRIPTION:	clean up blocks
 */
static void block_clear()
{
    bchunk.clean();
    thisblock = (block *) NULL;
    vindex = 0;
    nvars = 0;
    nparams = 0;
}


# define LOOP_CHUNK	16

struct loop : public ChunkAllocated {
    char type;			/* case label type */
    bool brk;			/* seen any breaks? */
    bool cont;			/* seen any continues? */
    bool dflt;			/* seen any default labels? */
    Uint ncase;			/* number of case labels */
    unsigned short nesting;	/* rlimits/catch nesting level */
    node *case_list;		/* previous list of case nodes */
    node *vlist;		/* variable list */
    loop *prev;			/* previous loop or switch */
    loop *env;			/* enclosing loop */
};

static Chunk<loop, LOOP_CHUNK> lchunk;

static unsigned short nesting;	/* current rlimits/catch nesting level */

/*
 * NAME:	loop->new()
 * DESCRIPTION:	create a new loop
 */
static loop *loop_new(loop *prev)
{
    loop *l;

    l = chunknew (lchunk) loop;
    l->brk = FALSE;
    l->cont = FALSE;
    l->nesting = nesting;
    l->prev = prev;
    return l;
}

/*
 * NAME:	loop->del()
 * DESCRIPTION:	delete a loop
 */
static loop *loop_del(loop *l)
{
    loop *f;

    f = l;
    l = l->prev;
    delete f;
    return l;
}

/*
 * NAME:	loop->clear()
 * DESCRIPTION:	delete all loops
 */
static void loop_clear()
{
    lchunk.clean();
}


struct context {
    char *file;				/* file to compile */
    Frame *frame;			/* current interpreter stack frame */
    context *prev;			/* previous context */
};

static context *current;		/* current context */
static char *auto_object;		/* auto object */
static char *driver_object;		/* driver object */
static char *include;			/* standard include file */
static char **paths;			/* include paths */
static bool stricttc;			/* strict typechecking */
static bool typechecking;		/* is current function typechecked? */
static bool seen_decls;			/* seen any declarations yet? */
static short ftype;			/* current function type & class */
static String *fclass;			/* function class string */
static loop *thisloop;			/* current loop */
static loop *switch_list;		/* list of nested switches */
static node *case_list;			/* list of case labels */
extern int nerrors;			/* # of errors during parsing */

/*
 * NAME:	compile->init()
 * DESCRIPTION:	initialize the compiler
 */
void c_init(char *a, char *d, char *i, char **p, int tc)
{
    stricttc = (tc == 2);
    node_init(stricttc);
    opt_init();
    auto_object = a;
    driver_object = d;
    include = i;
    paths = p;
    typechecking = tc;
}

/*
 * NAME:	compile->clear()
 * DESCRIPTION:	clean up the compiler
 */
static void c_clear()
{
    cg_clear();
    loop_clear();
    thisloop = (loop *) NULL;
    switch_list = (loop *) NULL;
    block_clear();
    cond_clear();
    node_clear();
    seen_decls = FALSE;
    nesting = 0;
}

/*
 * NAME:	compile->typechecking()
 * DESCRIPTION:	return the global typechecking flag
 */
bool c_typechecking()
{
    return typechecking;
}

static long ncompiled;		/* # objects compiled */

/*
 * NAME:	compile->inherit()
 * DESCRIPTION:	Inherit an object in the object currently being compiled.
 *		Return TRUE if compilation can continue, or FALSE otherwise.
 */
bool c_inherit(char *file, node *label, int priv)
{
    char buf[STRINGSZ];
    Object *obj;
    Frame *f;
    long ncomp;

    obj = NULL;

    if (strcmp(current->file, auto_object) == 0) {
	c_error("cannot inherit from auto object");
	return FALSE;
    }

    f = current->frame;
    if (strcmp(current->file, driver_object) == 0) {
	/*
	 * the driver object can only inherit the auto object
	 */
	file = path_resolve(buf, file);
	if (strcmp(file, auto_object) != 0) {
	    c_error("illegal inherit from driver object");
	    return FALSE;
	}
	obj = Object::find(file, OACC_READ);
	if (obj == (Object *) NULL) {
	    c_compile(f, file, (Object *) NULL, (String **) NULL, 0, TRUE);
	    return FALSE;
	}
    } else {
	ncomp = ncompiled;

	/* get associated object */
	PUSH_STRVAL(f, String::create(NULL, strlen(current->file) + 1L));
	f->sp->u.string->text[0] = '/';
	strcpy(f->sp->u.string->text + 1, current->file);
	PUSH_STRVAL(f, String::create(file, strlen(file)));
	PUSH_INTVAL(f, priv);

	strncpy(buf, file, STRINGSZ - 1);
	buf[STRINGSZ - 1] = '\0';
	if (call_driver_object(f, "inherit_program", 3)) {
	    if (f->sp->type == T_OBJECT) {
		obj = OBJR(f->sp->oindex);
		f->sp++;
	    } else {
		/* returned value not an object */
		error("Cannot inherit \"%s\"", buf);
	    }

	    if (ncomp != ncompiled) {
		return FALSE;	/* objects compiled inside inherit_program() */
	    }
	} else {
	    /* precompiling */
	    f->sp++;
	    file = path_from(buf, current->file, file);
	    obj = Object::find(file, OACC_READ);
	    if (obj == (Object *) NULL) {
		c_compile(f, file, (Object *) NULL, (String **) NULL, 0, TRUE);
		return FALSE;
	    }
	}
    }

    if (obj->flags & O_DRIVER) {
	/* would mess up too many things */
	c_error("illegal to inherit driver object");
	return FALSE;
    }

    return ctrl_inherit(current->frame, current->file, obj,
			(label == (node *) NULL) ?
			 (String *) NULL : label->l.string,
			priv);
}

extern int yyparse ();

/*
 * NAME:	compile->compile()
 * DESCRIPTION:	compile an LPC file
 */
Object *c_compile(Frame *f, char *file, Object *obj, String **strs,
	int nstr, int iflag)
{
    context c;
    char file_c[STRINGSZ + 2];
    Control *ctrl;

    if (iflag) {
	context *cc;
	int n;

	for (cc = current, n = 0; cc != (context *) NULL; cc = cc->prev, n++) {
	    if (strcmp(file, cc->file) == 0) {
		error("Cycle in inheritance from \"/%s.c\"", current->file);
	    }
	}
	if (n >= 255) {
	    error("Compilation nesting too deep");
	}

	pp_clear();
	ctrl_clear();
	c_clear();
    } else if (current != (context *) NULL) {
	error("Compilation within compilation");
    }

    c.file = file;
    if (strncmp(file, BIPREFIX, BIPREFIXLEN) == 0 ||
	strchr(file, '#') != (char *) NULL) {
	error("Illegal object name \"/%s\"", file);
    }
    strcpy(file_c, file);
    if (strs == (String **) NULL) {
	strcat(file_c, ".c");
    }
    c.frame = f;
    c.prev = current;
    current = &c;
    ncompiled++;

    try {
	ec_push((ec_ftn) NULL);
	for (;;) {
	    if (c_autodriver() != 0) {
		ctrl_init();
	    } else {
		Object *aobj;

		if (Object::find(driver_object, OACC_READ) == (Object *) NULL) {
		    /*
		     * compile the driver object to do pathname translation
		     */
		    current = (context *) NULL;
		    c_compile(f, driver_object, (Object *) NULL,
			      (String **) NULL, 0, FALSE);
		    current = &c;
		}

		aobj = Object::find(auto_object, OACC_READ);
		if (aobj == (Object *) NULL) {
		    /*
		     * compile auto object
		     */
		    aobj = c_compile(f, auto_object, (Object *) NULL,
				     (String **) NULL, 0, TRUE);
		}
		/* inherit auto object */
		if (O_UPGRADING(aobj)) {
		    error("Upgraded auto object while compiling \"/%s\"",
			  file_c);
		}
		ctrl_init();
		ctrl_inherit(c.frame, file, aobj, (String *) NULL, FALSE);
	    }

	    if (strs != (String **) NULL) {
		pp_init(file_c, paths, strs, nstr, 1);
	    } else if (!pp_init(file_c, paths, (String **) NULL, 0, 1)) {
		error("Could not compile \"/%s\"", file_c);
	    }
	    if (!tk_include(include, (String **) NULL, 0)) {
		error("Could not include \"/%s\"", include);
	    }

	    cg_init(c.prev != (context *) NULL);
	    if (yyparse() == 0 && ctrl_chkfuncs()) {
		if (obj != (Object *) NULL) {
		    if (obj->count == 0) {
			error("Object destructed during recompilation");
		    }
		    if (O_UPGRADING(obj)) {
			error("Object recompiled during recompilation");
		    }
		    if (O_INHERITED(obj)) {
			/* inherited */
			error("Object inherited during recompilation");
		    }
		}
		if (!Object::space()) {
		    error("Too many objects");
		}

		/*
		 * successfully compiled
		 */
		break;

	    } else if (nerrors == 0) {
		/* another try */
		pp_clear();
		ctrl_clear();
		c_clear();
	    } else {
		/* compilation failed */
		error("Failed to compile \"/%s\"", file_c);
	    }
	}
	ec_pop();
    } catch (...) {
	pp_clear();
	ctrl_clear();
	c_clear();
	current = c.prev;
	error((char *) NULL);
    }

    pp_clear();
    if (!seen_decls) {
	/*
	 * object with inherit statements only (or nothing at all)
	 */
	ctrl_create();
    }
    ctrl = ctrl_construct();
    ctrl_clear();
    c_clear();
    current = c.prev;

    if (obj == (Object *) NULL) {
	/* new object */
	obj = Object::create(file, ctrl);
	if (strcmp(file, driver_object) == 0) {
	    obj->flags |= O_DRIVER;
	} else if (strcmp(file, auto_object) == 0) {
	    obj->flags |= O_AUTO;
	}
    } else {
	unsigned short *vmap;

	/* recompiled object */
	obj->upgrade(ctrl, f);
	vmap = ctrl_varmap(obj->ctrl, ctrl);
	if (vmap != (unsigned short *) NULL) {
	    d_set_varmap(ctrl, vmap);
	}
    }
    return obj;
}

/*
 * NAME:	compile->autodriver()
 * DESCRIPTION:	indicate if the auto object or driver object is being
 *		compiled
 */
int c_autodriver()
{
    if (strcmp(current->file, auto_object) == 0) {
	return O_AUTO;
    }
    if (strcmp(current->file, driver_object) == 0) {
	return O_DRIVER;
    }
    return 0;
}


/*
 * NAME:	revert_list()
 * DESCRIPTION:	revert a "linked list" of nodes
 */
static node *revert_list(node *n)
{
    node *m;

    if (n != (node *) NULL && n->type == N_PAIR) {
	while ((m=n->l.left)->type == N_PAIR) {
	    /*
	     * ((a, b), c) -> (a, (b, c))
	     */
	    n->l.left = m->r.right;
	    m->r.right = n;
	    n = m;
	}
    }
    return n;
}

/*
 * NAME:	compile->objecttype()
 * DESCRIPTION:	handle an object type
 */
String *c_objecttype(node *n)
{
    char path[STRINGSZ];

    if (c_autodriver() == 0) {
	char *p;
	Frame *f;

	f = current->frame;
	p = tk_filename();
	PUSH_STRVAL(f, String::create(p, strlen(p)));
	PUSH_STRVAL(f, n->l.string);
	call_driver_object(f, "object_type", 2);
	if (f->sp->type != T_STRING) {
	    c_error("invalid object type");
	    p = n->l.string->text;
	} else {
	    p = f->sp->u.string->text;
	}
	path_resolve(path, p);
	i_del_value(f->sp++);
    } else {
	path_resolve(path, n->l.string->text);
    }

    return String::create(path, strlen(path));
}

/*
 * NAME:	compile->decl_func()
 * ACTION:	declare a function
 */
static void c_decl_func(unsigned short sclass, node *type, String *str,
	node *formals, bool function)
{
    char proto[5 + (MAX_LOCALS + 1) * 4];
    char tnbuf[TNBUFSIZE];
    char *p, t;
    int nargs, vargs;
    long l;
    bool typechecked, varargs;

    varargs = FALSE;

    /* check for some errors */
    if ((sclass & (C_PRIVATE | C_NOMASK)) == (C_PRIVATE | C_NOMASK)) {
	c_error("private contradicts nomask");
    }
    if (sclass & C_VARARGS) {
	if (stricttc) {
	    c_error("varargs must be in parameter list");
	}
	sclass &= ~C_VARARGS;
	varargs = TRUE;
    }
    t = type->mod;
    if ((t & T_TYPE) == T_NIL) {
	/* don't typecheck this function */
	typechecked = FALSE;
	t = T_MIXED;
    } else {
	typechecked = TRUE;
	if (t != T_VOID && (t & T_TYPE) == T_VOID) {
	    c_error("invalid type for function %s (%s)", str->text,
		    i_typename(tnbuf, t));
	    t = T_MIXED;
	}
    }

    /* handle function arguments */
    ftype = t;
    fclass = type->sclass;
    p = &PROTO_FTYPE(proto);
    nargs = vargs = 0;

    if (formals != (node *) NULL && (formals->flags & F_ELLIPSIS)) {
	sclass |= C_ELLIPSIS;
    }
    formals = revert_list(formals);
    for (;;) {
	*p++ = t;
	if ((t & T_TYPE) == T_CLASS) {
	    l = ctrl_dstring(type->sclass);
	    *p++ = l >> 16;
	    *p++ = l >> 8;
	    *p++ = l;
	}
	if (formals == (node *) NULL) {
	    break;
	}
	if (nargs == MAX_LOCALS) {
	    c_error("too many parameters in function %s", str->text);
	    break;
	}

	if (formals->type == N_PAIR) {
	    type = formals->l.left;
	    formals = formals->r.right;
	} else {
	    type = formals;
	    formals = (node *) NULL;
	}
	t = type->mod;
	if ((t & T_TYPE) == T_NIL) {
	    if (typechecked) {
		c_error("missing type for parameter %s", type->l.string->text);
	    }
	    t = T_MIXED;
	} else if ((t & T_TYPE) == T_VOID) {
	    c_error("invalid type for parameter %s (%s)", type->l.string->text,
		    i_typename(tnbuf, t));
	    t = T_MIXED;
	} else if (typechecked && t != T_MIXED) {
	    /* only bother to typecheck functions with non-mixed arguments */
	    sclass |= C_TYPECHECKED;
	}
	if (type->flags & F_VARARGS) {
	    if (varargs) {
		c_error("extra varargs for parameter %s", type->l.string->text);
	    }
	    varargs = TRUE;
	}
	if (formals == (node *) NULL && (sclass & C_ELLIPSIS)) {
	    /* ... */
	    varargs = TRUE;
	    if (((t + (1 << REFSHIFT)) & T_REF) == 0) {
		c_error("too deep indirection for parameter %s",
			type->l.string->text);
	    }
	    if (function) {
		block_pdef(type->l.string->text, t + (1 << REFSHIFT),
			   type->sclass);
	    }
	} else if (function) {
	    block_pdef(type->l.string->text, t, type->sclass);
	}

	if (!varargs) {
	    nargs++;
	} else {
	    vargs++;
	}
    }

    PROTO_CLASS(proto) = sclass;
    PROTO_NARGS(proto) = nargs;
    PROTO_VARGS(proto) = vargs;
    nargs = p - proto;
    PROTO_HSIZE(proto) = nargs >> 8;
    PROTO_LSIZE(proto) = nargs;

    /* define prototype */
    if (function) {
	ctrl_dfunc(str, proto, fclass);
    } else {
	PROTO_CLASS(proto) |= C_UNDEFINED;
	ctrl_dproto(str, proto, fclass);
    }
}

/*
 * NAME:	compile->decl_var()
 * DESCRIPTION:	declare a variable
 */
static void c_decl_var(unsigned short sclass, node *type, String *str,
	bool global)
{
    char tnbuf[TNBUFSIZE];

    if ((type->mod & T_TYPE) == T_VOID) {
	c_error("invalid type for variable %s (%s)", str->text,
		i_typename(tnbuf, type->mod));
	type->mod = T_MIXED;
    }
    if (global) {
	if (sclass & (C_ATOMIC | C_NOMASK | C_VARARGS)) {
	    c_error("invalid class for variable %s", str->text);
	}
	ctrl_dvar(str, sclass, type->mod, type->sclass);
    } else {
	if (sclass != 0) {
	    c_error("invalid class for variable %s", str->text);
	}
	block_vdef(str->text, type->mod, type->sclass);
    }
}

/*
 * NAME:	compile->decl_list()
 * DESCRIPTION:	handle a list of declarations
 */
static void c_decl_list(unsigned short sclass, node *type, node *list,
	bool global)
{
    node *n;

    list = revert_list(list);	/* for proper order of err mesgs */
    while (list != (node *) NULL) {
	if (list->type == N_PAIR) {
	    n = list->l.left;
	    list = list->r.right;
	} else {
	    n = list;
	    list = (node *) NULL;
	}
	type->mod = (type->mod & T_TYPE) | n->mod;
	if (n->type == N_FUNC) {
	    c_decl_func(sclass, type, n->l.left->l.string, n->r.right, FALSE);
	} else {
	    c_decl_var(sclass, type, n->l.string, global);
	}
    }
}

/*
 * NAME:	compile->global()
 * DESCRIPTION:	handle a global declaration
 */
void c_global(unsigned int sclass, node *type, node *n)
{
    if (!seen_decls) {
	ctrl_create();
	seen_decls = TRUE;
    }
    c_decl_list(sclass, type, n, TRUE);
}

static String *fname;		/* name of current function */
static unsigned short fline;	/* first line of function */

/*
 * NAME:	compile->function()
 * DESCRIPTION:	create a function
 */
void c_function(unsigned int sclass, node *type, node *n)
{
    if (!seen_decls) {
	ctrl_create();
	seen_decls = TRUE;
    }
    type->mod |= n->mod;
    c_decl_func(sclass, type, fname = n->l.left->l.string, n->r.right, TRUE);
}

/*
 * NAME:	compile->funcbody()
 * DESCRIPTION:	create a function body
 */
void c_funcbody(node *n)
{
    char *prog;
    Uint depth;
    unsigned short size;
    Float flt;

    flt.initZero();
    switch (ftype) {
    case T_INT:
	n = c_concat(n, node_mon(N_RETURN, 0, node_int((Int) 0)));
	break;

    case T_FLOAT:
	n = c_concat(n, node_mon(N_RETURN, 0, node_float(&flt)));
	break;

    default:
	n = c_concat(n, node_mon(N_RETURN, 0, node_nil()));
	break;
    }

    n = opt_stmt(n, &depth);
    if (depth > 0x7fff) {
	c_error("function uses too much stack space");
    } else {
	prog = cg_function(fname, n, nvars, nparams, (unsigned short) depth,
			   &size);
	ctrl_dprogram(prog, size);
    }
    node_clear();
    vindex = 0;
    nvars = 0;
    nparams = 0;
}

/*
 * NAME:	compile->local()
 * DESCRIPTION:	handle local declarations
 */
void c_local(unsigned int sclass, node *type, node *n)
{
    c_decl_list(sclass, type, n, FALSE);
}


/*
 * NAME:	compile->startcond()
 * DESCRIPTION:	start a condition
 */
void c_startcond()
{
    cond_new(thiscond);
}

/*
 * NAME:	compile->startcond2()
 * DESCRIPTION:	start a second condition
 */
void c_startcond2()
{
    cond_new(thiscond->prev);
}

/*
 * NAME:	compile->endcond()
 * DESCRIPTION:	end a condition
 */
void c_endcond()
{
    cond_del();
}

/*
 * NAME:	compile->matchcond()
 * DESCRIPTION:	match and end two conditions
 */
void c_matchcond()
{
    cond_match(thiscond->prev->prev, thiscond->prev, thiscond);
    cond_del();
    cond_del();
}

/*
 * NAME:	compile->nil()
 * DESCRIPTION:	check if an expression has the value nil
 */
bool c_nil(node *n)
{
    if (n->type == N_COMMA) {
	/* the parser always generates comma expressions as (a, b), c */
	n = n->r.right;
    }
    return (n->type == nil_node && n->l.number == 0);
}

/*
 * NAME:	compile->concat()
 * DESCRIPTION:	concatenate two statements
 */
node *c_concat(node *n1, node *n2)
{
    node *n;

    if (n1 == (node *) NULL) {
	return n2;
    } else if (n2 == (node *) NULL ||
	       ((n1->flags & F_END) && !(n2->flags & F_REACH))) {
	return n1;
    }

    n = node_bin(N_PAIR, 0, n1, n2);
    n->flags |= (n1->flags & (F_ENTRY | F_REACH)) |
		(n2->flags & (F_REACH | F_END));
    return n;
}

/*
 * NAME:	compile->exp_stmt()
 * DESCRIPTION:	reduce an expression to a statement
 */
node *c_exp_stmt(node *n)
{
    if (n != (node *) NULL) {
	return node_mon(N_POP, 0, n);
    }
    return n;
}

/*
 * NAME:	compile->if()
 * DESCRIPTION:	handle an if statement
 */
node *c_if(node *n1, node *n2)
{
    return node_bin(N_IF, 0, n1, node_mon(N_ELSE, 0, n2));
}

/*
 * NAME:	compile->endif()
 * DESCRIPTION:	end an if statement
 */
node *c_endif(node *n1, node *n3)
{
    node *n2;
    int flags1, flags2;

    n2 = n1->r.right->l.left;
    n1->r.right->r.right = n3;
    if (n2 != (node *) NULL) {
	flags1 = n2->flags & F_END;
	n1->flags |= n2->flags & F_REACH;
    } else {
	flags1 = 0;
    }
    if (n3 != (node *) NULL) {
	flags2 = n3->flags & F_END;
	n1->flags |= n3->flags & F_REACH;
    } else {
	flags2 = 0;
    }

    if (flags1 != 0 && flags2 != 0) {
	n1->flags |= flags1 | flags2;
    }
    return n1;
}

/*
 * NAME:	compile->block()
 * DESCRIPTION:	create a scope block for break or continue
 */
static node *c_block(node *n, int type, int flags)
{
    n = node_mon(N_BLOCK, type, n);
    n->flags |= n->l.left->flags & F_FLOW & ~F_EXIT & ~flags;
    return n;
}

/*
 * NAME:	compile->loop()
 * DESCRIPTION:	start a loop
 */
void c_loop()
{
    thisloop = loop_new(thisloop);
}

/*
 * NAME:	compile->reloop()
 * DESCRIPTION:	loop back a loop
 */
static node *c_reloop(node *n)
{
    return (thisloop->cont) ? c_block(n, N_CONTINUE, F_END) : n;
}

/*
 * NAME:	compile->endloop()
 * DESCRIPTION:	end a loop
 */
static node *c_endloop(node *n)
{
    if (thisloop->brk) {
	n = c_block(n, N_BREAK, F_BREAK);
    }
    thisloop = loop_del(thisloop);
    return n;
}

/*
 * NAME:	compile->do()
 * DESCRIPTION:	end a do-while loop
 */
node *c_do(node *n1, node *n2)
{
    n1 = node_bin(N_DO, 0, n1, n2 = c_reloop(n2));
    if (n2 != (node *) NULL) {
	n1->flags |= n2->flags & F_FLOW;
    }
    return c_endloop(n1);
}

/*
 * NAME:	compile->while()
 * DESCRIPTION:	end a while loop
 */
node *c_while(node *n1, node *n2)
{
    n1 = node_bin(N_FOR, 0, n1, n2 = c_reloop(n2));
    if (n2 != (node *) NULL) {
	n1->flags |= n2->flags & F_FLOW & ~(F_ENTRY | F_EXIT);
    }
    return c_endloop(n1);
}

/*
 * NAME:	compile->for()
 * DESCRIPTION:	end a for loop
 */
node *c_for(node *n1, node *n2, node *n3, node *n4)
{
    n4 = c_reloop(n4);
    n2 = node_bin((n2 == (node *) NULL) ? N_FOREVER : N_FOR,
		  0, n2, c_concat(n4, n3));
    if (n4 != (node *) NULL) {
	n2->flags = n4->flags & F_FLOW & ~(F_ENTRY | F_EXIT);
    }

    return c_concat(n1, c_endloop(n2));
}

/*
 * NAME:	compile->startrlimits()
 * DESCRIPTION:	begin rlimit handling
 */
void c_startrlimits()
{
    nesting++;
}

/*
 * NAME:	compile->endrlimits()
 * DESCRIPTION:	handle statements with resource limitations
 */
node *c_endrlimits(node *n1, node *n2, node *n3)
{
    --nesting;

    if (strcmp(current->file, driver_object) == 0 ||
	strcmp(current->file, auto_object) == 0) {
	n1 = node_bin(N_RLIMITS, 1, node_bin(N_PAIR, 0, n1, n2), n3);
    } else {
	Frame *f;

	f = current->frame;
	PUSH_STRVAL(f, String::create((char *) NULL,
				      strlen(current->file) + 1));
	f->sp->u.string->text[0] = '/';
	strcpy(f->sp->u.string->text + 1, current->file);
	call_driver_object(f, "compile_rlimits", 1);
	n1 = node_bin(N_RLIMITS, VAL_TRUE(f->sp), node_bin(N_PAIR, 0, n1, n2),
		      n3);
	i_del_value(f->sp++);
    }

    if (n3 != (node *) NULL) {
	n1->flags |= n3->flags & F_END;
    }
    return n1;
}

/*
 * NAME:	compile->startcatch()
 * DESCRIPTION:	begin catch handling
 */
void c_startcatch()
{
    nesting++;
}

/*
 * NAME:	compile->endcatch()
 * DESCRIPTION:	end catch handling
 */
void c_endcatch()
{
    --nesting;
}

/*
 * NAME:	compile->donecatch()
 * DESCRIPTION:	handle statements within catch
 */
node *c_donecatch(node *n1, node *n2)
{
    node *n;
    int flags1, flags2;

    n = node_bin(N_CATCH, 0, n1, n2);
    if (n1 != (node *) NULL) {
	flags1 = n1->flags & F_END;
    } else {
	flags1 = 0;
    }
    if (n2 != (node *) NULL) {
	n->flags |= n2->flags & F_REACH;
	flags2 = n2->flags & F_END;
    } else {
	flags2 = 0;
    }

    if (flags1 != 0 && flags2 != 0) {
	n->flags |= flags1 | flags2;
    }
    return n;
}

/*
 * NAME:	compile->startswitch()
 * DESCRIPTION:	start a switch statement
 */
void c_startswitch(node *n, int typechecked)
{
    char tnbuf[TNBUFSIZE];

    switch_list = loop_new(switch_list);
    switch_list->type = T_MIXED;
    if (typechecked &&
	n->mod != T_INT && n->mod != T_STRING && n->mod != T_MIXED) {
	c_error("bad switch expression type (%s)", i_typename(tnbuf, n->mod));
	switch_list->type = T_NIL;
    }
    switch_list->dflt = FALSE;
    switch_list->ncase = 0;
    switch_list->case_list = case_list;
    switch_list->vlist = (node *) NULL;
    case_list = (node *) NULL;
    switch_list->env = thisloop;
}

static int cmp (cvoid*, cvoid*);

/*
 * NAME:	cmp()
 * DESCRIPTION:	compare two case label nodes
 */
static int cmp(cvoid *cv1, cvoid *cv2)
{
    node **n1, **n2;

    n1 = (node **) cv1;
    n2 = (node **) cv2;
    if (n1[0]->l.left->type == N_STR) {
	if (n2[0]->l.left->type == N_STR) {
	    return n1[0]->l.left->l.string->cmp(n2[0]->l.left->l.string);
	} else {
	    return 1;	/* str > nil */
	}
    } else if (n2[0]->l.left->type == N_STR) {
	return -1;	/* nil < str */
    } else {
	return (n1[0]->l.left->l.number <= n2[0]->l.left->l.number) ? -1 : 1;
    }
}

/*
 * NAME:	compile->endswitch()
 * DESCRIPTION:	end a switch
 */
node *c_endswitch(node *expr, node *stmt)
{
    char tnbuf[TNBUFSIZE];
    node **v, **w, *n;
    unsigned short i, size;
    long l;
    unsigned long cnt;
    short type, sz;

    n = stmt;
    if (n != (node *) NULL) {
	n->r.right = switch_list->vlist;
	if (switch_list->prev != (loop *) NULL) {
	    switch_list->prev->vlist = c_concat(n->r.right,
						switch_list->prev->vlist);
	}
    }

    if (switch_list->type != T_NIL) {
	if (stmt == (node *) NULL) {
	    /* empty switch statement */
	    n = c_exp_stmt(expr);
	} else if (!(stmt->flags & F_ENTRY)) {
	    c_error("unreachable code in switch");
	} else if (switch_list->ncase > 0x7fff) {
	    c_error("too many cases in switch");
	} else if ((size=switch_list->ncase - switch_list->dflt) == 0) {
	    if (switch_list->ncase == 0) {
		/* can happen when recovering from syntax error */
		n = c_exp_stmt(expr);
	    } else {
		/* only a default label: erase N_CASE */
		n = case_list->r.right->r.right->l.left;
		*(case_list->r.right->r.right) = *n;
		n->type = N_FAKE;
		if (switch_list->brk) {
		    /*
		     * enclose the break statement with a proper block
		     */
		    stmt = c_concat(stmt, node_mon(N_BREAK, 0, (node *) NULL));
		    stmt = node_bin(N_FOREVER, 0, (node *) NULL, stmt);
		    stmt->flags |= stmt->r.right->flags & F_FLOW;
		    stmt = c_block(stmt, N_BREAK, F_BREAK);
		}
		n = c_concat(c_exp_stmt(expr), stmt);
	    }
	} else if (expr->mod != T_MIXED && expr->mod != switch_list->type &&
		   switch_list->type != T_MIXED) {
	    c_error("wrong switch expression type (%s)",
		    i_typename(tnbuf, expr->mod));
	} else {
	    /*
	     * get the labels in an array, and sort them
	     */
	    v = ALLOCA(node*, size);
	    for (i = size, n = case_list; i > 0; n = n->l.left) {
		if (n->r.right->l.left != (node *) NULL) {
		    *v++ = n->r.right;
		    --i;
		}
	    }
	    qsort(v -= size, size, sizeof(node *), cmp);

	    if (switch_list->type == T_STRING) {
		type = N_SWITCH_STR;
		if (size >= 2) {
		    /*
		     * check for duplicate cases
		     */
		    if (v[1]->l.left->type == nil_node) {
			c_error("duplicate case labels in switch");
		    } else {
			i = (v[0]->l.left->type == nil_node);
			for (w = v + i, i = size - i - 1; i > 0; w++, --i) {
			    if (w[0]->l.left->l.string->cmp(w[1]->l.left->l.string) == 0) {
				c_error("duplicate case labels in switch");
				break;
			    }
			}
		    }
		}
		sz = 0;
	    } else {
		type = N_SWITCH_INT;
		/*
		 * check for duplicate cases
		 */
		i = size;
		cnt = 0;
		w = v;
		for (;;) {
		    cnt += w[0]->l.left->r.number - w[0]->l.left->l.number + 1;
		    if (--i == 0) {
			break;
		    }
		    if (w[0]->l.left->r.number >= w[1]->l.left->l.number) {
			if (w[0]->l.left->l.number == w[1]->l.left->r.number) {
			    c_error("duplicate case labels in switch");
			} else {
			    c_error("overlapping case label ranges in switch");
			}
			break;
		    }
		    w++;
		}

		/* determine the number of bytes per case */
		l = v[0]->l.left->l.number;
		if (l < 0) {
		    l = -1 - l;
		}
		if (l < w[0]->l.left->r.number) {
		    l = w[0]->l.left->r.number;
		}
		if (l <= 127) {
		    sz = 1;
		} else if (l <= 32767) {
		    sz = 2;
		} else if (l <= 8388607L) {
		    sz = 3;
		} else {
		    sz = 4;
		}

		if (i == 0 && cnt > size) {
		    if (cnt > 0xffffffffL / 6 ||
			(sz + 2L) * cnt > (2 * sz + 2L) * size) {
			/*
			 * no point in changing the type of switch
			 */
			type = N_SWITCH_RANGE;
		    } else {
			/*
			 * convert range label switch to int label switch
			 * by adding new labels
			 */
			w = ALLOCA(node*, cnt);
			for (i = size; i > 0; --i) {
			    *w++ = *v;
			    for (l = v[0]->l.left->l.number;
				 l < v[0]->l.left->r.number; ) {
				/* insert N_CASE in statement */
				n = node_mon(N_CASE, 0, v[0]->r.right->l.left);
				v[0]->r.right->l.left = n;
				l++;
				*w++ = node_bin(N_PAIR, 0, node_int((Int)l), n);
			    }
			    v++;
			}
			AFREE(v - size);
			size = cnt;
			v = w - size;
		    }
		}
	    }

	    /*
	     * turn array into linked list
	     */
	    v += size;
	    n = (node *) NULL;
	    i = size;
	    do {
		(*--v)->r.right->mod = i;
		n = node_bin(N_PAIR, 0, v[0]->l.left, n);
	    } while (--i > 0);
	    AFREE(v);
	    if (switch_list->dflt) {
		/* add default case */
		n = node_bin(N_PAIR, 0, (node *) NULL, n);
		size++;
	    }

	    if (switch_list->brk) {
		stmt = c_block(stmt, N_BREAK, F_BREAK);
	    }
	    n = node_bin(type, size, n, node_bin(N_PAIR, sz, expr, stmt));
	}
    }

    case_list = switch_list->case_list;
    switch_list = loop_del(switch_list);
    if (switch_list == (loop *) NULL) {
	vindex = thisblock->vindex + thisblock->nvars;
    }

    return n;
}

/*
 * NAME:	compile->case()
 * DESCRIPTION:	handle a case label
 */
node *c_case(node *n1, node *n2)
{
    if (switch_list == (loop *) NULL) {
	c_error("case label not inside switch");
	return (node *) NULL;
    }
    if (switch_list->nesting != nesting) {
	c_error("illegal jump into rlimits or catch");
	return (node *) NULL;
    }
    if (switch_list->type == T_NIL) {
	return (node *) NULL;
    }

    if (n1->type == N_STR || n1->type == N_NIL) {
	/* string */
	if (n2 != (node *) NULL) {
	    c_error("bad case range");
	    switch_list->type = T_NIL;
	    return (node *) NULL;
	}
	/* compare type with other cases */
	if (switch_list->type == T_MIXED) {
	    switch_list->type = T_STRING;
	} else if (switch_list->type != T_STRING) {
	    c_error("multiple case types in switch");
	    switch_list->type = T_NIL;
	    return (node *) NULL;
	}
    } else {
	/* int */
	if (n1->type != N_INT) {
	    c_error("bad case expression");
	    switch_list->type = T_NIL;
	    return (node *) NULL;
	}
	if (n2 == (node *) NULL) {
	    n1->r.number = n1->l.number;
	} else {
	    /* range */
	    if (n2->type != N_INT) {
		c_error("bad case range");
		switch_list->type = T_NIL;
		return (node *) NULL;
	    }
	    if (n2->l.number < n1->l.number) {
		/* inverted range */
		n1->r.number = n1->l.number;
		n1->l.number = n2->l.number;
		n1->type = N_RANGE;
	    } else {
		n1->r.number = n2->l.number;
		if (n1->l.number != n1->r.number) {
		    n1->type = N_RANGE;
		}
	    }
	}
	/* compare type with other cases */
	if (n1->l.number != 0 || n2 != (node *) NULL || nil_type != T_INT) {
	    if (switch_list->type == T_MIXED) {
		switch_list->type = T_INT;
	    } else if (switch_list->type != T_INT) {
		c_error("multiple case types in switch");
		switch_list->type = T_NIL;
		return (node *) NULL;
	    }
	}
    }

    switch_list->ncase++;
    n2 = node_mon(N_CASE, 0, (node *) NULL);
    n2->flags |= F_ENTRY | F_CASE;
    case_list = node_bin(N_PAIR, 0, case_list, node_bin(N_PAIR, 0, n1, n2));
    return n2;
}

/*
 * NAME:	compile->default()
 * DESCRIPTION:	handle a default label
 */
node *c_default()
{
    node *n;

    n = (node *) NULL;
    if (switch_list == (loop *) NULL) {
	c_error("default label not inside switch");
    } else if (switch_list->dflt) {
	c_error("duplicate default label in switch");
	switch_list->type = T_NIL;
    } else if (switch_list->nesting != nesting) {
	c_error("illegal jump into rlimits or catch");
    } else {
	switch_list->ncase++;
	switch_list->dflt = TRUE;
	n = node_mon(N_CASE, 0, (node *) NULL);
	n->flags |= F_ENTRY | F_CASE;
	case_list = node_bin(N_PAIR, 0, case_list,
			     node_bin(N_PAIR, 0, (node *) NULL, n));
    }

    return n;
}

/*
 * NAME:	compile->label()
 * DESCRIPTION:	add a label
 */
node *c_label(node *n)
{
    block *b;
    node *l;

    for (b = thisblock; b != (block *) NULL; b = b->prev) {
	for (l = b->labels; l != (node *) NULL; l = l->r.right) {
	    if (n->l.string->cmp(l->l.string) == 0) {
		c_error("redeclaration of label: %s", n->l.string->text);
		return NULL;
	    }
	}
    }

    n->r.right = thisblock->labels;
    thisblock->labels = n;
    n->type = N_LABEL;
    n->mod = nesting;
    n->flags = F_ENTRY | F_LABEL;
    return n;
}

/*
 * NAME:	compile->goto()
 * DESCRIPTION:	handle goto
 */
node *c_goto(node *n)
{
    n->r.right = thisblock->gotos;
    thisblock->gotos = n;
    n->type = N_GOTO;
    n->mod = nesting;
    n->flags = F_EXIT;
    return n;
}

/*
 * NAME:	compile->break()
 * DESCRIPTION:	handle a break statement
 */
node *c_break()
{
    loop *l;
    node *n;

    l = switch_list;
    if (l == (loop *) NULL || switch_list->env != thisloop) {
	/* no switch, or loop inside switch */
	l = thisloop;
    }
    if (l == (loop *) NULL) {
	c_error("break statement not inside loop or switch");
	return (node *) NULL;
    }
    l->brk = TRUE;

    n = node_mon(N_BREAK, nesting - l->nesting, (node *) NULL);
    n->flags |= F_BREAK;
    return n;
}

/*
 * NAME:	compile->continue()
 * DESCRIPTION:	handle a continue statement
 */
node *c_continue()
{
    node *n;

    if (thisloop == (loop *) NULL) {
	c_error("continue statement not inside loop");
	return (node *) NULL;
    }
    thisloop->cont = TRUE;

    n = node_mon(N_CONTINUE, nesting - thisloop->nesting, (node *) NULL);
    n->flags |= F_CONTINUE;
    return n;
}

/*
 * NAME:	compile->return()
 * DESCRIPTION:	handle a return statement
 */
node *c_return(node *n, int typechecked)
{
    char tnbuf1[TNBUFSIZE], tnbuf2[TNBUFSIZE];

    if (n == (node *) NULL) {
	if (typechecked && ftype != T_VOID) {
	    c_error("function must return value");
	}
	n = node_nil();
    } else if (typechecked) {
	if (ftype == T_VOID) {
	    /*
	     * can't return anything from a void function
	     */
	    c_error("value returned from void function");
	} else if ((!c_nil(n) || !T_POINTER(ftype)) &&
		   c_tmatch(n->mod, ftype) == T_NIL) {
	    /*
	     * type error
	     */
	    c_error("returned value doesn't match %s (%s)",
		    i_typename(tnbuf1, ftype), i_typename(tnbuf2, n->mod));
	} else if ((ftype != T_MIXED && n->mod == T_MIXED) ||
		   (ftype == T_CLASS &&
		    (n->mod != T_CLASS || fclass->cmp(n->sclass) != 0))) {
	    /*
	     * typecheck at runtime
	     */
	    n = node_mon(N_CAST, ftype, n);
	    n->sclass = fclass;
	    if (fclass != (String *) NULL) {
		fclass->ref();
	    }
	}
    }

    n = node_mon(N_RETURN, nesting, n);
    n->flags |= F_EXIT;
    return n;
}

/*
 * NAME:	compile->startcompound()
 * DESCRIPTION:	start a compound statement
 */
void c_startcompound()
{
    if (thisblock == (block *) NULL) {
	fline = tk_line();
    }
    block_new();
}

/*
 * NAME:	compile->endcompound()
 * DESCRIPTION:	end a compound statement
 */
node *c_endcompound(node *n)
{
    int flags;

    if (n != (node *) NULL) {
      flags = n->flags & (F_REACH | F_END);
      if (n->type == N_PAIR) {
	  n = revert_list(n);
	  n->flags = (n->flags & ~F_END) | flags;
      }
      n = node_mon(N_COMPOUND, 0, n);
      n->flags = n->l.left->flags & ~F_LABEL;

      if (thisblock->nvars != 0) {
	  node *v, *l, *z, *f, *p;
	  int i;

	  /*
	   * create variable type definitions and implicit initializers
	   */
	  l = z = f = p = (node *) NULL;
	  i = thisblock->vindex;
	  if (i < nparams) {
	      i = nparams;
	  }
	  while (i < thisblock->vindex + thisblock->nvars) {
	      l = c_concat(node_var(variables[i].type, i), l);

	      if (switch_list != (loop *) NULL || (flags & F_LABEL) ||
		  variables[i].unset) {
		  switch (variables[i].type) {
		  case T_INT:
		      v = node_mon(N_LOCAL, T_INT, (node *) NULL);
		      v->line = 0;
		      v->r.number = i;
		      if (z == (node *) NULL) {
			  z = node_int(0);
			  z->line = 0;
		      }
		      z = node_bin(N_ASSIGN, T_INT, v, z);
		      z->line = 0;
		      break;

		  case T_FLOAT:
		      v = node_mon(N_LOCAL, T_FLOAT, (node *) NULL);
		      v->line = 0;
		      v->r.number = i;
		      if (f == (node *) NULL) {
			  Float flt;

			  flt.initZero();
			  f = node_float(&flt);
			  f->line = 0;
		      }
		      f = node_bin(N_ASSIGN, T_FLOAT, v, f);
		      f->line = 0;
		      break;

		  default:
		      v = node_mon(N_LOCAL, T_MIXED, (node *) NULL);
		      v->line = 0;
		      v->r.number = i;
		      if (p == (node *) NULL) {
			  p = node_nil();
			  p->line = 0;
		      }
		      p = node_bin(N_ASSIGN, T_MIXED, v, p);
		      p->line = 0;
		      break;
		  }
	      }
	      i++;
	  }

	  /* add vartypes and initializers to compound statement */
	  if (z != (node *) NULL) {
	      l = c_concat(c_exp_stmt(z), l);
	  }
	  if (f != (node *) NULL) {
	      l = c_concat(c_exp_stmt(f), l);
	  }
	  if (p != (node *) NULL) {
	      l = c_concat(c_exp_stmt(p), l);
	  }
	  n->r.right = l;
	  if (switch_list != (loop *) NULL) {
	      switch_list->vlist = c_concat(l, switch_list->vlist);
	  }
      }
    }

    block_del(switch_list != (loop *) NULL);
    return n;
}

/*
 * NAME:	compile->flookup()
 * DESCRIPTION:	look up a local function, inherited function or kfun
 */
node *c_flookup(node *n, int typechecked)
{
    char *proto;
    String *sclass;
    long call;

    proto = ctrl_fcall(n->l.string, &sclass, &call, typechecked);
    n->r.right = (proto == (char *) NULL) ? (node *) NULL :
		  node_fcall(PROTO_FTYPE(proto), sclass, proto, (Int) call);
    return n;
}

/*
 * NAME:	compile->iflookup()
 * DESCRIPTION:	look up an inherited function
 */
node *c_iflookup(node *n, node *label)
{
    char *proto;
    String *sclass;
    long call;

    proto = ctrl_ifcall(n->l.string, (label != (node *) NULL) ?
				     label->l.string->text : (char *) NULL,
			&sclass, &call);
    n->r.right = (proto == (char *) NULL) ? (node *) NULL :
		  node_fcall(PROTO_FTYPE(proto), sclass, proto, (Int) call);
    return n;
}

/*
 * NAME:	compile->aggregate()
 * DESCRIPTION:	create an aggregate
 */
node *c_aggregate(node *n, unsigned int type)
{
    return node_mon(N_AGGR, type, revert_list(n));
}

/*
 * NAME:	compile->local_var()
 * DESCRIPTION:	create a reference to a local variable
 */
node *c_local_var(node *n)
{
    int i;

    i = block_var(n->l.string->text);
    if (i < 0) {
	return (node *) NULL;
    }

    if (!BTST(thiscond->init, i)) {
	variables[i].unset++;
    }
    n = node_mon(N_LOCAL, variables[i].type, n);
    n->sclass = variables[i].cvstr;
    if (n->sclass != (String *) NULL) {
	n->sclass->ref();
    }
    n->r.number = i;
    return n;
}

/*
 * NAME:	compile->global_var()
 * DESCRIPTION:	create a reference to a global variable
 */
node *c_global_var(node *n)
{
    String *sclass;
    long ref;

    n = node_mon(N_GLOBAL, ctrl_var(n->l.string, &ref, &sclass), n);
    n->sclass = sclass;
    if (sclass != (String *) NULL) {
	sclass->ref();
    }
    n->r.number = ref;

    return n;
}

/*
 * NAME:	compile->vtype()
 * DESCRIPTION:	return the type of a variable
 */
short c_vtype(int i)
{
    return variables[i].type;
}

/*
 * NAME:	lvalue()
 * DESCRIPTION:	check if a value can be an lvalue
 */
static bool lvalue(node *n)
{
    if (n->type == N_CAST && n->mod == n->l.left->mod) {
	/* only an implicit cast is allowed */
	n = n->l.left;
    }
    switch (n->type) {
    case N_LOCAL:
    case N_GLOBAL:
    case N_INDEX:
    case N_FAKE:
	return TRUE;

    default:
	return FALSE;
    }
}

/*
 * NAME:	funcall()
 * DESCRIPTION:	handle a function call
 */
static node *funcall(node *call, node *args, int funcptr)
{
    char tnbuf[TNBUFSIZE];
    int n, nargs, t;
    node *func, **argv, **arg;
    char *argp, *proto, *fname;
    bool typechecked, ellipsis;
    int spread;

    /* get info, prepare return value */
    fname = call->l.string->text;
    func = call->r.right;
    if (func == (node *) NULL) {
	/* error during function lookup */
	return node_mon(N_FAKE, T_MIXED, (node *) NULL);
    }
    proto = func->l.ptr;
    if (func->mod == T_IMPLICIT) {
	func->mod = T_MIXED;
    }
    func->l.left = call;
    call->r.right = args;
    argv = &call->r.right;

# ifdef CLOSURES
    if (funcptr) {
	if (func->r.number >> 24 == KFCALL) {
	    c_error("cannot create pointer to kfun");
	}
	if (PROTO_CLASS(proto) & C_PRIVATE) {
	    c_error("cannot create pointer to private function");
	}
    }
# endif

    /*
     * check function arguments
     */
    typechecked = ((PROTO_CLASS(proto) & C_TYPECHECKED) != 0);
    ellipsis = ((PROTO_CLASS(proto) & C_ELLIPSIS) != 0);
    nargs = PROTO_NARGS(proto) + PROTO_VARGS(proto);
    argp = PROTO_ARGS(proto);
    for (n = 1; n <= nargs; n++) {
	if (args == (node *) NULL) {
	    if (n <= PROTO_NARGS(proto) && !funcptr) {
		c_error("too few arguments for function %s", fname);
	    }
	    break;
	}
	if ((*argv)->type == N_PAIR) {
	    arg = &(*argv)->l.left;
	    argv = &(*argv)->r.right;
	} else {
	    arg = argv;
	    args = (node *) NULL;
	}
	t = UCHAR(*argp);

	if ((*arg)->type == N_SPREAD) {
	    t = (*arg)->l.left->mod;
	    if (t != T_MIXED) {
		if ((t & T_REF) == 0) {
		    c_error("ellipsis requires array");
		    t = T_MIXED;
		} else {
		    t -= (1 << REFSHIFT);
		}
	    }

	    spread = n;
	    while (n <= nargs) {
		if (*argp == T_LVALUE) {
		    (*arg)->mod = n - spread;
		    break;
		}
		if (typechecked && c_tmatch(t, *argp) == T_NIL) {
		    c_error("bad argument %d for function %s (needs %s)", n,
			    fname, i_typename(tnbuf, *argp));
		}
		n++;
		argp += ((*argp & T_TYPE) == T_CLASS) ? 4 : 1;
	    }
	    break;
	} else if (t == T_LVALUE) {
	    if (!lvalue(*arg)) {
		c_error("bad argument %d for function %s (needs lvalue)",
			n, fname);
	    }
	    *arg = node_mon(N_LVALUE, (*arg)->mod, *arg);
	} else if ((typechecked || (*arg)->mod == T_VOID) &&
		   c_tmatch((*arg)->mod, t) == T_NIL &&
		   (!c_nil(*arg) || !T_POINTER(t))) {
	    c_error("bad argument %d for function %s (needs %s)", n, fname,
		    i_typename(tnbuf, t));
	}

	if (n == nargs && ellipsis) {
	    nargs++;
	} else {
	    argp += ((*argp & T_TYPE) == T_CLASS) ? 4 : 1;
	}
    }
    if (args != (node *) NULL && PROTO_FTYPE(proto) != T_IMPLICIT) {
	if (args->type == N_SPREAD) {
	    t = args->l.left->mod;
	    if (t != T_MIXED && (t & T_REF) == 0) {
		c_error("ellipsis requires array");
	    }
	} else {
	    c_error("too many arguments for function %s", fname);
	}
    }

    if (proto[PROTO_SIZE(proto) - 1] == T_LVALUE) {
	/* kfuns can have lvalue parameters */
	func->r.number |= (long) KFCALL_LVAL << 24;
    }

    return func;
}

/*
 * NAME:	compile->funcall()
 * DESCRIPTION:	handle a function call
 */
node *c_funcall(node *func, node *args)
{
    return funcall(func, revert_list(args), FALSE);
}

/*
 * NAME:	compile->arrow()
 * DESCRIPTION:	handle ->
 */
node *c_arrow(node *other, node *func, node *args)
{
    if (args == (node *) NULL) {
	args = func;
    } else {
	args = node_bin(N_PAIR, 0, func, revert_list(args));
    }
    return funcall(c_flookup(node_str(String::create("call_other", 10)),
			     FALSE),
		   node_bin(N_PAIR, 0, other, args), FALSE);
}

/*
 * NAME:	compile->address()
 * DESCRIPTION:	handle &func()
 */
node *c_address(node *func, node *args, int typechecked)
{
# ifdef CLOSURES
    args = revert_list(args);
    funcall(c_flookup(func, typechecked), args, TRUE);	/* check only */

    if (args == (node *) NULL) {
	args = func;
    } else {
	args = node_bin(N_PAIR, 0, func, args);
    }
    func = funcall(c_flookup(node_str(String::create("new.function", 12)),
			     FALSE),
		   args, FALSE);
    func->mod = T_CLASS;
    func->sclass = String::create(BIPREFIX "function", BIPREFIXLEN + 8);
    func->sclass->ref();
    return func;
# else
    UNREFERENCED_PARAMETER(func);
    UNREFERENCED_PARAMETER(args);
    UNREFERENCED_PARAMETER(typechecked);
    c_error("syntax error");
    return node_mon(N_FAKE, T_MIXED, (node *) NULL);
# endif
}

/*
 * NAME:	compile->extend()
 * DESCRIPTION:	handle &(*func)()
 */
node *c_extend(node *func, node *args, int typechecked)
{
# ifdef CLOSURES
    if (typechecked && func->mod != T_MIXED) {
	if (func->mod != T_OBJECT &&
	    (func->mod != T_CLASS ||
	     strcmp(func->sclass->text, BIPREFIX "function") != 0)) {
	    c_error("bad argument 1 for function * (needs function)");
	}
    }
    if (args == (node *) NULL) {
	args = func;
    } else {
	args = node_bin(N_PAIR, 0, func, revert_list(args));
    }
    func = funcall(c_flookup(node_str(String::create("extend.function", 15)),
			     FALSE),
		   args, FALSE);
    func->mod = T_CLASS;
    func->sclass = String::create(BIPREFIX "function", BIPREFIXLEN + 8);
    func->sclass->ref();
    return func;
# else
    UNREFERENCED_PARAMETER(func);
    UNREFERENCED_PARAMETER(args);
    UNREFERENCED_PARAMETER(typechecked);
    c_error("syntax error");
    return node_mon(N_FAKE, T_MIXED, (node *) NULL);
# endif
}

/*
 * NAME:	compile->call()
 * DESCRIPTION:	handle (*func)()
 */
node *c_call(node *func, node *args, int typechecked)
{
# ifdef CLOSURES
    if (typechecked && func->mod != T_MIXED) {
	if (func->mod != T_OBJECT &&
	    (func->mod != T_CLASS ||
	     strcmp(func->sclass->text, BIPREFIX "function") != 0)) {
	    c_error("bad argument 1 for function * (needs function)");
	}
    }
    if (args == (node *) NULL) {
	args = func;
    } else {
	args = node_bin(N_PAIR, 0, func, revert_list(args));
    }
    return funcall(c_flookup(node_str(String::create("call.function", 13)),
			     FALSE),
		   args, FALSE);
# else
    UNREFERENCED_PARAMETER(func);
    UNREFERENCED_PARAMETER(args);
    UNREFERENCED_PARAMETER(typechecked);
    c_error("syntax error");
    return node_mon(N_FAKE, T_MIXED, (node *) NULL);
# endif
}

/*
 * NAME:	compile->new_object()
 * DESCRIPTION:	handle new
 */
node *c_new_object(node *o, node *args)
{
    if (args != (node *) NULL) {
	args = node_bin(N_PAIR, 0, o, revert_list(args));
    } else {
	args = o;
    }
    return funcall(c_flookup(node_str(String::create("new_object", 10)),
			     FALSE),
		   args, FALSE);
}

/*
 * NAME:	compile->instanceof()
 * DESCRIPTION:	handle <-
 */
node *c_instanceof(node *n, node *prog)
{
    String *str;

    if (n->mod != T_MIXED && n->mod != T_OBJECT && n->mod != T_CLASS) {
	c_error("bad argument 1 for function <- (needs object)");
    }
    str = c_objecttype(prog);
    prog->l.string->del();
    prog->l.string = str;
    prog->l.string->ref();
    return node_bin(N_INSTANCEOF, T_INT, n, prog);
}

/*
 * NAME:	compile->checkcall()
 * DESCRIPTION:	check return value of a system call
 */
node *c_checkcall(node *n, int typechecked)
{
    if (n->type == N_FUNC && (n->r.number >> 24) == FCALL) {
	if (typechecked) {
	    if (n->mod != T_MIXED && n->mod != T_VOID) {
		/*
		 * make sure the return value is as it should be
		 */
		n = node_mon(N_CAST, n->mod, n);
		n->sclass = n->l.left->sclass;
		if (n->sclass != (String *) NULL) {
		    n->sclass->ref();
		}
	    }
	} else {
	    /* could be anything */
	    n->mod = T_MIXED;
	}
    } else if (n->mod == T_VOID && !typechecked) {
	/* no void expressions */
	n->mod = T_INT;
    }

    return n;
}

/*
 * NAME:	compile->tst()
 * DESCRIPTION:	handle a condition
 */
node *c_tst(node *n)
{
    switch (n->type) {
    case N_INT:
	n->l.number = (n->l.number != 0);
	return n;

    case N_FLOAT:
	return node_int((Int) !NFLT_ISZERO(n));

    case N_STR:
	return node_int((Int) TRUE);

    case N_NIL:
	return node_int((Int) FALSE);

    case N_TST:
    case N_NOT:
    case N_LAND:
    case N_EQ:
    case N_EQ_INT:
    case N_NE:
    case N_NE_INT:
    case N_GT:
    case N_GT_INT:
    case N_GE:
    case N_GE_INT:
    case N_LT:
    case N_LT_INT:
    case N_LE:
    case N_LE_INT:
	return n;

    case N_COMMA:
	n->mod = T_INT;
	n->r.right = c_tst(n->r.right);
	return n;
    }

    return node_mon(N_TST, T_INT, n);
}

/*
 * NAME:	compile->not()
 * DESCRIPTION:	handle a !condition
 */
node *c_not(node *n)
{
    switch (n->type) {
    case N_INT:
	n->l.number = (n->l.number == 0);
	return n;

    case N_FLOAT:
	return node_int((Int) NFLT_ISZERO(n));

    case N_STR:
	return node_int((Int) FALSE);

    case N_NIL:
	return node_int((Int) TRUE);

    case N_LAND:
	n->type = N_LOR;
	n->l.left = c_not(n->l.left);
	n->r.right = c_not(n->r.right);
	return n;

    case N_LOR:
	n->type = N_LAND;
	n->l.left = c_not(n->l.left);
	n->r.right = c_not(n->r.right);
	return n;

    case N_TST:
	n->type = N_NOT;
	return n;

    case N_NOT:
	n->type = N_TST;
	return n;

    case N_EQ:
	n->type = N_NE;
	return n;

    case N_EQ_INT:
	n->type = N_NE_INT;
	return n;

    case N_NE:
	n->type = N_EQ;
	return n;

    case N_NE_INT:
	n->type = N_EQ_INT;
	return n;

    case N_GT:
	n->type = N_LE;
	return n;

    case N_GT_INT:
	n->type = N_LE_INT;
	return n;

    case N_GE:
	n->type = N_LT;
	return n;

    case N_GE_INT:
	n->type = N_LT_INT;
	return n;

    case N_LT:
	n->type = N_GE;
	return n;

    case N_LT_INT:
	n->type = N_GE_INT;
	return n;

    case N_LE:
	n->type = N_GT;
	return n;

    case N_LE_INT:
	n->type = N_GT_INT;
	return n;

    case N_COMMA:
	n->mod = T_INT;
	n->r.right = c_not(n->r.right);
	return n;
    }

    return node_mon(N_NOT, T_INT, n);
}

/*
 * NAME:	compile->lvalue()
 * DESCRIPTION:	handle an lvalue
 */
node *c_lvalue(node *n, const char *oper)
{
    if (!lvalue(n)) {
	c_error("bad lvalue for %s", oper);
	return node_mon(N_FAKE, T_MIXED, n);
    }
    return n;
}

/*
 * NAME:      compile->lval_aggr()
 * DESCRIPTION:       check an aggregate of lvalues
 */
static void c_lval_aggr(node **n)
{
    node **m;

    if (*n == (node *) NULL) {
      c_error("no lvalues in aggregate");
    } else {
      while (n != (node **) NULL) {
	  if ((*n)->type == N_PAIR) {
	      m = &(*n)->l.left;
	      n = &(*n)->r.right;
	  } else {
	      m = n;
	      n = (node **) NULL;
	  }
	  if (!lvalue(*m)) {
	      c_error("bad lvalue in aggregate");
	      *m = node_mon(N_FAKE, T_MIXED, *m);
	  }
	  if ((*m)->type == N_LOCAL && !BTST(thiscond->init, (*m)->r.number)) {
	      BSET(thiscond->init, (*m)->r.number);
	      --variables[(*m)->r.number].unset;
	  }
      }
    }
}

/*
 * NAME:	compile->assign()
 * DESCRIPTION:	handle an assignment
 */
node *c_assign(node *n)
{
    if (n->type == N_AGGR) {
	c_lval_aggr(&n->l.left);
    } else {
	n = c_lvalue(n, "assignment");
	if (n->type == N_LOCAL && !BTST(thiscond->init, n->r.number)) {
	    BSET(thiscond->init, n->r.number);
	    --variables[n->r.number].unset;
	}
    }
    return n;
}

/*
 * NAME:	compile->tmatch()
 * DESCRIPTION:	See if the two supplied types are compatible. If so, return the
 *		combined type. If not, return T_NIL.
 */
unsigned short c_tmatch(unsigned int type1, unsigned int type2)
{
    if (type1 == T_NIL || type2 == T_NIL) {
	/* nil doesn't match with anything else */
	return T_NIL;
    }
    if (type1 == type2) {
	return type1;	/* identical types (including T_CLASS) */
    }
    if ((type1 & T_TYPE) == T_CLASS) {
	type1 = (type1 & T_REF) | T_OBJECT;
    }
    if ((type2 & T_TYPE) == T_CLASS) {
	type2 = (type2 & T_REF) | T_OBJECT;
    }
    if (type1 == type2) {
	return type1;	/* identical types (excluding T_CLASS) */
    }
    if (type1 == T_VOID || type2 == T_VOID) {
	/* void doesn't match with anything else, not even with mixed */
	return T_NIL;
    }
    if ((type1 & T_TYPE) == T_MIXED && (type1 & T_REF) <= (type2 & T_REF)) {
	/* mixed <-> int,  mixed * <-> int *,  mixed * <-> int ** */
	if (type1 == T_MIXED && (type2 & T_REF) != 0) {
	    type1 |= 1 << REFSHIFT;	/* mixed <-> int * */
	}
	return type1;
    }
    if ((type2 & T_TYPE) == T_MIXED && (type2 & T_REF) <= (type1 & T_REF)) {
	/* int <-> mixed,  int * <-> mixed *,  int ** <-> mixed * */
	if (type2 == T_MIXED && (type1 & T_REF) != 0) {
	    type2 |= 1 << REFSHIFT;	/* int * <-> mixed */
	}
	return type2;
    }
    return T_NIL;
}

/*
 * NAME:	compile->error()
 * DESCRIPTION:	Call the driver object with the supplied error message.
 */
void c_error(const char *format, ...)
{
    va_list args;
    char *fname, buf[4 * STRINGSZ];	/* file name + 2 * string + overhead */

    if (driver_object != (char *) NULL &&
	Object::find(driver_object, OACC_READ) != (Object *) NULL) {
	Frame *f;

	f = current->frame;
	fname = tk_filename();
	PUSH_STRVAL(f, String::create(fname, strlen(fname)));
	PUSH_INTVAL(f, tk_line());
	va_start(args, format);
	vsprintf(buf, format, args);
	va_end(args);
	PUSH_STRVAL(f, String::create(buf, strlen(buf)));

	call_driver_object(f, "compile_error", 3);
	i_del_value(f->sp++);
    } else {
	/* there is no driver object to call; show the error on stderr */
	sprintf(buf, "%s, %u: ", tk_filename(), tk_line());
	va_start(args, format);
	vsprintf(buf + strlen(buf), format, args);
	va_end(args);
	message("%s\012", buf);     /* LF */
    }

    nerrors++;
}
