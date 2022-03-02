#include <stdio.h>
#include <stdlib.h>
#include "gcc-common.h"
#include "tree.h"
#include "c-tree.h"
#include "cgraph.h"
#include "ipa-chkp.h"

#define FIXED_TEXT_SECTION_NAME ".fixed.text"
#define FIXED_DATA_SECTION_NAME ".fixed.data"
#define FIXED_RODATA_SECTION_NAME ".fixed.rodata"

char generated_str_name[] = "gen_str_cst_";
static int generated_str_counter = 0;

#define DEBUG_DIR "/home/cat/tmp/"
#define debug_filename(a) DEBUG_DIR a

#define concat(a,b) a b

#define REAL_FN_NAME_SUFFIX "real"

#define OUTPUT_INSN(str, file) fputs("\t", file); fputs(str, file); fputs(";\n", file)
#define OVERWRITE_INSN(insn_len) fseek(file, -insn_len, SEEK_CUR);

#define PRINT_DEBUG 1

#define OUTPUT(filename, str, args...) \
    if(PRINT_DEBUG) { \
    	FILE *out_file = fopen(debug_filename(filename), "a");   \
    	fprintf(out_file, str, args);  \
		fclose(out_file); \
    } \
	
#define DEBUG_OUTPUT(str, args...) OUTPUT("all-debug.txt", str, args)
#define OUTPUT_WRAPPED_FUNCTION(str, args...) OUTPUT("wrapped-functions.txt", str, args)
#define OUTPUT_IGNORED_FUNCTION(str, args...) OUTPUT("ignored-functions.txt", str, args)
#define OUTPUT_FIXED_VAR(str, args...) OUTPUT("fixed-structs.txt", str, args)
#define OUTPUT_STR_CONST(str, args...) OUTPUT("string-contants.txt", str, args)

/* All plugins must export this symbol so that they can be linked with
   GCC license-wise.  */
int plugin_is_GPL_compatible;


static struct plugin_info rerandomization_wrapper_plugin_info = {
        .version    = "1",
        .help        = "Wrap functions and variables so module can be rerandomized\n",
};


/*******************************************************************************************************************/
/* Function for hash table, probably move into separate file */
/* Need to be able to keep a record of all newly created functions (the wrappers of the real functions)
 * Create hash to store these
 * GCC provides hashing functions but was having issues trying to use them (think due to things getting garbage collected?)
 * This works for now but should probably replace this way of doing this */

int size_of_table = 50;

typedef struct _list_t_ {
    const char *str;
    tree fndecl;
    gimple stmt;
    int i;
    tree replace;
    struct _list_t_ *next;
} list_t;

typedef struct _hash_table_t_ {
    int size;
    list_t **table;
} hash_table_t;

hash_table_t *create_hash_table(int size)
{
    hash_table_t *new_table;
    if (size<1) return NULL;

    if ((new_table = ((hash_table_t *)xmalloc(sizeof(hash_table_t *)))) == NULL) {
        return NULL;
    }

    if ((new_table->table = ((list_t **)xmalloc(sizeof(list_t *) * size))) == NULL) {
        return NULL;
    }

    int i = 0;
    for(i=0; i<size_of_table; i++) new_table->table[i] = NULL;

    new_table->size = size;
    return new_table;
}

hash_table_t *wrapper_function_hash_table = create_hash_table(size_of_table);
hash_table_t *real_function_hash_table = create_hash_table(size_of_table);

/* strdup is poisoned so use this version of strdup with xmalloc */
char * strdup_(const char *s)
{
    size_t len = strlen (s) + 1;
    char *result = (char*) xmalloc (len);
    if (result == (char*) 0)
        return (char*) 0;
    return (char*) memcpy (result, s, len);
}

unsigned int hash(hash_table_t *hashtable, const char *str)
{
    unsigned int hashval;
    hashval = 0;
    for(; *str != '\0'; str++) hashval = *str + (hashval << 5) - hashval;
    return hashval % hashtable->size;
}

list_t *lookup_key(hash_table_t *hashtable, const char *str)
{
    list_t *list;
    unsigned int hashval = hash(hashtable, str);

    for(list = hashtable->table[hashval]; list != NULL; list = list->next) {
        if (strcmp(str, list->str) == 0) return list;
    }

    return NULL;
}

