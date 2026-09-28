#include "FrameHud.hpp"
#include "Sounds.hpp"
#include "../analysis/Analyzer.hpp"
#include "../analysis/LStar.hpp"

#include <cmath>

using namespace geode::prelude;

namespace {
    constexpr std::array<char const*, FrameHud::CATEGORY_COUNT> CATEGORY_NAMES = {
        "9-12", "7-8", "5-6", "4", "3", "2", "1",
    };
    // Sampled from NaN's showcase videos.
    constexpr std::array<ccColor3B, FrameHud::CATEGORY_COUNT> CATEGORY_COLORS = {{
        { 150, 150, 255 },
        { 150, 205, 255 },
        { 110, 255, 110 },
        { 255, 255, 255 },
        { 255, 255, 90 },
        { 255, 170, 70 },
        { 255, 80, 80 },
    }};

    constexpr float COUNT_SCALE = 0.5f;
    constexpr float COUNT_PEAK_SCALE = 0.6f;
    constexpr float ROW_HEIGHT = 18.f;
    constexpr int TRAIL_TICKS = 360;
    constexpr int FUTURE_TICKS = 480;
    constexpr int FAN_BEHIND_TICKS = 120;
    constexpr int FAN_AHEAD_TICKS = 240;

    ccColor4F toColor4F(ccColor3B c, float alpha) {
        return { c.r / 255.f, c.g / 255.f, c.b / 255.f, alpha };
    }

    bool isP2Input(PlayLayer* layer, BotInput const& input) {
        return !input.player1 && layer->m_gameState.m_isDualMode;
    }
}

FrameHud* FrameHud::create(PlayLayer* layer) {
    auto ret = new FrameHud();
    if (ret->init(layer)) {
        ret->autorelease();
        return ret;
    }
    delete ret;
    return nullptr;
}

int FrameHud::categoryFor(size_t index) {
    auto const& bot = Bot::get();
    auto const& a = bot.replay.analysis[index];
    if (!a.analyzed || a.unreliable || a.capped) return -1;
    // CBF windows are fractional: 3.44 counts as "3", 0.6 as "1".
    double w = bot.replay.windowOf(index, bot.useCbf);
    if (w >= 13.0) return -1;
    if (w >= 9.0) return 0;
    if (w >= 7.0) return 1;
    if (w >= 5.0) return 2;
    if (w >= 4.0) return 3;
    if (w >= 3.0) return 4;
    if (w >= 2.0) return 5;
    return 6;
}

ccColor3B FrameHud::categoryColor(int category) {
    if (category < 0 || category >= CATEGORY_COUNT) return { 200, 200, 200 };
    return CATEGORY_COLORS[category];
}

bool FrameHud::init(PlayLayer* layer) {
    if (!CCNode::init()) return false;
    m_layer = layer;
    this->setID("frame-hud"_spr);

    auto winSize = CCDirector::get()->getWinSize();

    this->buildCounter();

    m_shareLabel = CCLabelBMFont::create("0.00%", "bigFont.fnt");
    m_shareLabel->setAnchorPoint({ 0.f, 0.f });
    m_shareLabel->setScale(0.5f);
    m_shareLabel->setPosition({ 5.f, 24.f });
    this->addChild(m_shareLabel);

    m_lstarLabel = CCLabelBMFont::create("0.00", "bigFont.fnt");
    m_lstarLabel->setAnchorPoint({ 0.f, 0.f });
    m_lstarLabel->setScale(0.5f);
    m_lstarLabel->setPosition({ 5.f, 5.f });
    this->addChild(m_lstarLabel);

    m_cpsLabel = CCLabelBMFont::create("0/0/0 CPS", "bigFont.fnt");
    m_cpsLabel->setAnchorPoint({ 1.f, 1.f });
    m_cpsLabel->setScale(0.35f);
    m_cpsLabel->setColor({ 200, 200, 200 });
    m_cpsLabel->setOpacity(170);
    m_cpsLabel->setPosition({ winSize.width - 5.f, winSize.height - 5.f });
    this->addChild(m_cpsLabel);

    // Markers and trajectories live in the object layer, so they follow the camera.
    m_draw = CCDrawNode::create();
    m_draw->setID("trajectories"_spr);
    m_markers = CCNode::create();
    m_markers->setID("markers"_spr);
    if (auto objects = layer->m_objectLayer) {
        objects->addChild(m_draw, 1000);
        objects->addChild(m_markers, 1001);
    }

    this->scheduleUpdate();
    this->resetTo(0);
    return true;
}

