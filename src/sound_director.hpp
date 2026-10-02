#pragma once
#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include <utility>

namespace cleaner {
using Millis = std::uint64_t;
enum class SoundEventType { Start, Approach, Carry, Recycled, Finish, Interference };
struct SoundEvent {
    SoundEventType type;
    bool folder = false;
    bool shortcut = false;
    bool newFolder = false;
    bool movedAny = false;
};
enum class SoundPhase { Idle, Intro, Work, Closing, Final, Finished };

inline bool containsNewFolder(std::wstring name) {
    for (auto& c : name) {
        if (c >= L'А' && c <= L'Я') c += L'а' - L'А';
        else if (c == L'Ё') c = L'ё';
    }
    return name.find(L"новая папка") != std::wstring::npos;
}

// Distinct presses/clicks restart immediately. A continuous mouse gesture is one
// attempt, instead of hundreds of restarts from the mouse's polling frequency.
class InputAttempts {
    std::array<bool, 256> held{};
    Millis lastMove = 0;
    bool moved = false;
public:
    bool key(unsigned code, bool down) {
        if (code >= held.size()) return false;
        bool fresh = down && !held[code];
        held[code] = down;
        return fresh;
    }
    bool mouseMove(Millis now) {
        bool fresh = !moved || now - lastMove >= 120;
        moved = true;
        lastMove = now;
        return fresh;
    }
};

// Pure event policy: real time, random rolls and recording lengths are supplied
// by the caller so the complete conversation can be tested without Windows.
class SoundDirector {
    std::array<Millis, 17> lengths;
    std::function<unsigned(unsigned)> roll;
    SoundPhase state = SoundPhase::Idle;
    int clip = 0;
    std::uint64_t revision = 0;
    Millis started = 0, ends = 0, nextPeriodic = 0, earlyDue = 0, earlyExpires = 0;
    bool earlyPending = false, firstComment = true, interfering = false;
    bool approaching = false, hasTarget = false;
    unsigned recycled = 0;
    int pendingContext = 0;

    bool chance(unsigned percent) { return roll(100) < percent; }
    bool busy(Millis now) const { return clip != 0 && now < ends; }
    bool canComment(Millis now) const {
        if (!busy(now)) return true;
        if (clip == 2 || clip == 3 || clip == 6 || clip == 10 || clip == 11)
            return false;
        return now - started >= 1400;
    }
    void play(int id, Millis now) {
        clip = id;
        started = now;
        ends = now + lengths.at(id);
        ++revision; // Also changes for a restart of the same recording.
        if (id == 4) firstComment = false;
    }
    int approachComment() {
        if (firstComment) return 4;
        return chance(25) ? 4 : 5;
    }
    void beginWork(Millis now) {
        state = SoundPhase::Work;
        nextPeriodic = now + 5000;
        earlyPending = chance(50);
        earlyDue = now + 1200 + roll(1401);
        earlyExpires = now + 7000;
    }
public:
    SoundDirector(std::array<Millis, 17> durations,
                  std::function<unsigned(unsigned)> random)
        : lengths(durations), roll(std::move(random)) {}
    SoundPhase phase() const { return state; }
    int currentClip() const { return clip; }
    std::uint64_t generation() const { return revision; }
    Millis deadline() const { return ends; }

    void event(const SoundEvent& e, Millis now) {
        if (e.type == SoundEventType::Start) {
            if (state == SoundPhase::Idle) {
                state = SoundPhase::Intro;
                play(1, now);
            }
            return;
        }
        if (state == SoundPhase::Idle || state == SoundPhase::Finished) return;
        if (e.type == SoundEventType::Interference) {
            play(interfering ? 11 : 3, now);
            interfering = true;
            return;
        }
        if (e.type == SoundEventType::Finish) {
            if (state == SoundPhase::Closing || state == SoundPhase::Final) return;
            earlyPending = false;
            pendingContext = 0;
            state = e.movedAny ? SoundPhase::Closing : SoundPhase::Final;
            play(e.movedAny ? 13 : 15, now);
            return;
        }
        if (state != SoundPhase::Work) return;
        if (e.type == SoundEventType::Recycled) {
            approaching = false;
            pendingContext = 0;
            if (++recycled == 1) play(2, now);
        } else if (e.type == SoundEventType::Carry) {
            approaching = false;
            if (e.folder && chance(50) && canComment(now)) play(7, now);
        } else if (e.type == SoundEventType::Approach) {
            approaching = true;
            hasTarget = true;
            pendingContext = 0;
            if (e.folder && e.newFolder) { play(6, now); return; }
            else if (e.shortcut && chance(25)) pendingContext = 9;
            else if (firstComment || !busy(now) || chance(55))
                pendingContext = approachComment();
            if (pendingContext && canComment(now)) {
                play(pendingContext, now);
                pendingContext = 0;
            }
        }
    }
    void tick(Millis now) {
        if (state == SoundPhase::Intro) {
            if (!busy(now)) beginWork(now); // Never replay 1 after an interruption.
            return;
        }
        if (state == SoundPhase::Closing && !busy(now)) {
            state = SoundPhase::Final;
            play(15, now);
            return;
        }
        if (state == SoundPhase::Final && !busy(now)) {
            if (clip != 15) play(15, now); // Finish only after the final phrase.
            else { state = SoundPhase::Finished; clip = 0; ++revision; }
            return;
        }
        if (state != SoundPhase::Work) return;
        if (earlyPending && now > earlyExpires) earlyPending = false;
        if (earlyPending && now >= earlyDue && canComment(now)) {
            earlyPending = false;
            play(10, now);
            return;
        }
        if (now >= nextPeriodic) {
            nextPeriodic = now + 5000;
            if (chance(25) && canComment(now)) { play(8, now); return; }
        }
        if (pendingContext && approaching && canComment(now)) {
            play(pendingContext, now);
            pendingContext = 0;
            return;
        }
        if (hasTarget && !busy(now)) play(approaching ? approachComment() : 5, now);
    }
};
} // namespace cleaner
