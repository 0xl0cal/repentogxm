#include <stdint.h>

/* Mirror the runtime-owned declaration before vitaGL.h.  The common guard
 * makes duplicate typedefs a compile-time regression rather than a Vita fault. */
#define ISAAC_VITAGL_DISPLAY_QUEUE_PROBE_DEFINED 1
#define ISAAC_VITAGL_DISPLAY_QUEUE_SAMPLE_COUNT 8u
#define ISAAC_VITAGL_DISPLAY_QUEUE_STAGE_ADD_RESULT 1u
#define ISAAC_VITAGL_DISPLAY_QUEUE_STAGE_DISPLAY_CALLBACK 2u
#define ISAAC_VITAGL_DISPLAY_LINEAGE_BEGIN_MISMATCH 0x00000001u
#define ISAAC_VITAGL_DISPLAY_LINEAGE_END_MISMATCH   0x00000002u
typedef struct IsaacVitaGlDisplayQueueProbe {
    uint32_t size;
    uint32_t stage;
    uint32_t sequence;
    uint32_t front_index;
    uint32_t back_index;
    uint32_t address;
    union {
        struct {
            int32_t result;
            uint32_t old_sync;
            uint32_t new_sync;
            uint32_t begin_context;
            uint32_t begin_render_target;
            uint32_t begin_fragment_sync;
            uint32_t begin_color_surface;
            uint32_t begin_color_data;
            int32_t begin_result;
            uint32_t begin_count;
            uint32_t end_context;
            int32_t end_result;
            uint32_t end_count;
            uint32_t mismatch_mask;
            int32_t back_memblock_uid;
            int32_t back_get_base_result;
            int32_t back_map_result;
            uint32_t back_dedicated;
        } add;
        struct {
            int32_t result;
            uint32_t size;
            uint32_t base;
            uint32_t pitch;
            uint32_t pixel_format;
            uint32_t width;
            uint32_t height;
            uint32_t sync;
            uint32_t sample_count;
            uint32_t sparse_hash;
            uint32_t rgba[ISAAC_VITAGL_DISPLAY_QUEUE_SAMPLE_COUNT];
        } display;
    } detail;
} IsaacVitaGlDisplayQueueProbe;
void isaac_vitagl_display_queue_probe(
    const IsaacVitaGlDisplayQueueProbe *event);

#include <vitaGL.h>

_Static_assert(sizeof(IsaacVitaGlDisplayQueueProbe) == 96,
               "display-lineage probe ARM ABI drifted");
static volatile uint32_t display_queue_probe_sink;

void isaac_vitagl_display_queue_probe(
    const IsaacVitaGlDisplayQueueProbe *event)
{
    if (event && event->size == sizeof(*event))
        display_queue_probe_sink = event->sequence ^
            event->detail.display.sparse_hash ^
            (uint32_t)event->detail.add.back_memblock_uid ^
            event->detail.add.back_dedicated;
}

/* Link-only oracle.  It is never packaged or executed. */
int main(int argc, char **argv)
{
    GLuint framebuffer = 0;
    GLuint renderbuffer = 0;
    GLuint shader = 0;
    const GLchar *source = "void main(){gl_Position=vec4(0.0);}";

    (void)argv;
    if (argc == (int)(uintptr_t)&main) {
        if (!vglInit(0))
            return 1;
        shader = glCreateShader(GL_VERTEX_SHADER);
        glShaderSource(shader, 1, &source, NULL);
        glCompileShader(shader);
        glGenFramebuffers(1, &framebuffer);
        glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
        glGenRenderbuffers(1, &renderbuffer);
        glBindRenderbuffer(GL_RENDERBUFFER, renderbuffer);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, 32, 32);
        glFramebufferRenderbuffer(
            GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, renderbuffer);
        vglSwapBuffers(GL_FALSE);
    }
    return 0;
}
