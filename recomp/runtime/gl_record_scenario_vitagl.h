/* Layer-2 recorder oracle only (ISAAC_GL_RECORD_SCENARIO): every native call
 * the shim makes is traced with its arguments, and every client byte the
 * native side reads is hashed AT READ TIME. Included after the oracle facade,
 * only by gl_vita_backend.c. */
#ifndef GL_RECORD_SCENARIO_VITAGL_H
#define GL_RECORD_SCENARIO_VITAGL_H

void rt_glActiveTexture(GLenum texture);
void rt_glBindTexture(GLenum target, GLuint texture);
void rt_glBlendFuncSeparate(GLenum a, GLenum b, GLenum c, GLenum d);
void rt_glViewport(GLint x, GLint y, GLsizei w, GLsizei h);
void rt_glUseProgram(GLuint program);
void rt_glDepthFunc(GLenum function);
void rt_glClear(GLbitfield mask);
void rt_glEnable(GLenum capability);
void rt_glEnableVertexAttribArray(GLuint index);
void rt_glDisableVertexAttribArray(GLuint index);
void rt_glVertexAttribPointer(GLuint index, GLint size, GLenum type,
    GLboolean normalized, GLsizei stride, const void *pointer);
void rt_glDrawElements(GLenum mode, GLsizei count, GLenum type, const void *indices);
GLboolean rt_vglIsaacDrawCanonicalQuads(GLsizei count);
void rt_glUniform1i(GLint location, GLint value);
void rt_glUniform4fv(GLint location, GLsizei count, const GLfloat *value);
void rt_glUniformMatrix4fv(GLint location, GLsizei count, GLboolean transpose,
    const GLfloat *value);
GLint rt_glGetUniformLocation(GLuint program, const GLchar *name);
GLint rt_glGetAttribLocation(GLuint program, const GLchar *name);
void rt_glDeleteTextures(GLsizei count, const GLuint *names);
void rt_glDeleteFramebuffers(GLsizei count, const GLuint *names);
void rt_glDeleteRenderbuffers(GLsizei count, const GLuint *names);
void rt_glBindFramebuffer(GLenum target, GLuint framebuffer);
void rt_glFramebufferTexture2D(GLenum target, GLenum attachment,
    GLenum texture_target, GLuint texture, GLint level);
GLenum rt_glCheckFramebufferStatus(GLenum target);
void rt_glGenRenderbuffers(GLsizei count, GLuint *names);
void rt_glBindRenderbuffer(GLenum target, GLuint name);
void rt_glRenderbufferStorage(GLenum target, GLenum format, GLsizei w, GLsizei h);
void rt_glGetIntegerv(GLenum name, GLint *value);
void rt_glReadPixels(GLint x, GLint y, GLsizei w, GLsizei h,
    GLenum format, GLenum type, void *pixels);
GLuint rt_glCreateProgram(void);
void rt_glLinkProgram(GLuint program);
void rt_glDeleteProgram(GLuint program);

#undef glActiveTexture
#undef glBindTexture
#undef glBlendFuncSeparate
#undef glViewport
#undef glUseProgram
#undef glDepthFunc
#undef glClear
#undef glEnable
#undef glEnableVertexAttribArray
#undef glDisableVertexAttribArray
#undef glVertexAttribPointer
#undef glDrawElements
#undef glUniform1i
#undef glUniform4fv
#undef glUniformMatrix4fv
#undef glGetUniformLocation
#undef glGetAttribLocation
#undef glDeleteTextures
#undef glDeleteFramebuffers
#undef glDeleteRenderbuffers
#undef glBindFramebuffer
#undef glFramebufferTexture2D
#undef glCheckFramebufferStatus
#undef glGenRenderbuffers
#undef glBindRenderbuffer
#undef glRenderbufferStorage
#undef glGetIntegerv
#undef glReadPixels
#undef glCreateProgram
#undef glLinkProgram
#undef glDeleteProgram
#define glActiveTexture rt_glActiveTexture
#define glBindTexture rt_glBindTexture
#define glBlendFuncSeparate rt_glBlendFuncSeparate
#define glViewport rt_glViewport
#define glUseProgram rt_glUseProgram
#define glDepthFunc rt_glDepthFunc
#define glClear rt_glClear
#define glEnable rt_glEnable
#define glEnableVertexAttribArray rt_glEnableVertexAttribArray
#define glDisableVertexAttribArray rt_glDisableVertexAttribArray
#define glVertexAttribPointer rt_glVertexAttribPointer
#define glDrawElements rt_glDrawElements
#define vglIsaacDrawCanonicalQuads rt_vglIsaacDrawCanonicalQuads
#define glUniform1i rt_glUniform1i
#define glUniform4fv rt_glUniform4fv
#define glUniformMatrix4fv rt_glUniformMatrix4fv
#define glGetUniformLocation rt_glGetUniformLocation
#define glGetAttribLocation rt_glGetAttribLocation
#define glDeleteTextures rt_glDeleteTextures
#define glDeleteFramebuffers rt_glDeleteFramebuffers
#define glDeleteRenderbuffers rt_glDeleteRenderbuffers
#define glBindFramebuffer rt_glBindFramebuffer
#define glFramebufferTexture2D rt_glFramebufferTexture2D
#define glCheckFramebufferStatus rt_glCheckFramebufferStatus
#define glGenRenderbuffers rt_glGenRenderbuffers
#define glBindRenderbuffer rt_glBindRenderbuffer
#define glRenderbufferStorage rt_glRenderbufferStorage
#define glGetIntegerv rt_glGetIntegerv
#define glReadPixels rt_glReadPixels
#define glCreateProgram rt_glCreateProgram
#define glLinkProgram rt_glLinkProgram
#define glDeleteProgram rt_glDeleteProgram

#endif
