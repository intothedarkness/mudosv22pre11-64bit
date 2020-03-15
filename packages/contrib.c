#define SUPPRESS_COMPILER_INLINES
#ifdef LATTICE
#include "/lpc_incl.h"
#include "/comm.h"
#include "/file_incl.h"
#include "/file.h"
#include "/backend.h"
#include "/swap.h"
#include "/compiler.h"
#include "/main.h"
#include "/eoperators.h"
#include "/simul_efun.h"
#include "/add_action.h"
#else
#include "../lpc_incl.h"
#include "../comm.h"
#include "../file_incl.h"
#include "../file.h"
#include "../backend.h"
#include "../swap.h"
#include "../compiler.h"
#include "../main.h"
#include "../eoperators.h"
#include "../efun_protos.h"
#include "../simul_efun.h"
#include "../add_action.h"
#endif

/* should be done in configure */
#ifdef WIN32
#define strcasecmp(X, Y) stricmp(X, Y)
#endif

/*
 * This differs from the livings() efun in that this efun only returns
 * objects which have had set_living_name() called as well as 
 * enable_commands().  The other major difference is that this is
 * substantially faster.
 */
#ifdef F_NAMED_LIVINGS
void f_named_livings() {
    int i;
    int nob;
#ifdef F_SET_HIDE
    int apply_valid_hide, display_hidden = 0;
#endif
    object_t *ob, **obtab;
    array_t *vec;

    nob = 0;
#ifdef F_SET_HIDE
    apply_valid_hide = 1;
#endif

    obtab = CALLOCATE(max_array_size, object_t *, TAG_TEMPORARY, "named_livings");

    for (i = 0; i < CFG_LIVING_HASH_SIZE; i++) {
	for (ob = hashed_living[i]; ob; ob = ob->next_hashed_living) {
	    if (!(ob->flags & O_ENABLE_COMMANDS))
		continue;
#ifdef F_SET_HIDE
	    if (ob->flags & O_HIDDEN) {
		if (apply_valid_hide) {
		    apply_valid_hide = 0;
		    display_hidden = valid_hide(current_object);
		}
		if (!display_hidden)
		    continue;
	    }
#endif
	    if (nob == max_array_size)
		break;
	    obtab[nob++] = ob;
	}
    }

    vec = allocate_empty_array(nob);
    while (--nob >= 0) {
	vec->item[nob].type = T_OBJECT;
	vec->item[nob].u.ob = obtab[nob];
	add_ref(obtab[nob], "livings");
    }

    FREE(obtab);

    push_refed_array(vec);
}    
#endif

/* I forgot who wrote this, please claim it :) */
#ifdef F_REMOVE_SHADOW
void
f_remove_shadow PROT((void))
{
    object_t *ob;
    
    ob = current_object;
    if (st_num_arg) {
	ob = sp->u.ob;
	pop_stack();
    }
    if (ob == 0 || (ob->shadowing == 0 && ob->shadowed == 0))
	push_number(0);
    else {
	if (ob->shadowed)
	    ob->shadowed->shadowing = ob->shadowing;
	if (ob->shadowing)
	    ob->shadowing->shadowed = ob->shadowed;
	ob->shadowing = ob->shadowed = 0;
	push_number(1);
    }
}
#endif

/* This was originally written my Malic for Demon.  I rewrote parts of it
   when I added it (added function support, etc) -Beek */
#ifdef F_QUERY_NOTIFY_FAIL
void
f_query_notify_fail PROT((void)) {
    char *p;

    if (command_giver && command_giver->interactive) {
	if (command_giver->interactive->iflags & NOTIFY_FAIL_FUNC) {
	    push_funp(command_giver->interactive->default_err_message.f);
	    return;
	} else if ((p = command_giver->interactive->default_err_message.s)) {
	    STACK_INC;
	    sp->type = T_STRING;
	    sp->subtype = STRING_SHARED;
	    sp->u.string = p;
	    ref_string(p);
	    return;
	}
    }
    push_number(0);
}
#endif

/* Beek again */
#ifdef F_STORE_VARIABLE
void
f_store_variable PROT((void)) {
    int idx;
    svalue_t *sv;
    unsigned short type;
    
    idx = find_global_variable(current_object->prog, (sp-1)->u.string, &type, 0);
    if (idx == -1)
	error("No variable named '%s'!\n", (sp-1)->u.string);
    sv = &current_object->variables[idx];
    free_svalue(sv, "f_store_variable");
    *sv = *sp--;
    free_string_svalue(sp--);
}
#endif

#ifdef F_FETCH_VARIABLE
void
f_fetch_variable PROT((void)) {
    int idx;
    svalue_t *sv;
    unsigned short type;
    
    idx = find_global_variable(current_object->prog, sp->u.string, &type, 0);
    if (idx == -1)
	error("No variable named '%s'!\n", sp->u.string);
    sv = &current_object->variables[idx];
    free_string_svalue(sp--);
    push_svalue(sv);
}
#endif

/* Beek */
#ifdef F_SET_PROMPT
void
f_set_prompt PROT((void)) {
    object_t *who;
    if (st_num_arg == 2) {
	who = sp->u.ob;
	pop_stack();
    } else who = command_giver;
    
    if (!who || who->flags & O_DESTRUCTED || !who->interactive)
	error("Prompts can only be set for interactives.\n");
    
    /* Future work */
    /* ed() will nuke this; also we have to make sure the string will get
     * freed */
}
#endif

/* Gudu@VR wrote copy_array() and copy_mapping() which this is heavily
 * based on.  I made it into a general copy() efun which incorporates
 * both. -Beek
 */
#ifdef F_COPY
static int depth;

static void deep_copy_svalue PROT((svalue_t *, svalue_t *));

static array_t *deep_copy_array P1( array_t *, arg ) {
    array_t *vec;
    int i;
    
    vec = allocate_empty_array(arg->size);
    for (i = 0; i < arg->size; i++)
	deep_copy_svalue(&arg->item[i], &vec->item[i]);

    return vec;
}

static array_t *deep_copy_class P1(array_t *, arg) {
    array_t *vec;
    int i;
    
    vec = allocate_empty_class_by_size(arg->size);
    for (i = 0; i < arg->size; i++)
	deep_copy_svalue(&arg->item[i], &vec->item[i]);

    return vec;
}

static int doCopy P3( mapping_t *, map, mapping_node_t *, elt, mapping_t *, dest) {
    svalue_t *sv;
    
    sv = find_for_insert(dest, &elt->values[0], 1);
    if (!sv) {
	mapping_too_large();
	return 1;
    }
    
    deep_copy_svalue(&elt->values[1], sv);
    return 0;
}

static mapping_t *deep_copy_mapping P1( mapping_t *, arg ) {
    mapping_t *map;
    
    map = allocate_mapping( 0 ); /* this should be fixed.  -Beek */
    mapTraverse( arg, (int (*)()) doCopy, map); /* Not horridly efficient either */
    return map;
}

