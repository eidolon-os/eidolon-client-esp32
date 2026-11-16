#pragma once
// Only the pet's body is replaced: the view asks it for senses, power, LEDs and
// head moves. Head moves are counted so the test can see which ones were let out.
#include <cstdint>
#include <functional>
#include "eidolon/pet/pet_face.h"
#include "eidolon/pet/pet_sense.h"

class Board {
public:
    using PetSenseSink = std::function<void(const eidolon::pet::SenseEvent&)>;
    static Board& GetInstance() { static Board board; return board; }
    void SetPetSenseSink(PetSenseSink sink) { sink_ = std::move(sink); }
    bool GetPetPower(int&, bool&) { return false; }
    void SetPetLeds(const uint8_t*) {}
    bool PlayPetHeadClip(const eidolon::pet::HeadClip&) { ++head_clips; return true; }
    void SettlePetHead() {}
    int head_clips = 0;

private:
    PetSenseSink sink_;
};
