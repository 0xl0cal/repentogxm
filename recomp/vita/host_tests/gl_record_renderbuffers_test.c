/* Execute the unchanged RBO wrappers on the producer; only their native
 * Bind/Storage/Delete boundary is recorded. Gen is drain-and-execute. */
#include "gl_record_ring.h"
#include <assert.h>
#include <setjmp.h>
#include <stdio.h>
#include <string.h>
#include "gl_record_original_renderbuffers.inc"

enum { BIND = 1, STORE, DELETE, GEN };
typedef struct NativeTrace {
    uint32_t op, a, b, c, d, client_hash;
} NativeTrace;
typedef struct Result {
    NativeTrace trace[256];
    uint32_t trace_count;
    gl_vita_renderbuffer_record records[GL_VITA_RBO_CAPACITY];
    uint32_t bound;
} Result;

static IsaacGlRecordRing ring;
static Result current, direct;
static int recording, inside_gen, gen_shape;
static uint32_t next_name, sync_calls, queries;
static jmp_buf fault_boundary;
static uint32_t expected_token;
static const char *expected_message;

static uint32_t hash_client(const void *bytes, size_t count)
{
    const uint8_t *p = (const uint8_t *)bytes;
    uint32_t hash = UINT32_C(2166136261);
    size_t i;
    for (i = 0u; i < count; ++i) hash = (hash ^ p[i]) * UINT32_C(16777619);
    return hash;
}

static GUEST_NORETURN void guest_gl_backend_fault(uint32_t token, const char *message)
{
    assert(token == expected_token && !strcmp(message, expected_message));
    assert(!strcmp(s_last_error, expected_message));
    longjmp(fault_boundary, 1);
}

static void native(uint32_t op, const uint32_t *args, const uint32_t *names)
{
    NativeTrace *trace;
    assert(current.trace_count < sizeof current.trace / sizeof current.trace[0]);
    trace = &current.trace[current.trace_count++];
    trace->op = op;
    trace->a = args[0]; trace->b = args[1];
    trace->c = args[2]; trace->d = args[3];
    if (op == DELETE && args[0])
        trace->client_hash = hash_client(names, (size_t)args[0] * 4u);
}

static void replay(void *context, const IsaacGlRecordHeader *header, const void *payload)
{
    const uint32_t *words = (const uint32_t *)payload;
    assert(context == &ring && header->payload_bytes >= 16u);
    native(header->opcode, words, words + 4);
}

static void queue(uint32_t op, const uint32_t *args, const uint32_t *names)
{
    uint32_t words[4u + GL_VITA_RBO_CAPACITY];
    uint32_t bytes = 16u;
    if (!recording || inside_gen) {
        native(op, args, names);
        return;
    }
    memcpy(words, args, 16u);
    if (op == DELETE && args[0]) {
        bytes += args[0] * 4u;
        memcpy(words + 4, names, (size_t)args[0] * 4u);
    }
    assert(isaac_gl_record_ring_append(&ring, op, 0u, words, bytes));
}

static void glBindRenderbuffer(GLenum target, GLuint name)
{
    uint32_t args[4] = {target, name, 0u, 0u};
    queue(BIND, args, NULL);
}
static void glRenderbufferStorage(GLenum target, GLenum format, GLsizei w, GLsizei h)
{
    uint32_t args[4] = {target, format, (uint32_t)w, (uint32_t)h};
    queue(STORE, args, NULL);
}
static void glDeleteRenderbuffers(GLsizei count, const GLuint *names)
{
    uint32_t args[4] = {(uint32_t)count, 0u, 0u, 0u};
    queue(DELETE, args, names);
}
static void glGenRenderbuffers(GLsizei count, GLuint *names)
{
    GLsizei i;
    uint32_t args[4] = {(uint32_t)count, 0u, 0u, 0u};
    assert(inside_gen); /* no native Gen may run as an async command */
    native(GEN, args, NULL);
    for (i = 0; i < count; ++i)
        names[i] = gen_shape == 1 ? 0u : gen_shape == 2 ? 123u :
            gen_shape == 3 ? 77u : next_name++;
}

static void gen(GLsizei count, GLuint *names)
{
    if (recording) {
        assert(isaac_gl_record_ring_drain(&ring));
        ++sync_calls;
    }
    inside_gen = 1;
    vita_glGenRenderbuffers(count, (uint32_t)(uintptr_t)names);
    inside_gen = 0;
}

