#include <Geode/Geode.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/PlayerObject.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PauseLayer.hpp>
#include <Geode/modify/EndLevelLayer.hpp>
#include <Geode/binding/PlayerObject.hpp>
#include <Geode/binding/PlayLayer.hpp>
#include <Geode/binding/GameObject.hpp>
#include <Geode/ui/GeodeUI.hpp>
#include <Geode/ui/Popup.hpp>
#include <Geode/loader/SettingV3.hpp>

#include <deque>
#include <vector>
#include <unordered_map>
#include <cmath>
#include <algorithm>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <fstream>
#include <sstream>
#include <string>

using namespace geode::prelude;

// ============================================================
//  ApexBuffer v1.1
//  Smart adaptive trajectory-based input buffer
//  Focus: solid core systems first
// ============================================================

namespace Apex {

// -------------------- Settings --------------------
bool isEnabled()       { return Mod::get()->getSettingValue<bool>("enabled"); }
bool practiceOnly()    { return Mod::get()->getSettingValue<bool>("practice-only"); }
int  getStage()        { return static_cast<int>(Mod::get()->getSettingValue<int64_t>("stage")); }
int  getWaveStage()    { return static_cast<int>(Mod::get()->getSettingValue<int64_t>("wave-stage")); }
float getAggression()  { return static_cast<float>(Mod::get()->getSettingValue<double>("aggression")); }
bool autoAdapt()       { return Mod::get()->getSettingValue<bool>("auto-adapt"); }
float minConfidence()  { return static_cast<float>(Mod::get()->getSettingValue<double>("min-confidence")); }
int  getHorizon()      { return static_cast<int>(Mod::get()->getSettingValue<int64_t>("horizon")); }
int  getWaveHorizon()  { return static_cast<int>(Mod::get()->getSettingValue<int64_t>("wave-horizon")); }
bool smartOrb()        { return Mod::get()->getSettingValue<bool>("orb-smart"); }
bool showStatus()      { return Mod::get()->getSettingValue<bool>("show-status"); }
bool debugLog()        { return Mod::get()->getSettingValue<bool>("debug-log"); }

// -------------------- Logging --------------------
void log(const std::string& msg) {
    if (!debugLog()) return;
    static std::ofstream file;
    if (!file.is_open()) {
        auto path = Mod::get()->getSaveDir() / "apexbuffer.log";
        file.open(path.string(), std::ios::app);
    }
    if (file.is_open()) {
        auto now = std::chrono::system_clock::now();
        auto t = std::chrono::system_clock::to_time_t(now);
        file << std::put_time(std::localtime(&t), "%H:%M:%S") << " | " << msg << "\n";
        file.flush();
    }
}

// -------------------- Core Data --------------------
struct PendingInput {
    bool isPress = true;
    int  delayLeft = 0;
    float confidence = 0.f;
    int  createdAt = 0;     // for debugging
};

struct SectionStats {
    int deaths = 0;
    int buffered = 0;       // times we delayed here
    int successAfterBuffer = 0;
    float avgUsefulDelay = 0.f;
};

struct BufferState {
    bool holding = false;               // what the *game* currently thinks about the button
    bool realHolding = false;           // what the player is physically holding
    std::deque<PendingInput> queue;

    int tickCounter = 0;
    int currentStage = 16;
    float lastConfidence = 0.f;
    int lastDelay = 0;
    std::string lastAction = "Idle";

    // Adaptive memory (key = section)
    std::unordered_map<int, SectionStats> memory;

    void reset() {
        queue.clear();
        holding = false;
        realHolding = false;
        tickCounter = 0;
        lastAction = "Idle";
        lastConfidence = 0.f;
        lastDelay = 0;
    }

    int sectionOf(float x) const {
        return static_cast<int>(std::floor(x / 40.f));
    }

    void onDeath(float x) {
        memory[sectionOf(x)].deaths++;
    }

    void onBuffered(float x, int delay) {
        auto& s = memory[sectionOf(x)];
        s.buffered++;
        if (s.buffered == 1) s.avgUsefulDelay = static_cast<float>(delay);
        else s.avgUsefulDelay = s.avgUsefulDelay * 0.75f + delay * 0.25f;
    }

