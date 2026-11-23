#pragma once

#include "modelforge/util.hpp"

#include <string>

namespace mf {

// Read-only directory browsing for the Web UI folder picker. It never reads
// file contents and only reports directory names and paths.
namespace fs_browse {

struct Listing {
    bool ok = false;
    std::string code;
    std::string message;
    std::string suggestion;
    std::string path;
    std::string parent;
    bool truncated = false;
    json entries = json::array();

    json toJson() const;
};

// Drives on Windows, "/" and the user's home on every platform.
json roots();

// Lists sub-directories of requestedPath. An empty path starts at the user's
// home directory (or the filesystem root when no home is known).
Listing list(const std::string& requestedPath);

// Stable reason for a filesystem error, for API error mapping.
std::string classifyError(const std::error_code& ec);

} // namespace fs_browse
} // namespace mf