int add_entry(hash_table_t *hashtable, const char *str, tree fndecl, gimple stmt, int i, tree replace)
{
    list_t *new_list;
    list_t *current_list;
    unsigned int hashval = hash(hashtable, str);

    if ((new_list = (list_t *)xmalloc(sizeof(list_t))) == NULL) return 1;
    current_list = lookup_key(hashtable, str);
    if (current_list != NULL) return 2;

    new_list->str = strdup_(str);
    new_list->fndecl = fndecl;
    new_list->next = hashtable->table[hashval];
    new_list->stmt = stmt;
    new_list->i = i;
    new_list->replace = replace;
    hashtable->table[hashval] = new_list;

    return 0;
}

void
insert_wrapper_function(const char * name, tree fndecl, gimple stmt, int i, tree replace)
{
    add_entry(wrapper_function_hash_table, name, fndecl, stmt, i, replace);
}

list_t * 
get_wrapper_function (const char * name)
{
    list_t * entry = lookup_key(wrapper_function_hash_table, name);
    if (entry && entry->fndecl) {
        return entry; //->fndecl;
    }
    return NULL;
}

bool wrapper_function_exists(const char * name)
{
    return lookup_key(wrapper_function_hash_table, name);
}

/* END HASH TABLE FUNCTIONS */
/*******************************************************************************************************************/


bool starts_with(const char *pre, const char *str)
{
    size_t lenpre = strlen(pre),
            lenstr = strlen(str);
    return lenstr < lenpre ? false : memcmp(pre, str, lenpre) == 0;
}

bool str_equals(const char *str1, const char *str2)
{
    return (strcmp(str1, str2) == 0);
}

/* Determine whether the definition of node is contained within module */
/* This seems like rough way to do this, but didn't see an alternative, and this seems to be kind of like what other plugins have done. */
static bool is_node_decl_in_module(tree node) {
    if (DECL_BUILT_IN(node)) { //if (DECL_BUILT_IN_CLASS(node) != BUILT_IN_NORMAL) {
        return false;
    }
    else if (node && DECL_SOURCE_LOCATION(node)) {
        expanded_location xloc = expand_location(DECL_SOURCE_LOCATION(node));
        return !(starts_with("./", xloc.file));
    }
    return false;
}

// Return true if gimple is both a call statement and the call is to a function defined outside the module
static bool is_call_to_fn_outside_module(gimple stmt) {
    return is_gimple_call(stmt) && gimple_call_fndecl(stmt) && !is_node_decl_in_module(gimple_call_fndecl(stmt));
}


# define ASM_FORMAT_USE_UNDERSCORE(OUTPUT, NAME) \
  do { const char *const name_ = (NAME); \
       char *const output_ = (OUTPUT) = \
         (char *) alloca (strlen (name_) + 32); \
       sprintf (output_, "%s", name_); \
  } while (0)

/* Return a new assembler name for a clone of DECL with SUFFIX, using underscore to concatenate  */
char * clone_function_name_with_underscore(const char *name, const char *suffix)
{
    size_t len = strlen (name);
    char *tmp_name, *prefix;
    prefix = XALLOCAVEC (char, len + strlen (suffix) + 2);
    memcpy (prefix, name, len);
    strcpy (prefix + len + 1, suffix);
    prefix[len] = '.';
    ASM_FORMAT_USE_UNDERSCORE(tmp_name, prefix);
    return tmp_name;
}

char * get_real_function_name(tree decl)
{
    tree name = DECL_ASSEMBLER_NAME(decl);
    return clone_function_name_with_underscore(IDENTIFIER_POINTER(name), REAL_FN_NAME_SUFFIX);
}

tree get_real_function_tree(tree decl)
{
    char * real_name = get_real_function_name(decl);
    return get_identifier(real_name);
}

