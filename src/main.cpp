/**
 * Copyright(c) Live2D Inc. All rights reserved.
 *
 * Use of this source code is governed by the Live2D Open Software license
 * that can be found at https://www.live2d.com/eula/live2d-open-software-license-agreement_en.html.
 */

#include "LAppDelegate.hpp"
#include "LAppDefine.hpp"
#include <cstdlib>
#include <unistd.h>
#include <sys/types.h>
#include <pwd.h>
#include <cstring>
#include <string>
#include <climits>

int main(int argc, char* argv[])
{
    // waifuland toggle
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "toggle") == 0 || strcmp(argv[i], "--toggle") == 0) {
            system("killall -s SIGUSR1 waifuland");
            return 0;
        }
        if (strcmp(argv[i], "focus") == 0 || strcmp(argv[i], "--focus") == 0) {
            system("killall -s SIGUSR2 waifuland");
            return 0;
        }
    }

    // Parse models_dir
    std::string modelsDir = "";
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--models_dir") == 0 && i + 1 < argc) {
            modelsDir = argv[i + 1];
            i++;
        }
    }

    if (modelsDir.empty()) {
        const char *configHome = getenv("XDG_CONFIG_HOME");
        if (configHome && strlen(configHome) > 0) {
            modelsDir = std::string(configHome) + "/waifuland/models/";
        } else {
            const char *homeDir = getenv("HOME");
            if (!homeDir || strlen(homeDir) == 0) {
                homeDir = getpwuid(getuid())->pw_dir;
            }
            modelsDir = std::string(homeDir) + "/.config/waifuland/models/";
        }
    }

    // Resolve absolute path
    char resolved_path[PATH_MAX];
    if (realpath(modelsDir.c_str(), resolved_path) != NULL) {
        modelsDir = std::string(resolved_path);
    }

    // Ensure modelsDir ends with trailing slash
    if (!modelsDir.empty() && modelsDir.back() != '/') {
        modelsDir += "/";
    }

    LAppDefine::ModelsDir = modelsDir;

    // create the application instance
    if (LAppDelegate::GetInstance()->Initialize() == GL_FALSE)
    {
        return 1;
    }

    LAppDelegate::GetInstance()->Run();

    return 0;
}

