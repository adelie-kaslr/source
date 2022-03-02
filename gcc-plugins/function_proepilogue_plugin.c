#include <stdio.h>
#include "gcc-common.h"
#include "tree.h"
#include "c-tree.h"
#include "cgraph.h"

// Debugging
#define PRINT_DEBUG 1
#define DEBUG_OUTPUT(str, args...) \
    if(PRINT_DEBUG) {fprintf(stderr, str, args);} \

// Macros to help output asm instructions to file
#define OUTPUT_INSN(str, file) fputs("\t", file); fputs(str, file); fputs(";\n", file)
#define OUTPUT_PUSH_RBP_INSN(file) OUTPUT_INSN("push %rbp", file)
#define OUTPUT_POP_RBP_INSN(file) OUTPUT_INSN("pop %rbp", file)
#define OUTPUT_RETQ_INSN(file) OUTPUT_INSN("retq", file)

// Macros to help overwrite instructions
#define OVERWRITE_INSN(insn_len) fseek(file, -insn_len, SEEK_CUR);
#define RETQ_INSN_LEN 5
#define LEAVEQ_INSN_LEN 7
#define POP_RBP_INSN_LEN 11

// Macros to write prologue/epilogue
#define OUTPUT_R11_PROEPILOGUE(file) \
    OUTPUT_INSN("mov key@GOTPCREL(%rip), %r11", file); \
    OUTPUT_INSN("xor %r11, (%rsp)", file) \

#define OUTPUT_RBP_PROEPILOGUE(file) \
    OUTPUT_INSN("mov key@GOTPCREL(%rip), %rbp", file); \
    OUTPUT_INSN("xor %rbp, 8(%rsp)", file) \

// Store some statistics about how many functions were changed
typedef struct plugin_statistics {
    int num_functions;
    int num_static_functions;
    int num_nonstatic_functions;
    int num_functions_with_frame_pointer_added;
} plugin_statistics;

plugin_statistics *ps = (plugin_statistics *) xmalloc(sizeof(plugin_statistics));


/* All plugins must export this symbol so that they can be linked with
   GCC license-wise.  */
int plugin_is_GPL_compatible;

static struct plugin_info function_proepilogue_plugin_info = {
        .version    = "1",
        .help        = "Add function prologues and epilogues\n",
};

// Is the current function static?
static bool is_static() {
    return !TREE_PUBLIC(current_function_decl);
}

/* Determine if rtx operation is on rbp register */
static bool is_rbp_register_operation(rtx body, int reg_op_num) {
    const char *reg_name = reg_names[REGNO(XEXP(body, reg_op_num))];
    return strcmp(reg_name, "bp") == 0;
}

static bool is_retq_insn(rtx_insn *insn) {
    if (JUMP_P(insn)) {
        rtx jump_insn = ((rtx_jump_insn *) insn)->jump_label();
        return (ANY_RETURN_P(jump_insn));
    }
    return false;
}

static bool is_push_rbp_insn(rtx_insn *insn) {
    if (insn->frame_related) {
        rtx body = PATTERN(insn);

        if (GET_CODE(body) == SET) {
            rtx_code op0 = GET_CODE(XEXP(body, 0));
            rtx_code op1 = GET_CODE(XEXP(body, 1));

            // Test for push %rbp instructions (should find at beginning of functions)
            return (op0 == MEM && op1 == REG && is_rbp_register_operation(body, 1));
        }
    }
    return false;
}


static bool is_final_pop_rbp_insn(rtx_insn *insn) {
    if (insn->frame_related) {
        rtx body = PATTERN(insn);

        if (GET_CODE(body) == SET) {
            rtx_code op0 = GET_CODE(XEXP(body, 0));
            rtx_code op1 = GET_CODE(XEXP(body, 1));

            // Test for pop %rbp instruction, should find at end of functions
            if (op0 == REG && op1 == MEM && is_rbp_register_operation(body, 0)) {
                rtx_insn *next_insn = next_real_insn(insn);
                return (is_retq_insn(next_insn));
            }
        }
    }
    return false;
}

static bool is_leaveq_insn(rtx_insn *insn) {
    rtx body = PATTERN(insn);

    if (GET_CODE(body) == PARALLEL) {
        rtx op0 = XVECEXP(body, 0, 0);
        rtx op1 = XVECEXP(body, 0, 1);

        return ((GET_CODE(op0) == SET) && GET_CODE(XEXP(op0, 0)) == REG && GET_CODE(XEXP(op1, 0)) == REG &&
            is_rbp_register_operation(op1, 0));
    }
    return false;
}

void function_prologue(FILE *file) {
    ps->num_functions++;

    if (is_static()) {
        ps->num_static_functions++;

        if (!is_push_rbp_insn(next_real_insn(entry_of_function()))) {
            ps->num_functions_with_frame_pointer_added++;

            OUTPUT_PUSH_RBP_INSN(file);
            OUTPUT_RBP_PROEPILOGUE(file);
            OUTPUT_POP_RBP_INSN(file);
        }
    }
    else {
        ps->num_nonstatic_functions++;

        OUTPUT_R11_PROEPILOGUE(file);
    }
}