    void onSurvivedAfterBuffer(float x) {
        memory[sectionOf(x)].successAfterBuffer++;
    }

    int adaptiveMaxDelay(float x, int base) const {
        if (!autoAdapt()) return base;
        auto it = memory.find(sectionOf(x));
        if (it == memory.end()) return base;

        const auto& s = it->second;
        int total = s.deaths + s.successAfterBuffer;
        if (total < 4) return base;

        float deathRatio = static_cast<float>(s.deaths) / (s.deaths + s.successAfterBuffer + 1);

        if (deathRatio > 0.55f) return std::min(base + 10, 44);
        if (deathRatio > 0.35f) return std::min(base + 6, 40);
        if (deathRatio < 0.18f && s.buffered > 5) return std::max(base - 4, 6);
        return base;
    }
};

static BufferState g;

// -------------------- Helpers --------------------
bool shouldRun(PlayLayer* pl) {
    if (!isEnabled() || !pl) return false;
    if (practiceOnly() && !pl->m_isPracticeMode) return false;
    if (!pl->m_player1 || pl->m_player1->m_isDead) return false;
    return true;
}

// ============================================================
//  Prediction Engine v2 – with real object collision checks
// ============================================================

struct GhostState {
    float x = 0.f, y = 0.f;
    float vy = 0.f;
    float grav = 1.f;
    bool  onGround = false;
    bool  isWave = false;
    bool  isShip = false;
    bool  isBall = false;
    bool  isUFO = false;
    bool  isRobot = false;
    bool  isSpider = false;
    bool  isMini = false;
    bool  holding = false;
    float halfW = 7.5f;   // player hitbox half width (approx)
    float halfH = 7.5f;
};

struct PredictResult {
    bool  survived = true;
    int   ticksAlive = 0;
    float minHazardDist = 9999.f;
    float confidence = 0.5f;
    bool  hitHazard = false;
    bool  hitSolid = false;
};

// Physics constants (tuned for 240Hz feel)
constexpr float STEP = 1.f / 240.f;
constexpr float CUBE_JUMP = 11.18f;
constexpr float GRAV_CUBE = -0.958f * 60.f * STEP;   // scaled
constexpr float WAVE_VY   = 9.6f;
constexpr float SHIP_ACC  = 0.85f;
constexpr float X_SPEED   = 5.77f;                   // approx at 1x

// -------------------- Object helpers --------------------
bool isHazardType(GameObject* obj) {
    if (!obj) return false;
    auto t = obj->m_objectType;
    return t == GameObjectType::Hazard || t == GameObjectType::AnimatedHazard;
}

bool isSolidType(GameObject* obj) {
    if (!obj) return false;
    auto t = obj->m_objectType;
    return t == GameObjectType::Solid || t == GameObjectType::Slope ||
           t == GameObjectType::AnimatedHazard; // some animated are solid too
}

bool isOrbType(GameObject* obj) {
    if (!obj) return false;
    auto t = obj->m_objectType;
    return t == GameObjectType::YellowJumpRing || t == GameObjectType::PinkJumpRing ||
           t == GameObjectType::RedJumpRing || t == GameObjectType::GravityRing ||
           t == GameObjectType::GreenRing || t == GameObjectType::DropRing ||
           t == GameObjectType::DashRing || t == GameObjectType::GravityDashRing ||
           t == GameObjectType::SpiderOrb || t == GameObjectType::CustomRing;
}

bool isPadType(GameObject* obj) {
    if (!obj) return false;
    auto t = obj->m_objectType;
    return t == GameObjectType::YellowJumpPad || t == GameObjectType::PinkJumpPad ||
           t == GameObjectType::RedJumpPad || t == GameObjectType::GravityPad ||
           t == GameObjectType::SpiderPad;
}

// Simple AABB overlap
bool aabbOverlap(float ax, float ay, float aw, float ah,
                 float bx, float by, float bw, float bh) {
    return std::abs(ax - bx) < (aw + bw) && std::abs(ay - by) < (ah + bh);
}

// Collect nearby relevant objects once per decision (performance)
struct NearbyObjects {
    std::vector<GameObject*> hazards;
    std::vector<GameObject*> solids;
    std::vector<GameObject*> orbs;
    std::vector<GameObject*> pads;
};

NearbyObjects gatherNearby(PlayLayer* pl, float fromX, float toX, float centerY) {
    NearbyObjects out;
    if (!pl) return out;

    CCArray* objects = nullptr;

    // Safe access – different Geode / GD versions may layout differently
    // We try the most common member first
    objects = pl->m_objects;

    if (!objects || objects->count() == 0) {
        return out;
    }

    float minX = std::min(fromX, toX) - 50.f;
    float maxX = std::max(fromX, toX) + 140.f;
    float minY = centerY - 200.f;
    float maxY = centerY + 200.f;

    int count = objects->count();
    // Safety cap to avoid huge levels hitching
    int limit = std::min(count, 2500);

    for (int i = 0; i < limit; ++i) {
        auto* obj = static_cast<GameObject*>(objects->objectAtIndex(i));
        if (!obj) continue;

        float ox = obj->getPositionX();
        float oy = obj->getPositionY();
        if (ox < minX || ox > maxX || oy < minY || oy > maxY) continue;

        if (isHazardType(obj)) {
            out.hazards.push_back(obj);
        } else if (isSolidType(obj)) {
            out.solids.push_back(obj);
        } else if (isOrbType(obj)) {
            out.orbs.push_back(obj);
        } else if (isPadType(obj)) {
            out.pads.push_back(obj);
        }
    }
    return out;
}

// Check if ghost collides with any hazard at current position
bool checkHazardHit(const GhostState& gs, const NearbyObjects& near, float& outDist) {
    outDist = 9999.f;
    bool hit = false;
    for (auto* obj : near.hazards) {
        if (!obj) continue;
        float ox = obj->getPositionX();
        float oy = obj->getPositionY();

        // Approximate object size (most spikes ~15-30 units)
        float ow = 12.f;
        float oh = 12.f;
        if (obj->getScaleX() > 1.1f) ow *= obj->getScaleX();
        if (obj->getScaleY() > 1.1f) oh *= obj->getScaleY();

        float dx = std::abs(gs.x - ox);
        float dy = std::abs(gs.y - oy);
        float dist = std::sqrt(dx * dx + dy * dy);
        outDist = std::min(outDist, dist);

        if (aabbOverlap(gs.x, gs.y, gs.halfW, gs.halfH, ox, oy, ow, oh)) {
            hit = true;
        }
    }
    return hit;
}

bool checkSolidHit(const GhostState& gs, const NearbyObjects& near) {
    for (auto* obj : near.solids) {
        if (!obj) continue;
        float ox = obj->getPositionX();
        float oy = obj->getPositionY();
        float ow = 15.f * obj->getScaleX();
        float oh = 15.f * obj->getScaleY();

        if (aabbOverlap(gs.x, gs.y, gs.halfW, gs.halfH, ox, oy, ow * 0.5f, oh * 0.5f)) {
            return true;
        }
    }
    return false;
}

// -------------------- Capture & Step --------------------
GhostState capturePlayer(PlayerObject* p) {
    GhostState gs;
    if (!p) return gs;

    gs.x = p->getPositionX();
    gs.y = p->getPositionY();
    gs.vy = p->m_yVelocity;
    gs.grav = p->m_isUpsideDown ? -1.f : 1.f;
    gs.onGround = p->m_isOnGround;
    gs.isWave = p->m_isWave;
    gs.isShip = p->m_isShip;
    gs.isBall = p->m_isBall;
    gs.isUFO = p->m_isUFO;
    gs.isRobot = p->m_isRobot;
    gs.isSpider = p->m_isSpider;
    gs.isMini = p->m_vehicleSize < 0.85f;
    gs.halfW = gs.isMini ? 4.5f : 7.5f;
    gs.halfH = gs.isMini ? 4.5f : 7.5f;
    // holding state is tracked globally in BufferState
    return gs;
}

void stepGhost(GhostState& gs, bool hold) {
    gs.holding = hold;

    if (gs.isWave) {
        gs.vy = (hold ? WAVE_VY : -WAVE_VY) * gs.grav;
        gs.y += gs.vy * STEP * 60.f;
        gs.x += X_SPEED * STEP * 60.f;
        gs.onGround = false;
        return;
    }

    if (gs.isShip || gs.isUFO) {
        // Simplified ship/ufo: hold accelerates up, release falls
        float acc = hold ? SHIP_ACC : -SHIP_ACC;
        gs.vy += acc * gs.grav;
        gs.vy = std::clamp(gs.vy, -12.f, 12.f);
        gs.y += gs.vy * STEP * 60.f;
        gs.x += X_SPEED * STEP * 60.f;
        gs.onGround = false;
        return;
    }

    // Cube / Ball / Robot / Spider (approximate)
    if (hold && gs.onGround) {
        gs.vy = CUBE_JUMP * gs.grav;
        gs.onGround = false;
    }

    gs.vy += GRAV_CUBE * gs.grav * 60.f;   // gravity per step
    gs.y  += gs.vy * STEP * 60.f;
    gs.x  += X_SPEED * STEP * 60.f;

    // Ground snap (very basic – real solids will override this later)
    if (!gs.isWave && !gs.isShip && !gs.isUFO) {
        if (gs.grav > 0 && gs.y < 0.f) {
            gs.y = 0.f;
            gs.vy = 0.f;
            gs.onGround = true;
        } else if (gs.grav < 0 && gs.y > 999.f) { // ceiling-ish
            gs.vy = 0.f;
            gs.onGround = true;
        }
    }
}

// -------------------- Core simulation (one path) --------------------
PredictResult simulatePath(
    GhostState startGs,
    bool startHold,
    int delayTicks,
    int horizon,
    const NearbyObjects& near,
    bool clickOrbs          // true = click orbs when touching, false = ignore them
) {
    PredictResult res;
    GhostState gs = startGs;
    bool hold = g.holding;

    int alive = 0;
    float closestHazard = 9999.f;
    bool usedOrb = false;

    for (int t = 0; t < horizon; ++t) {
        if (t == delayTicks) {
            hold = startHold;
        }

        // Orb interaction
        if (clickOrbs) {
            for (auto* orb : near.orbs) {
                if (!orb) continue;
                float ox = orb->getPositionX();
                float oy = orb->getPositionY();
                if (std::abs(gs.x - ox) < 16.f && std::abs(gs.y - oy) < 16.f) {
                    // Simplified orb effect
                    auto otype = orb->m_objectType;
                    if (otype == GameObjectType::YellowJumpRing || otype == GameObjectType::PinkJumpRing ||
                        otype == GameObjectType::RedJumpRing) {
                        gs.vy = CUBE_JUMP * gs.grav * 0.95f;
                        gs.onGround = false;
                        usedOrb = true;
                    } else if (otype == GameObjectType::GravityRing) {
                        gs.grav = -gs.grav;
                        usedOrb = true;
                    } else if (otype == GameObjectType::DashRing || otype == GameObjectType::GravityDashRing) {
                        gs.vy = 0.f;
                        usedOrb = true;
                    }
                    break; // one orb per tick max
                }
            }
        }

        stepGhost(gs, hold);

        float dist = 9999.f;
        if (checkHazardHit(gs, near, dist)) {
            res.survived = false;
            res.hitHazard = true;
            closestHazard = dist;
            break;
        }
        closestHazard = std::min(closestHazard, dist);

        if (!gs.onGround && checkSolidHit(gs, near)) {
            res.survived = false;
            res.hitSolid = true;
            break;
        }

        alive++;
    }

    res.ticksAlive = alive;
    res.minHazardDist = closestHazard;

    if (res.survived) {
        float lifeRatio = static_cast<float>(alive) / std::max(horizon, 1);
        float hazardMargin = std::clamp(closestHazard / 40.f, 0.f, 1.f);
        float delayCost = static_cast<float>(delayTicks) / 48.f;
        float orbBonus = usedOrb ? 0.05f : 0.f;

        res.confidence = std::clamp(
            lifeRatio * 0.45f + hazardMargin * 0.38f + 0.15f - delayCost * 0.25f + orbBonus,
            0.20f, 0.96f
        );
    } else {
        float early = 1.f - static_cast<float>(alive) / std::max(horizon, 1);
        res.confidence = std::clamp(0.35f + early * 0.5f, 0.25f, 0.90f);
    }

    return res;
}

// -------------------- Main Prediction (with Smart Orb) --------------------
PredictResult runPrediction(PlayerObject* player, PlayLayer* pl, bool startHold, int delayTicks, int horizon) {
    PredictResult res;
    if (!player || !pl) {
        res.confidence = 0.25f;
        return res;
    }

    GhostState gs = capturePlayer(player);

    float travel = X_SPEED * STEP * 60.f * horizon + 50.f;
    auto near = gatherNearby(pl, gs.x, gs.x + travel, gs.y);

    // Path A: ignore orbs (skip)
    auto skipRes = simulatePath(gs, startHold, delayTicks, horizon, near, false);

    // If Smart Orb is off, or no orbs nearby, just return skip path
    if (!smartOrb() || near.orbs.empty()) {
        return skipRes;
    }

    // Path B: click orbs when touching
    auto clickRes = simulatePath(gs, startHold, delayTicks, horizon, near, true);

    // Choose the better path
    auto score = [](const PredictResult& r) -> float {
        if (r.survived) {
            return r.ticksAlive * 1.0f + r.confidence * 30.f + r.minHazardDist * 0.2f;
        }
        return r.ticksAlive * 0.3f;
    };

    if (score(clickRes) > score(skipRes)) {
        return clickRes;
    }
    return skipRes;
}

// -------------------- Decision --------------------
int decideDelay(PlayerObject* player, PlayLayer* pl, bool isPress, int maxDelay, int horizon) {
    if (maxDelay <= 0) return 0;

    int bestDelay = 0;
    float bestScore = -999.f;
    float bestConf = 0.f;
    PredictResult bestRes{};
    PredictResult noDelayRes = runPrediction(player, pl, isPress, 0, horizon);

    // Score for no-delay baseline
    float noDelayScore = 0.f;
    if (noDelayRes.survived) {
        noDelayScore = noDelayRes.ticksAlive + noDelayRes.confidence * 32.f + noDelayRes.minHazardDist * 0.15f;
    } else {
        noDelayScore = noDelayRes.ticksAlive * 0.3f;
    }

    bestScore = noDelayScore;
    bestConf = noDelayRes.confidence;
    bestRes = noDelayRes;

    for (int d = 1; d <= maxDelay; ++d) {
        auto r = runPrediction(player, pl, isPress, d, horizon);

        float score = 0.f;
        if (r.survived) {
            score = r.ticksAlive * 1.0f
                  + r.confidence * 32.f
                  + r.minHazardDist * 0.18f;
            // Prefer smaller delays when scores are close (anti delay-then-die)
            score -= d * (1.25f - getAggression() * 0.9f);
        } else {
            // Dying later is slightly better than dying early
            score = r.ticksAlive * 0.28f;
        }

        if (score > bestScore) {
            bestScore = score;
            bestDelay = d;
            bestConf = r.confidence;
            bestRes = r;
        }
    }

    // Hard confidence gate
    if (bestDelay > 0 && bestConf < minConfidence()) {
        log("Reject delay " + std::to_string(bestDelay) +
            " (conf " + std::to_string(bestConf) + " < min " + std::to_string(minConfidence()) + ")");
        g.lastConfidence = bestConf;
        g.lastDelay = 0;
        return 0;
    }

    // Require meaningful improvement over no-delay
    if (bestDelay > 0) {
        float requiredGain = 5.5f + (1.f - getAggression()) * 4.f; // higher aggression = easier to accept delay
        if (bestScore < noDelayScore + requiredGain) {
            bestDelay = 0;
            bestConf = noDelayRes.confidence;
            log("Delay not worth it (gain too small)");
        }
    }

    // Extra safety: if the chosen delay still dies in prediction, never take it
    if (bestDelay > 0 && !bestRes.survived) {
        bestDelay = 0;
        bestConf = noDelayRes.confidence;
    }

    g.lastConfidence = bestConf;
    g.lastDelay = bestDelay;
    return bestDelay;
}

// -------------------- Input Delivery --------------------
void deliverPress(PlayerObject* player) {
    if (!player) return;
    // Call the original
    player->pushButton(PlayerButton::Jump);
    g.holding = true;
}

void deliverRelease(PlayerObject* player) {
    if (!player) return;
    player->releaseButton(PlayerButton::Jump);
    g.holding = false;
}

void processQueue(PlayLayer* pl) {
    if (!pl || !pl->m_player1) return;

    // Count down
    for (auto& inp : g.queue) {
        if (inp.delayLeft > 0) inp.delayLeft--;
    }

    // Deliver ready inputs (in order)
    while (!g.queue.empty() && g.queue.front().delayLeft <= 0) {
        auto inp = g.queue.front();
        g.queue.pop_front();

        if (inp.isPress) {
            deliverPress(pl->m_player1);
            log("Delivered delayed PRESS (conf was " + std::to_string(inp.confidence) + ")");
            g.lastAction = "Delivered +" + std::to_string(inp.createdAt);
        } else {
            deliverRelease(pl->m_player1);
            log("Delivered delayed RELEASE");
            g.lastAction = "Delivered release";
        }
    }
}

} // namespace Apex

