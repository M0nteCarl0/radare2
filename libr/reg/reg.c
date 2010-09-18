/* radare - LGPL - Copyright 2009-2010 pancake<nopcode.org> */

#include <r_reg.h>
#include <r_util.h>
#include <list.h>

static const char *types[R_REG_TYPE_LAST+1] = {
	"gpr", "drx", "fpu", "mmx", "xmm", "flg", "seg", NULL
};

R_API const char *r_reg_get_type(int idx) {
	if (idx>=0 && idx<R_REG_TYPE_LAST)
		return types[idx];
	return NULL;
}

static void r_reg_item_free(RRegItem *item) {
	free (item->name);
	free (item);
}

R_API int r_reg_get_name_idx(const char *type) {
	if (type)
	switch (*type | (type[1]<<8)) {
	case 'p'+('c'<<8): return R_REG_NAME_PC;
	case 's'+('r'<<8): return R_REG_NAME_SR;
	case 's'+('p'<<8): return R_REG_NAME_SP;
	case 'b'+('p'<<8): return R_REG_NAME_BP;
	case 'a'+('0'<<8): return R_REG_NAME_A0;
	case 'a'+('1'<<8): return R_REG_NAME_A1;
	case 'a'+('2'<<8): return R_REG_NAME_A2;
	case 'a'+('3'<<8): return R_REG_NAME_A3;
	}
	return -1;
}


R_API int r_reg_set_name(RReg *reg, int role, const char *name) {
	// TODO: ensure this range check in a define.. somewhere
	if (role>=0 && role<R_REG_NAME_LAST) {
		reg->name[role] = r_str_dup (reg->name[role], name);
		return R_TRUE;
	}
	return R_FALSE;
}

R_API const char *r_reg_get_name(RReg *reg, int role) {
	if (reg && role>=0 && role<R_REG_NAME_LAST)
		return reg->name[role];
	return NULL;
}

R_API void r_reg_free_internal(RReg *reg) {
	int i;
	for (i=0; i<R_REG_TYPE_LAST; i++) {
		r_list_destroy (reg->regset[i].regs);
		r_list_destroy (reg->regset[i].pool);
		reg->regset[i].arena = r_reg_arena_new (0);
	}
}

R_API RReg *r_reg_free(RReg *reg) {
	if (reg) {
		r_reg_free_internal (reg);
		free (reg);
	}
	return NULL;
}

R_API RReg *r_reg_new() {
	RReg *reg = R_NEW (RReg);
	int i;
	reg->profile = NULL;
	for (i=0; i<R_REG_NAME_LAST; i++)
		reg->name[i] = NULL;
	for (i=0; i<R_REG_TYPE_LAST; i++) {
		reg->regset[i].pool = r_list_new ();
		reg->regset[i].pool->free = (RListFree)r_reg_arena_free;
		reg->regset[i].regs = r_list_new ();
		reg->regset[i].regs->free = (RListFree)r_reg_item_free;
		if (!(reg->regset[i].arena = r_reg_arena_new (0)))
			return NULL;
		r_list_append (reg->regset[i].pool, reg->regset[i].arena);
	}
	return reg;
}

R_API int r_reg_push(RReg *reg) {
	int i;
	for (i=0; i<R_REG_TYPE_LAST; i++) {
		if (!(reg->regset[i].arena = r_reg_arena_new (0)))
			return 0;
		r_list_prepend (reg->regset[i].pool, reg->regset[i].arena);
	}
	return r_list_length (reg->regset[0].pool);
}

/* 

arena2

// notify the debugger that something has changed, and registers needs to be pushed
r_debug_reg_change()

for(;;) {
  read_regs  \___ reg_pre()
  r_reg_push /

  step

  read_regs    \_.
  r_reg_cmp(); /  '- reg_post()

  r_reg_pop() >-- reg_fini()
}
*/


R_API void r_reg_pop(RReg *reg) {
	int i;
	for (i=0; i<R_REG_TYPE_LAST; i++) {
		if (r_list_length(reg->regset[i].pool)>0) {
			r_list_delete (reg->regset[i].pool, 
				r_list_head (reg->regset[i].pool));
			// SEGFAULT: r_reg_arena_free (reg->regset[i].arena);
			reg->regset[i].arena = (RRegArena*)r_list_head (
				reg->regset[i].pool);
		} else {
			eprintf ("Cannot pop more\n");
			break;
		}
	}
}

static RRegItem *r_reg_item_new() {
	RRegItem *item = R_NEW (RRegItem);
	memset (item, 0, sizeof (RRegItem));
	return item;
}

