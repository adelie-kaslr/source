#include <stdio.h>
#include "gcc-common.h"
#include "tree.h"
#include "c-tree.h"
#include "cgraph.h"

#define PRINT_DEBUG 1
#define DEBUG_OUTPUT(str, args...) \
    if(PRINT_DEBUG) {fprintf(stderr, str, args);} \

/* All plugins must export this symbol so that they can be linked with
   GCC license-wise.  */
int plugin_is_GPL_compatible;

char generated_str_name[] = "gen_str_cst_";
static int generated_str_counter = 0;
#define FIXED_RODATA_SECTION_NAME ".fixed.rodata.str"

static struct plugin_info fix_relocations_plugin_info = {
        .version    = "1",
        .help        = "Make strings static and put into variable\n",
};

bool starts_with(const char *pre, const char *str) {
    size_t lenpre = strlen(pre),
            lenstr = strlen(str);
    return lenstr < lenpre ? false : memcmp(pre, str, lenpre) == 0;
}


/* Determine whether the definition of node is contained within module */
/* This seems like rough way to do this, but didn't see an alternative, and this seems to be kind of like what other plugins have done. */
static bool is_node_decl_in_module(tree node) {
    if (node && DECL_SOURCE_LOCATION(node)) {
        expanded_location xloc = expand_location(DECL_SOURCE_LOCATION(node));
        return !(starts_with("./", xloc.file));
    }
    return false;
}

// Return true if gimple is both a call statement and the call is to a function defined outside the module
static bool is_call_to_fn_outside_module(gimple stmt) {
    return is_gimple_call(stmt) && gimple_call_fndecl(stmt) && !is_node_decl_in_module(gimple_call_fndecl(stmt));
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

    DEBUG_OUTPUT("Gen str name: %s\n", new_str_name);
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

    varpool_add_new_variable(mdecl);
    set_decl_section_name(mdecl, FIXED_RODATA_SECTION_NAME);

    return build_fold_addr_expr(mdecl);
}

static void fix_relocations_finish_decl(void *event_data, void *user_data) {
  	tree decl = (tree) event_data;
	if (TREE_CODE(decl) == VAR_DECL) {
		if (DECL_CONTEXT(decl) == NULL && is_node_decl_in_module(decl)) {
       	//		DEBUG_OUTPUT("\n%s\n", get_name(decl));
	//		debug_tree(decl);
			TREE_PUBLIC(decl) = 1;
		}
	}
}


static void do_execute() {
    basic_block bb;
    gimple_stmt_iterator gsi;

    const char *current_function_name = DECL_NAME_POINTER(current_function_decl);

    DEBUG_OUTPUT("Analyzing function: %s\n", current_function_name);

    FOR_EACH_BB_FN(bb, cfun)
    {
        for (gsi = gsi_start_bb(bb); !gsi_end_p(gsi); gsi_next(&gsi)) {

            gimple stmt = gsi_stmt(gsi);

            // Look for calls made to functions defined outside of module
            if (is_call_to_fn_outside_module(stmt)) {

                    // Iterate over each of the call's arguments to see if they contain pointers to string constants that need to be wrapped
                    size_t i;
                    for (i = 0; i < gimple_call_num_args(stmt); i++) {
                        tree call_arg = gimple_call_arg(stmt, i);

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
                    }


            }
        }
    }
}



/* Main function */
static unsigned int fix_relocations_instrument_execute(void) {
    do_execute();
    return 0;
}


#define PASS_NAME fix_relocations_instrument
#define NO_GATE

#include "gcc-generate-gimple-pass.h"


__visible int
plugin_init(struct plugin_name_args *plugin_info,
            struct plugin_gcc_version *version) {

    const char *const plugin_name = plugin_info->base_name;
    PASS_INFO(fix_relocations_instrument, "optimized", 1, PASS_POS_INSERT_BEFORE);

    if (!plugin_default_version_check(version, &gcc_version)) {
        error(G_("incompatible gcc/plugin versions"));
        return 1;
    }
    register_callback(plugin_name, PLUGIN_INFO, NULL,
                      &fix_relocations_plugin_info);

    register_callback(plugin_name, PLUGIN_FINISH_DECL,
                      fix_relocations_finish_decl, NULL);

    register_callback(plugin_name, PLUGIN_PASS_MANAGER_SETUP, NULL,
                      &fix_relocations_instrument_pass_info);

    return 0;
}
