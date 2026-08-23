#!/usr/bin/env python3

# Copyright 2026 Kottos AI, Inc.
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0

"""Mock OpenAI-compatible provider for the llmbridge Phase A benchmark.

Pure stdlib asyncio (no aiohttp): a raw HTTP/1.1 server that answers
POST /v1/chat/completions with a canned chat-completion JSON after a fixed,
configurable delay. The delay simulates a real provider's generation time; the
whole benchmark measures llmbridge's *added* latency on top of it, so this backend
just needs to be (a) stable in its latency and (b) able to hold thousands of
concurrent keep-alive connections without itself becoming the bottleneck.

asyncio gives us cheap concurrency: each connection is a coroutine parked on
asyncio.sleep() during the simulated delay, so 1000+ in-flight requests cost
~nothing in threads. Connection: keep-alive is honored so llmbridge can pool
upstream sockets and we avoid TIME_WAIT churn at high RPS.

Streaming mode (Phase B): when the request body contains "stream": true and
--format anthropic, the mock instead emits an Anthropic SSE event stream over
chunked transfer-encoding, one content_block_delta every --token-interval-ms.

Each delta's text carries the mock's own CLOCK_MONOTONIC emission timestamp
("t=<ns> "). That timestamp rides the payload through whichever gateway is under
test, so the load generator can compute exact per-chunk added latency
(arrival - emission) with no clock synchronisation and no coordinated-omission
guesswork: it never has to assume when a token *should* have arrived.

  python3 mock_provider.py [--port 9001] [--latency-ms 200]
                           [--tokens 100] [--token-interval-ms 20]
"""

import argparse
import asyncio
import json
import time

# OpenAI chat-completion shape (default): what a passthrough gateway forwards.
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

# Anthropic Messages shape, used when the gateway under test translates
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

# Body is selected at startup from --format (see main()).
BODY = json.dumps(CANNED_OPENAI).encode()


def _chunk(payload: bytes) -> bytes:
    """One HTTP/1.1 transfer-encoding chunk."""
    return b"%x\r\n" % len(payload) + payload + b"\r\n"


def _sse(event: str, data: dict) -> bytes:
    return ("event: %s\ndata: %s\n\n" % (event, json.dumps(data, separators=(",", ":")))).encode()


async def stream_anthropic(writer: asyncio.StreamWriter, latency: float,
                           tokens: int, interval: float):
    """Emit an Anthropic SSE stream at a fixed token rate, chunk-encoded.

    Timing is absolute (scheduled off a single start point), not cumulative
    sleeps, so the emission cadence doesn't drift with per-iteration overhead.
    The benchmark needs the provider to be the stable reference.
    """
    writer.write(
        b"HTTP/1.1 200 OK\r\n"
        b"Content-Type: text/event-stream\r\n"
        b"Cache-Control: no-cache\r\n"
        b"Transfer-Encoding: chunked\r\n"
        b"Connection: keep-alive\r\n\r\n"
    )
    # --latency-ms models prefill (time-to-first-token).
    if latency > 0:
        await asyncio.sleep(latency)

    writer.write(_chunk(_sse("message_start", {
        "type": "message_start",
        "message": {"id": "msg_llmbridge_mock", "type": "message", "role": "assistant",
                    "model": "claude-mock-1", "content": [],
                    "usage": {"input_tokens": 8, "output_tokens": 1}},
    })))
    writer.write(_chunk(_sse("content_block_start", {
        "type": "content_block_start", "index": 0,
        "content_block": {"type": "text", "text": ""},
    })))
    await writer.drain()

    t0 = time.monotonic()
    for i in range(tokens):
        target = t0 + i * interval
        delay = target - time.monotonic()
        if delay > 0:
            await asyncio.sleep(delay)
        # The emission stamp travels inside the token text; see the module docstring.
        text = "t=%d " % time.monotonic_ns()
        writer.write(_chunk(_sse("content_block_delta", {
            "type": "content_block_delta", "index": 0,
            "delta": {"type": "text_delta", "text": text},
        })))
        await writer.drain()

    writer.write(_chunk(_sse("content_block_stop", {"type": "content_block_stop", "index": 0})))
    writer.write(_chunk(_sse("message_delta", {
        "type": "message_delta",
        "delta": {"stop_reason": "end_turn", "stop_sequence": None},
        "usage": {"output_tokens": tokens},
    })))
    writer.write(_chunk(_sse("message_stop", {"type": "message_stop"})))
    writer.write(b"0\r\n\r\n")  # end of chunked body
    await writer.drain()


async def handle(reader: asyncio.StreamReader, writer: asyncio.StreamWriter, latency: float,
                 stream_fmt: bool = False, tokens: int = 100, interval: float = 0.02):
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
            body = b""
            if content_length:
                body = await reader.readexactly(content_length)

            # --- streaming path (Phase B): the client asked for stream:true ---
            if stream_fmt and b'"stream"' in body and b'"stream":true' in body.replace(b" ", b""):
                await stream_anthropic(writer, latency, tokens, interval)
                if not keep_alive:
                    break
                continue

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
    ap.add_argument("--tokens", type=int, default=100,
                    help="streaming: number of content_block_delta events per response")
    ap.add_argument("--token-interval-ms", type=float, default=20.0,
                    help="streaming: gap between tokens (20ms = 50 tok/s, a realistic rate)")
    args = ap.parse_args()
    latency = args.latency_ms / 1000.0
    BODY = json.dumps(CANNED_ANTHROPIC if args.format == "anthropic" else CANNED_OPENAI).encode()

    server = await asyncio.start_server(
        lambda r, w: handle(r, w, latency, args.format == "anthropic",
                            args.tokens, args.token_interval_ms / 1000.0),
        "127.0.0.1", args.port, limit=1 << 20,
        # Fairness: asyncio's default listen backlog is 100. A gateway that opens
        # upstream connections in bursts instead of pooling them, can overflow
        # that at high concurrency and get its connects refused, which would
        # look like the gateway failing when it is really the mock's socket
        # queue. A deep backlog removes the harness from the comparison.
        backlog=4096,
    )
    addr = server.sockets[0].getsockname()
    print(f"mock provider on {addr}  latency={args.latency_ms}ms  format={args.format}"
          f"  stream:{args.tokens}tok@{args.token_interval_ms}ms", flush=True)
    async with server:
        await server.serve_forever()


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        pass
