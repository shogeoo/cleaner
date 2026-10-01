#include "../src/sound_director.hpp"
#include <cassert>
#include <iostream>
using namespace cleaner;
std::array<Millis, 17> durations() { std::array<Millis,17> a{}; a.fill(1000); return a; }
auto high = [](unsigned n) { return n-1; };
auto low = [](unsigned) { return 0u; };
int main() {
    {
        SoundDirector d(durations(),high);
        d.event({SoundEventType::Start},0);
        assert(d.currentClip()==1 && d.phase()==SoundPhase::Intro);
        d.tick(999);assert(d.phase()==SoundPhase::Intro);
        d.event({SoundEventType::Interference},200);
        assert(d.currentClip()==3);
        d.event({SoundEventType::Interference},201);
        assert(d.currentClip()==11);
        auto serial=d.generation();
        d.event({SoundEventType::Interference},202);
        assert(d.currentClip()==11 && d.generation()==serial+1 && d.deadline()==1202);
        d.tick(1201);assert(d.phase()==SoundPhase::Intro);
        d.tick(1202);assert(d.phase()==SoundPhase::Work);
        d.event({SoundEventType::Start},1203);assert(d.currentClip()!=1);
        d.event({SoundEventType::Approach},1203);assert(d.currentClip()==4);
        d.event({SoundEventType::Recycled},1300);assert(d.currentClip()==2);
        auto firstDrop=d.generation();d.event({SoundEventType::Recycled},1400);
        assert(d.generation()==firstDrop);
        d.tick(2300);assert(d.currentClip()==5); // Fill a silent gap during work.
        d.event({SoundEventType::Finish,false,false,false,true},2400);
        assert(d.currentClip()==13);
        d.tick(3400);assert(d.currentClip()==15);
        d.event({SoundEventType::Interference},3500);assert(d.currentClip()==11);
        d.tick(4500);assert(d.currentClip()==15);
        d.tick(5500);assert(d.phase()==SoundPhase::Finished);
    }
    {
        SoundDirector d(durations(),low);d.event({SoundEventType::Start},0);d.tick(1000);
        d.event({SoundEventType::Approach,true,false,true},1000);assert(d.currentClip()==6);
        d.tick(2200);assert(d.currentClip()==10); // Early once, without item-count condition.
        d.event({SoundEventType::Carry,true},3200);assert(d.currentClip()==7);
        d.event({SoundEventType::Approach,false,true},4200);assert(d.currentClip()==9);
        d.tick(6000);assert(d.currentClip()==8); // Five seconds after work began.
        d.event({SoundEventType::Finish,false,false,false,false},6001);assert(d.currentClip()==15);
    }
    {
        InputAttempts a;
        assert(a.key(65,true));assert(!a.key(65,true));assert(!a.key(65,false));
        assert(a.key(65,true)); // Second real press has no cooldown.
        assert(a.mouseMove(0));assert(!a.mouseMove(1));assert(!a.mouseMove(50));
        assert(a.mouseMove(170));
        assert(containsNewFolder(L"Копия — НоВаЯ ПаПкА (2)"));
        assert(!containsNewFolder(L"Новая_папка"));
    }
    {
        SoundDirector d(durations(),high);d.event({SoundEventType::Start},0);d.tick(1000);
        d.tick(1001);assert(d.currentClip()==1); // No random filler before approaching anything.
        d.event({SoundEventType::Approach},1002);assert(d.currentClip()==4);
        d.event({SoundEventType::Recycled},1003);assert(d.currentClip()==2);
        d.event({SoundEventType::Approach,true,false,true},1004);assert(d.currentClip()==6);
        d.event({SoundEventType::Finish,false,false,false,true},1005);
        auto serial=d.generation();d.event({SoundEventType::Finish,false,false,false,true},1006);
        assert(d.generation()==serial); // Last-drop event and loop completion do not restart 13.
        d.tick(2005);assert(d.currentClip()==15);
        d.tick(3005);assert(d.phase()==SoundPhase::Finished);
        d.event({SoundEventType::Interference},3006);assert(d.phase()==SoundPhase::Finished);
    }
    // Exhaustive probability boundaries for the 75/25 follow-up choice.
    for(unsigned r=0;r<100;r++) {
        SoundDirector d(durations(),[r](unsigned n){return r%n;});
        d.event({SoundEventType::Start},0);d.tick(1000);
        d.event({SoundEventType::Approach},1000);assert(d.currentClip()==4);
        d.event({SoundEventType::Approach},2000);assert(d.currentClip()==(r<25?4:5));
    }
    std::cout << "PASS: intro interruption, immediate restarts, event mapping, timing, probabilities and outro\n";
}
