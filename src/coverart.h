#pragma once

#include <string>
#include <vector>

// Reads the cover art that ships with the audio file itself, rather than
// guessing it from a catalogue.
//
// This goes through the Windows shell thumbnail provider instead of parsing tag
// formats by hand. Windows already has property handlers for FLAC, MP3, M4A,
// WMA and friends, so one code path covers every format the OS can play, with
// no bespoke ID3/Vorbis/MP4 parsers to get wrong on a malformed file.
namespace coverart {

// GDI+ has to be running before any extraction. Safe to call more than once.
void Startup();
void Shutdown();

// Extracts artwork for `audioFile`, re-encoded as JPEG and scaled so neither
// edge exceeds `maxEdge`. Returns false when the file has no artwork.
//
// Tries the shell thumbnail first, then a cover image sitting next to the file
// (cover.jpg, folder.jpg, ...) for libraries that keep art out of the tags.
bool ExtractJpeg(const std::wstring& audioFile, int maxEdge, std::vector<unsigned char>* jpeg);

// Loads a standalone image file and re-encodes it the same way, used for the
// configurable "no cover" placeholder.
bool LoadImageAsJpeg(const std::wstring& imageFile, int maxEdge, std::vector<unsigned char>* jpeg);

// Lowercase hex SHA-1 of a byte range. Used to recognise artwork we have
// already uploaded, so identical covers are not uploaded twice.
std::string HashBytes(const std::vector<unsigned char>& bytes);

} // namespace coverart