tree build_wrapper_function(tree fndecl) {
	
	if (DECL_DECLARED_INLINE_P(fndecl)) {
		 fprintf(stderr, "INLINE function: %s\n", get_name(fndecl));
		 return NULL_TREE;
	}
	fprintf(stderr,"Building wrapper for function: %s\n", get_name(fndecl));
	
    OUTPUT_WRAPPED_FUNCTION("%s\n", get_name(fndecl));
   
    struct cgraph_node * node = cgraph_node::get(fndecl); 


    // TODO: think building of replace_tree can be removed? Make sure still works if replace_trees is null when passed to clone
    vec<ipa_replace_map *, va_gc> *replace_trees = NULL;
    //vec<ipa_replace_map *, va_gc> *replace_trees2 = NULL;
//    struct ipa_replace_map *replace_map;
//    replace_map = ggc_alloc<ipa_replace_map> ();
//    replace_map->old_tree = fndecl; //node->decl;
//    replace_map->new_tree = NULL;
//    replace_map->replace_p = true;
//    vec_safe_push (replace_trees, replace_map);

  //  vec<cgraph_edge *> redirect_callers = vec<cgraph_edge * >();
   // vec_safe_push (redirect_callers, node->callers);

    // Create a cloned version of the outer wrapping function that will wrap the inner _real function
   // tree name = clone_function_name(fndecl, "real");
   // cgraph_node *clone2 = cgraph_node::create(fndecl); //node->create_virtual_clone(vec<cgraph_edge * >(), replace_trees, NULL, "real"); // 
 //   cgraph_node *clone = node->create_virtual_clone(node->collect_callers(), replace_trees, NULL, "base");
    cgraph_node *clone2 = node->create_virtual_clone(node->collect_callers(), replace_trees, NULL, REAL_FN_NAME_SUFFIX); //vec<cgraph_edge * >()
      
    DECL_ARTIFICIAL(clone2->decl) = 1;
    DECL_PRESERVE_P(clone2->decl) = 1;
    TREE_USED(clone2->decl) = 1;
    DECL_UNINLINABLE(clone2->decl) = 1;
  
   // TREE_PUBLIC(fndecl) = 1;
   // TREE_PUBLIC(clone2->decl) = 1;
    TREE_STATIC(fndecl) = 0;
   TREE_STATIC(clone2->decl) = 0;
    //DECL_EXTERNAL(clone2->decl) = 0;

    
   // gimple_set_body (clone->decl, NULL);
    gimple_set_body (clone2->decl, NULL);
    gimple_set_body (fndecl, NULL);
    
  //  tree naked_attr = tree_cons(get_identifier("weakref"), NULL, NULL);
	//decl_attributes(&clone2->decl, naked_attr, 0);
   
   
  
    // Make sure cloned function gets added and output
    node->force_output = true;
    clone2->force_output = true;

    symtab->materialize_all_clones();
    
    // Make original function hidden and call it _real
//    change_decl_assembler_name(fndecl, get_real_function_name(fndecl));
    DECL_VISIBILITY(fndecl) = VISIBILITY_DEFAULT;
	//TREE_PUBLIC(fndecl) = 1;
	//DECL_EXTERNAL(fndecl) = 1;
	//DECL_COMDAT(fndecl) = 1;
	//DECL_WEAK(fndecl) = 1;
	
	//node->thunk.thunk_p = true;
    set_decl_section_name(fndecl, FIXED_TEXT_SECTION_NAME);
    
    set_decl_section_name(clone2->decl, ".text"); //FIXED_TEXT_SECTION_NAME);
    change_decl_assembler_name(clone2->decl, get_real_function_tree(fndecl));

	
    fprintf(stderr,"DONE: Building wrapper for function: %s\n", get_name(fndecl));
    return clone2->decl;
}

static void mark_function_for_wrap(tree t, gimple stmt, int i, tree replace) {
	const char * current_function_name = DECL_NAME_POINTER(t);
	if (str_equals(current_function_name, "randomize_module") 
			|| str_equals(current_function_name, "e1000e_rerandomize") 
			|| str_equals(current_function_name, "_e1000e_rerandomize")) {
		return;
	}
	
	set_decl_section_name(t, FIXED_TEXT_SECTION_NAME);
	insert_wrapper_function(get_name(t), t, stmt, i, replace);
	
//	const char * current_function_name = DECL_NAME_POINTER(t);
//	list_t * entry = get_wrapper_function(current_function_name);
//	if(!entry) {
//	    fprintf(stderr, "NO WRAPPER FOR: %s\n", current_function_name);
//
//	    tree node_to_replace = build_wrapper_function(t);
//	    if (node_to_replace) {
//	    	
//	    }
//	    
//	    
//	} 
    
}