// ============================================================
//  Hooks
// ============================================================

class $modify(ApexPlayLayer, PlayLayer) {
    struct Fields {
        CCLabelBMFont* statusLabel = nullptr;
    };

    bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
        if (!PlayLayer::init(level, useReplay, dontCreateObjects))
            return false;

        Apex::g.reset();
        Apex::g.currentStage = Apex::getStage();

        if (Apex::showStatus()) {
            auto* label = CCLabelBMFont::create("ApexBuffer", "bigFont.fnt");
            label->setScale(0.32f);
            label->setOpacity(170);
            label->setAnchorPoint({0.f, 1.f});
            auto win = CCDirector::get()->getWinSize();
            label->setPosition({8.f, win.height - 8.f});
            label->setID("apex-status"_spr);
            this->addChild(label, 999);
            m_fields->statusLabel = label;
        }

        Apex::log("ApexBuffer ready");
        return true;
    }

    void resetLevel() {
        PlayLayer::resetLevel();
        Apex::g.reset();
    }

    void destroyPlayer(PlayerObject* player, GameObject* obj) {
        if (player == m_player1) {
            Apex::g.onDeath(player->getPositionX());
            Apex::log("Death @ x=" + std::to_string(player->getPositionX()));
        }
        PlayLayer::destroyPlayer(player, obj);
    }

    void postUpdate(float dt) {
        PlayLayer::postUpdate(dt);

        Apex::g.tickCounter++;

        // Process delayed inputs every physics-ish update
        if (Apex::shouldRun(this)) {
            Apex::processQueue(this);
        }

        // Status text
        if (m_fields->statusLabel && Apex::showStatus()) {
            std::string t = "Apex ";
            if (!Apex::isEnabled()) {
                t += "OFF";
            } else if (Apex::practiceOnly() && !m_isPracticeMode) {
                t += "Practice only";
            } else {
                t += "S" + std::to_string(Apex::g.currentStage);
                if (Apex::g.lastDelay > 0) {
                    t += " D" + std::to_string(Apex::g.lastDelay);
                    t += " " + std::to_string(static_cast<int>(Apex::g.lastConfidence * 100)) + "%";
                } else {
                    t += " ready";
                }
            }
            m_fields->statusLabel->setString(t.c_str());
        }
    }
};

