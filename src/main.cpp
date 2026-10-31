#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include "App.h"
#include "config/Settings.h"

#include <string>

int main(int argc, char* argv[])
{
    // Build step: writes the example settings file and exits, without opening
    // a window (so the example can't drift from the built-in defaults).
    if (argc == 3 && SDL_strcmp(argv[1], "--write-example-settings") == 0) {
        const std::string text = Settings::exampleText();
        if (!SDL_SaveFile(argv[2], text.data(), text.size())) {
            SDL_Log("Could not write %s: %s", argv[2], SDL_GetError());
            return 1;
        }
        return 0;
    }

    App app;
    if (!app.init(argc, argv))
        return 1;
    app.run();
    return 0;
}