static bool is_var_already_wrapped(tree var_decl) {
    const char * section_name = DECL_SECTION_NAME(var_decl);
    return (section_name) ? str_equals(section_name, FIXED_RODATA_SECTION_NAME) || str_equals(section_name, FIXED_DATA_SECTION_NAME) : false;
}
/* Add section attribute to variable so it is kept in .fixed.data or .fixed.rodata */
static void add_section_attributes_for_var(tree var_decl) {
	if (is_var_already_wrapped(var_decl)) {
		return;
	}
    if (TREE_READONLY(var_decl) == 1) {
        DEBUG_OUTPUT("Placing in fixed.rodata: %s\n", DECL_NAME_POINTER(var_decl));
        OUTPUT_FIXED_VAR("%s (%s)\n", DECL_NAME_POINTER(var_decl), FIXED_RODATA_SECTION_NAME);
        set_decl_section_name(var_decl, FIXED_RODATA_SECTION_NAME);
    } else {
        DEBUG_OUTPUT("Placing in fixed.data: %s\n", DECL_NAME_POINTER(var_decl));
        OUTPUT_FIXED_VAR("%s (%s)\n", DECL_NAME_POINTER(var_decl), FIXED_DATA_SECTION_NAME);
        set_decl_section_name(var_decl, FIXED_DATA_SECTION_NAME);
    }
}


// Determine if argument to call is a string constant, lots of confusing logic here to determine this
static bool is_call_arg_a_string_constant(tree call_arg) {
    if (call_arg != NULL_TREE && !get_name(call_arg)) {
        tree type = TREE_TYPE(call_arg);

        return (TREE_CODE(call_arg) == ADDR_EXPR && TREE_CODE(type) == POINTER_TYPE &&
                TREE_OPERAND_LENGTH(call_arg) == 1 && TREE_CODE(TREE_OPERAND(call_arg, 0)) == STRING_CST);
    }
    return false;

}

// TODO: Clean this up
static char * build_string_var_name(const char * current_function_name) {
    char gen_str_counter[5];
    sprintf(gen_str_counter, "%d", generated_str_counter++);

    int new_str_len = strlen(generated_str_name) + strlen(current_function_name) +  6;
    char *new_str_name = (char *) xmalloc(sizeof(char) * new_str_len);
    strcpy(new_str_name, generated_str_name);
    strcat(new_str_name, current_function_name);
    strcat(new_str_name, "_");
    strcat(new_str_name, gen_str_counter);

    //DEBUG_OUTPUT("Gen str name: %s\n", new_str_name);
    return new_str_name;

}

static const_tree get_str_cst(const_tree node)
{
    const_tree str = node;

    /* Filter out types we are ignoring */
    if (TREE_CODE(str) == VAR_DECL)
    {
        if (!(str = DECL_INITIAL(node))) /* nop expr  */
            return NULL_TREE;
        else if (TREE_CODE(str) == INTEGER_CST) /* Ignore single chars */
            return NULL_TREE;

        str = TREE_OPERAND(str, 0); /* addr expr */
    }
    else if (TREE_CODE(str) == ADDR_EXPR)
        str = TREE_OPERAND(str, 0);

    /* We only deal with readonly stuff */
    if (!TYPE_READONLY(str) && (TREE_CODE(str) != ARRAY_REF))
        return NULL_TREE;

    if (TREE_CODE(str) != STRING_CST)
        str = TREE_OPERAND(str, 0);

    if (TREE_CODE(str) != STRING_CST)
        return NULL_TREE;
    else
        return str;
}

static tree get_var(tree node)
{
    tree str = node;

    /* Filter out types we are ignoring */
    if (TREE_CODE(str) == VAR_DECL)
    {
        if (!(str = DECL_INITIAL(node))) /* nop expr  */
            return NULL_TREE;
        else if (TREE_CODE(str) == INTEGER_CST) /* Ignore single chars */
            return NULL_TREE;

        str = TREE_OPERAND(str, 0); /* addr expr */
    }
    else if (TREE_CODE(str) == ADDR_EXPR) // || TREE_CODE(str) == SSA_NAME)
        str = TREE_OPERAND(str, 0);

    /* We only deal with readonly stuff */
//    if (!TYPE_READONLY(str) && (TREE_CODE(str) != ARRAY_REF))
//        return NULL_TREE;

//    if (TREE_CODE(str) != STRING_CST)
//        str = TREE_OPERAND(str, 0);

//    if (TREE_CODE(str) != STRING_CST)
//        return NULL_TREE;
   
    return str;
}



