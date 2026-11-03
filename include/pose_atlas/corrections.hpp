// SPDX-License-Identifier: Apache-2.0
// Hand corrections that survive a re-render. The artist paints on the
// "Manual corrections" layer of atlas.aseprite; before a new render
// overwrites that file, its corrections are backed up, read with the key of
// their frame, and written into the new file at the frame with the same key.
#pragma once

#include <stdexcept>
#include <string>
#include <vector>

#include "pose_atlas/atlas.hpp"
#include "pose_atlas/image.hpp"

namespace pose_atlas {

inline constexpr const char* kCorrectionsReport = "corrections-report.json";
inline constexpr const char* kCorrectionsReportFormat = "pose-atlas-corrections-report";
inline constexpr int kCorrectionsReportVersion = 1;

class CorrectionsError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

/// One painted frame of an existing atlas.aseprite.
struct ExistingCorrection {
    std::string key;            // the frame's key
    int frame = 0;              // frame index in the existing file
    Image image;                // the corrections layer, sprite-sized
    std::string render_sha256;  // of the render it was painted over
};

struct ExistingCorrections {
    /// File name (in the same directory) of the backup of atlas.aseprite;
    /// empty when there were no corrections and so no backup.
    std::string backup;
    bool discarded = false;
    std::vector<ExistingCorrection> corrections;  // in frame order
};

/// "atlas.before-<first 12 hex digits of the file's SHA-256>.aseprite".
std::string corrections_backup_name(const std::vector<std::uint8_t>& aseprite_bytes);

/// Looks at `<directory>/atlas.aseprite`. Without that file, or when its
/// corrections layer has no pixel with non-zero alpha, returns nothing.
/// Otherwise copies it to corrections_backup_name() in the same directory
/// (an existing copy is never overwritten) and, unless `discard`, reads each
/// painted frame with its key: the user data text of the frame's Render cel
/// or, for files written before frame keys, the frame at the same index of
/// `<directory>/pose-contract.json` when that has as many frames. The render
/// it was painted over is identified by the old contract's render_sha256 for
/// that key or, without one, by the hash of the frame's Render layer. Throws
/// CorrectionsError, naming the backup, when a key cannot be found.
ExistingCorrections read_existing_corrections(const std::string& directory, bool discard);

struct CarriedCorrection {
    std::string key;
    int old_frame = 0;
    int frame = 0;  // in the new atlas
    bool stale = false;
};

struct OrphanedCorrection {
    std::string key;
    int old_frame = 0;
    std::string reason;
};

struct CorrectionsReport {
    std::string backup;  // empty when there was nothing to back up
    bool discarded = false;
    std::vector<CarriedCorrection> carried;  // stale ones included
    std::vector<OrphanedCorrection> orphaned;
    std::size_t stale_count() const;
    std::string to_json() const;
    /// One line, e.g. "corrections: 3 carried (1 stale), 2 orphaned; previous
    /// file kept as atlas.before-0123456789ab.aseprite".
    std::string summary() const;
};

/// Puts each existing correction into `atlas.corrections` at the frame with
/// the same key, at the same position (clipped to the cell when the cell size
/// changed). A correction is stale when that frame's render (the SHA-256 of
/// its RGBA) differs from the one it was painted over. Corrections whose key
/// is not in the atlas, or whose key an earlier painted frame already had,
/// are orphaned: left out (they remain in the backup) and reported.
CorrectionsReport carry_corrections(const ExistingCorrections& existing, Atlas& atlas);

}  // namespace pose_atlas
