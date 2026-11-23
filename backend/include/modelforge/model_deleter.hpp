#pragma once

#include "modelforge/database.hpp"
#include "modelforge/util.hpp"

#include <string>

namespace mf {

// Outcome of DELETE /api/v1/models/{id}. A failed result keeps the model
// record intact so the user can fix the problem (close the file, grant
// permissions) and retry without losing track of the files.
struct ModelDeleteResult {
    bool ok = false;
    std::string code;
    std::string message;
    std::string suggestion;
    json details = json::object();

    json toJson() const;
};

namespace model_delete {

// Maps a filesystem error to a stable, user-facing code:
// MISSING_FILE / PERMISSION_DENIED / FILE_DELETE_FAILED.
std::string classifyFileError(const std::error_code& ec);

// Deletes a model record and, when deleteFiles is true, the files that were
// registered for it. Only paths recorded in model_files are touched and only
// when they live inside the model directory, so a shared or unrelated file is
// never removed. Files still referenced by another model are skipped.
ModelDeleteResult erase(Database& db, const std::string& modelId, bool deleteFiles);

} // namespace model_delete
} // namespace mf