void FrameHud::buildCounter() {
    m_counter = CCNode::create();
    auto winSize = CCDirector::get()->getWinSize();
    m_counter->setPosition({ 5.f, winSize.height - 5.f });
    this->addChild(m_counter);

    std::array<CCLabelBMFont*, CATEGORY_COUNT> names {};
    float nameWidth = 25.f;
    for (int i = 0; i < CATEGORY_COUNT; ++i) {
        auto name = CCLabelBMFont::create(fmt::format("{}:", CATEGORY_NAMES[i]).c_str(), "bigFont.fnt");
        name->setAnchorPoint({ 0.f, 1.f });
        name->setScale(COUNT_SCALE);
        name->setColor(CATEGORY_COLORS[i]);
        nameWidth = std::max(nameWidth, name->getScaledContentSize().width);
        names[i] = name;
    }
    for (int i = 0; i < CATEGORY_COUNT; ++i) {
        float y = -i * ROW_HEIGHT;
        names[i]->setPosition({ 0.f, y });
        m_counter->addChild(names[i]);

        auto count = CCLabelBMFont::create("0", "bigFont.fnt");
        count->setAnchorPoint({ 0.f, 1.f });
        count->setScale(COUNT_SCALE);
        count->setColor(CATEGORY_COLORS[i]);
        count->setPosition({ nameWidth + 12.f, y });
        m_counter->addChild(count);
        m_countLabels[i] = count;
    }
}

bool FrameHud::active() const {
    auto& bot = Bot::get();
    return bot.mode == BotMode::Play && !bot.analyzing && !Analyzer::get().isActive();
}

void FrameHud::resetTo(uint32_t tick) {
    auto& bot = Bot::get();
    auto const& replay = bot.replay;

    m_counts.fill(0);
    m_lastIndex = -1;
    m_pressTimes.clear();
    m_maxCps = 0;
    m_totalPresses = 0;
    if (m_markers) m_markers->removeAllChildren();

    for (size_t i = 0; i < replay.inputs.size() && replay.inputs[i].tick < tick; ++i) {
        m_lastIndex = static_cast<long>(i);
        if (replay.inputs[i].down) ++m_totalPresses;
        if (replay.hasAnalysis()) {
            int category = categoryFor(i);
            if (category >= 0) ++m_counts[category];
        }
    }
    this->refreshCounts(false, -1);
    this->refreshPrecision();
}

void FrameHud::refreshCounts(bool animate, int changed) {
    for (int i = 0; i < CATEGORY_COUNT; ++i) {
        m_countLabels[i]->setString(std::to_string(m_counts[i]).c_str());
    }
    if (!animate || changed < 0) return;

    // Pop and flash white, then settle back, like the original counter.
    auto label = m_countLabels[changed];
    label->stopAllActions();
    label->setScale(COUNT_SCALE);
    label->runAction(CCSequence::create(
        CCEaseSineOut::create(CCScaleTo::create(0.06f, COUNT_PEAK_SCALE)),
        CCEaseSineOut::create(CCScaleTo::create(0.24f, COUNT_SCALE)),
        nullptr
    ));
    auto color = CATEGORY_COLORS[changed];
    label->runAction(CCSequence::create(
        CCEaseSineOut::create(CCTintTo::create(0.06f, 255, 255, 255)),
        CCEaseSineOut::create(CCTintTo::create(0.24f, color.r, color.g, color.b)),
        nullptr
    ));
}

