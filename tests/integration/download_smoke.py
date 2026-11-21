#!/usr/bin/env python3
"""Real Hugging Face download integration tests for ModelForge.

Every check uses the live Hugging Face REST API and real HTTP Range transfers.
No progress or file content is simulated.

Usage:
  python tests/integration/download_smoke.py --base-url http://127.0.0.1:8080
  python tests/integration/download_smoke.py --base-url http://127.0.0.1:8080 --full
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import shutil
import sys
import time
import urllib.error
import urllib.request
from pathlib import Path

TINY_REPO = "hf-internal-testing/tiny-random-LlamaForCausalLM"
SHARDED_REPO = "bumblebee-testing/tiny-random-GPT2Model-sharded"
GGUF_REPO = "ggml-org/tiny-llamas"

RESULTS: list[tuple[str, bool, str]] = []


def record(name: str, ok: bool, detail: str = "") -> None:
    RESULTS.append((name, ok, detail))
    print(("PASS  " if ok else "FAIL  ") + name + ((" · " + detail) if detail else ""))


class Api:
    def __init__(self, base: str):
        self.base = base.rstrip("/")
        if not self.base.endswith("/api/v1"):
            self.base += "/api/v1"

    def call(self, path: str, body=None, method: str | None = None, timeout: int = 120):
        data = None if body is None else json.dumps(body).encode()
        request = urllib.request.Request(
            self.base + path,
            data=data,
            headers={"Content-Type": "application/json"},
            method=method or ("POST" if body is not None else "GET"),
        )
        try:
            with urllib.request.urlopen(request, timeout=timeout) as response:
                return json.loads(response.read().decode())
        except urllib.error.HTTPError as error:
            payload = json.loads(error.read().decode())
            raise RuntimeError(payload.get("error", {}).get("code", "HTTP_" + str(error.code)) + ": " +
                               payload.get("error", {}).get("message", "")) from None

    def ok(self, path: str, body=None, method: str | None = None, timeout: int = 120):
        payload = self.call(path, body, method, timeout)
        if not payload.get("ok"):
            raise RuntimeError(json.dumps(payload.get("error"), ensure_ascii=False))
        return payload["data"]


def wait_task(api: Api, task_id: str, timeout: int = 180) -> dict:
    deadline = time.time() + timeout
    last = {}
    while time.time() < deadline:
        last = api.ok("/downloads/" + task_id)
        if last["status"] in ("completed", "failed", "cancelled"):
            return last
        time.sleep(1)
    return last


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def pick(repo: dict, predicate) -> list[dict]:
    return [file for file in repo["files"] if predicate(file)]


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--base-url", default="http://127.0.0.1:8080/api/v1")
    parser.add_argument("--work-dir", default="")
    parser.add_argument("--full", action="store_true", help="also run full-repo and corruption tests")
    args = parser.parse_args()

    api = Api(args.base_url)
    work = Path(args.work_dir or (Path(__file__).resolve().parents[2] / "models" / "_download_tests"))
    if work.exists():
        shutil.rmtree(work)
    work.mkdir(parents=True)

    # 1. Resolve a public repository by ID.
    try:
        tiny = api.ok("/huggingface/repository?repo=" + TINY_REPO + "&revision=main")
        record("解析公开仓库", tiny["repoId"] == TINY_REPO and tiny["fileCount"] > 0,
               f"{tiny['fileCount']} 文件 · {tiny['totalSize']} 字节 · commit {tiny['commitHash'][:12]}")
    except Exception as error:  # noqa: BLE001
        record("解析公开仓库", False, str(error))
        tiny = None

    # 2. Parse a full page URL and extract the revision.
    if tiny:
        try:
            url = f"https://huggingface.co/{TINY_REPO}/tree/main"
            parsed = api.ok("/huggingface/resolve", {"input": url, "revision": ""})
            record("从页面 URL 提取仓库与分支", parsed["repoId"] == TINY_REPO and parsed["revision"] == "main",
                   parsed["repoId"] + "@" + parsed["revision"])
        except Exception as error:  # noqa: BLE001
            record("从页面 URL 提取仓库与分支", False, str(error))

    # 3. Nonexistent repository must fail explicitly (HF returns 401 to hide existence).
    try:
        api.ok("/huggingface/resolve", {"input": "hf-internal-testing/definitely-not-a-real-repo-xyz"})
        record("不存在仓库错误", False, "服务端错误地接受了不存在的仓库")
    except Exception as error:  # noqa: BLE001
        record("不存在仓库错误", "HF_TOKEN_REQUIRED" in str(error) or "NOT_FOUND" in str(error), str(error)[:100])

    # 4. Path traversal must be rejected before any network or disk write.
    try:
        api.ok("/downloads", {"repoId": "a/b", "revision": "main", "destination": str(work / "unsafe"),
                              "files": ["../../escape.bin"]})
        record("路径穿越拒绝", False, "服务端接受了 ../ 路径")
    except Exception as error:  # noqa: BLE001
        record("路径穿越拒绝", "UNSAFE_PATH" in str(error), str(error)[:100])

    # 5. Single-file download with real SHA256 verification.
    single_task = None
    if tiny:
        model = next(file for file in tiny["files"] if file["path"] == "model.safetensors")
        task = api.ok("/downloads", {
            "repoId": tiny["repoId"], "revision": tiny["revision"], "commitHash": tiny["commitHash"],
            "destination": str(work / "single"), "files": [model], "concurrency": 1,
            "verifyChecksum": True, "autoImport": False,
        })
        single_task = wait_task(api, task["id"])
        local = Path(single_task["files"][0]["localPath"])
        ok = single_task["status"] == "completed" and local.exists() and sha256(local) == model["sha256"]
        record("单文件下载 + SHA256", ok, f"{single_task['status']} · {local.stat().st_size if local.exists() else 0} 字节")

    # 6. Multi-shard repository: every shard and the index file must be present.
    try:
        sharded = api.ok("/huggingface/repository?repo=" + SHARDED_REPO + "&revision=main")
        selected = pick(sharded, lambda f: f["category"] in ("safetensors", "index", "config"))
        task = api.ok("/downloads", {
            "repoId": sharded["repoId"], "revision": sharded["revision"], "commitHash": sharded["commitHash"],
            "destination": str(work / "sharded"), "files": selected, "concurrency": 4,
            "verifyChecksum": True, "autoImport": True,
            "repository": {"repoId": sharded["repoId"], "revision": sharded["revision"],
                           "commitHash": sharded["commitHash"], "license": sharded.get("license", "")},
        })
        done = wait_task(api, task["id"])
        shards = [f for f in done["files"] if f["remotePath"].startswith("model-")]
        all_sizes = all(Path(f["localPath"]).stat().st_size == f["size"] for f in done["files"])
        ok = (done["status"] == "completed" and len(shards) == 3 and all_sizes and
              bool(done["result"].get("modelId")))
        record("多分片下载 + 自动导入", ok,
               f"{len(shards)} 分片 · {len(done['files'])} 文件 · 模型 {done['result'].get('modelId', '—')}")
    except Exception as error:  # noqa: BLE001
        record("多分片下载 + 自动导入", False, str(error)[:140])

    # 7. Re-downloading the same destination must skip verified files.
    try:
        duplicate = api.ok("/downloads", {
            "repoId": sharded["repoId"], "revision": sharded["revision"], "commitHash": sharded["commitHash"],
            "destination": str(work / "sharded"), "files": selected, "concurrency": 4,
            "verifyChecksum": True, "autoImport": False,
        })
        done = wait_task(api, duplicate["id"])
        logs = api.ok(f"/downloads/{duplicate['id']}/logs?limit=200")["items"]
        skips = [item for item in logs if "跳过下载" in item["message"]]
        record("重复下载跳过", done["status"] == "completed" and len(skips) == len(selected),
               f"{len(skips)} 个文件被跳过")
        api.ok(f"/downloads/{duplicate['id']}", method="DELETE")
    except Exception as error:  # noqa: BLE001
        record("重复下载跳过", False, str(error)[:140])

    # 8. Pause, verify the .mfpart breakpoint, then resume and finish.
    if tiny:
        task = api.ok("/downloads", {
            "repoId": tiny["repoId"], "revision": tiny["revision"], "commitHash": tiny["commitHash"],
            "destination": str(work / "pause"), "files": [model], "concurrency": 1,
            "maxSpeed": 220000, "verifyChecksum": True, "autoImport": False,
        })
        time.sleep(3)
        paused_response = api.ok(f"/downloads/{task['id']}/pause", {})
        time.sleep(1.5)
        part = Path(task["files"][0]["localPath"] + ".mfpart") if task.get("files") else None
        paused = api.ok(f"/downloads/{task['id']}")
        part_size = part.stat().st_size if part and part.exists() else 0
        time.sleep(1.5)
        part_stable = part.stat().st_size if part and part.exists() else 0
        api.ok(f"/downloads/{task['id']}/resume", {})
        done = wait_task(api, task["id"])
        record("暂停 / 断点 / 继续", paused["status"] == "paused" and part_size > 0 and
               part_size == part_stable and done["status"] == "completed",
               f"断点 {part_size} 字节 · 最终 {done['downloadedBytes']} 字节")

    # 9. Cancel keeps the partial data and retry completes it.
    if tiny:
        task = api.ok("/downloads", {
            "repoId": tiny["repoId"], "revision": tiny["revision"], "commitHash": tiny["commitHash"],
            "destination": str(work / "cancel"), "files": [model], "concurrency": 1,
            "maxSpeed": 160000, "verifyChecksum": True, "autoImport": False,
        })
        time.sleep(3)
        api.ok(f"/downloads/{task['id']}/cancel", {})
        time.sleep(1.5)
        cancelled = api.ok(f"/downloads/{task['id']}")
        api.ok(f"/downloads/{task['id']}/retry", {})
        done = wait_task(api, task["id"])
        record("取消 / 保留分片 / 重试", cancelled["status"] == "cancelled" and
               cancelled["downloadedBytes"] < cancelled["totalBytes"] and done["status"] == "completed",
               f"取消时 {cancelled['downloadedBytes']} 字节 · 重试后 {done['downloadedBytes']} 字节")

    # 10. GGUF repository must be validated with the GGUF reader, not imported as safetensors.
    try:
        gguf_repo = api.ok("/huggingface/repository?repo=" + GGUF_REPO + "&revision=main")
        gguf = next(file for file in gguf_repo["files"] if file["path"] == "stories260K.gguf")
        task = api.ok("/downloads", {
            "repoId": gguf_repo["repoId"], "revision": gguf_repo["revision"], "commitHash": gguf_repo["commitHash"],
            "destination": str(work / "gguf"), "files": [gguf], "concurrency": 1,
            "verifyChecksum": True, "autoImport": True,
        })
        done = wait_task(api, task["id"])
        record("GGUF 仓库识别与校验", done["status"] == "completed" and done["result"].get("kind") == "gguf" and
               done["result"].get("verification", {}).get("valid") is True,
               done["result"].get("summary", "")[:80])
    except Exception as error:  # noqa: BLE001
        record("GGUF 仓库识别与校验", False, str(error)[:140])

    # 11. Invalid token is rejected by the whoami endpoint.
    try:
        check = api.ok("/huggingface/token/validate", {"token": "hf_invalidtokenvalue1234567890"})
        record("错误 Token 校验", check.get("valid") is False and check.get("code") == "TOKEN_INVALID",
               check.get("message", ""))
    except Exception as error:  # noqa: BLE001
        record("错误 Token 校验", False, str(error)[:140])

    # 12. Task list and per-file detail are consistent.
    try:
        listing = api.ok("/downloads?limit=100")
        detail = api.ok("/downloads/" + single_task["id"]) if single_task else None
        record("任务列表 / 详情一致", bool(listing["items"]) and detail is not None and
               all(file["size"] == Path(file["localPath"]).stat().st_size
                   for file in detail["files"] if file["status"] == "completed"),
               f"{len(listing['items'])} 个任务")
    except Exception as error:  # noqa: BLE001
        record("任务列表 / 详情一致", False, str(error)[:140])

    if args.full:
        # 13. Full repository (including README and ONNX) must match every remote size.
        try:
            task = api.ok("/downloads", {
                "repoId": tiny["repoId"], "revision": tiny["revision"], "commitHash": tiny["commitHash"],
                "destination": str(work / "full"), "files": tiny["files"], "concurrency": 4,
                "verifyChecksum": True, "autoImport": True,
            })
            done = wait_task(api, task["id"], timeout=300)
            sizes = all(Path(f["localPath"]).stat().st_size == f["size"] for f in done["files"])
            record("下载完整仓库", done["status"] == "completed" and len(done["files"]) == len(tiny["files"]) and sizes,
                   f"{len(done['files'])} 文件 · {done['downloadedBytes']} 字节")
        except Exception as error:  # noqa: BLE001
            record("下载完整仓库", False, str(error)[:140])

        # 14. A wrong checksum must fail the task and delete the corrupt part file.
        try:
            bad = dict(gguf)
            bad["sha256"] = "0" * 64
            task = api.ok("/downloads", {
                "repoId": gguf_repo["repoId"], "revision": gguf_repo["revision"], "commitHash": gguf_repo["commitHash"],
                "destination": str(work / "corrupt"), "files": [bad], "concurrency": 1,
                "verifyChecksum": True, "autoImport": False,
            })
            done = wait_task(api, task["id"], timeout=180)
            local = Path(work / "corrupt" / "stories260K.gguf")
            record("哈希校验失败处理", done["status"] == "failed" and not local.exists() and
                   not Path(str(local) + ".mfpart").exists(),
                   done["errorMessage"][:80])
        except Exception as error:  # noqa: BLE001
            record("哈希校验失败处理", False, str(error)[:140])

    passed = sum(1 for _, ok, _ in RESULTS if ok)
    print(f"\n{passed}/{len(RESULTS)} 项通过")
    return 0 if passed == len(RESULTS) else 1


if __name__ == "__main__":
    sys.exit(main())
