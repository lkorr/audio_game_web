add_executable(audio_game
    src/main.cpp
    src/GLLoader.cpp
    src/AudioDevice.cpp
    src/AudioWorld.cpp
    src/DevMode.cpp
    src/EngineTuning.cpp
    src/Occlusion.cpp
    src/SceneIO.cpp
    src/SoundDirector.cpp
    src/SoundLibrary.cpp
    src/Source.cpp
    src/Sounds.cpp
    src/Player.cpp
    src/Scene.cpp
    src/Stalker.cpp
    src/SliceGame.cpp
    src/Renderer.cpp
)

target_link_libraries(audio_game PRIVATE
    xyzpan_engine
    SDL3::SDL3-static
    glm::glm
)

target_include_directories(audio_game PRIVATE
    ${miniaudio_SOURCE_DIR}
)

if(MSVC)
    target_compile_options(audio_game PRIVATE $<$<CONFIG:Release>:/O2>)
endif()