class $modify(ApexPlayerObject, PlayerObject) {
    void pushButton(PlayerButton btn) {
        auto* pl = PlayLayer::get();
        if (!pl || this != pl->m_player1 || btn != PlayerButton::Jump || !Apex::shouldRun(pl)) {
            PlayerObject::pushButton(btn);
            return;
        }

        Apex::g.realHolding = true;

        // Already holding according to game → ignore duplicate
        if (Apex::g.holding) {
            PlayerObject::pushButton(btn);
            return;
        }

        int maxDelay = Apex::getStage();
        int horizon  = Apex::getHorizon();

        if (this->m_isWave) {
            maxDelay = Apex::getWaveStage();
            horizon  = Apex::getWaveHorizon();
        }

        // Adaptive
        maxDelay = Apex::g.adaptiveMaxDelay(this->getPositionX(), maxDelay);
        Apex::g.currentStage = maxDelay;

        int delay = Apex::decideDelay(this, pl, true, maxDelay, horizon);

        if (delay <= 0) {
            // Send immediately
            PlayerObject::pushButton(btn);
            Apex::g.holding = true;
            Apex::g.lastAction = "Instant";
            Apex::g.lastDelay = 0;
        } else {
            // Queue it
            Apex::PendingInput p;
            p.isPress = true;
            p.delayLeft = delay;
            p.confidence = Apex::g.lastConfidence;
            p.createdAt = delay;
            Apex::g.queue.push_back(p);
            Apex::g.onBuffered(this->getPositionX(), delay);

            Apex::g.lastAction = "Delay " + std::to_string(delay);
            Apex::log("Queued PRESS delay=" + std::to_string(delay) + " conf=" + std::to_string(p.confidence));
            // Do not call original pushButton yet
        }
    }

