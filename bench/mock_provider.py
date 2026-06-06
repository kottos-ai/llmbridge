#!/usr/bin/env python3

# Copyright 2026 Kottos AI, Inc.
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0

"""Mock OpenAI-compatible provider for the llmbridge Phase A benchmark.

Pure stdlib asyncio (no aiohttp) — a raw HTTP/1.1 server that answers
POST /v1/chat/completions with a canned chat-completion JSON after a fixed,
configurable delay. The delay simulates a real provider's generation time; the
whole benchmark measures llmbridge's *added* latency on top of it, so this backend
just needs to be (a) stable in its latency and (b) able to hold thousands of
concurrent keep-alive connections without itself becoming the bottleneck.

asyncio gives us cheap concurrency: each connection is a coroutine parked on
asyncio.sleep() during the simulated delay, so 1000+ in-flight requests cost
~nothing in threads. Connection: keep-alive is honored so llmbridge can pool
upstream sockets and we avoid TIME_WAIT churn at high RPS.

  python3 mock_provider.py [--port 9001] [--latency-ms 200]
"""

import argparse
import asyncio
import json

# OpenAI chat-completion shape (default — what a passthrough gateway forwards).
CANNED_OPENAI = {
    "id": "chatcmpl-llmbridge-mock",
    "object": "chat.completion",
    "created": 0,
    "model": "mock-1",
    "choices": [
        {
            "index": 0,
            "message": {"role": "assistant", "content": "pong"},
            "finish_reason": "stop",
        }
    ],
    "usage": {"prompt_tokens": 8, "completion_tokens": 1, "total_tokens": 9},
}

# Anthropic Messages shape — used when the gateway under test translates
# (OpenAI -> Anthropic outbound, Anthropic -> OpenAI on the way back).
CANNED_ANTHROPIC = {
    "id": "msg_llmbridge_mock",
    "type": "message",
    "role": "assistant",
    "model": "claude-mock-1",
    "content": [{"type": "text", "text": "pong"}],
    "stop_reason": "end_turn",
    "usage": {"input_tokens": 8, "output_tokens": 1},
}

# BODY is selected at startup from --format (see main()).
BODY = json.dumps(CANNED_OPENAI).encode()


async def handle(reader: asyncio.StreamReader, writer: asyncio.StreamWriter, latency: float):
    try:
        while True:
            # --- read request headers ---
            header = await reader.readuntil(b"\r\n\r\n")
            if not header:
                break
            content_length = 0
            keep_alive = True
            for line in header.split(b"\r\n")[1:]:
                if not line:
                    continue
                k, _, v = line.partition(b":")
                k = k.strip().lower()
                v = v.strip()
                if k == b"content-length":
                    content_length = int(v or b"0")
                elif k == b"connection" and v.lower() == b"close":
                    keep_alive = False
            # --- consume body ---
            if content_length:
                await reader.readexactly(content_length)

            # --- simulated generation latency ---
            if latency > 0:
                await asyncio.sleep(latency)

            # --- respond ---
            conn_hdr = b"keep-alive" if keep_alive else b"close"
            resp = (
                    b"HTTP/1.1 200 OK\r\n"
                    b"Content-Type: application/json\r\n"
                    b"Content-Length: " + str(len(BODY)).encode() + b"\r\n"
                                                                    b"Connection: " + conn_hdr + b"\r\n"
                                                                                                 b"\r\n" + BODY
            )
            writer.write(resp)
            await writer.drain()
            if not keep_alive:
                break
    except (asyncio.IncompleteReadError, ConnectionResetError, BrokenPipeError):
        pass
    finally:
        try:
            writer.close()
        except Exception:
            pass


async def main():
    global BODY
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=9001)
    ap.add_argument("--latency-ms", type=float, default=200.0)
    ap.add_argument("--format", choices=["openai", "anthropic"], default="openai",
                    help="response body shape (anthropic = for the translation benchmark)")
    args = ap.parse_args()
    latency = args.latency_ms / 1000.0
    BODY = json.dumps(CANNED_ANTHROPIC if args.format == "anthropic" else CANNED_OPENAI).encode()

    server = await asyncio.start_server(
        lambda r, w: handle(r, w, latency), "127.0.0.1", args.port, limit=1 << 20
    )
    addr = server.sockets[0].getsockname()
    print(f"mock provider on {addr}  latency={args.latency_ms}ms  format={args.format}", flush=True)
    async with server:
        await server.serve_forever()


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        pass