static void deep_copy_svalue P2(svalue_t *, from, svalue_t *, to) {
    switch (from->type) {
    case T_ARRAY:
	depth++;
	if (depth > MAX_SAVE_SVALUE_DEPTH) {
	    depth = 0;
	    error("Mappings, arrays and/or classes nested too deep (%d) for copy()\n",
		  MAX_SAVE_SVALUE_DEPTH);
	}
	*to = *from;
	to->u.arr = deep_copy_array( from->u.arr );
	depth--;
	break;
    case T_CLASS:
	depth++;
	if (depth > MAX_SAVE_SVALUE_DEPTH) {
	    depth = 0;
	    error("Mappings, arrays and/or classes nested too deep (%d) for copy()\n",
		  MAX_SAVE_SVALUE_DEPTH);
	}
	*to = *from;
	to->u.arr = deep_copy_class( from->u.arr );
	depth--;
	break;
    case T_MAPPING:
	depth++;
	if (depth > MAX_SAVE_SVALUE_DEPTH) {
	    depth = 0;
	    error("Mappings, arrays and/or classes nested too deep (%d) for copy()\n",
		  MAX_SAVE_SVALUE_DEPTH);
	}
	*to = *from;
	to->u.map = deep_copy_mapping( from->u.map );
	depth--;
	break;
#ifndef NO_BUFFER_TYPE
    case T_BUFFER:
        *to = *from;
        to->u.buf = allocate_buffer(from->u.buf->size);
        memcpy(to->u.buf->item, to->u.buf->item, from->u.buf->size);
        break;
#endif
    default:
	assign_svalue_no_free( to, from );
    }
}

void f_copy PROT((void))
{
    svalue_t ret;
    
    depth = 0;
    deep_copy_svalue(sp, &ret);
    free_svalue(sp, "f_copy");
    *sp = ret;
}
#endif    

/* Gudu@VR */    
/* flag and extra info by Beek */
#ifdef F_FUNCTIONS
void f_functions PROT((void)) {
    int i, j, num, index;
    array_t *vec, *subvec;
    function_t *funp;
    program_t *prog;
    int flag = (sp--)->u.number;
    unsigned short *types;
    char buf[256];
    char *end = EndOf(buf);
    program_t *progp;
    
    if (sp->u.ob->flags & O_SWAPPED) 
	load_ob_from_swap(sp->u.ob);

    progp = sp->u.ob->prog;
    num = progp->num_functions_defined + progp->last_inherited;
    if (progp->num_functions_defined &&
	progp->function_table[progp->num_functions_defined-1].name[0]
	== APPLY___INIT_SPECIAL_CHAR)
	num--;
	
    vec = allocate_empty_array(num);
    i = num;
    
    while (i--) {
	unsigned short low, high, mid;
	
	prog = sp->u.ob->prog;
	index = i;

	/* Walk up the inheritance tree to the real definition */	
	if (prog->function_flags[index] & FUNC_ALIAS) {
	    index = prog->function_flags[index] & ~FUNC_ALIAS;
	}
	
	while (prog->function_flags[index] & FUNC_INHERITED) {
	    low = 0;
	    high = prog->num_inherited -1;
	    
	    while (high > low) {
		mid = (low + high + 1) >> 1;
		if (prog->inherit[mid].function_index_offset > index)
		    high = mid -1;
		else low = mid;
	    }
	    index -= prog->inherit[low].function_index_offset;
	    prog = prog->inherit[low].prog;
	}
    
	index -= prog->last_inherited;

	funp = prog->function_table + index;

	if (flag) {
	    if (prog->type_start && prog->type_start[index] != INDEX_START_NONE)
		types = &prog->argument_types[prog->type_start[index]];
	    else
		types = 0;

	    vec->item[i].type = T_ARRAY;
	    subvec = vec->item[i].u.arr = allocate_empty_array(3 + funp->num_arg);
	    
	    subvec->item[0].type = T_STRING;
	    subvec->item[0].subtype = STRING_SHARED;
	    subvec->item[0].u.string = ref_string(funp->name);

	    subvec->item[1].type = T_NUMBER;
	    subvec->item[1].subtype = 0;
	    subvec->item[1].u.number = funp->num_arg;

	    get_type_name(buf, end, funp->type);
	    subvec->item[2].type = T_STRING;
	    subvec->item[2].subtype = STRING_SHARED;
	    subvec->item[2].u.string = make_shared_string(buf);

	    for (j = 0; j < funp->num_arg; j++) {
		if (types) {
		    get_type_name(buf, end, types[j]);
		    subvec->item[3 + j].type = T_STRING;
		    subvec->item[3 + j].subtype = STRING_SHARED;
		    subvec->item[3 + j].u.string = make_shared_string(buf);
		} else {
		    subvec->item[3 + j].type = T_NUMBER;
		    subvec->item[3 + j].u.number = 0;
		}
	    }
	} else {
	    vec->item[i].type = T_STRING;
	    vec->item[i].subtype = STRING_SHARED;
	    vec->item[i].u.string = ref_string(funp->name);
	}
    }
    
    pop_stack();
    push_refed_array(vec);
}
#endif

/* Beek */
#ifdef F_VARIABLES
static void fv_recurse P5(array_t *, arr, int *, idx, program_t *, prog, int, type, int, flag) {
    int i;
    array_t *subarr;
    char buf[256];
    char *end = EndOf(buf);
    
    for (i = 0; i < prog->num_inherited; i++) {
	fv_recurse(arr, idx, prog->inherit[i].prog, 
		   type | prog->inherit[i].type_mod, flag);
    }
    for (i = 0; i < prog->num_variables_defined; i++) {
	if (flag) {
	    arr->item[*idx + i].type = T_ARRAY;
	    subarr = arr->item[*idx + i].u.arr = allocate_empty_array(2);
	    subarr->item[0].type = T_STRING;
	    subarr->item[0].subtype = STRING_SHARED;
	    subarr->item[0].u.string = ref_string(prog->variable_table[i]);
	    get_type_name(buf, end, prog->variable_types[i]);
	    subarr->item[1].type = T_STRING;
	    subarr->item[1].subtype = STRING_SHARED;
	    subarr->item[1].u.string = make_shared_string(buf);
	} else {
	    arr->item[*idx + i].type = T_STRING;
	    arr->item[*idx + i].subtype = STRING_SHARED;
	    arr->item[*idx + i].u.string = ref_string(prog->variable_table[i]);
	}
    }
    *idx += prog->num_variables_defined;
}

void f_variables PROT((void)) {
    int idx = 0;
    array_t *arr;
    int flag = (sp--)->u.number;
    program_t *prog = sp->u.ob->prog;
    
    if (sp->u.ob->flags & O_SWAPPED)
	load_ob_from_swap(sp->u.ob);
    
    arr = allocate_empty_array(prog->num_variables_total);
    fv_recurse(arr, &idx, prog, 0, flag);
    
    pop_stack();
    push_refed_array(arr);
}
#endif

/* also Beek */
#ifdef F_HEART_BEATS
void f_heart_beats PROT((void)) {
    push_refed_array(get_heart_beats());
}
#endif

