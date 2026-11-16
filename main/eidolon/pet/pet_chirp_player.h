#pragma once

// Plays the pet's chirps (pet_sound.h) on the speaker from one worker task, so the UI
// never blocks on the codec. The caller decides whether a chirp may play at all
// (outside a conversation, Owner's AudioCue permission); this only plays it.

#include "eidolon/pet/pet_sound.h"

namespace eidolon::pet {

// Queues `chirp` at `volume` (0..1). A chirp already playing finishes; a newer request
// replaces any waiting one, so a burst of taps never builds a backlog. False if the
// worker could not start.
bool PlayChirp(Chirp chirp, float volume);

}  // namespace eidolon::pet
