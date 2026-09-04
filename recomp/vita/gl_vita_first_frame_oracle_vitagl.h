/* Native-GL facade for the executable first-frame behavior oracle. */
#ifndef GL_VITA_FIRST_FRAME_ORACLE_VITAGL_H
#define GL_VITA_FIRST_FRAME_ORACLE_VITAGL_H

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

void oracle_ff_glBindFramebuffer(GLenum target, GLuint framebuffer);
void oracle_ff_glClear(GLbitfield mask);
void oracle_ff_glClearColor(GLfloat red, GLfloat green,
                            GLfloat blue, GLfloat alpha);
void oracle_ff_glColorMask(GLboolean red, GLboolean green,
                           GLboolean blue, GLboolean alpha);
void oracle_ff_glDisable(GLenum capability);
void oracle_ff_glDrawElements(GLenum mode, GLsizei count, GLenum type,
                              const void *indices);
void oracle_ff_glEnable(GLenum capability);
void oracle_ff_glFinish(void);
void oracle_ff_glFramebufferTexture2D(
    GLenum target, GLenum attachment, GLenum texture_target,
    GLuint texture, GLint level);
void oracle_ff_glGetBooleanv(GLenum name, GLboolean *values);
GLenum oracle_ff_glGetError(void);
void oracle_ff_glGetFloatv(GLenum name, GLfloat *values);
void oracle_ff_glGetIntegerv(GLenum name, GLint *values);
GLboolean oracle_ff_glIsEnabled(GLenum capability);
void oracle_ff_glReadPixels(
    GLint x, GLint y, GLsizei width, GLsizei height,
    GLenum format, GLenum type, void *pixels);
void oracle_ff_glViewport(GLint x, GLint y, GLsizei width, GLsizei height);
GLenum oracle_ff_glCheckNamedFramebufferStatus(
    GLuint framebuffer, GLenum target);
void oracle_ff_glBlitNamedFramebuffer(
    GLuint read_framebuffer, GLuint draw_framebuffer,
    GLint src_x0, GLint src_y0, GLint src_x1, GLint src_y1,
    GLint dst_x0, GLint dst_y0, GLint dst_x1, GLint dst_y1,
    GLbitfield mask, GLenum filter);
void oracle_ff_glNoop();
GLuint oracle_ff_glReturnUint();
GLint oracle_ff_glReturnInt();
const GLubyte *oracle_ff_glReturnString();

#define glBindFramebuffer            oracle_ff_glBindFramebuffer
#define glClear                      oracle_ff_glClear
#define glClearColor                 oracle_ff_glClearColor
#define glColorMask                  oracle_ff_glColorMask
#define glDisable                    oracle_ff_glDisable
#define glDrawElements               oracle_ff_glDrawElements
#define glEnable                     oracle_ff_glEnable
#define glFinish                     oracle_ff_glFinish
#define glFramebufferTexture2D       oracle_ff_glFramebufferTexture2D
#define glGetBooleanv                oracle_ff_glGetBooleanv
#define glGetError                   oracle_ff_glGetError
#define glGetFloatv                  oracle_ff_glGetFloatv
#define glGetIntegerv                oracle_ff_glGetIntegerv
#define glIsEnabled                  oracle_ff_glIsEnabled
#define glReadPixels                 oracle_ff_glReadPixels
#define glViewport                   oracle_ff_glViewport
#define glCheckNamedFramebufferStatus oracle_ff_glCheckNamedFramebufferStatus
#define glBlitNamedFramebuffer       oracle_ff_glBlitNamedFramebuffer

#define glActiveTexture              oracle_ff_glNoop
#define glAlphaFunc                  oracle_ff_glNoop
#define glAttachShader               oracle_ff_glNoop
#define glBindRenderbuffer           oracle_ff_glNoop
#define glBindTexture                oracle_ff_glNoop
#define glBlendFuncSeparate          oracle_ff_glNoop
#define glCheckFramebufferStatus     oracle_ff_glReturnUint
#define glClearDepth                 oracle_ff_glNoop
#define glCompileShader              oracle_ff_glNoop
#define glCreateProgram              oracle_ff_glReturnUint
#define glCreateShader               oracle_ff_glReturnUint
#define glCullFace                   oracle_ff_glNoop
#define glDeleteFramebuffers         oracle_ff_glNoop
#define glDeleteProgram              oracle_ff_glNoop
#define glDeleteRenderbuffers        oracle_ff_glNoop
#define glDeleteShader               oracle_ff_glNoop
#define glDeleteTextures             oracle_ff_glNoop
#define glDepthFunc                  oracle_ff_glNoop
#define glDisableVertexAttribArray   oracle_ff_glNoop
#define glEnableVertexAttribArray    oracle_ff_glNoop
#define glFramebufferRenderbuffer    oracle_ff_glNoop
#define glGenFramebuffers            oracle_ff_glNoop
#define glGenRenderbuffers           oracle_ff_glNoop
#define glGenTextures                oracle_ff_glNoop
#define glGetAttribLocation          oracle_ff_glReturnInt
#define glGetProgramInfoLog          oracle_ff_glNoop
#define glGetProgramiv               oracle_ff_glNoop
#define glGetShaderInfoLog           oracle_ff_glNoop
#define glGetShaderiv                oracle_ff_glNoop
#define glGetString                  oracle_ff_glReturnString
#define glGetStringi                 oracle_ff_glReturnString
#define glGetUniformLocation         oracle_ff_glReturnInt
#define glLinkProgram                oracle_ff_glNoop
#define glRenderbufferStorage        oracle_ff_glNoop
#define glShaderSource               oracle_ff_glNoop
#define glTexImage2D                 oracle_ff_glNoop
#define glTexParameteri              oracle_ff_glNoop
#define glTexSubImage2D              oracle_ff_glNoop
#define glUniform1fv                 oracle_ff_glNoop
#define glUniform1i                  oracle_ff_glNoop
#define glUniform1iv                 oracle_ff_glNoop
#define glUniform2fv                 oracle_ff_glNoop
#define glUniform2iv                 oracle_ff_glNoop
#define glUniform3fv                 oracle_ff_glNoop
#define glUniform3iv                 oracle_ff_glNoop
#define glUniform4fv                 oracle_ff_glNoop
#define glUniform4iv                 oracle_ff_glNoop
#define glUniformMatrix2fv           oracle_ff_glNoop
#define glUniformMatrix3fv           oracle_ff_glNoop
#define glUniformMatrix4fv           oracle_ff_glNoop
#define glUseProgram                 oracle_ff_glNoop
#define glVertexAttribPointer        oracle_ff_glNoop

#endif