R_API int r_reg_type_by_name(const char *str) {
	int i;
	for (i=0; types[i] && i<R_REG_TYPE_LAST; i++)
		if (!strcmp (types[i], str))
			return i;
	if (!strcmp (str, "all"))
		return R_REG_TYPE_ALL;
	eprintf ("Unknown register type: '%s'\n", str);
	return R_REG_TYPE_LAST;
}

/* TODO: make this parser better and cleaner */
static int r_reg_set_word(RRegItem *item, int idx, char *word) {
	int ret = R_TRUE;
	switch(idx) {
	case 0:
		item->type = r_reg_type_by_name (word);
		break;
	case 1:
		item->name = strdup (word);
		break;
	/* spaguetti ftw!!1 */
	case 2:
		if (*word=='.') // XXX; this is kinda ugly
			item->size = atoi (word+1);
		else item->size = atoi (word)*8;
		break;
	case 3:
		if (*word=='.') // XXX; this is kinda ugly
			item->offset = atoi (word+1);
		else item->offset = atoi (word)*8;
		break;
	case 4:
		if (*word=='.') // XXX; this is kinda ugly
			item->packed_size = atoi (word+1);
		else item->packed_size = atoi (word)*8;
		break;
	default:
		eprintf ("register set fail (%s)\n", word);
		ret = R_FALSE;
	}
	return ret;
}

/* TODO: make this parser better and cleaner */
R_API int r_reg_set_profile_string(RReg *reg, const char *str) {
	RRegItem *item;
	int setname = -1;
	int ret = R_FALSE;
	int lastchar = 0;
	int chidx = 0;
	int word = 0;
	char buf[256];

	if (!str||!reg)
		return R_FALSE;
	buf[0] = '\0';
	/* format file is: 'type name size offset packedsize' */
	r_reg_free_internal (reg);
	item = r_reg_item_new ();

	while (*str) {
		if (*str == '#') {
			/* skip until newline */
			while (*str && *str != '\n') str++;
			continue;
		}
		switch (*str) {
		case ' ':
		case '\t':
			/* UGLY PASTAFARIAN PARSING */
			if (word==0 && *buf=='=') {
				setname = r_reg_get_name_idx (buf+1);
				if (setname == -1)
					eprintf ("Invalid register type: '%s'\n", buf+1);
			} else
			if (lastchar != ' ' && lastchar != '\t')
				r_reg_set_word (item, word, buf);
			chidx = 0;
			word++;
			break;
		case '\n':
			if (setname != -1)
				r_reg_set_name (reg, setname, buf);
			else if (word>3) {
				r_reg_set_word (item, word, buf);
				if (item->name != NULL) {
					r_list_append (reg->regset[item->type].regs, item);
					item = r_reg_item_new();
				}
			}
			chidx = word = 0;
			setname = -1;
			break;
		default:
			if (chidx>128) // WTF!!
				return R_FALSE;
			buf[chidx++] = *str;
			buf[chidx] = 0;
			break;
		}
		lastchar = *str;
		str++;
	}
	r_reg_item_free (item);
	r_reg_fit_arena (reg);

	return *str?ret:R_TRUE;
}

R_API int r_reg_set_profile(RReg *reg, const char *profile) {
	int ret = R_FALSE;
	const char *base;
	char *str, *file;
	/* TODO: append .regs extension to filename */
	if ((str = r_file_slurp (profile, NULL))==NULL) {
 		// XXX we must define this varname in r_lib.h /compiletime/
		base = r_sys_getenv ("LIBR_PLUGINS");
		if (base) {
			file = r_str_concat (strdup (base), profile);
			str = r_file_slurp (file, NULL);
			free (file);
		}
	}
	if (str) ret = r_reg_set_profile_string (reg, str);
	else eprintf ("r_reg_set_profile: Cannot find '%s'\n", profile);
	return ret;
}

R_API RRegItem *r_reg_get(RReg *reg, const char *name, int type) {
	RListIter *iter;
	RRegItem *r;
	int i, e;
	if (!reg)
		return NULL;
	if (type == -1) {
		i = 0;
		e = R_REG_TYPE_LAST;
	} else {
		i = type;
		e = type+1;
	}

	for (; i<e; i++) {
		r_list_foreach (reg->regset[i].regs, iter, r) {
			if (!strcmp (r->name, name))
				return r;
		}
	}
	return NULL;
}

R_API RList *r_reg_get_list(RReg *reg, int type) {
	if (type<0 || type>R_REG_TYPE_LAST)
		return NULL;
	return reg->regset[type].regs;
}
