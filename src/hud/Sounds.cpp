#include "Sounds.hpp"

#include <Geode/Geode.hpp>

#include <array>
#include <filesystem>
#include <unordered_map>

using namespace geode::prelude;

namespace {
    constexpr std::array<char const*, 7> FILE_STEMS = {
        "fw-9-12", "fw-7-8", "fw-5-6", "fw-4", "fw-3", "fw-2", "fw-1",
    };

    FMOD::ChannelGroup* s_group = nullptr;
    std::unordered_map<int, FMOD::Sound*> s_sounds;
    bool s_loaded = false;
    bool s_enabled = true;

    std::filesystem::path findFile(int category) {
        auto stem = std::string(FILE_STEMS[category]);
        auto custom = Mod::get()->getConfigDir() / "sounds";
        for (auto ext : { ".ogg", ".mp3", ".wav" }) {
            auto path = custom / (stem + ext);
            if (std::filesystem::exists(path)) return path;
        }
        // Packaged files normally land flat in the resources folder; keep the subfolder as a fallback.
        auto flat = Mod::get()->getResourcesDir() / (stem + ".wav");
        if (std::filesystem::exists(flat)) return flat;
        return Mod::get()->getResourcesDir() / "sounds" / (stem + ".wav");
    }

    void loadAll(FMOD::System* system) {
        s_loaded = true;
        if (system->createChannelGroup("ill-replay-bot-sfx", &s_group) != FMOD_OK) s_group = nullptr;
        for (int category = 0; category < static_cast<int>(FILE_STEMS.size()); ++category) {
            auto path = findFile(category);
            if (!std::filesystem::exists(path)) continue;
            FMOD::Sound* sound = nullptr;
            auto res = system->createSound(
                utils::string::pathToString(path).c_str(),
                FMOD_DEFAULT | FMOD_LOOP_OFF | FMOD_CREATESAMPLE, nullptr, &sound
            );
            if (res == FMOD_OK && sound) s_sounds[category] = sound;
        }
    }
}

namespace Sounds {
    void setEnabled(bool enabled) {
        s_enabled = enabled;
    }

    bool isEnabled() {
        return s_enabled;
    }

    void playCategory(int category) {
        if (!s_enabled || category < 0 || category >= static_cast<int>(FILE_STEMS.size())) return;
        auto engine = FMODAudioEngine::get();
        if (!engine || !engine->m_system || engine->m_sfxVolume <= 0.f) return;
        if (!s_loaded) loadAll(engine->m_system);

        auto it = s_sounds.find(category);
        if (it == s_sounds.end()) return;
        if (s_group) s_group->setVolume(engine->m_sfxVolume);
        engine->m_system->playSound(it->second, s_group, false, nullptr);
    }
}
