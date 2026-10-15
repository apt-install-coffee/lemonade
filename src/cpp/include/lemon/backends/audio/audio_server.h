#pragma once

#include "lemon/backends/backend_registry.h"

#include "lemon/backends/backend_utils.h"
#include "lemon/server_capabilities.h"
#include "lemon/wrapped_server.h"
#include <filesystem>
#include <string>
#include <vector>

namespace lemon {
namespace backends {

class AudioServer : public WrappedServer, public ITextToSpeechServer {
public:
    static InstallParams get_install_params(const std::string& backend, const std::string& version);

    explicit AudioServer(const std::string& log_level,
                         ModelManager* model_manager,
                         BackendManager* backend_manager);
    ~AudioServer() override;

    void load(const std::string& model_name,
              const ModelInfo& model_info,
              const RecipeOptions& options,
              bool do_not_upgrade) override;

    void unload() override;

    void audio_speech(const json& request, httplib::DataSink& sink) override;
    // audiocpp_server emits WAV out of the box; MP3 needs the optional
    // mp3_encode frontend module, which the ROCm packages do not build.
    std::vector<std::string> supported_audio_formats() const override {
        return {"wav"};
    }
    std::vector<std::string> supported_streaming_audio_formats() const override {
        return {"pcm"};
    }

private:
    std::filesystem::path config_dir_;
};

namespace audio {
std::unique_ptr<WrappedServer> create(const BackendContext& ctx);
const BackendSpec* spec();
const BackendOps* ops();
constexpr uint32_t capabilities() { return capability_mask_of<AudioServer>(); }
}  // namespace audio
}  // namespace backends
}  // namespace lemon