/*Aleas@Nightmare */
#ifdef F_TERMINAL_COLOUR
/* A fast implementation of the Nightmare color support.

   [Ed note: These codes were actually used on Discworld
   before Nightmare]

   Rewritten several times, since Beek wants it to be
   perfect :)

   Takes a string and a mapping as args. The string is
   exploded using "%^" as delimiter, then all keys of
   the mapping found in the resulting array are replaced
   by their values. Afterwards a string imploded from
   the array is returned.

   No actual string copying is done except for the
   creation of the final string and a temporary copy of
   the input string to avoid destruction of shared
   input strings. An array of pointers to the segments
   of the string is compared against the mapping keys
   and replaced with a pointer to the value belonging to
   that key where matches are found.

   After the replacement pass the result string is created
   from the pointer array.

   Further speed is gained by the fact that no parsing is
   done if the input string does not contain any "%^" 
   delimiter sequence.

   by Aleas@Nightmare, dec-94 */

/* number of input string segments, if more, it still works, but a
   _slow_ realloc is required */
#define NSTRSEGS 32
#define TC_FIRST_CHAR '%'
#define TC_SECOND_CHAR '^'

static int at_end(int i, int imax, int z, int *lens) {
    if (z + 1 != lens[i])
	return 0;
    for (i++; i < imax; i++) {
	if (lens[i] > 0)
	    return 0;
    }
    return 1;
}

void 
f_terminal_colour PROT((void))
{
    char *instr, *cp, *savestr, *deststr, **parts;
    int num, i, j, k, col, start, space, *lens, maybe_at_end;
    int space_garbage = 0;
    mapping_node_t *elt, **mtab;
    int buflen, max_buflen, space_buflen;
    int wrap = 0;
    int indent = 0;

    if (st_num_arg >= 3) {
	if (st_num_arg == 4)
	    indent = (sp--)->u.number;
	wrap = (sp--)->u.number;
	if (wrap < 2 && wrap != 0) wrap = 2;
	if (indent < 0 || indent >= wrap - 1)
	    indent = wrap - 2;
    }

    cp = instr = (sp-1)->u.string;
    do {
	cp = strchr(cp, TC_FIRST_CHAR);
	if (cp) 
	{
	    if (cp[1] == TC_SECOND_CHAR)
	    {
		savestr = string_copy(instr, "f_terminal_colour");
		cp = savestr + ( cp - instr );
		instr = savestr;
		break;
	    }
	    cp++;
	}
    } while (cp);
    if (cp == NULL) {
	if (wrap) {
	    num = 1;
	    parts = CALLOCATE(1, char *, TAG_TEMPORARY, "f_terminal_colour: parts");
	    parts[0] = instr;
	    savestr = 0;
	} else {
	    pop_stack(); /* no delimiter in string, so return the original */
	    return;
	}
    } else {
        /* here we have something to parse */

	parts = CALLOCATE(NSTRSEGS, char *, TAG_TEMPORARY, "f_terminal_colour: parts");
	if (cp - instr) {	/* starting seg, if not delimiter */
	    num = 1;
	    parts[0] = instr;
	    *cp = 0;
	} else
	    num = 0;
	while (cp) {
	    cp += 2;
	    instr = cp;
	    do {
		cp = strchr(cp,TC_FIRST_CHAR);
		if (cp) {
		    if (cp[1] == TC_SECOND_CHAR)
			break;
		    cp++;
		}
	    } while (cp);
	    if (cp) {
		*cp = 0;
		if (cp > instr) {
		    if (num && num % NSTRSEGS == 0) {
			parts = RESIZE(parts, num + NSTRSEGS, char *, 
				       TAG_TEMPORARY, "f_terminal_colour: parts realloc");
		    }
		    parts[num++] = instr;
		}
	    }
	}
	if (*instr) {	/* trailing seg, if not delimiter */
	    if (num && num % NSTRSEGS == 0) {
		parts = RESIZE(parts, num + NSTRSEGS, char *,
			       TAG_TEMPORARY, "f_terminal_colour: parts realloc");
	    }
	    parts[num++] = instr;
	}
    }

    if (num == 0) {
	/* string consists entirely of %^'s */
	FREE(parts);
	if (savestr)
	    FREE_MSTR(savestr);
	pop_stack();
	free_string_svalue(sp);
	sp->type = T_STRING;
	sp->subtype = STRING_CONSTANT;
	sp->u.string = "";
	return;
    }

    /* Could keep track of the lens as we create parts, removing the need
       for a strlen() below */
    lens = CALLOCATE(num, int, TAG_TEMPORARY, "f_terminal_colour: lens");
    mtab = sp->u.map->table;

    /* Do the the pointer replacement and calculate the lengths */
    col = 0;
    start = -1;
    space = 0;
    maybe_at_end = 0;
    buflen = max_buflen = space_buflen = 0;
    for (j = i = 0, k = sp->u.map->table_size; i < num; i++) {
	if ((cp = findstring(parts[i]))) {
	    int tmp = MAP_POINTER_HASH(cp);
	    for (elt = mtab[tmp & k]; elt; elt = elt->next)
		if ( elt->values->type == T_STRING && 
		     (elt->values + 1)->type == T_STRING &&
		     cp == elt->values->u.string) {
		    parts[i] = (elt->values + 1)->u.string;
		    /* Negative indicates don't count for wrapping */
		    lens[i] = SVALUE_STRLEN(elt->values + 1);
		    if (wrap) lens[i] = -lens[i];
		    break;
		}
	    if (!elt)
		lens[i] = SHARED_STRLEN(cp);
	} else {
	    lens[i] = strlen(parts[i]);
	}

	if (lens[i] <= 0) {
	    if (j + -lens[i] > max_string_length)
		lens[i] = -(-(lens[i]) - (j + -lens[i] - max_string_length));
	    j += -lens[i];
	    buflen += -lens[i];
	    continue;
	}

	if (maybe_at_end) {
	    if (j + indent > max_string_length) {
		/* this string no longer counts, so we are still in
		   a maybe_at_end condition.  This means we will end
		   up truncating the rest of the fragments too, since
		   the indent will never fit. */
		lens[i] = 0;
	    } else {
		j += indent;
		col += indent;
		maybe_at_end = 0;
	    }
	}

	j += lens[i];
	if (j > max_string_length) {
	    lens[i] -= j - max_string_length;
	    j = max_string_length;
	}

	if (wrap) {
	    int z;
	    char *p = parts[i];
	    for (z = 0; z < lens[i]; z++) {
		char c = p[z];
		buflen++;
		if (c == '\n') {
		    col = 0;
		    space = space_buflen = 0;
		    start = -1;
		    max_buflen = (buflen > max_buflen ? buflen : max_buflen);
		    buflen = 0;
		} else {
		    if (col > start || (c != ' ' && c != '\t'))
			col++;
		    else {
			j--;
			buflen--;
		    }

		    if (col > start && c == '\t')
			col += (8 - ((col - 1) % 8));
		    if (c == ' ' || c == '\t') {
			space = col;
			space_buflen = buflen;
		    }
		    if (col == wrap+1) {
			if (space) {
			    col -= space;
			    space = 0;
			    max_buflen = (buflen > max_buflen ? buflen : max_buflen);
			    buflen -= space_buflen;
			    space_buflen = 0;
			} else {
			    j++;
			    col = 1;
			    max_buflen = (buflen > max_buflen ? buflen : max_buflen);
			    buflen = 1;
			}
			start = indent;
		    } else
			continue;
		}

		/* If we get here, we ended a line by wrapping */
		if (z + 1 != lens[i] || col) {
		    j += indent;
		    col += indent;
		} else
		    maybe_at_end = 1;

		if (j > max_string_length) {
		    lens[i] -= (j - max_string_length);
		    j = max_string_length;
		    if (lens[i] < z) {
			/* must have been ok or we wouldn't be here */
			lens[i] = z;
			break;
		    }
		}
	    }
	}
    }

    if (wrap && buflen > max_buflen)
	max_buflen = buflen;
    
    /* now we have the final string in parts and length in j. 
       let's compose it, wrapping if necessary */
    cp = deststr = new_string(j, "f_terminal_colour: deststr");
    if (wrap) {
	char *tmp = new_string(max_buflen, "f_terminal_colour: wrap");
	char *pt = tmp;
	
	col = 0;
	start = -1;
	space = 0;
	buflen = space_buflen = 0;
	for (i = 0; i < num; i++) {
	    int kind;
	    char *p = parts[i];
	    if (lens[i] < 0) {
		memcpy(pt, p, -lens[i]);
		pt += -lens[i];
		buflen += -lens[i];
		space_garbage += -lens[i]; /* Number of chars due to ignored junk
					      since last space */
		continue;
	    }
	    for (k = 0; k < lens[i]; k++) {
		int n;
		char c = p[k];
		*pt++ = c;
		buflen++;
		if (c == '\n') {
		    col = 0;
		    kind = 0;
		    space = space_garbage = 0;
		    start = -1;
		    buflen = 0;
		} else {
		    if (col > start || (c != ' ' && c != '\t'))
			col++;
		    else {
			pt--;
			buflen--;
		    }
		    
		    if (col > start && c == '\t')
			col += (8 - ((col - 1) % 8));
		    if (c == ' ' || c == '\t') {
			space = col;
			space_garbage = 0;
			space_buflen = buflen;
		    }
		    if (col == wrap+1) {
			if (space) {
			    col -= space;
			    space = 0;
			    kind = 1;
			    buflen -= space_buflen;
			    space_buflen = 0;
			} else {
			    col = 1;
			    kind = 2;
			    buflen = 1;
			}
			start = indent;
		    } else
			continue;
		}
		/* If we get here, we ended a line */
		n = (pt - tmp) - buflen;
		memcpy(cp, tmp, n);
		cp += n;
		if (kind == 1) {
		    /* replace the space */
		    cp[-1] = '\n';
		}
		if (kind == 2) {
		    /* need to insert a newline */
		    *cp++ = '\n';
		}
		memmove(tmp, tmp + n, buflen);
		pt = tmp + buflen;
		if (col || !at_end(i, num, k, lens)) {
		    memset(cp, ' ', indent);
		    cp += indent;
		    col += indent;
		}
	    }
	}
	memcpy(cp, tmp, pt - tmp);
	cp += pt - tmp;
	FREE_MSTR(tmp);
    } else {
	for (i = 0; i < num; i++) {
	    memcpy(cp, parts[i], lens[i]);
	    cp += lens[i];
	}
    }
    *cp = 0;
    FREE(lens);
    FREE(parts);
    if (savestr)
	FREE_MSTR(savestr);
    /* now we have what we want */
    pop_stack();
#ifdef DEBUG
    if (cp - deststr != j) {
	fatal("Length miscalculated in terminal_colour()\n    Expected: %i Was: %i\n    String: %s\n    Indent: %i Wrap: %i\n", j, cp - deststr, sp->u.string, indent, wrap);
    }
#endif
    free_string_svalue(sp);
    sp->type = T_STRING;
    sp->subtype = STRING_MALLOC;
    sp->u.string = deststr;
}
#endif

