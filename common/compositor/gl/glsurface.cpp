/*
// Copyright (c) 2016 Intel Corporation
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
*/

#include "glsurface.h"

#include "hwctrace.h"
#include "overlaybuffer.h"
#include "resourcemanager.h"
#include "shim.h"

#ifndef GL_DRAW_FRAMEBUFFER
#define GL_DRAW_FRAMEBUFFER 0x8CA9
#endif

namespace hwcomposer {

GLSurface::GLSurface(uint32_t width, uint32_t height)
    : NativeSurface(width, height) {
}

GLSurface::~GLSurface() {
}

bool GLSurface::InitializeGPUResources() {
  EGLDisplay egl_display = eglGetCurrentDisplay();
  OverlayBuffer* layer_buffer = layer_.GetBuffer();
  // Create EGLImage.
  if (!layer_buffer) {
    ETRACE("Failed to get layer buffer for EGL image");
    return false;
  }
  const ResourceHandle& import =
      layer_buffer->GetGpuResource(egl_display, false);

  if (import.image_ == EGL_NO_IMAGE_KHR) {
    ETRACE("Failed to make EGL image.");
    return false;
  }

  // Bind Fb.
  fb_ = import.fb_;
  texture_id_ = import.texture_;
  return true;
}

bool GLSurface::MakeCurrent() {
  if (!fb_ && !InitializeGPUResources()) {
    ETRACE("Failed to initialize gpu resources.");
    return false;
  }

  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fb_);
  glBindTexture(GL_TEXTURE_2D, texture_id_);
  GLenum status = glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER);
  if (status != GL_FRAMEBUFFER_COMPLETE) {
    switch (status) {
      case (GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT):
        ETRACE("GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT.");
        break;
      case (GL_FRAMEBUFFER_INCOMPLETE_MISSING_ATTACHMENT):
        ETRACE("GL_FRAMEBUFFER_INCOMPLETE_MISSING_ATTACHMENT.");
        break;
      case (GL_FRAMEBUFFER_UNSUPPORTED):
        ETRACE("GL_FRAMEBUFFER_UNSUPPORTED.");
        break;
      default:
        break;
    }

    ETRACE("GL Framebuffer is not complete %d.", fb_);
    return false;
  }

  return true;
}

}  // namespace hwcomposer
