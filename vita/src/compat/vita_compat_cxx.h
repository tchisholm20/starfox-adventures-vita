/* vita_compat_cxx.h - force-included into map_view.cpp (C++). Its `main` is
 * renamed to map_view_main by -D, and a renamed C++ function would get a
 * MANGLED name the C wrapper cannot call. Declaring it extern "C" first gives
 * the later definition C linkage. */
#pragma once
extern "C" int map_view_main(int argc, char** argv);