static void query(uint32_t pname, int32_t expected)
{
    uint32_t redzone[3] = {0x12345678u, UINT32_MAX, 0x87654321u};
    uint32_t records = ring.records, used = ring.used, waits = sync_calls;
    vita_glGetRenderbufferParameteriv(GL_VITA_RENDERBUFFER, pname,
        (uint32_t)(uintptr_t)&redzone[1]);
    assert(redzone[0] == 0x12345678u && redzone[2] == 0x87654321u);
    assert((int32_t)redzone[1] == expected);
    assert(ring.records == records && ring.used == used && sync_calls == waits);
    ++queries;
}

static void fault(int which, uint32_t token, const char *message)
{
    uint32_t names[2] = {77u, 77u}, output = 0u;
    expected_token = token;
    expected_message = message;
    if (!setjmp(fault_boundary)) {
        switch (which) {
        case 0: vita_glBindRenderbuffer(0u, 77u); break;
        case 1: vita_glBindRenderbuffer(GL_VITA_RENDERBUFFER, 0xdead); break;
        case 2: vita_glGetRenderbufferParameteriv(0u, GL_VITA_RENDERBUFFER_WIDTH,
            (uint32_t)(uintptr_t)&output); break;
        case 3: vita_glGetRenderbufferParameteriv(GL_VITA_RENDERBUFFER, 0x8d44u,
            (uint32_t)(uintptr_t)&output); break;
        case 4: vita_glGetRenderbufferParameteriv(GL_VITA_RENDERBUFFER,
            GL_VITA_RENDERBUFFER_WIDTH, 0u); break;
        case 5: vita_glGetRenderbufferParameteriv(GL_VITA_RENDERBUFFER,
            GL_VITA_RENDERBUFFER_WIDTH, (uint32_t)(uintptr_t)&output); break;
        case 6: vita_glRenderbufferStorage(GL_VITA_RENDERBUFFER, 0x81a6u, -1, 4); break;
        case 7: vita_glRenderbufferStorage(GL_VITA_RENDERBUFFER, 0x81a6u, 4, 4); break;
        case 8: vita_glDeleteRenderbuffers(-1, (uint32_t)(uintptr_t)names); break;
        case 9: vita_glDeleteRenderbuffers(1, 0u); break;
        case 10: gen(2, names); break; /* zero/duplicate/existing output */
        case 11: gen(1, names); break; /* capacity preflight */
        default: assert(0);
        }
        assert(0);
    }
    inside_gen = 0; /* emulate unwinding the synchronous command boundary */
}