static tree
build_str_decl(tree id, const char * str)
{
    location_t loc = UNKNOWN_LOCATION;
    tree decl, type, init;
    size_t length = strlen (str);
    type = build_array_type (char_type_node,
                             build_index_type (size_int (length)));
    type = c_build_qualified_type (type, TYPE_QUAL_CONST);
    decl = build_decl (loc, VAR_DECL, id, type);
    TREE_STATIC (decl) = 1;
    TREE_PUBLIC(decl) = 1;
    DECL_PRESERVE_P(decl) = 1;
    TREE_READONLY (decl) = 1;
    TREE_USED(decl) = 1;
    DECL_EXTERNAL(decl) = 0;
    init = build_string (length + 1, str);
    TREE_TYPE (init) = type;
    DECL_INITIAL (decl) = init;
    TREE_USED (decl) = 1;
    if (current_function_decl) {
        DECL_CONTEXT (decl) = current_function_decl;
    }
    finish_decl (decl, loc, init, NULL_TREE, NULL_TREE);
    return decl;
}

// Build a string variable decl tree
static tree build_string_var_decl(tree call_arg, const char * str, const char * current_function_name) {
    char * var_name = build_string_var_name(current_function_name);
    tree id = get_identifier(var_name);

    tree mdecl = build_str_decl(id, str);
    
    OUTPUT_STR_CONST("%s for %s in function %s\n", DECL_NAME_POINTER(mdecl), str, current_function_name);

    varpool_add_new_variable(mdecl);
    set_decl_section_name(mdecl, FIXED_RODATA_SECTION_NAME);

    return build_fold_addr_expr(mdecl);
}

static bool is_struct_type(tree node) {
    return TREE_CODE(TREE_TYPE(node)) == RECORD_TYPE || TREE_CODE(TREE_TYPE(node)) == UNION_TYPE;
}
static bool is_struct_type2(tree node) {
    return TREE_CODE(node) == RECORD_TYPE || TREE_CODE(node) == UNION_TYPE;
}


static bool is_function_already_wrapped(tree fn_decl) {
    const char * section_name = DECL_SECTION_NAME(fn_decl);
    return (section_name) ? str_equals(section_name, FIXED_TEXT_SECTION_NAME)  : false;
}

static void wrap_struct(tree field) {
	DEBUG_OUTPUT("WRAP Struct Field: %s\n", get_name(field));
	if (is_node_decl_in_module(field)) {
			DEBUG_OUTPUT("Struct Field: %s\n", get_name(field));
			tree substruct = TREE_TYPE(field);
			//add_section_attributes_for_var(substruct);
			tree field2;
			unsigned long j;
			for (field2 = TYPE_FIELDS(substruct), j= 0; field2; field2 = TREE_CHAIN(field2), j++) {
				DEBUG_OUTPUT("Sub-Struct Field: %s\n", get_name(field2));
				if (is_node_decl_in_module(field2)) {
					switch(TREE_CODE(TREE_TYPE(field2))) {
						case ARRAY_TYPE:
							DEBUG_OUTPUT("Sub-Struct Field was array type: %s\n", get_name(field2));
							break;
							
						case UNION_TYPE:
						case RECORD_TYPE:
						
							wrap_struct(field2);
							break;
						case POINTER_TYPE:
							DEBUG_OUTPUT("Sub-Struct Field was pointer type: %s\n", get_name(field2));
							//debug_tree(TREE_TYPE(TREE_TYPE(field2)));
							
							if (is_struct_type(TREE_TYPE(field2))) {
								//wrap_struct(TREE_TYPE(field2));
							}
							else {
								if (TREE_CODE(TREE_TYPE(TREE_TYPE(field2))) == FUNCTION_TYPE) {
								//	DEBUG_OUTPUT("FUNCTION FOR: %s\n", get_name(field2));
									debug_tree(TREE_VALUE(field2));
									tree field3;
									unsigned long k;
									for (field3 = substruct, k= 0; field3; field3 = TREE_CHAIN(field3), k++) {
										if (TREE_CODE(field3) == FUNCTION_DECL) {
											if (is_node_decl_in_module(field3)) {
												//DEBUG_OUTPUT("ATTR FUNCTION FOR: %s\n", get_name(field3));
												debug_tree(field3);
											
												mark_function_for_wrap(field3, NULL, 0, field3);
											}
										}
									}
									
									
								}

							}
							
							// Update function pointer to wrapper function
						   // field2 = TREE_TYPE(wrapper_fn);
							
							break;
	//
	//					default:
	//						break;
					}
				}
				
			}
	}
	//	}
}

