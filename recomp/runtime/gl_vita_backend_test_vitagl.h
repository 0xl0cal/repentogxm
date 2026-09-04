/* Minimal native-GL facade used only by the host/softfp RBO oracle. */
#ifndef GL_VITA_BACKEND_TEST_VITAGL_H
#define GL_VITA_BACKEND_TEST_VITAGL_H

#include <stdint.h>

typedef uint32_t GLenum;
typedef uint32_t GLbitfield;
typedef uint32_t GLuint;
typedef int32_t  GLint;
typedef int32_t  GLsizei;
typedef uint8_t  GLboolean;
typedef float    GLfloat;
typedef double   GLdouble;
typedef char     GLchar;
typedef uint8_t  GLubyte;

#define GL_SHADER_TYPE 0x00008b4fu

void oracle_glBindRenderbuffer(GLenum target, GLuint renderbuffer);
void oracle_glBindFramebuffer(GLenum target, GLuint framebuffer);
void oracle_glActiveTexture(GLenum texture);
void oracle_glBindTexture(GLenum target, GLuint texture);
void oracle_glBlendFuncSeparate(
    GLenum source_rgb, GLenum destination_rgb,
    GLenum source_alpha, GLenum destination_alpha);
void oracle_glClearDepth(GLdouble depth);
void oracle_glClear(GLbitfield mask);
void oracle_glClearColor(
    GLfloat red, GLfloat green, GLfloat blue, GLfloat alpha);
void oracle_glEnable(GLenum capability);
void oracle_glDeleteFramebuffers(
    GLsizei count, const GLuint *framebuffers);
void oracle_glDeleteRenderbuffers(GLsizei count, const GLuint *renderbuffers);
void oracle_glDrawElements(
    GLenum mode, GLsizei count, GLenum type, const void *indices);
void oracle_glGenRenderbuffers(GLsizei count, GLuint *renderbuffers);
void oracle_glGenTextures(GLsizei count, GLuint *textures);
void oracle_glRenderbufferStorage(
    GLenum target, GLenum format, GLsizei width, GLsizei height);
void oracle_glGetIntegerv(GLenum name, GLint *value);
GLuint oracle_glCreateProgram(void);
void oracle_glDeleteProgram(GLuint program);
void oracle_glDeleteTextures(GLsizei count, const GLuint *textures);
void oracle_glFramebufferTexture2D(
    GLenum target, GLenum attachment, GLenum texture_target,
    GLuint texture, GLint level);
void oracle_glTexImage2D(
    GLenum target, GLint level, GLint internal_format,
    GLsizei width, GLsizei height, GLint border,
    GLenum format, GLenum type, const void *pixels);
void oracle_glTexSubImage2D(
    GLenum target, GLint level, GLint x_offset, GLint y_offset,
    GLsizei width, GLsizei height, GLenum format, GLenum type,
    const void *pixels);
void oracle_glDepthFunc(GLenum function);
void oracle_glDisableVertexAttribArray(GLuint index);
void oracle_glGetProgramiv(GLuint program, GLenum name, GLint *value);
void oracle_glGetShaderiv(GLuint shader, GLenum name, GLint *value);
GLint oracle_glGetUniformLocation(GLuint program, const GLchar *name);
void oracle_glLinkProgram(GLuint program);
void oracle_glEnableVertexAttribArray(GLuint index);
void oracle_glShaderSource(
    GLuint shader, GLsizei count, const GLchar *const *strings,
    const GLint *lengths);
void oracle_glUniform1i(GLint location, GLint value);
void oracle_glUniform4fv(
    GLint location, GLsizei count, const GLfloat *value);
void oracle_glUniformMatrix4fv(
    GLint location, GLsizei count, GLboolean transpose,
    const GLfloat *value);
void oracle_glUseProgram(GLuint program);
void oracle_glVertexAttribPointer(
    GLuint index, GLint size, GLenum type, GLboolean normalized,
    GLsizei stride, const void *pointer);
void oracle_glViewport(
    GLint x, GLint y, GLsizei width, GLsizei height);
void oracle_glNoop();
#if defined(ISAAC_GL_VITA_LOCATION_ORACLE)
/* gl_vita_location_cache_oracle.c models attribute locations and shader
 * attachment; every other oracle keeps the constant facade below. */
