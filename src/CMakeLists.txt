# ---------------------------------------------------------------------------
# Game executable
# ---------------------------------------------------------------------------
add_executable(pimbalgame
    main.cpp
    pimbalgame/Game.cpp
    pimbalgame/World.cpp
    pimbalgame/Ball.cpp
    pimbalgame/Flipper.cpp
    pimbalgame/Bumper.cpp
)

target_include_directories(pimbalgame PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

# SFML is built in-tree from the submodule, which exposes the namespaced alias
# targets SFML::System, SFML::Window and SFML::Graphics. No find_package /
# install step is required.
target_link_libraries(pimbalgame PRIVATE
    SFML::System
    SFML::Window
    SFML::Graphics
)

# Keep the console window on Windows when launched from an IDE/Explorer.
if(WIN32)
    set_target_properties(pimbalgame PROPERTIES WIN32_EXECUTABLE FALSE)
endif()

# Copy the bundled HUD font next to the executable so it can be found at
# runtime (see Game::loadFont).
file(COPY ${CMAKE_CURRENT_SOURCE_DIR}/../assets/fonts/DejaVuSans.ttf
     DESTINATION ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/fonts)
