#pragma once

#include <string>

namespace Sounds {
    // Plays the click sound for a frame window category (0 = "9-12" ... 6 = "1").
    // A file named like the built-in one (fw-1.wav / .ogg / .mp3, ...) in the mod's
    // config folder under sounds/ replaces the built-in sound.
    void playCategory(int category);
    // Clickbot: a mouse press / release sound for each click the bot makes
    // (click-down / click-up in the same folders, replaceable the same way).
    void playClick(bool down);
    void playSound(int index);
    void setEnabled(bool enabled);
    bool isEnabled();
}