void FrameHud::refreshPrecision() {
    auto const& replay = Bot::get().replay;
    if (m_lastIndex >= 0 && static_cast<size_t>(m_lastIndex) < replay.lstar.size()) {
        m_shareLabel->setString(fmt::format("{:.2f}%", replay.lstarShare[m_lastIndex]).c_str());
        m_lstarLabel->setString(fmt::format("{:.2f}", replay.lstar[m_lastIndex]).c_str());
    }
    else if (LStar::isComputing()) {
        m_shareLabel->setString("L*");
        m_lstarLabel->setString("...");
    }
    else {
        m_shareLabel->setString("0.00%");
        m_lstarLabel->setString("0.00");
    }
}

void FrameHud::onInputPlayed(size_t index) {
    auto const& replay = Bot::get().replay;
    if (index >= replay.inputs.size()) return;
    m_lastIndex = static_cast<long>(index);

    auto const& input = replay.inputs[index];
    if (input.down) {
        double now = m_layer->m_gameState.m_levelTime;
        m_pressTimes.push_back(now);
        ++m_totalPresses;
    }

    if (replay.hasAnalysis()) {
        auto const& analysis = replay.analysis[index];
        int category = categoryFor(index);
        if (category >= 0) {
            ++m_counts[category];
            this->refreshCounts(true, category);
            if (Bot::get().playSounds) Sounds::playCategory(category);
        }
        if (category >= 0 || analysis.unreliable) this->spawnMarker(index, category);
    }
    this->refreshPrecision();
}

void FrameHud::spawnMarker(size_t index, int category) {
    if (!m_markers || !Bot::get().showCounter) return;
    auto const& replay = Bot::get().replay;
    auto const& input = replay.inputs[index];
    auto const& analysis = replay.analysis[index];

    auto player = isP2Input(m_layer, input) ? m_layer->m_player2 : m_layer->m_player1;
    if (!player) return;

    auto color = categoryColor(category);
    auto marker = CCNode::create();
    marker->setPosition(player->getPosition());

    constexpr int SEGMENTS = 48;
    constexpr float RADIUS = 13.f;
    CCPoint verts[SEGMENTS];
    for (int i = 0; i < SEGMENTS; ++i) {
        float angle = i * 6.2831853f / SEGMENTS;
        verts[i] = CCPoint(RADIUS * std::cos(angle), RADIUS * std::sin(angle));
    }
    auto ring = CCDrawNode::create();
    ring->drawPolygon(verts, SEGMENTS, { 0.f, 0.f, 0.f, 0.f }, 4.f, { 0.f, 0.f, 0.f, 1.f });
    ring->drawPolygon(verts, SEGMENTS, { 0.f, 0.f, 0.f, 0.f }, 2.f, toColor4F(color, 1.f));
    marker->addChild(ring);

    bool cbf = Bot::get().useCbf && analysis.cbf > 0.f;
    auto text = analysis.unreliable ? std::string("?")
        : cbf ? fmt::format("{:.2f}", analysis.cbf)
        : std::to_string(analysis.window());
    auto label = CCLabelBMFont::create(text.c_str(), "bigFont.fnt");
    label->setAnchorPoint({ 1.f, 0.5f });
    label->setPosition({ -18.f, 0.f });
    label->setScale(0.5f);
    label->setColor(color);
    marker->addChild(label);

    marker->setScale(0.3f);
    marker->runAction(CCSequence::create(
        CCEaseBackOut::create(CCScaleTo::create(0.15f, 1.f)),
        CCDelayTime::create(3.f),
        CCRemoveSelf::create(),
        nullptr
    ));
    m_markers->addChild(marker);
}

