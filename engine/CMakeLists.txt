add_library(xyzpan_engine STATIC
    src/Engine.cpp
    src/BinauralPipeline.cpp
    src/DistancePipeline.cpp
    src/ChestPipeline.cpp
    src/FloorPipeline.cpp
    src/ERPipeline.cpp
    src/Coordinates.cpp
    src/FDNReverb.cpp
    src/LFO.cpp
    src/SwarmMode.cpp
)

target_include_directories(xyzpan_engine
    PUBLIC include
)

target_compile_features(xyzpan_engine PUBLIC cxx_std_20)

# Release optimization: full optimization, SIMD, relaxed float for auto-vectorization
if(MSVC)
    target_compile_options(xyzpan_engine PRIVATE
        $<$<CONFIG:Release>:/O2 /arch:AVX2 /fp:fast>
    )
elseif(APPLE)
    target_compile_options(xyzpan_engine PRIVATE
        $<$<CONFIG:Release>:-O3 -ffast-math>
    )
    # No -mavx2: Apple Silicon uses ARM NEON; Clang auto-vectorizes with -O3.
elseif(EMSCRIPTEN)
    # WebAssembly: no -mavx2 (x86-only). WASM SIMD is 128-bit via -msimd128;
    # Clang auto-vectorizes the DSP loops into it at -O3. Same relaxed-float.
    target_compile_options(xyzpan_engine PRIVATE
        $<$<CONFIG:Release>:-O3 -msimd128 -ffast-math>
        $<$<NOT:$<CONFIG:Release>>:-msimd128>
    )
elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(xyzpan_engine PRIVATE
        $<$<CONFIG:Release>:-O3 -mavx2 -ffast-math>
    )
endif()

# NO juce:: link here — intentional architectural invariant.
# Engine must compile and link with zero JUCE headers.
