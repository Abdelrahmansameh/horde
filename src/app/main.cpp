#include "app/App.h"
#include "app/AutoplayMode.h"
#include "app/Cli.h"
#include "app/LevelTools.h"
#include "app/Modes.h"
#include "core/Log.h"

#include <cstdio>

int main(int argc, char** argv) {
    using namespace immune;

    const app::Options options = app::parse_args(argc, argv);

    if (options.quiet) {
        log::set_enabled(false);
    } else if (options.verbose) {
        log::set_level(log::Level::Debug);
    }

    switch (options.mode) {
        case app::Mode::Help:
            std::fputs(app::usage_text(), stdout);
            return 0;

        case app::Mode::Invalid:
            std::fprintf(stderr, "error: %s\n\n", options.error.c_str());
            std::fputs(app::usage_text(), stderr);
            return 2;

        case app::Mode::ListScenarios:
            return app::run_list_scenarios();

        case app::Mode::DumpConfig:
            return app::run_dump_config(options);

        case app::Mode::LevelCheck:
            return app::run_level_check(options);

        case app::Mode::LevelFmt:
            return app::run_level_fmt(options);

        case app::Mode::Bench:
            return app::run_bench(options);

        case app::Mode::SimTest:
            return app::run_sim_test(options);

        case app::Mode::Screenshot:
            return app::run_screenshot(options);

        case app::Mode::Autoplay:
            return app::run_autoplay(options);

        // The editor is an interactive App state, not a headless mode: it needs
        // the window, the renderer and the real level-load path.
        case app::Mode::Editor:
        case app::Mode::Play:
        default: {
            app::App application;
            if (!application.init(options)) {
                application.shutdown();
                return 1;
            }
            const int code = application.run();
            application.shutdown();
            return code;
        }
    }
}