void FrameHud::update(float) {
    auto& bot = Bot::get();
    bool show = active() && bot.replay.hasAnalysis() && bot.showCounter;
    m_counter->setVisible(show);
    m_shareLabel->setVisible(show);
    m_lstarLabel->setVisible(show);
    m_cpsLabel->setVisible(active() && bot.showCounter);

    if (m_cpsLabel->isVisible()) {
        double now = m_layer->m_gameState.m_levelTime;
        while (!m_pressTimes.empty() && now - m_pressTimes.front() > 1.0) m_pressTimes.pop_front();
        int current = static_cast<int>(m_pressTimes.size());
        m_maxCps = std::max(m_maxCps, current);
        m_cpsLabel->setString(fmt::format("{}/{}/{} CPS", current, m_maxCps, m_totalPresses).c_str());
    }
    if (show && LStar::isComputing()) this->refreshPrecision();

    this->drawTrajectories();
}

void FrameHud::drawTrajectories() {
    if (!m_draw) return;
    m_draw->clear();

    auto& bot = Bot::get();
    if (!active() || !bot.showPaths) return;

    auto const& track = bot.track;
    int64_t now = bot.tick;
    bool dual = m_layer->m_gameState.m_isDualMode;

    auto drawTrack = [&](int64_t from, int64_t to, bool dashed, ccColor4F color, bool p2) {
        from = std::max<int64_t>(from, 0);
        to = std::min<int64_t>(to, static_cast<int64_t>(track.size()) - 1);
        for (int64_t t = from; t < to; ++t) {
            if (dashed && (t / 3) % 2) continue;
            auto const& a = track[t];
            auto const& b = track[t + 1];
            if (!a.valid || !b.valid) continue;
            auto pa = p2 ? a.p2 : a.p1;
            auto pb = p2 ? b.p2 : b.p1;
            // A jump this large is a teleport (portal), not movement.
            if (std::abs(pb.x - pa.x) > 60.f || std::abs(pb.y - pa.y) > 60.f) continue;
            m_draw->drawSegment({ pa.x, pa.y }, { pb.x, pb.y }, 0.8f, color);
        }
    };

    for (bool p2 : { false, true }) {
        if (p2 && !dual) break;
        drawTrack(now - TRAIL_TICKS, now, false, { 1.f, 1.f, 1.f, 0.35f }, p2);
        drawTrack(now, now + FUTURE_TICKS, true, { 0.35f, 0.9f, 1.f, 0.6f }, p2);
    }

    // Every alternative path found by the analyzer around the current position.
    auto const& replay = bot.replay;
    auto const& paths = Analyzer::get().paths;
    if (!replay.hasAnalysis() || paths.size() != replay.inputs.size()) return;
    for (size_t i = 0; i < replay.inputs.size(); ++i) {
        int64_t tick = replay.inputs[i].tick;
        if (tick < now - FAN_BEHIND_TICKS) continue;
        if (tick > now + FAN_AHEAD_TICKS) break;
        int category = categoryFor(i);
        if (category < 0) continue;
        auto color = categoryColor(category);
        for (auto const& path : paths[i]) {
            auto lineColor = path.alive ? toColor4F(color, 0.45f) : ccColor4F { 1.f, 0.25f, 0.25f, 0.45f };
            for (size_t k = 2; k < path.points.size(); k += 2) {
                auto const& a = path.points[k - 2];
                auto const& b = path.points[k];
                if (std::abs(b.x - a.x) > 60.f || std::abs(b.y - a.y) > 60.f) continue;
                m_draw->drawSegment(a, b, 0.6f, lineColor);
            }
            if (!path.alive && !path.points.empty()) {
                auto end = path.points.back();
                m_draw->drawSegment(end + CCPoint { -4.f, -4.f }, end + CCPoint { 4.f, 4.f }, 1.f, lineColor);
                m_draw->drawSegment(end + CCPoint { -4.f, 4.f }, end + CCPoint { 4.f, -4.f }, 1.f, lineColor);
            }
        }
    }
}
