#pragma once
#include <cstdio>
namespace graphics { class ContextImpl; }

// Our64: the backend that draws nothing (null_ContextImpl.cpp).
graphics::ContextImpl * our64_createNullContext();
extern unsigned our64_null_draws; // draw calls the decoder made
extern FILE * our64_trace;    // when set, every draw and texture upload is hashed into it
extern bool our64_trace_verbose; // and every hashed value is printed too
