eacp_add_app(Mandelbrot Main.cpp)
target_link_libraries(Mandelbrot PRIVATE eacp-gpu)
set_target_properties(Mandelbrot PROPERTIES MACOSX_BUNDLE TRUE)

set_default_target_setting(Mandelbrot)
