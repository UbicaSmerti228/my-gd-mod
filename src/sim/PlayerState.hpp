#pragma once

#include "PlayerState.gen.hpp"

// Both players' full state at a checkpoint. GD's own checkpoints leave part of the
// player state behind, so a restored level can behave differently from the run that
// made the checkpoint; this is attached to every checkpoint taken while the bot is on
// and put back after the game's own restore (see hooks.cpp).
class PlayerStateHolder : public cocos2d::CCObject {
public:
    PlayerStateSnapshot p1;
    PlayerStateSnapshot p2;

    static PlayerStateHolder* create() {
        auto ret = new PlayerStateHolder();
        ret->autorelease();
        return ret;
    }
};