#ifdef F_PLURALIZE

#define PLURAL_SUFFIX  1
#define PLURAL_SAME    2
/* number to chop is added */
#define PLURAL_CHOP    2

static char *pluralize P1(char *, str) {
    char *pre, *rel, *end;
    char *p, *of_buf;
    int of_len = 0, plen, slen;
    int sz;

    /* default rule */
    int found = 0;
    char *suffix = "s";
    
    sz = strlen(str);
    if (sz == 0) return 0;

    /* if it is of the form 'X of Y', pluralize the 'X' part */
    if ((p = strstr(str, " of "))) {
	of_buf = alloc_cstring(p, "pluralize: of");
	of_len = strlen(of_buf);
	sz = p - str;
    }

    /*
     * first, get rid of determiners.  pluralized forms never have them ;)
     * They can have 'the' so don't remove that 
     */  
    if (str[0] == 'a' || str[0] == 'A') {
	if (str[1] == ' ') {
	    plen = sz - 2;
	    pre = DXALLOC(plen + 1, TAG_TEMPORARY, "pluralize: pre");
	    strncpy(pre, str + 2, plen);
	} else if (str[1] == 'n' && str[2] == ' ') {
	    plen = sz - 3;
	    pre = DXALLOC(plen + 1, TAG_TEMPORARY, "pluralize: pre");
	    strncpy(pre, str + 3, plen);
	} else {
	    plen = sz;
	    pre = DXALLOC(plen + 1, TAG_TEMPORARY, "pluralize: pre");
	    strncpy(pre, str, plen);
	}
    } else {
	plen = sz;
	pre = DXALLOC(plen + 1, TAG_TEMPORARY, "pluralize: pre");
	strncpy(pre, str, plen);
    }
    pre[plen] = 0;

    /*
     * only pluralize the last word, ie: lose adjectives.
     */
    if ((p = strrchr(pre, ' ')))
	rel = p + 1;
    else
	rel = pre;
	
    end = rel + strlen(rel);

    /*
     * trap the exceptions to the rules below and special cases.
     *
     * Hmm, maybe this should be a prebuilt hash table to make maintenance
     * a bit easier.  Possibly gperf based; or is that overkill? :-)
     */
    switch (rel[0]) {
    case 'A':
    case 'a':
	if (!strcasecmp(rel + 1, "re")) {
	    found = PLURAL_CHOP + 3;
	    suffix = "is";
	}
	break;
    case 'B':
    case 'b':
	if (!strcasecmp(rel + 1, "us")) {
	    found = PLURAL_SUFFIX;
	    suffix = "es";
	} else
	if (!strcasecmp(rel + 1, "onus")) {
	    found = PLURAL_SUFFIX;
	    suffix = "es";
	}
	break;
    case 'C':
    case 'c':
	if (!strcasecmp(rel + 1, "hild")) {
	    found = PLURAL_SUFFIX;
	    suffix = "ren";
	}
	break;
    case 'D':
    case 'd':
	if (!strcasecmp(rel + 1, "datum")) {
	    found = PLURAL_CHOP + 2;
	    suffix = "a";
	} else
	if (!strcasecmp(rel + 1, "ie")) {
	    found = PLURAL_CHOP + 1;
	    suffix = "ce";
	} else
	if (!strcasecmp(rel + 1, "eer")) {
	    found = PLURAL_SAME;
	} else
	if (!strcasecmp(rel + 1, "o")) {
	    found = PLURAL_SUFFIX;
	    suffix = "es";
        } else
	if (!strcasecmp(rel + 1, "ynamo"))
	    found = PLURAL_SUFFIX;
	break;
    case 'F':
    case 'f':
	if (!strcasecmp(rel + 1, "oot")) {
	    found = PLURAL_CHOP + 3;
	    suffix = "eet";
	    break;
	}
	if (!strcasecmp(rel + 1, "ish")) {
	    found = PLURAL_SAME;
	    break;
	}
	if (!strcasecmp(rel + 1, "forum")) {
	    found = PLURAL_CHOP + 2;
	    suffix = "a";
	    break;
	}
	if (!strcasecmp(rel + 1, "ife"))
	    found = PLURAL_SUFFIX;
	break;
    case 'G':
    case 'g':
	if (!strcasecmp(rel + 1, "oose")) {
	    found = PLURAL_CHOP + 4;
	    suffix = "eese";
	} else
	if (!strcasecmp(rel + 1, "o")) {
	    found = PLURAL_SUFFIX;
	    suffix = "es";
	} else
	if (!strcasecmp(rel + 1, "um")) {
	    found = PLURAL_SUFFIX;
	}
	break;
    case 'H':
    case 'h':
	if (!strcasecmp(rel + 1, "uman"))
	    found = PLURAL_SUFFIX;
	else if (!strcasecmp(rel + 1, "ave")) {
	    found = PLURAL_CHOP + 2;
	    suffix = "s";
	}	    
	break;
    case 'I':
    case 'i':
	if (!strcasecmp(rel + 1, "ndex")) {
	    found = PLURAL_CHOP + 2;
	    suffix = "ices";
	}
	break;
    case 'L':
    case 'l':
	if (!strcasecmp(rel + 1, "ouse")) {
	    found = PLURAL_CHOP + 4;
	    suffix = "ice";
	}
        if (!strcasecmp(rel + 1, "otus")) {
            found = PLURAL_SUFFIX;
            break;
        }
	break;
    case 'M':
    case 'm':
	if (!strcasecmp(rel + 1, "ackerel")) {
	    found = PLURAL_SAME;
	    break;
	}
	if (!strcasecmp(rel + 1, "oose")) {
	    found = PLURAL_SAME;
	    break;
	}
	if (!strcasecmp(rel + 1, "ouse")) {
	    found = PLURAL_CHOP + 4;
	    suffix = "ice";
	    break;
	}
	if (!strcasecmp(rel + 1, "atrix")) {
	    found = PLURAL_CHOP + 1;
	    suffix = "ces";
	}
	break;
    case 'O':
    case 'o':
	if (!strcasecmp(rel + 1, "x")) {
	    found = PLURAL_SUFFIX;
	    suffix = "en";
	}
	break;
    case 'P':
    case 'p':
        if (!strcasecmp(rel + 1, "ants"))
            found = PLURAL_SAME;
        break;
    case 'R':
    case 'r':
        if (!strcasecmp(rel + 1, "oof"))
            found = PLURAL_SUFFIX;
        break;
    case 'S':
    case 's':
        if (!strcasecmp(rel + 1, "niff")) {
	    found = PLURAL_SUFFIX;
	    break;
	}
	if (!strcasecmp(rel + 1, "heep")) {
	    found = PLURAL_SAME;
	    break;
	}
	if (!strcasecmp(rel + 1, "phinx")) {
	    found = PLURAL_CHOP + 1;
	    suffix = "ges";
	    break;
	}
	if (!strcasecmp(rel + 1, "taff")) {
	    found = PLURAL_CHOP + 2;
	    suffix = "ves";
	    break;
	}
	if (!strcasecmp(rel + 1, "afe")) {
	    found = PLURAL_SUFFIX;
	    break;
	}
	if (!strcasecmp(rel + 1, "haman")) 
	    found = PLURAL_SUFFIX;
	break;
    case 'T':
    case 't':
	if (!strcasecmp(rel + 1, "hief")) {
	    found = PLURAL_CHOP + 1;
	    suffix = "ves";
	    break;
	}
	if (!strcasecmp(rel + 1, "ooth")) {
	    found = PLURAL_CHOP + 4;
	    suffix = "eeth";
	}
	break;
    case 'V':
    case 'v':
	if (!strcasecmp(rel + 1, "ax")) {
	    found = PLURAL_SUFFIX;
	    suffix = "en";
	}
	if (!strcasecmp(rel + 1, "irus")) {
	    found = PLURAL_SUFFIX;
	    suffix = "es";
	}
	break;
    case 'W':
    case 'w':
	if (!strcasecmp(rel + 1, "as")) {
	    found = PLURAL_CHOP + 2;
	    suffix = "ere";
	}
	break;
    }
    /*
     * now handle "rules" ... god I hate english!!
     */
    /*
     * *x -> *xes (fox -> foxes)
     * *s -> *ses (pass -> passes)
     * *ch -> *ches (church -> churches)
     * *sh -> *shes (brush -> brushes)
     */
    /*
     * *fe -> *ves (knife -> knives)
     */
    /*
     * *f -> *ves (half -> halves)
     * *ef -> *efs (chef -> chefs) (really a rule for a special case)
     */
    /*
     * *y -> *ies (gumby -> gumbies)
     */
    /*
     * *us -> *i (cactus -> cacti)
     */
    /*
     * *man -> *men (foreman -> foremen)
     */
    /*
     * *is -> *es (this is from gordons pluralize ... )
     */
    /*
     * *o -> *s (also from gordon)
     */

    /* don't have to set found to PLURAL_SUFFIX in these rules b/c
       found == 0 is interpreted as PLURAL_SUFFIX */
    if (!found)
	switch (end[-1]) {
	case 'E': case 'e':
	    if (end[-2] == 'f' || end[-2] == 'F') {
		found = PLURAL_CHOP + 2;
		suffix = "ves";
	    }
	    break;
	case 'F': case 'f':
	    if (end[-2] == 'e' || end[-2] == 'E')
		break;
            if (end[-2] == 'f' || end[-2] == 'F')
	        found = PLURAL_CHOP + 2;
            else
                found = PLURAL_CHOP + 1;
	    suffix = "ves";
	    break;
	case 'H': case 'h':
	    if (end[-2] == 'c' || end[-2]=='s')
		suffix = "es";
	    break;
#if 0
	/*
	 * This rule is causing more problems than not.  As such, I'm removing
	 * it in favour of adding exceptions for words above that should use
	 * this rule.  I'm aware that this rule is proper for Latin derived
	 * English words, however its use has fallen out of common speech and
	 * writing for the majority of cases.  Currently known common exceptions
	 * are forum (fora) and datum (data).
	 * -- Marius, 23-Jun-2000
	 */
	case 'M': case 'm':
	    if (end[-2] == 'u') {
		found = PLURAL_CHOP + 2;
		suffix = "a";
	    }
	    break;
#endif
	case 'N': case 'n':
	    if (end[-2] == 'a' && end[-3] == 'm') {
		found = PLURAL_CHOP + 3;
		suffix = "men";
	    }
	    break;
	case 'O': case 'o':
	    if (end[-2] != 'o')
		suffix = "es";
	    break;
	case 'S': case 's':
	    if (end[-2] == 'i') {
		found = PLURAL_CHOP + 2;
		suffix = "es";
		break;
	    }
	    if (end[-2] == 'u') {
		found = PLURAL_CHOP + 2;
		suffix = "i";
		break;
	    }
	    if (end[-2] == 'a' || end[-2] == 'e' || end[-2] == 'o')
		suffix = "ses";
	    else
		suffix = "es";
	    break;
	case 'X': case 'x':
	    suffix = "es";
	    break;
	case 'Y': case 'y':
	    if (end[-2] != 'a' && end[-2] != 'e' && end[-2] != 'i'
		&& end[-2] != 'o' && end[-2] != 'u') {
		found = PLURAL_CHOP + 1;    
		suffix = "ies";
	    }
	    break;
	case 'Z': case 'z':
	    if (end[-2] == 'a' || end[-2] == 'e' || end[-2] == 'o'
		|| end[-2] == 'i' || end[-2] == 'u')
		suffix = "zes";
	    else
		suffix = "es";
    }

    switch (found) {
    case PLURAL_SAME:
	slen = 0;
	sz = plen + of_len;
	break;
    default:
	plen -= (found - PLURAL_CHOP);
	/* fallthrough */
    case 0:
    case PLURAL_SUFFIX:
	slen = strlen(suffix);
	sz = plen + slen + of_len;
	break;
    }

    p = new_string(sz, "pluralize");
    p[sz] = 0;
    
    strncpy(p, pre, plen);
    if (slen) 
	strncpy(p + plen, suffix, slen);
    if (of_len) {
	strcpy(p + plen + slen, of_buf);
	FREE(of_buf);
    }

    FREE(pre);
    return p;
} /* end of pluralize() */