    void releaseButton(PlayerButton btn) {
        auto* pl = PlayLayer::get();
        if (!pl || this != pl->m_player1 || btn != PlayerButton::Jump || !Apex::shouldRun(pl)) {
            PlayerObject::releaseButton(btn);
            return;
        }

        Apex::g.realHolding = false;

        // If there are still pending presses in queue, we may need to cancel or adjust
        // For simplicity in v1: if a press is still waiting, just drop the release for now
        // (better handling later)
        bool hasPendingPress = false;
        for (auto& q : Apex::g.queue) {
            if (q.isPress) { hasPendingPress = true; break; }
        }

        if (this->m_isWave) {
            int maxDelay = Apex::getWaveStage();
            int horizon  = Apex::getWaveHorizon();
            maxDelay = Apex::g.adaptiveMaxDelay(this->getPositionX(), maxDelay);

            int delay = Apex::decideDelay(this, pl, false, maxDelay, horizon);
            if (delay > 0) {
                Apex::PendingInput p;
                p.isPress = false;
                p.delayLeft = delay;
                p.confidence = Apex::g.lastConfidence;
                p.createdAt = delay;
                Apex::g.queue.push_back(p);
                Apex::log("Queued RELEASE delay=" + std::to_string(delay));
                return;
            }
        }

        // Normal release
        PlayerObject::releaseButton(btn);
        Apex::g.holding = false;
    }
};

