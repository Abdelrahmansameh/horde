#include "app/BalanceMode.h"
#include "app/Modes.h"
#include "game/balance/BalanceRun.h"
#include "game/balance/RunRecording.h"
#include "game/balance/BalanceCatalog.h"
#include "game/level/Level.h"
#include "platform/FileIO.h"
#include "platform/Window.h"
#include "render/Camera.h"
#include "render/Renderer.h"
#include "render/Screenshot.h"
#include "sim/SimWorld.h"
#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>

namespace immune::app {
namespace {
using json = config::Json;
json invalid(const std::string& error) {
    return {{"schema",1},{"summary",{{"status","invalid"},{"valid",false}}},
            {"diagnostics",{{"errors",json::array({error})}}}};
}
int emit(json report, const Options& options) {
    report["provenance"]["build_stamp"] = std::string(__DATE__) + " " + __TIME__;
    std::ifstream binary(options.executable_path,std::ios::binary);
    if (binary) {
        const std::string data{std::istreambuf_iterator<char>(binary),std::istreambuf_iterator<char>()};
        report["provenance"]["executable_hash"] = game::balance::fingerprint(data);
    }
    report["provenance"]["executable"] = options.executable_path;
    const std::string output = report.dump(2)+"\n";
    if (options.output_explicit && (options.mode == Mode::BalanceRun || options.mode == Mode::BalanceCatalog)) {
        const auto parent = std::filesystem::path(options.out_path).parent_path();
        if (!parent.empty()) std::filesystem::create_directories(parent);
        if (!platform::write_text_file(options.out_path,output)) {
            std::fputs("could not write balance report\n",stderr); return 1;
        }
    }
    std::fwrite(output.data(),1,output.size(),stdout);
    return report.contains("summary") && report.at("summary").value("status",std::string("invalid")) == "invalid" ? 1 : 0;
}
json read_json(const std::string& path) {
    const auto text = platform::read_text_file(path);
    if (!text) throw std::runtime_error("cannot read '"+path+"'");
    return json::parse(*text);
}
}

int run_balance(const Options& options) {
    try {
        const auto document = read_json(options.balance_path);
        if (document.value("kind",std::string()) == "recording") {
            if (!document.value("replayable",false)) return emit(invalid("recording is marked as not replayable; inspect its diagnostics"),options);
            game::GameConfig base;
            std::string error;
            if (!game::parse_game_config_documents(document.at("base_config").get<std::vector<json>>(),base,error))
                return emit(invalid("invalid recording config: "+error),options);
            auto report = game::balance::execute(document.at("spec"),base);
            std::vector<std::string> errors;
            const bool matches = game::balance::recording_matches(document,report,errors);
            report["recording_validation"] = {{"matches",matches},{"errors",errors},{"source",options.balance_path}};
            if (!matches) {
                report["summary"]["status"] = "invalid"; report["summary"]["valid"] = false;
                for (const auto& e : errors) report["diagnostics"]["errors"].push_back("player replay mismatch: "+e);
            }
            return emit(std::move(report),options);
        }
        const auto& spec = document;
        auto config_options = options;
        if (!config_options.config_pinned && spec.contains("config_dir"))
            config_options.config_dir = spec.at("config_dir").get<std::string>();
        const auto tuning = load_headless_config(config_options);
        if (!tuning.ok) return emit(invalid("gameplay config loading failed"),options);
        std::ofstream progress;
        game::balance::AttemptCallback attempt_completed;
        if (spec.contains("campaign") && options.output_explicit) {
            const auto parent = std::filesystem::path(options.out_path).parent_path();
            if (!parent.empty()) std::filesystem::create_directories(parent);
            progress.open(options.out_path + ".progress.jsonl", std::ios::out | std::ios::trunc);
            if (!progress) throw std::runtime_error("cannot write campaign progress journal");
            attempt_completed = [&](const json& attempt) {
                json event = {{"schema",1},{"kind","completed_campaign_attempt"},
                    {"level",attempt.at("replay_spec").at("level_document").at("name")}};
                for (const char* field : {"attempt_index","level_index","level_attempt","summary","reward",
                                          "purchases","discovery","progression_after","state_hash"})
                    if (attempt.contains(field)) event[field] = attempt.at(field);
                progress << event.dump() << '\n' << std::flush;
                if (!progress) throw std::runtime_error("could not append campaign progress journal");
            };
        }
        auto report = game::balance::execute(spec,tuning.cfg,{},attempt_completed);
        report["provenance"]["config_dir"] = resolve_config_dir(config_options);
        report["provenance"]["config_file_hash"] = tuning.hash;
        report["provenance"]["spec_path"] = options.balance_path;
        return emit(std::move(report),options);
    } catch (const std::exception& error) { return emit(invalid(error.what()),options); }
}

int run_balance_catalog(const Options& options) {
    const auto tuning = load_headless_config(options);
    if (!tuning.ok) return emit(invalid("gameplay config loading failed"),options);
    return emit(game::balance::describe_catalog(tuning.cfg),options);
}

int run_balance_replay(const Options& options) {
    try {
        const auto recorded = read_json(options.balance_path);
        if (recorded.value("kind",std::string()) != "run" || !recorded.contains("replay_spec"))
            throw std::runtime_error("choose an individual run report for replay");
        game::GameConfig base;
        std::string error;
        if (!game::parse_game_config_documents(recorded.at("provenance").at("base_config").get<std::vector<json>>(),base,error))
            throw std::runtime_error("invalid pinned replay config: "+error);
        auto spec = recorded.at("replay_spec");
        const u64 ticks = options.ticks_explicit ? options.ticks
            : recorded.at("summary").at("duration_ticks").get<u64>();
        if (ticks == 0) throw std::runtime_error("capture tick must be positive");
        spec["max_ticks"] = ticks;
        // Future actions are irrelevant to a capture before their tick.
        auto& actions = spec.at("actions");
        actions.erase(std::remove_if(actions.begin(),actions.end(),[ticks](const json& a) {
            return a.at("tick").get<u64>() >= ticks;
        }),actions.end());
        bool captured = false;
        const auto callback = [&](const game::balance::CaptureView& view) {
            platform::Window window;
            if (!platform::create_headless_gl(window,options.width,options.height))
                throw std::runtime_error("replay GL context: "+window.error());
            render::Renderer renderer;
            render::RendererDesc rd;
            rd.framebuffer_width = window.width(); rd.framebuffer_height = window.height();
            rd.max_chaff_instances = static_cast<u32>(view.world.desc().max_chaff);
            if (!renderer.init(rd)) throw std::runtime_error("replay renderer: "+renderer.error());
            render::Camera camera;
            camera.set_viewport(window.width(),window.height());
            camera.set_bounds(view.world.desc().world_bounds);
            camera.set_center(options.has_focus ? options.focus : view.level.camera.has_center
                ? view.level.camera.center : view.world.desc().world_bounds.center());
            camera.set_view_height(options.view_height > 0 ? options.view_height
                : view.level.camera.view_height > 0 ? view.level.camera.view_height
                : view.world.desc().world_bounds.size().y);
            camera.clamp_to_bounds();
            renderer.begin_frame(camera,1.0f);
            render::TissueDecor decor;
            decor.flow = &view.world.flow();
            decor.pattern_scale = render::tissue_pattern_scale(camera.view_height());
            renderer.submit_tissue(view.world.tissue(),view.world.sdf(),0,&decor);
            renderer.submit_chaff(view.world.chaff(),view.world.spatial());
            renderer.submit_entities(view.world.ecs());
            renderer.submit_fields(view.world.damage().rendered_fields().data(),view.world.damage().rendered_fields().size());
            renderer.submit_projectiles(view.world.projectiles());
            renderer.submit_swarmers(view.world.swarmers());
            renderer.submit_fluid(view.world.fluid(),view.world.fluid_system().draw_radius());
            renderer.end_frame();
            const auto parent = std::filesystem::path(options.out_path).parent_path();
            if (!parent.empty()) std::filesystem::create_directories(parent);
            if (!render::capture_framebuffer_png(options.out_path,window.width(),window.height()))
                throw std::runtime_error("could not write replay PNG");
            captured = true;
        };
        auto result = game::balance::execute(spec,base,callback);
        if (!captured) return emit(result,options);
        const bool at_end = ticks == recorded.at("summary").at("duration_ticks").get<u64>();
        const bool matches = at_end && result.at("state_hash") == recorded.at("state_hash") &&
            result.at("state") == recorded.at("state") && result.at("economy") == recorded.at("economy") &&
            result.at("wave_status") == recorded.at("wave_status");
        if (at_end && !matches) {
            result["summary"]["valid"] = false; result["summary"]["status"] = "invalid";
            result["diagnostics"]["errors"].push_back("terminal replay differs from the recorded gameplay/economy/wave state");
        }
        result["capture"] = {{"path",options.out_path},{"tick",result.at("summary").at("duration_ticks")},
            {"compared_to_recorded_terminal",at_end},{"terminal_state_matches",matches}};
        // Full results remain available in the original report; replay stdout is concise.
        json output = {{"schema",1},{"summary",result.at("summary")},{"capture",result.at("capture")},
            {"state_hash",result.at("state_hash")},{"diagnostics",result.at("diagnostics")}};
        return emit(std::move(output),options);
    } catch (const std::exception& error) { return emit(invalid(error.what()),options); }
}
} // namespace immune::app