void 
f_pluralize PROT((void))
{
   char *s;

   s = pluralize(sp->u.string);
   pop_stack();
   if (!s)
      push_number(0);
   else
      push_malloced_string(s);
}
#endif

#ifdef F_FILE_LENGTH
/*
 * file_length() efun, returns the number of lines in a file.
 * Returns -1 if no privs or file doesn't exist.
 */
static int file_length P1(char *, file)
{
  struct stat st;
  FILE *f;
  int ret = 0;
  int num;
  char buf[2049];
  char *p, *newp;

  file = check_valid_path(file, current_object, "file_size", 0);
  
  if (!file) return -1;
  if (stat(file, &st) == -1)
      return -1;
  if (st.st_mode & S_IFDIR)
      return -2;
  if (!(f = fopen(file, "r")))
      return -1;
  
  do {
      num = fread(buf, 1, 2048, f);
      p = buf - 1;
      while ((newp = memchr(p + 1, '\n', num))) {
	  num -= (newp - p);
	  p = newp;
	  ret++;
      }
  } while (!feof(f));

  fclose(f);
  return ret;
} /* end of file_length() */

void 
f_file_length PROT((void))
{
    int l;
    
    l = file_length(sp->u.string);
    pop_stack();
    push_number(l);
}
#endif

#ifdef F_UPPER_CASE
void
f_upper_case PROT((void))
{
    char *str;

    str = sp->u.string;
    /* find first upper case letter, if any */
    for (; *str; str++) {
	if (uislower(*str)) {
	    int l = str - sp->u.string;
	    unlink_string_svalue(sp);
	    str = sp->u.string + l;
	    *str = toupper((unsigned char)*str);
	    for (str++; *str; str++) {
		if (uislower((unsigned char)*str))
		    *str = toupper((unsigned char)*str);
	    }
	    return;
	}
    }
}
#endif

