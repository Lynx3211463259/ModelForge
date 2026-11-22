#include "modelforge/crypto.hpp"
#include "modelforge/hf_client.hpp"
#include "modelforge/http_client.hpp"
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

} // namespace

int main() {
    testSha256();
    testSanitize();
    testText();
    testCryptoEncoding();
    testHfParsing();
    testRedaction();
    if (failures) {
        std::cerr << failures << " test(s) failed\n";
        return 1;
    }
    std::cout << "All core tests passed\n";
    return 0;
}