// ============================================================
//  Pause Menu UI (mobile-friendly)
// ============================================================

class ApexPausePopup : public geode::Popup<> {
protected:
    CCLabelBMFont* m_stageLabel = nullptr;
    CCLabelBMFont* m_statusLabel = nullptr;
    CCMenuItemToggler* m_enableToggle = nullptr;

    bool setup() override {
        auto winSize = CCDirector::get()->getWinSize();

        this->setTitle("ApexBuffer");

        auto* menu = CCMenu::create();
        menu->setPosition({0, 0});
        m_mainLayer->addChild(menu);

        // ----- Enable toggle -----
        auto* enableLabel = CCLabelBMFont::create("Enable Buffer", "bigFont.fnt");
        enableLabel->setScale(0.45f);
        enableLabel->setPosition({winSize.width / 2 - 40.f, winSize.height / 2 + 70.f});
        m_mainLayer->addChild(enableLabel);

        m_enableToggle = CCMenuItemToggler::create(
            CCSprite::createWithSpriteFrameName("GJ_checkOff_001.png"),
            CCSprite::createWithSpriteFrameName("GJ_checkOn_001.png"),
            this,
            menu_selector(ApexPausePopup::onToggleEnable)
        );
        m_enableToggle->setPosition({winSize.width / 2 + 80.f, winSize.height / 2 + 70.f});
        m_enableToggle->toggle(Apex::isEnabled());
        menu->addChild(m_enableToggle);

        // ----- Stage display -----
        m_stageLabel = CCLabelBMFont::create(
            ("Stage: " + std::to_string(Apex::getStage())).c_str(),
            "bigFont.fnt"
        );
        m_stageLabel->setScale(0.5f);
        m_stageLabel->setPosition({winSize.width / 2, winSize.height / 2 + 25.f});
        m_mainLayer->addChild(m_stageLabel);

        // Stage - button
        auto* minusBtn = CCMenuItemSpriteExtra::create(
            CCSprite::createWithSpriteFrameName("GJ_zoomOutBtn_001.png"),
            this,
            menu_selector(ApexPausePopup::onStageMinus)
        );
        minusBtn->setPosition({winSize.width / 2 - 70.f, winSize.height / 2 - 15.f});
        minusBtn->setScale(0.9f);
        menu->addChild(minusBtn);

        // Stage + button
        auto* plusBtn = CCMenuItemSpriteExtra::create(
            CCSprite::createWithSpriteFrameName("GJ_zoomInBtn_001.png"),
            this,
            menu_selector(ApexPausePopup::onStagePlus)
        );
        plusBtn->setPosition({winSize.width / 2 + 70.f, winSize.height / 2 - 15.f});
        plusBtn->setScale(0.9f);
        menu->addChild(plusBtn);

        // ----- Quick info -----
        m_statusLabel = CCLabelBMFont::create("Practice recommended", "chatFont.fnt");
        m_statusLabel->setScale(0.55f);
        m_statusLabel->setPosition({winSize.width / 2, winSize.height / 2 - 55.f});
        m_statusLabel->setColor({180, 180, 180});
        m_mainLayer->addChild(m_statusLabel);

        // Open full Geode settings button
        auto* fullBtn = CCMenuItemSpriteExtra::create(
            ButtonSprite::create("Full Settings", "bigFont.fnt", "GJ_button_01.png", 0.6f),
            this,
            menu_selector(ApexPausePopup::onFullSettings)
        );
        fullBtn->setPosition({winSize.width / 2, winSize.height / 2 - 100.f});
        menu->addChild(fullBtn);

        return true;
    }