/* Place variable in fixed text section and iterate over structs to find nested structs or function pointers that might also need to be wrapped */
static void wrap_var(tree var_decl) {

	  tree field3;
	unsigned long k;
   
  //   if ( is_struct_type(var_decl)) { // && TREE_CODE(DECL_CONTEXT(var_decl)) == TRANSLATION_UNIT_DECL) { //is_global_var(var_decl) &&
    	 

	  tree initial_var_decl = get_var(TREE_TYPE(TREE_TYPE(var_decl)));
	  for (field3 = initial_var_decl, k= 0; field3; field3 = TREE_CHAIN(field3), k++) {
		  	  
				if (TREE_CODE(field3) == FUNCTION_DECL) {
					if (is_node_decl_in_module(field3)) {
						//DEBUG_OUTPUT("ATTR FUNCTION FOR: %s\n", get_name(field3));
						//debug_tree(field3);
						mark_function_for_wrap(field3, NULL, 0, field3);
					}
				}
				else if (TREE_CODE(field3) == VAR_DECL){
					if (is_node_decl_in_module(field3)) {
						//DEBUG_OUTPUT("ATTR FOR: %s\n", get_name(field3));
						//debug_tree(field3);
						add_section_attributes_for_var(field3);
					}
				}
			}	
}


static void do_execute() {

    basic_block bb;
    gimple_stmt_iterator gsi;

    const char *current_function_name = DECL_NAME_POINTER(current_function_decl);
    
     //if (strstr(current_function_name, "fuse_dev_init") || 
//		     strstr(current_function_name, "fuse_free_conn") 
//		     ) {
  //      	  DEBUG_OUTPUT("SKIP Analyzing function: %s\n", current_function_name);
    //    	  OUTPUT_IGNORED_FUNCTION("%s\n", current_function_name);
      //  	  return;
     //}

    
    DEBUG_OUTPUT("Analyzing function: %s\n", current_function_name);
    
    list_t * entry = get_wrapper_function(current_function_name);
    if(entry) {
    	DEBUG_OUTPUT("FOUND WRAPPER FOR: %s\n", current_function_name);

    	tree node_to_replace = build_wrapper_function(entry->fndecl);
    	if (node_to_replace) {
    		add_entry(real_function_hash_table, current_function_name, node_to_replace, NULL, 0, NULL_TREE);
    	}
    	    
    	    
//		/* Find all entries for this function */
//		list_t *list;
//		unsigned int hashval = hash(wrapper_function_hash_table, current_function_name);
//
//		for(list = wrapper_function_hash_table->table[hashval]; list != NULL; list = list->next) {
//			if (strcmp(current_function_name, list->str) == 0) {
//				if (list->stmt) {
//					fprintf(stderr, "REPLACE GIMPLE: %s\n", current_function_name);
//					gimple_call_set_arg(list->stmt, list->i, build_fold_addr_expr(node_to_replace));
//				}
//				else if (list->replace) {
//					fprintf(stderr, "REPLACE STRUCT: %s\n", current_function_name);
//					list->replace = node_to_replace;
//				}
//			}
//		}

    }
    DEBUG_OUTPUT("Analyzing function 2: %s\n", current_function_name);

    

    FOR_EACH_BB_FN(bb, cfun)
    {
        for (gsi = gsi_start_bb(bb); !gsi_end_p(gsi); gsi_next(&gsi)) {

            gimple stmt = gsi_stmt(gsi);

             // Look for calls made to functions defined outside of module
            if (is_call_to_fn_outside_module(stmt)) {
		    //       DEBUG_OUTPUT("CALL TO : %s\n", get_name(gimple_call_fndecl(stmt)));

                // Iterate over each of the call's arguments to see if they contain pointers to functions / variables that need to be wrapped
                size_t i;
                for (i = 0; i < gimple_call_num_args(stmt); i++) {
                    tree call_arg = gimple_call_arg(stmt, i);
                    //DEBUG_OUTPUT("CALL ARG : %s\n", );
//		    fprintf(stderr,"CALL ARG : %s\n", "");
//		    debug_tree(call_arg);

                    // Replace call args that are string constants with variable in .fixed.rodata section instead
                    if (is_call_arg_a_string_constant(call_arg)) {
                    		const_tree str_tree = get_str_cst(call_arg);
                            if (str_tree) {
                                const char *str = TREE_STRING_POINTER(str_tree);
                                tree str_decl = build_string_var_decl(call_arg, str, current_function_name);
                                gimple_call_set_arg(stmt, i, str_decl);
                                tree new_call_arg = gimple_call_arg(stmt, i);
                            }
		    }
		    else if (TREE_CODE(TREE_TYPE(call_arg)) == POINTER_TYPE) {
				if (TREE_CODE(call_arg) == INTEGER_CST) {	
//					fprintf(stderr,"INTEGER VAR : %s\n", "");
//					debug_tree(call_arg);
				}
				else if (TREE_CODE(TREE_TYPE(TREE_TYPE(call_arg))) == FUNCTION_TYPE) {
					tree decl = TREE_OPERAND(call_arg, 0);
					DEBUG_OUTPUT("NEED TO WRAP %s\n", "");
//					fprintf(stderr,"WRAP POINTER VAR : %s\n", "");
//					debug_tree(call_arg);
					if (DECL_EXTERNAL(decl)) {
						DEBUG_OUTPUT("nvm its external %s\n", "");
					}
					else {
						if (is_node_decl_in_module(decl)) {
							mark_function_for_wrap(decl, stmt, i, NULL_TREE);
						}
					}
				}
				else if (TREE_CODE(TREE_TYPE(TREE_TYPE(call_arg))) == RECORD_TYPE) {
//					fprintf(stderr,"WRAP VAR : %s\n", "");
//					wrap_var(call_arg); 
				}
					
		}
               
            
	    }
            //if (is_gimple_assign(stmt)) {
                //TODO: Case where assignments need to be handled?
           // }
   	 }
    }
    }

    
}