#ifdef F_REPLACEABLE
void f_replaceable PROT((void)) {
    object_t *obj;
    program_t *prog;
    int i, j, num, numignore, replaceable;
    char **ignore;
    
    if (st_num_arg == 2) {
	numignore = sp->u.arr->size;
	if (numignore)
	    ignore = CALLOCATE(numignore + 2, char *, TAG_TEMPORARY, "replaceable");
	else
	    ignore = 0;
        ignore[0] = findstring(APPLY_CREATE);
        ignore[1] = findstring(APPLY___INIT);
	for (i = 0; i < numignore; i++) {
	    if (sp->u.arr->item[i].type == T_STRING)
		ignore[i + 2] = findstring(sp->u.arr->item[i].u.string);
	    else
		ignore[i + 2] = 0;
	}
        numignore += 2;
        obj = (sp-1)->u.ob;
    } else {
	numignore = 2;
	ignore = CALLOCATE(2, char *, TAG_TEMPORARY, "replaceable");
	ignore[0] = findstring(APPLY_CREATE);
        ignore[1] = findstring(APPLY___INIT);
        obj = sp->u.ob;
    }
    
    prog = obj->prog;
    num = prog->num_functions_defined + prog->last_inherited;
    
    for (i = 0; i < num; i++) {
	if (prog->function_flags[i] & (FUNC_INHERITED | FUNC_NO_CODE)) continue;
	for (j = 0; j < numignore; j++)
	    if (ignore[j] == find_func_entry(prog, i)->name)
		break;
	if (j == numignore)
	    break;
    }

    replaceable = (i == num);
    if (obj == simul_efun_ob || prog->func_ref)
        replaceable = 0;

    if (st_num_arg == 2)
	free_array((sp--)->u.arr);
    FREE(ignore);
    free_svalue(sp, "f_replaceable");
    put_number(replaceable);
}
#endif

#ifdef F_PROGRAM_INFO
void f_program_info PROT((void)) {
    int func_size = 0;
    int string_size = 0;
    int var_size = 0;
    int inherit_size = 0;
    int prog_size = 0;
    int hdr_size = 0;
    int class_size = 0;
    int type_size = 0;
    int total_size = 0;
    object_t *ob;
    mapping_t *m;
    program_t *prog;
    int i, n;

    if (st_num_arg == 1) {
	ob = sp->u.ob;
	prog = ob->prog;
	if (!(ob->flags & (O_CLONE|O_SWAPPED))) {
	    hdr_size += sizeof(program_t);
	    prog_size += prog->program_size;
	    
	    /* function flags */
	    func_size += (prog->last_inherited +
			  prog->num_functions_defined) *sizeof(unsigned short); 
	         
	    /* definitions */
	    func_size += prog->num_functions_defined * 
	     sizeof(function_t);

	    string_size += prog->num_strings * sizeof(char *);
	    var_size += prog->num_variables_defined * (sizeof(char *) + sizeof(unsigned short));
	    inherit_size += prog->num_inherited * sizeof(inherit_t);
	    if (prog->num_classes)
		class_size += prog->num_classes * sizeof(class_def_t) + (prog->classes[prog->num_classes - 1].index + prog->classes[prog->num_classes - 1].size) * sizeof(class_member_entry_t);
	    type_size += prog->num_functions_defined * sizeof(short);
	    n = 0;
	    if (prog->type_start) {
		unsigned short *ts = prog->type_start;
		int nfd = prog->num_functions_defined;

		for (i = 0; i < nfd; i++) {
		    if (ts[i] == INDEX_START_NONE)
			continue;
		    n += prog->function_table[i].num_arg;
		}
	    }
	    type_size += n * sizeof(short);
	    total_size += prog->total_size;
	}
	pop_stack();
    } else {
	for (ob = obj_list; ob; ob = ob->next_all) {
	    if (ob->flags & (O_CLONE|O_SWAPPED)) continue;
	    prog = ob->prog;
	    hdr_size += sizeof(program_t);
	    prog_size += prog->program_size;

	    /* function flags */
	    func_size += (prog->last_inherited +
			  prog->num_functions_defined) << 1; 
	                  
	    /* definitions */
	    func_size += prog->num_functions_defined * 
	      sizeof(function_t);


	    string_size += prog->num_strings * sizeof(char *);
	    var_size += prog->num_variables_defined * (sizeof(char *) + sizeof(unsigned short));
	    inherit_size += prog->num_inherited * sizeof(inherit_t);
	    if (prog->num_classes)
		class_size += prog->num_classes * sizeof(class_def_t) + (prog->classes[prog->num_classes - 1].index + prog->classes[prog->num_classes - 1].size) * sizeof(class_member_entry_t);
	    type_size += prog->num_functions_defined * sizeof(short);
	    n = 0;
	    if (prog->type_start) {
		unsigned short *ts = prog->type_start;
		int nfd = prog->num_functions_defined;

		for (i = 0; i < nfd; i++) {
		    if (ts[i] == INDEX_START_NONE)
			continue;
		    n += prog->function_table[i].num_arg;
		}
	    }
	    type_size += n * sizeof(short);
	    total_size += prog->total_size;
	}
    }

    m = allocate_mapping(0);
    add_mapping_pair(m, "header size", hdr_size);
    add_mapping_pair(m, "code size", prog_size);
    add_mapping_pair(m, "function size", func_size);
    add_mapping_pair(m, "string size", string_size);
    add_mapping_pair(m, "var size", var_size);
    add_mapping_pair(m, "class size", class_size);
    add_mapping_pair(m, "inherit size", inherit_size);
    add_mapping_pair(m, "saved type size", type_size);

    add_mapping_pair(m, "total size", total_size);

    push_refed_mapping(m);
}
#endif

