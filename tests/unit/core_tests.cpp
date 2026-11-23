#include "modelforge/crypto.hpp"
#include "modelforge/hf_client.hpp"
#include "modelforge/database.hpp"
#include "modelforge/fs_browser.hpp"
#include "modelforge/http_client.hpp"
#include "modelforge/model_deleter.hpp"
#include "modelforge/settings.hpp"
#include "modelforge/update_service.hpp"
#include "modelforge/util.hpp"

#include <iostream>
#include <string>

namespace {
int failures = 0;

void expect(bool condition, const std::string& name) {
    if (condition) {
        std::cout << "[PASS] " << name << "\n";
    } else {
        std::cerr << "[FAIL] " << name << "\n";
        ++failures;
    }
}

void testSha256() {
    mf::Sha256 hash;
    const std::string input = "abc";
    hash.update(input.data(), input.size());
    expect(hash.hexDigest() == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", "SHA256 abc vector");

    mf::Sha256 emptyHash;
    expect(emptyHash.hexDigest() == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855", "SHA256 empty vector");
}

void testSanitize() {
    expect(mf::sanitizeFileName("model:Q4?.gguf") == "model_Q4_.gguf", "sanitize invalid filename characters");
    expect(mf::sanitizeFileName("..") == "model", "sanitize traversal name");
    expect(mf::sanitizeFileName("CON") == "model_CON", "sanitize Windows reserved name");
}

void testText() {
    expect(mf::toUpper("Q4_k_m") == "Q4_K_M", "uppercase");
    expect(mf::humanBytes(1024) == "1.00 KB", "human readable size");
    expect(mf::sanitizeSingleLine("a\nb\tc") == "a b c", "single line log");
    expect(mf::endsWith("model.gguf", ".gguf"), "suffix detection");
}

void testCryptoEncoding() {
    expect(mf::base64Encode("hello") == "aGVsbG8=", "base64 hello");
    expect(mf::sha1("abc") == "a9993e364706816aba3e25717850c26c9cd0d89d", "SHA1 abc vector");
    const std::string websocketKey = "dGhlIHNhbXBsZSBub25jZQ==258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    expect(mf::base64Encode(mf::sha1Raw(websocketKey)) == "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=", "WebSocket accept digest");
}
void testHfParsing() {
    std::string repoId, revision, error;
    expect(mf::HfClient::parseRepositoryInput("Qwen/Qwen3-0.6B", "", repoId, revision, error) &&
           repoId == "Qwen/Qwen3-0.6B" && revision == "main", "HF repo id parsing");

    expect(mf::HfClient::parseRepositoryInput("https://huggingface.co/Qwen/Qwen2.5-0.5B-Instruct/tree/dev",
                                              "", repoId, revision, error) &&
           repoId == "Qwen/Qwen2.5-0.5B-Instruct" && revision == "dev", "HF page URL with revision");

    expect(mf::HfClient::parseRepositoryInput("owner/model@v1.0", "", repoId, revision, error) &&
           repoId == "owner/model" && revision == "v1.0", "HF @revision parsing");

    expect(!mf::HfClient::parseRepositoryInput("not-a-repo", "", repoId, revision, error),
           "HF invalid repo rejected");

    std::string reason;
    expect(!mf::HfClient::isSafeRelativePath("../etc/passwd", reason), "reject traversal path");
    expect(!mf::HfClient::isSafeRelativePath("C:/Windows/system32", reason), "reject absolute drive path");
    expect(!mf::HfClient::isSafeRelativePath("/etc/passwd", reason), "reject absolute path");
    expect(!mf::HfClient::isSafeRelativePath("sub/CON.txt", reason), "reject Windows reserved name");
    expect(mf::HfClient::isSafeRelativePath("subdir/model-00001-of-00003.safetensors", reason),
           "accept nested model file");

    std::string pathError;
    // Use a real absolute directory so the check behaves identically on Windows and
    // POSIX: a drive-letter path is relative on POSIX and resolves against the cwd.
    const mf::fs::path root = mf::fs::temp_directory_path() / "mf-hf-root";
    const mf::fs::path local = mf::HfClient::localPathFor(root, "sub/model.safetensors", pathError);
    expect(pathError.empty() && mf::isPathInside(root, local),
           "local path stays inside destination");

    expect(mf::HfClient::classifyFile("model-00001-of-00002.safetensors") == "safetensors", "classify safetensors");
    expect(mf::HfClient::classifyFile("model.safetensors.index.json") == "index", "classify shard index");
    expect(mf::HfClient::classifyFile("tokenizer.model") == "vocab", "classify vocab");
    expect(mf::HfClient::classifyFile("model.Q4_K_M.gguf") == "gguf", "classify gguf");

    const std::string url = mf::HfClient::resolveUrl("owner/model", "main", "sub dir/model.safetensors");
    expect(url == "https://huggingface.co/owner/model/resolve/main/sub%20dir/model.safetensors",
           "resolve URL encoding");
}

void testRedaction() {
    const std::string secret = "hf_abcdefghijklmnopqrstuvwxyz123456";
    const std::string redacted = mf::HttpClient::redact("Authorization: Bearer " + secret + " failed");
    expect(redacted.find(secret) == std::string::npos, "token redacted from log text");
    expect(mf::HttpClient::redact("hf_abcdefghijklmnop").find("hf_abcdefghijklmnop") == std::string::npos,
           "hf token pattern redacted");
}

void testUpdateHelpers() {
    std::vector<int> parts;
    expect(mf::updates::parseVersion("v1.2.0", parts) && parts == std::vector<int>({1, 2, 0}),
           "parse semver tag");
    expect(mf::updates::parseVersion("1.10.0-beta.3", parts) && parts == std::vector<int>({1, 10, 0}),
           "parse prerelease tag ignores suffix");
    expect(!mf::updates::parseVersion("release-candidate", parts), "reject non-version tag");
    expect(mf::updates::compareVersions("v1.10.0", "v1.9.9") > 0, "numeric segment comparison");
    expect(mf::updates::compareVersions("1.0.0", "1.0.0") == 0, "equal versions");
    expect(mf::updates::compareVersions("v1.0.0", "1.0.1") < 0, "older version");
    expect(mf::updates::compareVersions("bad", "1.0.0") == 0, "malformed version never upgrades");

    expect(mf::updates::normalizeChannel(" Nightly ") == "nightly", "channel normalization");
    expect(mf::updates::normalizeChannel("whatever") == "stable", "unknown channel falls back to stable");

    const mf::json releases = mf::json::array({
        {{"tag_name", "v1.0.0"}, {"prerelease", false}, {"draft", false}},
        {{"tag_name", "v1.2.0-nightly.3"}, {"prerelease", true}, {"draft", false}},
        {{"tag_name", "v1.1.0"}, {"prerelease", false}, {"draft", false}},
        {{"tag_name", "v9.9.9"}, {"prerelease", false}, {"draft", true}}});
    expect(mf::updates::selectReleaseIndex(releases, "stable") == 2, "stable channel skips prereleases and drafts");
    expect(mf::updates::selectReleaseIndex(releases, "nightly") == 1, "nightly channel prefers prereleases");
    expect(mf::updates::selectReleaseIndex(releases, "auto") == 1, "auto channel takes the highest version");
    expect(mf::updates::selectReleaseIndex(mf::json::array(), "auto") == -1, "empty release list");
    const mf::json stableOnly = mf::json::array({{{"tag_name", "v2.0.0"}, {"prerelease", false}}});
    expect(mf::updates::selectReleaseIndex(stableOnly, "nightly") == 0, "nightly falls back to stable");

    const std::string suffix = mf::updates::platformAssetSuffix();
    expect(!suffix.empty() && (suffix == "win64" || suffix.find('-') != std::string::npos),
           "platform asset suffix");

    const std::string sums =
        "abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789  ModelForge-1.0.0-linux-x64.zip\n"
        "1234567890abcdef1234567890abcdef1234567890abcdef1234567890abcdef *other.zip\n";
    expect(mf::updates::checksumFor(sums, "ModelForge-1.0.0-linux-x64.zip") ==
           "abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789", "checksum lookup");
    expect(mf::updates::checksumFor(sums, "missing.zip").empty(), "missing checksum");

    mf::UpdateInfo info;
    info.currentVersion = "1.0.0";
    info.latestVersion = "v1.1.0";
    info.code = "UPDATE_AVAILABLE";
    info.updateAvailable = true;
    const mf::json projected = info.toJson();
    expect(projected.value("updateAvailable", false) && projected.value("latestVersion", "") == "v1.1.0",
           "update info projection");
    mf::UpdateJob job;
    job.id = "upd_1";
    job.status = "ready";
    expect(job.toJson().value("status", "") == "ready", "update job projection");
}

void testFsBrowser() {
    const mf::fs::path base = mf::fs::temp_directory_path() / ("mf-fs-" + mf::newId("t"));
    std::error_code ec;
    mf::fs::create_directories(base / "beta", ec);
    mf::fs::create_directories(base / "Alpha", ec);
    mf::fs::create_directories(base / ".hidden", ec);
    mf::writeTextFile(base / "note.txt", "x");

    const mf::fs_browse::Listing listing = mf::fs_browse::list(mf::pathToUtf8(base));
    expect(listing.ok, "directory listing succeeds");
    expect(listing.entries.size() == 3, "listing returns directories only");
    expect(listing.entries[0].value("name", "") == ".hidden" || listing.entries[0].value("name", "") == "Alpha",
           "entries are ordered");
    bool hasFile = false;
    bool hiddenFlag = false;
    for (const auto& entry : listing.entries) {
        if (entry.value("name", "") == "note.txt") hasFile = true;
        if (entry.value("name", "") == ".hidden") hiddenFlag = entry.value("hidden", false);
    }
    expect(!hasFile, "files are excluded from the directory tree");
    expect(hiddenFlag, "hidden directories are flagged");
    expect(!listing.parent.empty(), "parent directory is reported");

    const mf::fs_browse::Listing notDir = mf::fs_browse::list(mf::pathToUtf8(base / "note.txt"));
    expect(!notDir.ok && notDir.code == "NOT_A_DIRECTORY", "file path is rejected");

    const mf::fs_browse::Listing missing = mf::fs_browse::list(mf::pathToUtf8(base / "missing"));
    expect(!missing.ok && missing.code == "DIRECTORY_NOT_FOUND", "missing directory is reported");

    const mf::fs_browse::Listing home = mf::fs_browse::list("");
    expect(home.ok && !home.path.empty(), "empty path starts at home or the filesystem root");

    const mf::json roots = mf::fs_browse::roots();
    expect(roots.is_array() && !roots.empty(), "filesystem roots are available");
    expect(mf::fs_browse::classifyError(std::make_error_code(std::errc::permission_denied)) ==
               "DIRECTORY_PERMISSION_DENIED", "permission error is classified");

    const mf::fs_browse::Listing withFiles = mf::fs_browse::list(mf::pathToUtf8(base), true);
    expect(withFiles.ok && withFiles.entries.size() == 4, "file mode includes files");
    bool fileMarked = false;
    for (const auto& entry : withFiles.entries) {
        if (entry.value("name", "") == "note.txt") fileMarked = !entry.value("directory", true);
    }
    expect(fileMarked, "regular files are marked as non-directories");

    const mf::fs_browse::FileInfo fileInfo = mf::fs_browse::describeFile(mf::pathToUtf8(base / "note.txt"));
    expect(fileInfo.ok && fileInfo.name == "note.txt" && fileInfo.size > 0, "file validation succeeds");
    expect(mf::fs_browse::describeFile(mf::pathToUtf8(base)).code == "NOT_A_FILE", "directory is not a file");
    expect(mf::fs_browse::describeFile(mf::pathToUtf8(base / "missing")).code == "FILE_NOT_FOUND",
           "missing file is reported");
    expect(mf::fs_browse::describeFile("").code == "FILE_REQUIRED", "empty file path is rejected");

    mf::fs::remove_all(base, ec);
}

void testSettingsPortValidation() {
    const mf::fs::path base = mf::fs::temp_directory_path() / ("mf-port-" + mf::newId("t"));
    std::error_code ec;
    mf::fs::create_directories(base, ec);
    mf::Database db;
    std::string dbError;
    expect(db.open(mf::pathToUtf8(base / "test.sqlite"), dbError), "port tests: open database");
    mf::SettingsService settings(db, base, base / "data", base);

    std::string error;
    const mf::json valid = settings.update(mf::json{{"port", 18300}}, error);
    expect(error.empty() && valid.value("port", 0) == 18300, "valid port is saved");

    const std::vector<mf::json> badPorts{mf::json(80), mf::json(70000), mf::json(0), mf::json("abc")};
    for (const auto& bad : badPorts) {
        settings.update(mf::json{{"port", bad}}, error);
        expect(!error.empty(), "invalid port " + bad.dump() + " is rejected");
    }
    expect(settings.get().port == 18300, "rejected port never changes the setting");
    db.close();
    mf::fs::remove_all(base, ec);
}

void testVersionSource() {
    std::string fileVersion;
    try {
        fileVersion = mf::trim(mf::readTextFile(std::string(MODELEFORGE_SOURCE_DIR) + "/VERSION"));
    } catch (const std::exception&) {
        fileVersion.clear();
    }
    expect(fileVersion == MODELEFORGE_VERSION, "VERSION file matches the compiled version");
}

void testModelDeletion() {
    const mf::fs::path base = mf::fs::temp_directory_path() / ("mf-del-" + mf::newId("t"));
    std::error_code ec;
    mf::fs::create_directories(base, ec);
    mf::Database db;
    std::string dbError;
    expect(db.open(mf::pathToUtf8(base / "test.sqlite"), dbError), "delete tests: open database");

    auto makeModel = [&](const std::string& id, const std::string& name, const mf::fs::path& dir) {
        mf::fs::create_directories(dir, ec);
        db.query("INSERT INTO models(id,name,path,status,supported,created_at,updated_at) VALUES(?,?,?,?,?,?,?)",
                 {id, name, mf::pathToUtf8(dir), "READY", 1, mf::isoNow(), mf::isoNow()});
    };
    auto addFile = [&](const std::string& id, const mf::fs::path& file, const std::string& content) {
        mf::writeTextFile(file, content);
        db.query("INSERT INTO model_files(model_id,path,file_name,size,kind,valid) VALUES(?,?,?,?,?,?)",
                 {id, mf::pathToUtf8(file), mf::pathToUtf8(file.filename()),
                  static_cast<int64_t>(content.size()), "weights", 1});
    };
    auto modelCount = [&](const std::string& id) {
        return db.query("SELECT COUNT(*) AS n FROM models WHERE id=?", {id})[0].value("n", 0ll) != 0;
    };
    auto fileRowCount = [&](const std::string& id) {
        return db.query("SELECT COUNT(*) AS n FROM model_files WHERE model_id=?", {id})[0].value("n", 0ll) != 0;
    };

    // 1. Normal deletion removes record, file rows and files.
    const mf::fs::path dirA = base / "modelA";
    makeModel("m_a", "A", dirA);
    addFile("m_a", dirA / "a.safetensors", "weights-a");
    addFile("m_a", dirA / "config.json", "{}");
    const mf::ModelDeleteResult okA = mf::model_delete::erase(db, "m_a", true);
    expect(okA.ok, "delete record and files succeeds");
    expect(!mf::fs::exists(dirA / "a.safetensors") && !mf::fs::exists(dirA / "config.json"),
           "registered model files are removed");
    expect(!modelCount("m_a"), "model record removed");
    expect(!fileRowCount("m_a"), "model_files rows removed");
    expect(okA.details.value("removedFiles", 0) == 2, "removed file count reported");
    expect(okA.details.value("freedBytes", 0ll) > 0, "freed bytes reported");

    // 2. Missing files are tolerated and reported.
    const mf::fs::path dirB = base / "modelB";
    makeModel("m_b", "B", dirB);
    db.query("INSERT INTO model_files(model_id,path,file_name,size,kind,valid) VALUES(?,?,?,?,?,?)",
             {"m_b", mf::pathToUtf8(dirB / "gone.safetensors"), "gone.safetensors", 0ll, "weights", 1});
    const mf::ModelDeleteResult missing = mf::model_delete::erase(db, "m_b", true);
    expect(missing.ok && missing.details["missingFiles"].size() == 1, "missing file is reported and tolerated");
    expect(!modelCount("m_b"), "record removed even when a file is missing");

    // 3. Files referenced by another model are never deleted.
    const mf::fs::path dirC = base / "modelC";
    makeModel("m_c", "C", dirC);
    addFile("m_c", dirC / "shared.safetensors", "shared");
    makeModel("m_d", "D", base / "modelD");
    db.query("INSERT INTO model_files(model_id,path,file_name,size,kind,valid) VALUES(?,?,?,?,?,?)",
             {"m_d", mf::pathToUtf8(dirC / "shared.safetensors"), "shared.safetensors", 6ll, "weights", 1});
    const mf::ModelDeleteResult shared = mf::model_delete::erase(db, "m_c", true);
    expect(shared.ok, "deletion succeeds when a file is shared");
    expect(mf::fs::exists(dirC / "shared.safetensors"), "shared file kept on disk");
    expect(shared.details["sharedFiles"].size() == 1, "shared file reported");
    expect(modelCount("m_d"), "other model record untouched");

    // 4. Paths outside the model directory are refused.
    const mf::fs::path dirE = base / "modelE";
    makeModel("m_e", "E", dirE);
    const mf::fs::path outside = base / "outside.txt";
    addFile("m_e", outside, "keep me");
    const mf::ModelDeleteResult unsafe = mf::model_delete::erase(db, "m_e", true);
    expect(unsafe.ok && mf::fs::exists(outside), "file outside the model directory is kept");
    expect(unsafe.details["unsafeFiles"].size() == 1, "outside path reported");

    // 5. Active conversion tasks block deletion.
    const mf::fs::path dirF = base / "modelF";
    makeModel("m_f", "F", dirF);
    addFile("m_f", dirF / "f.safetensors", "f");
    db.query("INSERT INTO conversion_tasks(id,model_id,model_name,status,stage,input_path,params_json,created_at) "
             "VALUES(?,?,?,?,?,?,?,?)",
             {"task_in_use", "m_f", "F", "RUNNING", "converting", mf::pathToUtf8(dirF), "{}",
              static_cast<int64_t>(mf::nowMs())});
    const mf::ModelDeleteResult inUse = mf::model_delete::erase(db, "m_f", true);
    expect(!inUse.ok && inUse.code == "MODEL_IN_USE", "running task blocks deletion");
    expect(mf::fs::exists(dirF / "f.safetensors") && modelCount("m_f"), "blocked deletion keeps record and files");
    db.query("UPDATE conversion_tasks SET status='SUCCEEDED' WHERE id=?", {"task_in_use"});

    // 6. Enabled workflows that reference the model block deletion.
    const mf::fs::path dirG = base / "modelG";
    makeModel("m_g", "G", dirG);
    addFile("m_g", dirG / "g.safetensors", "g");
    const std::string definition =
        mf::json{{"steps", mf::json::array({mf::json{{"modelId", "m_g"}}})}}.dump();
    db.query("INSERT INTO workflows(id,name,description,definition_json,enabled,created_at,updated_at) "
             "VALUES(?,?,?,?,1,?,?)",
             {"wf_ref", "ref", "", definition, mf::isoNow(), mf::isoNow()});
    const mf::ModelDeleteResult wfBlocked = mf::model_delete::erase(db, "m_g", true);
    expect(!wfBlocked.ok && wfBlocked.code == "MODEL_IN_USE", "enabled workflow blocks deletion");
    db.query("UPDATE workflows SET enabled=0 WHERE id=?", {"wf_ref"});
    const mf::ModelDeleteResult afterWorkflow = mf::model_delete::erase(db, "m_g", true);
    expect(afterWorkflow.ok, "deletion proceeds once the workflow is disabled");

    // 7. deleteFiles=false keeps the files but removes the record.
    const mf::fs::path dirH = base / "modelH";
    makeModel("m_h", "H", dirH);
    addFile("m_h", dirH / "h.safetensors", "h");
    const mf::ModelDeleteResult keep = mf::model_delete::erase(db, "m_h", false);
    expect(keep.ok && mf::fs::exists(dirH / "h.safetensors") && !modelCount("m_h"),
           "deleteFiles=false keeps files and removes the record");

    // 8. Helpers: error classification and path containment.
    expect(mf::model_delete::classifyFileError(std::make_error_code(std::errc::permission_denied)) ==
               "PERMISSION_DENIED", "permission error classified");
    expect(mf::model_delete::classifyFileError(std::make_error_code(std::errc::no_such_file_or_directory)) ==
               "MISSING_FILE", "missing-file error classified");
    expect(mf::model_delete::classifyFileError(std::error_code()).empty(), "clean error has no code");
    expect(mf::isPathInside(base, base / "inside.txt"), "path containment accepts child");
    expect(!mf::isPathInside(base / "a", base / "ab"), "path containment rejects sibling prefix");

    db.close();
    mf::fs::remove_all(base, ec);
}

} // namespace

int main() {
    testSha256();
    testSanitize();
    testText();
    testCryptoEncoding();
    testHfParsing();
    testRedaction();
    testUpdateHelpers();
    testVersionSource();
    testSettingsPortValidation();
    testFsBrowser();
    testModelDeletion();
    if (failures) {
        std::cerr << failures << " test(s) failed\n";
        return 1;
    }
    std::cout << "All core tests passed\n";
    return 0;
}
