// Pure file-naming / file-moving logic. No OBS or Qt dependencies, so it can be unit-tested.
#pragma once
#include <string>

namespace osp {

// Removes characters Windows does not allow in file names, trims trailing dots/spaces,
// avoids reserved device names (CON, NUL, COM1...) and caps the length.
// Returns "" when nothing usable is left.
std::string sanitize_name(const std::string &input);

// Removes a trailing extension (e.g. ".mp4") from `name` if it matches `ext` (case-insensitive).
std::string strip_matching_ext(const std::string &name, const std::string &ext);

// Builds the final full path for a recording.
//  - folder: destination folder (UTF-8). If empty, the original file's folder is used.
//  - typed_name: what the user typed; empty/invalid means "keep the OBS name".
//  - original_path: the file OBS just recorded (UTF-8).
// Never returns a path that already exists: " (2)", " (3)"... is appended instead.
std::string build_target_path(const std::string &folder, const std::string &typed_name,
			      const std::string &original_path);

// Moves/renames a file. Creates the destination folder if needed, never overwrites an
// existing file, and falls back to copy+verify+delete when moving across drives.
// Returns true on success; on failure `error` explains why and the source file is left untouched.
bool move_file(const std::string &from, const std::string &to, std::string &error);

// Returns the folder part of a path (UTF-8), without a trailing separator.
std::string folder_of(const std::string &path);

// True when both paths point to the same location (compares the normalized form).
bool same_path(const std::string &a, const std::string &b);

} // namespace osp