static void get_location(tree fndecl)
{
    location_t loc_t1 = TYPE_NAME (fndecl) && TREE_CODE (TYPE_NAME (fndecl)) == TYPE_DECL
                        ? DECL_SOURCE_LOCATION (TYPE_NAME (fndecl))
                        : UNKNOWN_LOCATION;

    expanded_location xloc1 = expand_location (loc_t1);
    fprintf(stderr, "Location of %s: %s\n", get_name(fndecl), xloc1.file);
}

/* Determine whether to run execute function for given function decl. Only analyze function decls for current module. */
static bool rerandomization_wrapper_instrument_gate(void)
{
    return is_node_decl_in_module(current_function_decl); // && !(strstr(DECL_NAME_POINTER(current_function_decl), REAL_FN_NAME_SUFFIX));
}

/* Main function */
static unsigned int rerandomization_wrapper_instrument_execute(void) {
    do_execute();
    return 0;
}

void CALL_MOD_FUNC(const char * function_name1, FILE * file) {
    const char * insn10 = "mov ";
    const char * insn20 = "@GOTPCREL(%rip), %rax";
    char dest2[strlen(insn10) + strlen(insn20) + strlen(function_name1)] = "";
    strcat(dest2, insn10);
    strcat(dest2, function_name1);
    strcat(dest2, insn20);

    const gasm * g1 = gimple_build_asm_vec(dest2, NULL, NULL, NULL, NULL);

    DEBUG_OUTPUT("Gimple: %s\n", gimple_asm_string(g1));
    OUTPUT_INSN(gimple_asm_string(g1), file);

    gasm * g2 = gimple_build_asm_vec("call *%rax", NULL, NULL, NULL, NULL);
    gimple_asm_set_volatile (g2, true);
    OUTPUT_INSN(gimple_asm_string(g2), file);
}

void CALL_KERNEL_FUNC(const char * function_name, FILE * file) {
    const char * insn1 = "movabs ";
    const char * insn2 = ", %rax";
    char dest[strlen(insn1) + strlen(insn2) + strlen(function_name)] = "";
    strcat(dest, insn1);
    strcat(dest, function_name);
    strcat(dest, insn2);

    const gasm * g = gimple_build_asm_vec(dest, NULL, NULL, NULL, NULL);

    DEBUG_OUTPUT("Gimple: %s\n", gimple_asm_string(g));
    OUTPUT_INSN(gimple_asm_string(g), file);

    gasm * g2 = gimple_build_asm_vec("call *%rax", NULL, NULL, NULL, NULL);
    gimple_asm_set_volatile (g2, true);
    OUTPUT_INSN(gimple_asm_string(g2), file);
}

void MOD_GET_STACK(FILE * file) {
    CALL_KERNEL_FUNC("module_get_stack", file);
    OUTPUT_INSN("mov %rax, %rsp", file);
}

void MOD_OFFER_STACK(FILE * file) {
    OUTPUT_INSN("mov %rsp, %rdi", file);
    OUTPUT_INSN("lea -0x40(%rbp), %rsp", file);
}

void MOD_OFFER_STACK_CALL(FILE * file) {
	CALL_KERNEL_FUNC("module_offer_stack", file);
}