/* Magician - 08May95
 * int remove_interactive(object ob)
 * If the object isn't destructed and is interactive, then remove it's
 * interactivity and disconnect it.  (useful for exec()ing to an already
 * interactive object, ie, Linkdead reconnection)
 */

#ifdef F_REMOVE_INTERACTIVE
void f_remove_interactive PROT((void)) {
    if( (sp->u.ob->flags & O_DESTRUCTED) || !(sp->u.ob->interactive) ) {
	free_object(sp->u.ob, "f_remove_interactive");
	*sp = const0;
    } else {
        remove_interactive(sp->u.ob, 0);
	/* It may have been dested */
	if (sp->type == T_OBJECT)
	    free_object(sp->u.ob, "f_remove_interactive");
	*sp = const1;
    }
}
#endif

/* Zakk - August 23 1995
 * return the port number the interactive object used to connect to the
 * mud.
 */
#ifdef F_QUERY_IP_PORT
static int query_ip_port P1(object_t *, ob)
{
    if (!ob || ob->interactive == 0)
	return 0;
    return ob->interactive->local_port;
}    

void
f_query_ip_port PROT((void))
{
    int tmp;
    
    if (st_num_arg) {
	tmp = query_ip_port(sp->u.ob);
	free_object(sp->u.ob, "f_query_ip_port");
    } else {
	tmp = query_ip_port(command_giver);
	STACK_INC;
    }
    put_number(tmp);
}
#endif

/*
** John Viega (rust@lima.imaginary.com) Jan, 1996
** efuns for doing time zone conversions.  Much friendlier 
** than doing all the lookup tables in LPC.
** most muds have traditionally just used an offset of the 
** mud time or GMT, and this isn't always correct.
*/

#ifdef F_ZONETIME

char *
set_timezone (char * timezone)
{
  char put_tz[20];
  char *old_tz;

  old_tz = getenv("TZ");
  sprintf (put_tz, "TZ=%s", timezone);
  putenv (put_tz);
  tzset ();
  return old_tz;
}

void 
reset_timezone (char *old_tz)
{
  int  i = 0;
  int  env_size = 0;
  char put_tz[20];

  if (!old_tz)
    {
      while (environ[env_size] != NULL)
        {
          if (strlen (environ[env_size]) > 3 && environ[env_size][2] == '='
             && environ[env_size][1] == 'Z' && environ[env_size][0] == 'T')
            {
              i = env_size;
            }
          env_size++;
        }
      if ((i+1) == env_size)
        {
          environ[i] = NULL;
        }
      else
        {
          environ[i] = environ[env_size-1];
          environ[env_size-1] = NULL;
        }
    }
  else
    {
      sprintf (put_tz, "TZ=%s", old_tz);
      putenv (put_tz);
    }
  tzset ();
}

void 
f_zonetime PROT((void))
{
  char *timezone, *old_tz;
  char *retv;
  int  time_val;
  int  len;
  
  time_val   = sp->u.number;
  pop_stack ();
  timezone   = sp->u.string;
  pop_stack ();

  old_tz = set_timezone (timezone);
  retv = ctime ((time_t *)&time_val);
  len  = strlen (retv);
  retv[len-1] = '\0';
  reset_timezone (old_tz);
  push_malloced_string (string_copy(retv, "zonetime"));
  
}
#endif

#ifdef F_IS_DAYLIGHT_SAVINGS_TIME
void
f_is_daylight_savings_time PROT((void))
{
  struct tm *t;
  int       time_to_check;
  char      *timezone;
  char      *old_tz;

  time_to_check = sp->u.number;
  pop_stack ();
  timezone = sp->u.string;
  pop_stack ();

  old_tz = set_timezone (timezone);
 
  t = localtime ((time_t *)&time_to_check);

  push_number ((t->tm_isdst) > 0);

  reset_timezone (old_tz);
}
#endif

#ifdef F_DEBUG_MESSAGE
void f_debug_message PROT((void)) {
    debug_message("%s\n", sp->u.string);
    free_string_svalue(sp--);
}
#endif

#ifdef F_FUNCTION_OWNER
void f_function_owner PROT((void)) {
    object_t *owner = sp->u.fp->hdr.owner;
    
    free_funp(sp->u.fp);
    put_unrefed_object(owner, "f_function_owner");
}
#endif

#ifdef F_REPEAT_STRING
void f_repeat_string PROT((void)) {
    char *str;
    int repeat, len, newlen;
    char *ret, *p;
    int i;
    
    repeat = (sp--)->u.number;    
    if (repeat > 0) {
	str = sp->u.string;
	len = SVALUE_STRLEN(sp);
        if ((newlen = len * repeat) > max_string_length)
            repeat = max_string_length / len;
    }
    if (repeat <= 0) {
	free_string_svalue(sp);
	sp->type = T_STRING;
	sp->subtype = STRING_CONSTANT;
	sp->u.string = "";
    } else if (repeat != 1) {
	p = ret = new_string(newlen, "f_repeat_string");
	for (i = 0; i < repeat; i++) {
	    memcpy(p, str, len);
	    p += len;
	}
	*p = 0;
	free_string_svalue(sp);
	sp->type = T_STRING;
	sp->subtype = STRING_MALLOC;
	sp->u.string = ret;
    }
}
#endif

#ifdef F_MEMORY_SUMMARY
static int memory_share PROT((svalue_t *));