static void run(int record, int poison)
{
    uint32_t names[2] = {0u, 0u}, i, waits;
    recording = record; gen_shape = 0; next_name = 900u;
    sync_calls = queries = 0u;
    memset(&current, 0, sizeof current);
    isaac_gl_record_ring_init(&ring, replay, &ring);
    gl_vita_rbo_reset(); s_installed = 1; s_last_error = NULL;
    assert(gl_vita_backend_register_renderbuffer(77u, 0x81a6u, 1024, 1024));
    query(GL_VITA_RENDERBUFFER_WIDTH, 1024);
    query(GL_VITA_RENDERBUFFER_HEIGHT, 1024);
    gen(2, names);
    assert(names[0] == 900u && names[1] == 901u);
    vita_glBindRenderbuffer(GL_VITA_RENDERBUFFER, names[0]);
    vita_glRenderbufferStorage(GL_VITA_RENDERBUFFER, 0x81a6u, 321, 123);
    query(GL_VITA_RENDERBUFFER_WIDTH, 321);
    query(GL_VITA_RENDERBUFFER_HEIGHT, 123);
    waits = sync_calls;
    for (i = 0u; i < 10u; ++i) {
        vita_glBindRenderbuffer(GL_VITA_RENDERBUFFER, 77u);
        query(GL_VITA_RENDERBUFFER_WIDTH, 1024);
        query(GL_VITA_RENDERBUFFER_HEIGHT, 1024);
    }
    assert(sync_calls == waits && queries == 24u);
    vita_glBindRenderbuffer(GL_VITA_RENDERBUFFER, names[0]);
    vita_glDeleteRenderbuffers(1, (uint32_t)(uintptr_t)names);
    if (poison) names[0] = 0xa5a5a5a5u;
    fault(5, GL_VITA_TOKEN_GET_RBO_PARAMETER,
        "Vita GL glGetRenderbufferParameteriv has no bound renderbuffer");
    fault(7, GL_VITA_TOKEN_RBO_STORAGE,
        "Vita GL glRenderbufferStorage has no tracked bound renderbuffer");
    vita_glBindRenderbuffer(GL_VITA_RENDERBUFFER, names[1]);
    vita_glRenderbufferStorage(GL_VITA_RENDERBUFFER, 0x81a6u, 640, 360);
    query(GL_VITA_RENDERBUFFER_WIDTH, 640);
    query(GL_VITA_RENDERBUFFER_HEIGHT, 360);
    assert(isaac_gl_record_ring_drain(&ring)); /* present */
    fault(0, GL_VITA_TOKEN_BIND_RENDERBUFFER,
        "Vita GL glBindRenderbuffer received an invalid target");
    fault(1, GL_VITA_TOKEN_BIND_RENDERBUFFER,
        "Vita GL glBindRenderbuffer received an unknown renderbuffer");
    fault(2, GL_VITA_TOKEN_GET_RBO_PARAMETER,
        "Vita GL glGetRenderbufferParameteriv received an invalid target");
    fault(3, GL_VITA_TOKEN_GET_RBO_PARAMETER,
        "Vita GL glGetRenderbufferParameteriv received an unsupported pname");
    fault(4, GL_VITA_TOKEN_GET_RBO_PARAMETER,
        "Vita GL glGetRenderbufferParameteriv received a null output");
    fault(6, GL_VITA_TOKEN_RBO_STORAGE,
        "Vita GL glRenderbufferStorage received invalid dimensions or target");
    fault(8, GL_VITA_TOKEN_DELETE_RENDERBUFFERS,
        "Vita GL glDeleteRenderbuffers received an invalid array");
    fault(9, GL_VITA_TOKEN_DELETE_RENDERBUFFERS,
        "Vita GL glDeleteRenderbuffers received an invalid array");
    for (gen_shape = 1; gen_shape <= 3; ++gen_shape)
        fault(10, GL_VITA_TOKEN_GEN_RENDERBUFFERS,
            "Vita GL glGenRenderbuffers returned an invalid name");
    gen_shape = 0;
    /* Manual KAGE unregister/register paths share the same producer registry. */
    gl_vita_backend_unregister_renderbuffer(77u);
    assert(!gl_vita_rbo_find(77u));
    assert(gl_vita_backend_register_renderbuffer(77u, 0x81a6u, 512, 256));
    query(GL_VITA_RENDERBUFFER_WIDTH, 512);
    query(GL_VITA_RENDERBUFFER_HEIGHT, 256);
    memcpy(current.records, s_renderbuffers, sizeof s_renderbuffers);
    current.bound = s_bound_renderbuffer;
    if (!record) direct = current;
    else assert(!memcmp(&current, &direct, sizeof current));
    gl_vita_rbo_reset();
    for (i = 1u; i <= GL_VITA_RBO_CAPACITY; ++i)
        assert(gl_vita_backend_register_renderbuffer(i, 0x81a6u, 4, 8));
    assert(!gl_vita_backend_register_renderbuffer(999u, 0x81a6u, 4, 8));
    assert(!strcmp(s_last_error, "Vita GL renderbuffer registry exhausted"));
    waits = current.trace_count;
    fault(11, GL_VITA_TOKEN_GEN_RENDERBUFFERS,
        "Vita GL glGenRenderbuffers received an invalid output array");
    assert(current.trace_count == waits); /* no allocation on exhausted preflight */
    gl_vita_rbo_reset();
    assert(!gl_vita_backend_register_renderbuffer(0u, 0u, 4, 8));
    assert(!gl_vita_backend_register_renderbuffer(77u, 0u, -1, 8));
    s_installed = 0;
    assert(!gl_vita_backend_register_renderbuffer(77u, 0u, 4, 8));
}

int main(void)
{
    run(0, 0);
    run(1, 0);
    run(1, 1);
    printf("PASS ORIGINAL RBO producer registry: direct/replay/poison native-read hashes/order/state/returns/faults; 10 target switches, 20 queries, zero query records/waits\n");
    return 0;
}