GLint oracle_glGetAttribLocation(GLuint program, const GLchar *name);
void oracle_glAttachShader(GLuint program, GLuint shader);
#endif
GLuint oracle_glReturnUint();
GLint oracle_glReturnInt();
const GLubyte *oracle_glReturnString();
GLboolean vglIsaacDrawCanonicalQuads(GLsizei count);
void vglIsaacMarkColorOffsetShader(
    uint32_t shader, uint32_t source_fnv1a,
    uint32_t source_first512_fnv1a, uint32_t source_size);

#define glBindRenderbuffer      oracle_glBindRenderbuffer
#define glDeleteRenderbuffers   oracle_glDeleteRenderbuffers
#define glGenRenderbuffers      oracle_glGenRenderbuffers
#define glRenderbufferStorage   oracle_glRenderbufferStorage

#define glActiveTexture              oracle_glActiveTexture
#define glAlphaFunc                  oracle_glNoop
#if defined(ISAAC_GL_VITA_LOCATION_ORACLE)
#define glAttachShader               oracle_glAttachShader
#else
#define glAttachShader               oracle_glNoop
#endif
#define glBindFramebuffer            oracle_glBindFramebuffer
#define glBindTexture                oracle_glBindTexture
#define glBlendFuncSeparate          oracle_glBlendFuncSeparate
#define glCheckFramebufferStatus     oracle_glReturnUint
#define glClear                      oracle_glClear
#define glClearColor                 oracle_glClearColor
#define glClearDepth                 oracle_glClearDepth
#define glCompileShader              oracle_glNoop
#define glCreateProgram              oracle_glCreateProgram
#define glCreateShader               oracle_glReturnUint
#define glCullFace                   oracle_glNoop
#define glDeleteFramebuffers         oracle_glDeleteFramebuffers
#define glDeleteProgram              oracle_glDeleteProgram
#define glDeleteShader               oracle_glNoop
#define glDeleteTextures             oracle_glDeleteTextures
#define glDepthFunc                  oracle_glDepthFunc
#define glDisableVertexAttribArray   oracle_glDisableVertexAttribArray
#define glDrawElements               oracle_glDrawElements
#define glEnable                     oracle_glEnable
#define glEnableVertexAttribArray    oracle_glEnableVertexAttribArray
#define glFramebufferRenderbuffer    oracle_glNoop
#define glFramebufferTexture2D       oracle_glFramebufferTexture2D
#define glGenFramebuffers            oracle_glNoop
#define glGenTextures                oracle_glGenTextures
#if defined(ISAAC_GL_VITA_LOCATION_ORACLE)
#define glGetAttribLocation          oracle_glGetAttribLocation
#else
#define glGetAttribLocation          oracle_glReturnInt
#endif
#define glGetIntegerv                oracle_glGetIntegerv
#define glGetProgramInfoLog          oracle_glNoop
#define glGetProgramiv               oracle_glGetProgramiv
#define glGetShaderInfoLog           oracle_glNoop
#define glGetShaderiv                oracle_glGetShaderiv
#define glGetString                  oracle_glReturnString
#define glGetStringi                 oracle_glReturnString
#define glGetUniformLocation         oracle_glGetUniformLocation
#define glLinkProgram                oracle_glLinkProgram
#define glReadPixels                 oracle_glNoop
#define glShaderSource               oracle_glShaderSource
#define glTexImage2D                 oracle_glTexImage2D
#define glTexParameteri              oracle_glNoop
#define glTexSubImage2D              oracle_glTexSubImage2D
#define glUniform1fv                 oracle_glNoop
#define glUniform1i                  oracle_glUniform1i
#define glUniform1iv                 oracle_glNoop
#define glUniform2fv                 oracle_glNoop
#define glUniform2iv                 oracle_glNoop
#define glUniform3fv                 oracle_glNoop
#define glUniform3iv                 oracle_glNoop
#define glUniform4fv                 oracle_glUniform4fv
#define glUniform4iv                 oracle_glNoop
#define glUniformMatrix2fv           oracle_glNoop
#define glUniformMatrix3fv           oracle_glNoop
#define glUniformMatrix4fv           oracle_glUniformMatrix4fv
#define glUseProgram                 oracle_glUseProgram
#define glVertexAttribPointer        oracle_glVertexAttribPointer
#define glViewport                   oracle_glViewport

#endif
