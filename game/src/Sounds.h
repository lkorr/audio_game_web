#pragma once
// Procedural builtin sound synthesis. Registers the following sets into a
// SoundLibrary at startup (all mono float at the device rate):
//
//   water (loop)  wind (loop)  bird (loop, 3 variants)
//   grass (8 step variants)    stone (8 step variants)
//   knock (4 impact variants)  thud (4 impact variants)
//   saw (loop, 3 pitches -- bug drones)
//   ping (1 -- player echo probe)   stalker (loop, 2 pitches -- predator drone)
//   sting_win, sting_fail (1 each -- slice outcome stingers)
//   music (loop -- sanctuary pad)
//
// File-based sounds layer on top of these via SoundLibrary::rescan().

class SoundLibrary;

void registerBuiltinSounds(SoundLibrary& lib, double sampleRate);
