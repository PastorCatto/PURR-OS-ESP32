/* First real module built through the real purrstrap pipeline (Modules/SPEC.md), not the
 * ModuleSpike's hand-built byte array. Deliberately includes a string literal this time --
 * the spike's second test module cut one to avoid needing real relocation; this is the
 * end-to-end check that real relocation (a genuine .rodata address, not just a call table
 * entry) actually gets found and applied correctly. */
typedef struct {
    void (*log)(const char *msg);
} purr_core_table_t;

static const char s_greeting[] = "hello from a real, relocated PURR module";

__attribute__((noinline, used))
static int helper_double(int x)
{
    return x * 2;
}

__attribute__((used))
int hello_module_entry(const purr_core_table_t *core, int a, int b)
{
    core->log(s_greeting);
    int h = helper_double(a);
    return h + b;
}
