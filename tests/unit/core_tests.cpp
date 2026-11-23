#include "modelforge/crypto.hpp"
#include "modelforge/hf_client.hpp"
#include "modelforge/http_client.hpp"
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

} // namespace

int main() {
    testSha256();
    testSanitize();
    testText();
    testCryptoEncoding();
    testHfParsing();
    testRedaction();
    testUpdateHelpers();
    if (failures) {
        std::cerr << failures << " test(s) failed\n";
        return 1;
    }
    std::cout << "All core tests passed\n";
    return 0;
}
