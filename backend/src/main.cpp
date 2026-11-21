#include "modelforge/app.hpp"
#include "modelforge/util.hpp"

#include <iostream>
#include <map>
#include <string>

#ifdef _WIN32
#include <windows.h>
#endif

namespace {

void printHelp() {
    std::cout <<
        "ModelForge " MODELEFORGE_VERSION " - Local AI model conversion platform\n\n"
        "Usage:\n"
        "  modelforge serve [--port 8080] [--root <dir>] [--data <dir>] [--no-browser]\n"
        "  modelforge scan --input <model-dir> [--json]\n"
        "  modelforge validate --file <model.gguf> [--json]\n"
        "  modelforge convert --input <model-dir> --output <dir> [--name name] [--outtype f16]\n"
        "                     [--quant Q4_K_M] [--threads N] [--overwrite]\n\n"
        "  modelforge download hf --repo <owner/name|url> [--revision main] [--output <dir>]\n"
        "                     [--concurrency 4] [--max-speed bytes/s] [--files a,b]\n"
        "                     [--include <glob>] [--exclude <glob>] [--weights-only] [--token <hf_token>]\n"
        "  modelforge download list [--json]\n"
        "  modelforge download pause|resume|cancel|retry|delete <task_id>\n\n"
        "  modelforge llama status [--json]\n"
        "  modelforge llama check [--json]\n"
        "  modelforge llama assets <tag> [--json]\n"
        "  modelforge llama install [--tag b12532] [--asset <file>]\n\n"
        "Examples:\n"
        "  modelforge serve\n"
        "  modelforge scan --input ./models/Qwen3-0.6B\n"
        "  modelforge convert --input ./models/Qwen3-0.6B --output ./output --quant Q4_K_M\n"
        "  modelforge download hf --repo Qwen/Qwen2.5-0.5B-Instruct --output ./models --concurrency 4\n";
}

std::map<std::string, std::string> parseOptions(int argc, char** argv, int start) {
    std::map<std::string, std::string> options;
    for (int i = start; i < argc; ++i) {
        std::string key = argv[i];
        if (key == "--overwrite" || key == "--json" || key == "--no-browser" || key == "--weights-only") {
            options[key] = "true";
            continue;
        }
        if (mf::startsWith(key, "--") && i + 1 < argc) {
            options[key] = argv[++i];
        } else if (mf::startsWith(key, "--")) {
            options[key] = "true";
        }
    }
    return options;
}

std::string option(const std::map<std::string, std::string>& options, const std::string& key,
                   const std::string& fallback = "") {
    auto it = options.find(key);
    return it == options.end() ? fallback : it->second;
}

bool hasOption(const std::map<std::string, std::string>& options, const std::string& key) {
    return options.find(key) != options.end();
}

} // namespace

int main(int argc, char** argv) {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
#endif
    if (argc >= 2 && (std::string(argv[1]) == "--version" || std::string(argv[1]) == "-v")) {
        std::cout << MODELEFORGE_VERSION << std::endl;
        return 0;
    }
    if (argc >= 2 && (std::string(argv[1]) == "--help" || std::string(argv[1]) == "-h" || std::string(argv[1]) == "help")) {
        printHelp();
        return 0;
    }

    std::string command = "serve";
    std::string subcommand;
    int optionStart = 1;
    if (argc >= 2 && !mf::startsWith(argv[1], "--")) {
        command = argv[1];
        optionStart = 2;
        if (command == "download" && argc >= 3 && !mf::startsWith(argv[2], "--")) {
            subcommand = argv[2];
            optionStart = 3;
        }
        if (command == "llama" && argc >= 3 && !mf::startsWith(argv[2], "--")) {
            subcommand = argv[2];
            optionStart = 3;
        }
    }
    const auto options = parseOptions(argc, argv, optionStart);
    mf::AppOptions appOptions;
    if (hasOption(options, "--root")) appOptions.root = mf::pathFromUtf8(option(options, "--root"));
    if (hasOption(options, "--data")) appOptions.dataDir = mf::pathFromUtf8(option(options, "--data"));
    appOptions.noBrowser = hasOption(options, "--no-browser");
    if (hasOption(options, "--port")) {
        try { appOptions.portOverride = std::stoi(option(options, "--port")); } catch (...) {}
    }

    mf::App app(appOptions);
    if (command == "serve") {
        return app.runServer();
    }
    if (command == "scan") {
        const std::string input = option(options, "--input");
        if (input.empty()) {
            std::cerr << "scan 需要 --input <model-dir>\n";
            return 1;
        }
        return app.runScan(input, hasOption(options, "--json"));
    }
    if (command == "validate") {
        const std::string file = option(options, "--file");
        if (file.empty()) {
            std::cerr << "validate 需要 --file <model.gguf>\n";
            return 1;
        }
        return app.runValidate(file, hasOption(options, "--json"));
    }
    if (command == "convert") {
        const std::string input = option(options, "--input");
        const std::string output = option(options, "--output");
        if (input.empty() || output.empty()) {
            std::cerr << "convert 需要 --input <model-dir> 与 --output <dir>\n";
            return 1;
        }
        int threads = 0;
        if (hasOption(options, "--threads")) {
            try { threads = std::stoi(option(options, "--threads")); } catch (...) { threads = 0; }
        }
        return app.runConvert(input, output, option(options, "--name"), option(options, "--outtype", "f16"),
                              option(options, "--quant"), threads, hasOption(options, "--overwrite"));
    }
    if (command == "download") {
        if (subcommand.empty() || subcommand == "list") {
            return app.runDownloadList(hasOption(options, "--json"));
        }
        if (subcommand == "hf") {
            const std::string repo = option(options, "--repo", option(options, "--repo-id"));
            if (repo.empty()) {
                std::cerr << "download hf 需要 --repo <owner/name 或完整地址>\n";
                return 1;
            }
            int concurrency = 4;
            if (hasOption(options, "--concurrency")) {
                try { concurrency = std::stoi(option(options, "--concurrency")); } catch (...) { concurrency = 4; }
            }
            uint64_t maxSpeed = 0;
            if (hasOption(options, "--max-speed")) {
                try { maxSpeed = std::stoull(option(options, "--max-speed")); } catch (...) { maxSpeed = 0; }
            }
            return app.runDownloadHf(repo, option(options, "--revision", "main"), option(options, "--output"),
                                     concurrency, maxSpeed, option(options, "--files"),
                                     option(options, "--include"), option(options, "--exclude"),
                                     hasOption(options, "--weights-only"), option(options, "--token"));
        }
        if (subcommand == "pause" || subcommand == "resume" || subcommand == "cancel" ||
            subcommand == "retry" || subcommand == "delete") {
            std::string id = option(options, "--id");
            if (id.empty() && argc > 3 && !mf::startsWith(argv[3], "--")) id = argv[3];
            if (id.empty()) {
                std::cerr << "download " << subcommand << " 需要 <task_id>\n";
                return 1;
            }
            return app.runDownloadAction(subcommand, id);
        }
        std::cerr << "未知 download 子命令: " << subcommand << "\n";
        printHelp();
        return 1;
    }
    if (command == "llama") {
        std::string action = subcommand.empty() ? "status" : subcommand;
        std::string tag = option(options, "--tag");
        std::string asset = option(options, "--asset");
        if (action == "assets" && tag.empty() && argc > 3 && !mf::startsWith(argv[3], "--")) tag = argv[3];
        return app.runLlama(action, tag, asset, hasOption(options, "--json"));
    }
    std::cerr << "未知命令: " << command << "\n";
    printHelp();
    return 1;
}