// Handle instructions from non-static functions
void final_postscan_insn_nonstatic(rtx_insn *insn, FILE *file) {
    // In the case where we already have an final push %rbp, add the rbp epilogue before it
    if (is_final_pop_rbp_insn(insn)) {
        OUTPUT_R11_PROEPILOGUE(file);
    }

    // If we find a retq without a %pop rbp before it
    else if (is_retq_insn(insn) && !is_final_pop_rbp_insn(prev_real_insn(insn))) {
        OVERWRITE_INSN(RETQ_INSN_LEN);
        OUTPUT_R11_PROEPILOGUE(file);
        OUTPUT_RETQ_INSN(file);
    }

}

// Handle instructions from static functions
void final_postscan_insn_static(rtx_insn *insn, FILE *file) {
    // In the case where we already have an initial push %rbp, just add the rbp prologue after it
    if (is_push_rbp_insn(insn) && !prev_real_insn(insn)) {
        OUTPUT_RBP_PROEPILOGUE(file);
    }

    //Convert leaveq instructions
    else if (is_leaveq_insn(insn)) {
        OVERWRITE_INSN(LEAVEQ_INSN_LEN)// overwrite leaveq instruction

        OUTPUT_INSN("mov %rbp, %rsp", file);
        OUTPUT_RBP_PROEPILOGUE(file);
        OUTPUT_POP_RBP_INSN(file);
    }

    // In the case where we already have an final push %rbp, add the rbp epilogue before it
    else if (is_final_pop_rbp_insn(insn)) {
        OVERWRITE_INSN(POP_RBP_INSN_LEN);
        OUTPUT_RBP_PROEPILOGUE(file);
        OUTPUT_POP_RBP_INSN(file);
    }

        // If we find a retq without a %pop rbp before it
    else if (is_retq_insn(insn) && !is_final_pop_rbp_insn(prev_real_insn(insn)) && !is_leaveq_insn(prev_real_insn(insn))) {
        OVERWRITE_INSN(RETQ_INSN_LEN); // Overwrite retq insn

        OUTPUT_PUSH_RBP_INSN(file);
        OUTPUT_RBP_PROEPILOGUE(file);
        OUTPUT_POP_RBP_INSN(file);

        OUTPUT_RETQ_INSN(file); //Re-write retq
    }
}

/* Called after each instruction that is output.
 * Look for push/pop %rbp and overwrite. */
void final_postscan_insn(FILE *file, rtx_insn *insn, rtx *opvec, int noperands) {
    is_static() ? final_postscan_insn_static(insn, file) : final_postscan_insn_nonstatic(insn, file);
}


/* Add gcc target hooks for asm generation */
static void function_proepilogue_start_unit(void *gcc_data, void *user_data) {
    targetm.asm_out.final_postscan_insn = final_postscan_insn;
    targetm.asm_out.function_prologue = function_prologue;
}


/* Main function, don't need to do anything here right now. */
static unsigned int function_proepilogue_instrument_execute(void) {
    return 0;
}

// Print out statistics found during compilation with plugin
static void function_proepilogue_finish(void *gcc_data, void *user_data) {
    DEBUG_OUTPUT("Number of functions: %d\n", ps->num_functions);
    DEBUG_OUTPUT("Number of non-static functions: %d (%2.1f%% of functions)\n", ps->num_nonstatic_functions,
                 ((float) ps->num_nonstatic_functions / ps->num_functions) * 100);
    DEBUG_OUTPUT("Number of static functions: %d (%2.1f%% of functions)\n", ps->num_static_functions,
                 ((float) ps->num_static_functions / ps->num_functions) * 100);
    DEBUG_OUTPUT("Number of functions with push/pop %rbp added: %d (%2.1f%% of static functions)\n",
                 ps->num_functions_with_frame_pointer_added,
                 ((float) ps->num_functions_with_frame_pointer_added / ps->num_static_functions) * 100);
    DEBUG_OUTPUT("\n\n\n\n", "");
}


#define PASS_NAME function_proepilogue_instrument
#define NO_GATE

#include "gcc-generate-rtl-pass.h"


__visible int
plugin_init(struct plugin_name_args *plugin_info,
            struct plugin_gcc_version *version) {

    const char *const plugin_name = plugin_info->base_name;

    // Initialize statistics
    ps->num_functions = 0;
    ps->num_static_functions = 0;
    ps->num_nonstatic_functions = 0;
    ps->num_functions_with_frame_pointer_added = 0;

    if (!plugin_default_version_check(version, &gcc_version)) {
        error(G_("incompatible gcc/plugin versions"));
        return 1;
    }

    PASS_INFO(function_proepilogue_instrument, "expand", 1, PASS_POS_INSERT_AFTER);

    register_callback(plugin_name, PLUGIN_INFO, NULL,
                      &function_proepilogue_plugin_info);

    register_callback(plugin_name, PLUGIN_PASS_MANAGER_SETUP, NULL,
                      &function_proepilogue_instrument_pass_info);

    register_callback(plugin_name, PLUGIN_START_UNIT,
                      function_proepilogue_start_unit, NULL);

    register_callback(plugin_name, PLUGIN_FINISH,
                      function_proepilogue_finish, NULL);


    return 0;
}