void function_prologue(FILE *file) {
	if (lookup_key(real_function_hash_table, DECL_NAME_POINTER(current_function_decl))) {
	//if (wrapper_function_exists()) {
	//if (strstr(DECL_NAME_POINTER(current_function_decl), "real")) {

		/* Save base pointer */
		OUTPUT_INSN("push %rbp", file);
		OUTPUT_INSN("mov %rsp,%rbp", file);

		/* Save Args */
		OUTPUT_INSN("push %rdi", file);
		OUTPUT_INSN("push %rsi", file);
		OUTPUT_INSN("push %rdx", file);
		OUTPUT_INSN("push %rcx", file);
		OUTPUT_INSN("push %r8", file);
		OUTPUT_INSN("push %r9", file);

		/* Call smr_enter save return */
		//CALL_KERNEL_FUNC(smr_enter);
		CALL_KERNEL_FUNC("smr_enter", file);

		OUTPUT_INSN("push %rax", file);
		OUTPUT_INSN("push %rdx", file);

		/* Get new stack */
		MOD_GET_STACK(file);

		/* Restore Args*/
		OUTPUT_INSN("mov -0x30(%rbp), %r9", file);
		OUTPUT_INSN("mov -0x28(%rbp), %r8", file);
		OUTPUT_INSN("mov -0x20(%rbp), %rcx", file);
		OUTPUT_INSN("mov -0x18(%rbp), %rdx", file);
		OUTPUT_INSN("mov -0x10(%rbp), %rsi", file);
		OUTPUT_INSN("mov -0x8(%rbp), %rdi", file);

		const char * str = DECL_NAME_POINTER(current_function_decl);

//		s[len] = '\0';
		//const char * st = clone_function_name_with_underscore(str, REAL_FN_NAME_SUFFIX);
	    char dest[strlen(str) + strlen(".real")] = "";
	    strcat(dest, str);
	    strcat(dest, ".real");
		
//		const char * str = DECL_NAME_POINTER(current_function_decl);
//		int len = strlen(str) - strlen(".real");
//		char s[len];
//		for (int i=0; i<len; i++) {
//			s[i] = str[i];
//		}
//		s[len] = '\0';
//	    
	  
	    
		//fprintf(stderr, "%s %s \n", str, dest);
		CALL_MOD_FUNC(dest, file);
		
		/* Restore old stack */
		MOD_OFFER_STACK(file);
		OUTPUT_INSN("mov %rax, %rbp", file);
		MOD_OFFER_STACK_CALL(file);


		/* Prepare smr_leave args */
		OUTPUT_INSN("pop %rsi", file);
		OUTPUT_INSN("pop %rdi", file);
		
		OUTPUT_INSN("add $48, %rsp", file);
		CALL_KERNEL_FUNC("smr_leave", file);
		OUTPUT_INSN("mov %rbp, %rax", file);

		/* Restore base pointer */
		OUTPUT_INSN("pop %rbp", file);

		OUTPUT_INSN("ret", file);
    }
}


/* Add gcc target hooks for asm generation */
static void rerandomization_wrapper_plugin_start_unit(void *gcc_data, void *user_data) {
	//targetm.asm_out.final_postscan_insn = final_postscan_insn;
	
	targetm.asm_out.function_prologue = function_prologue;
	struct cgraph_node * node;
	FOR_EACH_DEFINED_FUNCTION (node) {
		TREE_PUBLIC(node->decl) = 1;
	}
	
	
}


#define PASS_NAME rerandomization_wrapper_instrument
#include "gcc-generate-gimple-pass.h"

__visible int
plugin_init(struct plugin_name_args *plugin_info,
            struct plugin_gcc_version *version) {

    const char *const plugin_name = plugin_info->base_name;

    PASS_INFO(rerandomization_wrapper_instrument, "ssa", 1, PASS_POS_INSERT_AFTER);

    if (!plugin_default_version_check(version, &gcc_version)) {
        error(G_("incompatible gcc/plugin versions"));
        return 1;
    }

    register_callback(plugin_name, PLUGIN_START_UNIT,
                      rerandomization_wrapper_plugin_start_unit, NULL);

    register_callback(plugin_name, PLUGIN_INFO, NULL,
                      &rerandomization_wrapper_plugin_info);

    register_callback(plugin_name, PLUGIN_PASS_MANAGER_SETUP, NULL,
                      &rerandomization_wrapper_instrument_pass_info);
    

    return 0;
}
