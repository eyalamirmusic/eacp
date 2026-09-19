# glad

A [glad2](https://github.com/Dav1dde/glad) 2.0.8 loader, generated once and
committed, for the OpenGL backend (`Lib/eacp/GPU/OpenGL/`). It carries EGL 1.5,
OpenGL 3.3-4.6 core and OpenGL ES 3.0-3.2 in one merged `glad/gl.h`, so one
binary loads either profile: `gladLoadGL` for desktop GL and `gladLoadGLES2`
for ES, both over a loader function we supply.

Generated with no internal loader (`--loader` is deliberately absent), so glad
opens nothing itself. `GLContext-Linux.cpp` dlopens `libEGL.so.1`, takes
`eglGetProcAddress` out of it and hands that to `gladLoadEGL`; the GL entry
points come from the same `eglGetProcAddress` once a context is current. That
is what makes `nothing links libEGL or libGL` true - the whole of the backend
is reached through `dlopen`, so a machine with no EGL builds the same binary
and reports `Device::isValid()` false.

Built by `CMake/FindGLBackend.cmake` as the `eacp-gl` target rather than from
`ThirdParty/CMakeLists.txt`, because that file is added on every platform while
nothing outside the Linux GL backend links these sources - the miniz precedent
is "its own C target, warnings silenced, never in a unity build", which is what
`FindGLBackend.cmake` gives it, one `find_package(GLBackend)` away from its one
consumer.

## The generator command

```bash
python3 -m venv venv && ./venv/bin/pip install glad2   # 2.0.8
./venv/bin/glad --api='gl:core=4.6,gles2=3.2,egl=1.5' \
                --extensions="$(paste -sd, extensions.txt)" \
                --merge --out-path=. c
```

`extensions.txt` is the list below, one per line. To update: install the next
glad2, run the command again over the same list, and change the version above.

## The extensions

The GL ones are the features `GLCapabilities` has a flag for (plan.md D4), each
of them a path the backend takes when it is there and does without when it is
not:

    GL_ARB_buffer_storage           GL_EXT_buffer_storage
    GL_ARB_clip_control             GL_EXT_clip_control
    GL_ARB_compute_shader
    GL_ARB_explicit_uniform_location
    GL_ARB_separate_shader_objects
    GL_ARB_shader_image_load_store
    GL_ARB_shader_storage_buffer_object
    GL_ARB_shading_language_420pack
    GL_ARB_sync
    GL_ARB_texture_compression_bptc GL_EXT_texture_compression_bptc
    GL_ARB_texture_storage          GL_EXT_texture_storage
    GL_ARB_timer_query              GL_EXT_disjoint_timer_query
    GL_EXT_color_buffer_float       GL_EXT_color_buffer_half_float
    GL_EXT_read_format_bgra         GL_EXT_texture_format_BGRA8888
    GL_EXT_texture_compression_dxt1 GL_EXT_texture_compression_s3tc
    GL_KHR_debug
    GL_OES_texture_float_linear     GL_OES_texture_half_float_linear

And the EGL ones - the display, the context and the surfaces of plan.md D2 and
stage 3:

    EGL_EXT_device_base             EGL_EXT_device_enumeration
    EGL_EXT_device_query            EGL_EXT_platform_base
    EGL_EXT_platform_device         EGL_EXT_platform_wayland
    EGL_EXT_platform_xcb            EGL_KHR_create_context
    EGL_KHR_fence_sync              EGL_KHR_no_config_context
    EGL_KHR_platform_wayland        EGL_KHR_platform_x11
    EGL_KHR_surfaceless_context     EGL_MESA_platform_surfaceless

`EGL_MESA_device_software` is named by D5 and is **not** in this list: the
Khronos registry carries no XML for it, and it defines no entry point or token
of its own - a software device is recognised by that string appearing in
`eglQueryDeviceStringEXT(device, EGL_EXTENSIONS)`, which needs nothing
generated.

## License

`LICENSE` is upstream's: the glad source code is MIT, and the Khronos
specifications it is generated from carry their own terms, reproduced there.