    void onToggleEnable(CCObject*) {
        bool newVal = !Apex::isEnabled();
        Mod::get()->setSettingValue("enabled", newVal);
        // force toggle visual
        if (m_enableToggle) m_enableToggle->toggle(newVal);
    }

    void onStageMinus(CCObject*) {
        int stage = Apex::getStage();
        stage = std::max(0, stage - 2);
        Mod::get()->setSettingValue<int64_t>("stage", stage);
        if (m_stageLabel) {
            m_stageLabel->setString(("Stage: " + std::to_string(stage)).c_str());
        }
        Apex::g.currentStage = stage;
    }

    void onStagePlus(CCObject*) {
        int stage = Apex::getStage();
        stage = std::min(48, stage + 2);
        Mod::get()->setSettingValue<int64_t>("stage", stage);
        if (m_stageLabel) {
            m_stageLabel->setString(("Stage: " + std::to_string(stage)).c_str());
        }
        Apex::g.currentStage = stage;
    }

    void onFullSettings(CCObject*) {
        openSettingsPopup(Mod::get());
    }

public:
    static ApexPausePopup* create() {
        auto* ret = new ApexPausePopup();
        if (ret && ret->initAnchored(280.f, 240.f)) {
            ret->autorelease();
            return ret;
        }
        CC_SAFE_DELETE(ret);
        return nullptr;
    }
};

class $modify(ApexPauseLayer, PauseLayer) {
    void customSetup() {
        PauseLayer::customSetup();

        // Create a dedicated menu so we don't fight with existing pause buttons
        auto* apexMenu = CCMenu::create();
        apexMenu->setPosition({0.f, 0.f});
        apexMenu->setID("apex-pause-menu"_spr);
        this->addChild(apexMenu, 100);

        auto* btnSprite = ButtonSprite::create("Apex", "bigFont.fnt", "GJ_button_02.png", 0.5f);
        auto* btn = CCMenuItemSpriteExtra::create(
            btnSprite,
            this,
            menu_selector(ApexPauseLayer::onApexButton)
        );
        btn->setID("apex-pause-btn"_spr);

        // Place on the left side of the pause screen – usually less crowded
        // Works better on both PC and mobile
        auto winSize = CCDirector::get()->getWinSize();
        btn->setPosition({60.f, winSize.height / 2 + 40.f});

        apexMenu->addChild(btn);
    }

    void onApexButton(CCObject*) {
        if (auto* popup = ApexPausePopup::create()) {
            popup->show();
        }
    }
};

$execute {
    log::info("ApexBuffer v1.3 loaded – Pause menu + Smart Orb ready");
}
