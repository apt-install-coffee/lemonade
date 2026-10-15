#include "lemon/backends/audio/audio.h"
#include "lemon/backends/audio/audio_server.h"
#include "lemon/backend_manager.h"
#include "lemon/backends/backend_ops.h"
#include "lemon/backends/backend_registry.h"
#include "lemon/backends/backend_utils.h"
#include "lemon/error_types.h"
#include "lemon/model_manager.h"
#include "lemon/runtime_config.h"
#include "lemon/system_info.h"
#include "lemon/utils/custom_args.h"
#include "lemon/utils/json_utils.h"
#include "lemon/utils/path_utils.h"
#include <filesystem>
#include <httplib.h>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>
#include <lemon/utils/aixlog.hpp>

namespace fs = std::filesystem;
using namespace lemon::utils;

namespace lemon {
namespace backends {

namespace {
bool is_rocm_backend(const std::string& backend) {
    return backend == "rocm" || backend == "rocm-stable";
}
}  // namespace

InstallParams AudioServer::get_install_params(const std::string& backend, const std::string& version) {
    (void)version;
    InstallParams params;
    // Upstream publishes no AMD binaries ("no hardware to test"); the HIP
    // packages with the ROCm runtime bundled come from the community fork.
    params.repo = "IIIIIllllIIIIIlllll/audio.cpp";

    if (is_rocm_backend(backend)) {
#ifdef _WIN32
        // ROCm 7.1 ships no hipBLASLt kernels for gfx1102 (RX 7600); that GPU
        // stays on the ROCm 6.4 package.
        if (SystemInfo::get_rocm_arch() == "gfx1102") {
            params.filename = "audiocpp-bin-win-hip-rocm6.4-x64.zip";
        } else {
            params.filename = "audiocpp-bin-win-hip-rocm7.1-x64.zip";
        }
#else
        throw std::runtime_error(
            "audio.cpp ROCm has no downloadable package on Linux; install "
            "audiocpp_server on PATH or set audio.rocm_bin to its full path");
#endif
    } else {
        throw std::runtime_error("[AudioServer] Unknown audio.cpp backend: " + backend);
    }

    return params;
}

AudioServer::AudioServer(const std::string& log_level, ModelManager* model_manager, BackendManager* backend_manager)
    : WrappedServer("audiocpp_server", log_level, model_manager, backend_manager) {
}

AudioServer::~AudioServer() {
    unload();
}

void AudioServer::load(const std::string& model_name,
                       const ModelInfo& model_info,
                       const RecipeOptions& options,
                       bool do_not_upgrade) {
    LOG(INFO, "AudioServer") << "Loading model: " << model_name << std::endl;
    LOG(INFO, "AudioServer") << "Per-model settings: " << options.to_log_string() << std::endl;
    (void)do_not_upgrade;

    std::string model_path = model_info.resolved_path();
    if (model_path.empty() || !fs::exists(path_from_utf8(model_path))) {
        throw std::runtime_error("Model file not found for checkpoint: " + model_info.checkpoint());
    }

    std::string backend = options.get_option("audio.cpp_backend");
    if (backend.empty()) {
        auto supported = SystemInfo::get_supported_backends(audio::spec()->recipe);
        if (supported.backends.empty()) {
            throw UnsupportedOperationException(
                "Audio.cpp speech", "this system: no supported AMD ROCm GPU detected");
        }
        backend = supported.backends[0];
    }
    RuntimeConfig::validate_backend_choice("audio", backend);

    LOG(INFO, "AudioServer") << "Using model: " << model_path << std::endl;
    LOG(INFO, "AudioServer") << "Using backend: " << backend << std::endl;

    // A *_bin path pin skips the install flow entirely. Without one, Windows
    // downloads the fork's HIP package; Linux has no package to download
    // (upstream ships no AMD binaries, the fork's zips are Windows-only), so it
    // runs the system audiocpp_server — the nix package puts it on PATH.
    std::string exe_path = BackendUtils::find_external_backend_binary(audio::spec()->recipe, backend);
    if (exe_path.empty()) {
#ifdef _WIN32
        backend_manager_->install_backend(audio::spec()->recipe, backend);
        exe_path = BackendUtils::get_backend_binary_path(*audio::spec(), backend);
#else
        exe_path = utils::find_executable_in_path(audio::spec()->binary);
        if (exe_path.empty()) {
            throw std::runtime_error(
                "audio.cpp ROCm has no downloadable package on Linux; install "
                "audiocpp_server on PATH or set audio.rocm_bin to its full path");
        }
#endif
    }

    port_ = choose_port();
    if (port_ == 0) {
        throw std::runtime_error("Failed to find an available port");
    }

    LOG(INFO, "AudioServer") << "Starting server on port " << port_ << std::endl;

    // audiocpp_server is config-driven: every model it may serve is declared in
    // server.json, so the Lemonade model becomes one entry under a fixed family.
    // Supertonic is the family the ROCm packages can actually run: it streams
    // and needs no espeak-ng, unlike kokoro_tts on this release.
    std::string family = model_info.extra<std::string>("audio_family", "supertonic");
    // OpenAI voice aliases have no native equivalent in either family.
    json voice_presets = json::object();
    if (family == "kokoro_tts") {
        voice_presets["onyx"] = {{"voice_id", "am_onyx"}};
    } else {
        voice_presets["onyx"] = {{"voice_id", "M1"}};
    }

    // The engine's GPU backend name differs per package: the Windows HIP fork
    // accepts "hip", while the nix Linux package is a Vulkan build (RADV on
    // amd_gpu). The config carries the value; audio_args may still pass
    // --backend to override it (the CLI flag wins over the config).
#ifdef _WIN32
    const std::string engine_backend = "hip";
#else
    const std::string engine_backend = "vulkan";
#endif

    json server_config = {
        {"host", "127.0.0.1"},
        {"port", port_},
        {"backend", engine_backend},
        {"lazy_load", false},
        {"models", json::array({{
            {"id", model_name},
            {"family", family},
            {"path", model_path},
            {"task", "tts"},
            {"mode", "streaming"},
            {"voice_presets", voice_presets},
        }})},
    };

    // Native filesystem math keeps wide Windows temp paths intact; converting
    // through .string() would use the ANSI code page.
    config_dir_ = fs::temp_directory_path() / ("lemon-audio-" + std::to_string(port_));
    fs::create_directories(config_dir_);
    fs::path config_path = config_dir_ / "server.json";
    JsonUtils::save_to_file(server_config, path_to_utf8(config_path));

    std::vector<std::string> args = {
        "--config", path_to_utf8(config_path),
        "--no-ui"
    };

    std::set<std::string> reserved_flags = {
        "--config",
        "--host",
        "--port",
        "--ui",
        "--no-ui"
    };

    std::string audio_args = options.get_option("audio_args");
    if (!audio_args.empty()) {
        std::string validation_error = validate_custom_args(audio_args, reserved_flags);
        if (!validation_error.empty()) {
            throw std::invalid_argument(
                "Invalid custom audiocpp_server arguments:\n" + validation_error
            );
        }

        LOG(DEBUG, "AudioServer") << "Adding custom arguments: " << audio_args << std::endl;
        std::vector<std::string> custom_args_vec = parse_custom_args(audio_args);
        args.insert(args.end(), custom_args_vec.begin(), custom_args_vec.end());
    }

    ServerCommand command;
    command.program = exe_path;
    command.args = std::move(args);
    command.port = port_;
    start_server(std::make_unique<NativeProcess>(ProcessOutput{is_debug(), false}), command);
}

void AudioServer::unload() {
    WrappedServer::unload();
    std::error_code ec;
    if (!config_dir_.empty()) {
        fs::remove_all(config_dir_, ec);
    }
}

void AudioServer::audio_speech(const json& request, httplib::DataSink& sink) {
    json tts_request = request;
    // server.json declares the loaded model under its canonical Lemonade name;
    // a client alias ("builtin.Audio-TTS") would be an unknown id upstream.
    tts_request["model"] = get_model_name();

    // audiocpp_server streams only when stream_format is present, and its only
    // raw streaming transport is headerless PCM — which is also the only
    // format lemonade serves for stream_format=audio.
    if (request.contains("stream_format") || request.value("stream", false)) {
        tts_request["stream_format"] = "audio";
        tts_request["response_format"] = "pcm";
    }

    forward_streaming_request("/v1/audio/speech", tts_request.dump(), sink, false);
}

} // namespace backends
} // namespace lemon

namespace lemon {
namespace backends {
namespace audio {

std::unique_ptr<WrappedServer> create(const BackendContext& ctx) {
    return make_server<AudioServer>(ctx);
}

const BackendSpec* spec() { return make_spec<AudioServer>(descriptor); }
const BackendOps* ops() { return default_backend_ops(); }

}  // namespace audio
}  // namespace backends
}  // namespace lemon
