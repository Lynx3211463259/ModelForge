#!/usr/bin/env python3
"""ModelForge API integration smoke test.

This test talks to a running ModelForge instance. It never invents conversion
results: conversion is only attempted when --model-path is provided.
"""
import argparse
import json
import sys
import time
import urllib.error
import urllib.parse
import urllib.request


def request(base, path, method="GET", body=None, timeout=60):
    data = None
    headers = {}
    if body is not None:
        data = json.dumps(body).encode("utf-8")
        headers["Content-Type"] = "application/json"
    req = urllib.request.Request(base + path, data=data, headers=headers, method=method)
    try:
        with urllib.request.urlopen(req, timeout=timeout) as response:
            payload = json.loads(response.read().decode("utf-8"))
            return response.status, payload
    except urllib.error.HTTPError as exc:
        try:
            payload = json.loads(exc.read().decode("utf-8"))
        except Exception:
            payload = {"ok": False, "error": {"code": "HTTP", "message": str(exc)}}
        return exc.code, payload


def expect_ok(name, status, payload, allowed=(200, 202, 207)):
    if status not in allowed or not payload.get("ok"):
        print(f"[FAIL] {name}: HTTP {status} {payload}")
        return False
    print(f"[PASS] {name}")
    return True


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--base-url", default="http://127.0.0.1:8080")
    parser.add_argument("--model-path", default="")
    parser.add_argument("--quant", default="Q4_K_M")
    parser.add_argument("--wait", action="store_true")
    args = parser.parse_args()
    base = args.base_url.rstrip("/") + "/api/v1"
    failures = 0

    status, payload = request(base, "/health")
    failures += 0 if expect_ok("health", status, payload) else 1
    websocket = payload.get("data", {}).get("websocketRunning") if payload.get("ok") else False

    status, payload = request(base, "/capabilities?refresh=1")
    failures += 0 if expect_ok("capabilities", status, payload) else 1
    caps = payload.get("data", {}) if payload.get("ok") else {}

    status, payload = request(base, "/system/summary", timeout=90)
    failures += 0 if expect_ok("system summary", status, payload) else 1

    status, payload = request(base, "/models")
    failures += 0 if expect_ok("model list", status, payload) else 1

    status, payload = request(base, "/quantization/profiles")
    failures += 0 if expect_ok("quantization profiles", status, payload) else 1

    status, payload = request(base, "/workflows")
    failures += 0 if expect_ok("workflows", status, payload) else 1

    status, payload = request(base, "/history?limit=10")
    failures += 0 if expect_ok("history", status, payload) else 1

    print(f"[INFO] websocketRunning={websocket}")
    print(f"[INFO] conversionReady={caps.get('conversionReady')} quantTypes={len(caps.get('quantTypes', []))}")

    if not args.model_path:
        print("[SKIP] conversion: no --model-path supplied")
    else:
        status, payload = request(base, "/models/import", method="POST", body={"path": args.model_path}, timeout=120)
        failures += 0 if expect_ok("model import", status, payload) else 1
        model = payload.get("data", {}).get("models", [None])[0] if payload.get("ok") else None
        if not model:
            print("[FAIL] imported model missing")
            failures += 1
        else:
            if not caps.get("conversionReady"):
                print("[SKIP] conversion: engine not ready")
            else:
                output = str(__import__("pathlib").Path(args.model_path).parent.parent / "output")
                body = {
                    "modelId": model["id"],
                    "inputPath": model["path"],
                    "outputDir": output,
                    "outputName": model["name"] + ".smoke." + args.quant,
                    "outType": "f16",
                    "quantType": args.quant,
                    "threads": 4,
                    "overwrite": True,
                    "verify": True,
                    "computeSha256": True,
                }
                status, payload = request(base, "/conversions", method="POST", body=body)
                failures += 0 if expect_ok("conversion create", status, payload) else 1
                task = payload.get("data") if payload.get("ok") else None
                if task and args.wait:
                    task_id = task["id"]
                    for _ in range(240):
                        time.sleep(1)
                        status, payload = request(base, f"/conversions/{urllib.parse.quote(task_id)}")
                        if not payload.get("ok"):
                            continue
                        task = payload["data"]
                        print(f"  {task['status']} {task['stage']} {task['progress']} {task.get('stageDetail', '')[:80]}")
                        if task["status"] in ("SUCCEEDED", "FAILED", "CANCELED"):
                            break
                    if task and task["status"] == "SUCCEEDED":
                        status, result = request(base, f"/conversions/{task['id']}/result")
                        ok = result.get("ok") and result.get("data", {}).get("ready")
                        print(f"[{'PASS' if ok else 'FAIL'}] conversion result")
                        failures += 0 if ok else 1
                    else:
                        print(f"[FAIL] conversion ended as {task['status'] if task else 'unknown'}")
                        failures += 1

    print(f"\n{failures} failure(s)")
    return 0 if failures == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