static int node_share P3(mapping_t *, m, mapping_node_t *, elt, void *, tp) {
    int *t = (int *)tp;
    
    *t += sizeof(mapping_node_t) - 2*sizeof(svalue_t);
    *t += memory_share(&elt->values[0]);
    *t += memory_share(&elt->values[1]);

    return 0;
}

static int memory_share P1(svalue_t *, sv) {
    int i, total = sizeof(svalue_t);
    int subtotal;
    static int depth = 0;
    
    switch (sv->type) {
    case T_STRING:
	switch (sv->subtype) {
	case STRING_MALLOC:
	    return total + 
		(1 + COUNTED_STRLEN(sv->u.string) + sizeof(malloc_block_t))/
		(COUNTED_REF(sv->u.string));
	case STRING_SHARED:
	    return total + 
		(1 + COUNTED_STRLEN(sv->u.string) + sizeof(block_t))/
		(COUNTED_REF(sv->u.string));
	}
	break;
    case T_ARRAY:
    case T_CLASS:
        if (++depth > 100)
            return 0;

	/* first svalue is stored inside the array struct, so sizeof(array_t)
	 * includes one svalue.
	 */
	subtotal = sizeof(array_t) - sizeof(svalue_t);
	for (i = 0; i < sv->u.arr->size; i++)
	    subtotal += memory_share(&sv->u.arr->item[i]);
        depth--;
	return total + subtotal/sv->u.arr->ref;
    case T_MAPPING:
        if (++depth > 100)
            return 0;
	subtotal = sizeof(mapping_t);
	mapTraverse(sv->u.map, node_share, &subtotal);
        depth--;
	return total + subtotal/sv->u.map->ref;
    case T_FUNCTION:
    {
	svalue_t tmp;
	tmp.type = T_ARRAY;
	tmp.u.arr = sv->u.fp->hdr.args;

        if (++depth > 100)
            return 0;

	if (tmp.u.arr)
	    subtotal = sizeof(funptr_hdr_t) + memory_share(&tmp) - sizeof(svalue_t);
	else
	    subtotal = sizeof(funptr_hdr_t);
	switch (sv->u.fp->hdr.type) {
	case FP_EFUN:
	    subtotal += sizeof(efun_ptr_t);
	    break;
	case FP_LOCAL | FP_NOT_BINDABLE:
	    subtotal += sizeof(local_ptr_t);
	    break;
	case FP_SIMUL:
	    subtotal += sizeof(simul_ptr_t);
	    break;
	case FP_FUNCTIONAL:
	case FP_FUNCTIONAL | FP_NOT_BINDABLE:
	    subtotal += sizeof(functional_t);
	    break;
	}
        depth--;
	return total + subtotal/sv->u.fp->hdr.ref;
    }
#ifndef NO_BUFFER_TYPE
    case T_BUFFER:
	/* first byte is stored inside the buffer struct */
	return total + (sizeof(buffer_t) + sv->u.buf->size - 1)/sv->u.buf->ref;
#endif
    }
    return total;
}


/*
 * The returned mapping is:
 * 
 * map["program name"]["variable name"] = memory usage
 */
#ifdef F_MEMORY_SUMMARY
static void fms_recurse P4(mapping_t *, map, object_t *, ob, 
			   int *, idx, program_t *, prog) {
    int i;
    svalue_t *entry;
    svalue_t sv;
    
    sv.type = T_STRING;
    sv.subtype = STRING_SHARED;

    for (i = 0; i < prog->num_inherited; i++)
	fms_recurse(map, ob, idx, prog->inherit[i].prog);

    for (i = 0; i < prog->num_variables_defined; i++) {
	int size = memory_share(ob->variables + *idx + i);
	
	sv.u.string = prog->variable_table[i];
	entry = find_for_insert(map, &sv, 0);
	entry->u.number += size;
    }
    *idx += prog->num_variables_defined;
}

void f_memory_summary PROT((void)) {
    mapping_t *result = allocate_mapping(8);
    object_t *ob;
    int idx;
    svalue_t sv;
    
    sv.type = T_STRING;
    sv.subtype = STRING_SHARED;
    
    for (ob = obj_list; ob; ob = ob->next_all) {
	svalue_t *entry;
	
	if (ob->flags & O_SWAPPED) 
	    load_ob_from_swap(ob);

	sv.u.string = ob->prog->name;
	entry = find_for_insert(result, &sv, 0);
	if (entry->type == T_NUMBER) {
	    entry->type = T_MAPPING;
	    entry->u.map = allocate_mapping(8);
	}
	idx = 0;
	fms_recurse(entry->u.map, ob, &idx, ob->prog);
    }
    push_refed_mapping(result);
}
#endif

#endif

/* Marius */
#ifdef F_QUERY_REPLACED_PROGRAM
void f_query_replaced_program PROT((void))
{
    char *res = 0;

    if (st_num_arg)
    {
        if (sp->u.ob->replaced_program)
            res = add_slash(sp->u.ob->replaced_program);
        free_object(sp->u.ob, "f_query_replaced_program");
    }
    else
    {
        if (current_object->replaced_program)
            res = add_slash(sp->u.ob->replaced_program);
        STACK_INC;
    }

    if (res) {
        put_malloced_string(res);
    } else {
        put_number(0);
    }
}
#endif

/* Skullslayer@Realms of the Dragon */
#ifdef F_NETWORK_STATS
void f_network_stats PROT((void))
{
    mapping_t *m;
    int i, ports = 0;

    for (i = 0;  i < 5;  i++)
	if (external_port[i].port)
	    ports += 4;

#ifndef PACKAGE_SOCKETS
    m = allocate_mapping(ports + 4);
#else
    m = allocate_mapping(ports + 8);
#endif

    add_mapping_pair(m, "incoming packets total", inet_in_packets);
    add_mapping_pair(m, "incoming volume total", inet_in_volume);
    add_mapping_pair(m, "outgoing packets total", inet_out_packets);
    add_mapping_pair(m, "outgoing volume total", inet_out_volume);

#ifdef PACKAGE_SOCKETS
    add_mapping_pair(m, "incoming packets sockets", inet_socket_in_packets);
    add_mapping_pair(m, "incoming volume sockets", inet_socket_in_volume);
    add_mapping_pair(m, "outgoing packets sockets", inet_socket_out_packets);
    add_mapping_pair(m, "outgoing volume sockets", inet_socket_out_volume);
#endif

    if (ports) {
    	for (i = 0;  i < 5;  i++) {
    	    if (external_port[i].port) {
    		char buf[30];

    		sprintf(buf, "incoming packets port %d", external_port[i].port);
		add_mapping_pair(m, buf, external_port[i].in_packets);
		sprintf(buf, "incoming volume port %d", external_port[i].port);
    		add_mapping_pair(m, buf, external_port[i].in_volume);
		sprintf(buf, "outgoing packets port %d", external_port[i].port);
		add_mapping_pair(m, buf, external_port[i].out_packets);
		sprintf(buf, "outgoing volume port %d", external_port[i].port);
		add_mapping_pair(m, buf, external_port[i].out_volume);
	    }
	}
    }

    push_refed_mapping(m);
}
#endif
