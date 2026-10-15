#pragma once

#include "lemon/backends/backend_descriptor.h"

namespace lemon {
namespace backends {
namespace audio {

// The audio.cpp backend descriptor (plain data). Header-only `inline const` so it
// links into both the lemonade CLI and lemond without a separate source file.
inline const BackendDescriptor descriptor = {
    /*recipe*/          "audio.cpp",
    /*display_name*/    "Audio.cpp",
#ifdef _WIN32
    /*binary*/          "audiocpp_server.exe",
#else
    /*binary*/          "audiocpp_server",
#endif
    /*config_section*/  "audio",
    /*default_device*/  DEVICE_GPU,
    /*slot_policy*/     SlotPolicy::Standard,
    /*selectable_backend*/ true,
    /*uses_ctx_size*/   false,
    /*dynamic_models*/  false,
    /*options*/ {
        {"audio.cpp_backend", "--audio", "", "BACKEND",
         "audio.cpp backend to use", "Audio.cpp Options"},
        {"audio_args", "--audio-args", "", "ARGS",
         "Custom arguments to pass to audiocpp_server (must not conflict with managed args)", "Audio.cpp Options"},
    },
    /*support*/ {
        {"rocm", BackendTier::Experimental, BackendFormat::Native, {"windows"},
         {{"amd_gpu", {"gfx1100", "gfx1101", "gfx1102", "gfx1103", "gfx1150", "gfx1151", "gfx1200", "gfx1201"}}},
         "AMD Radeon (gfx1100 family and newer)"},
    },
    /*supported_modes*/ {"tts"},
    /*required_checkpoints*/ {"main"},
    /*default_capabilities*/ {},
    /*web_display_name*/ "audio.cpp",
    /*rocm_channels*/   {},  // single bundled-runtime artifact, no stable/nightly channels
    /*exposes_prometheus_metrics*/ false,
    /*rocm_requires_cwsr_fix*/ false,
    /*version_policy*/  VersionPolicy::Exact,
    /*self_manages_downloads*/ false,
    /*takes_args*/      true,
    /*arg_variants*/    {"rocm"},
    /*bin_variants*/    {"rocm"},
    /*config_extra*/    nlohmann::json::object(),
};

}  // namespace audio
}  // namespace backends
}  // namespace lemon
