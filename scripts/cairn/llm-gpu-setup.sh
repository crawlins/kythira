#!/usr/bin/env bash
# Copyright (c) 2026 Clark Rawlins
# SPDX-License-Identifier: Apache-2.0

# Sets up and serves the M1 model on a GPU host (.kiro/specs/cairn-chaos-
# testing/ task 11): Qwen3.8-27B (unsloth UD-Q4_K_M, 16.46 GB, about 17.4 GB
# of VRAM at a 32k context) under llama.cpp's llama-server, which the
# llm-tunnel reaches over SSH. Run it on the host as its login user; it is
# what the second campaign's g5.xlarge (24 GB A10G, 16 GB RAM) ran.
#
# It guards its own cost: the host powers off after MAX_MINUTES (default
# 780), and after 30 minutes with nothing written to ~/server.log.
#
# The serve flags are the ones that gave Cairn bare JSON:
#   --jinja                    use the model's chat template (tool calls)
#   --no-reasoning-preserve --reasoning-budget 0
#                              no thinking, and no <think> text in content
#   -lm none                   no mmap: the model is larger than the RAM
# Do not add --reasoning-format none: it leaves "<think></think>" inside
# message.content and the bootstrap parse fails.
#
# A host restored from a snapshot reads the model lazily from S3 (about
# 15-20 MB/s); the model is read through once in parallel first, so the
# server's load does not crawl. Environment: MODEL_DIR (default ~/models),
# LLAMA_CPP_REF (default 8642541b2, the build the campaign used),
# MAX_MINUTES.

set -uo pipefail

cd "$HOME" || exit 1
sudo shutdown -h "+${MAX_MINUTES:-780}" 2>/dev/null || true

model_dir="${MODEL_DIR:-$HOME/models}"
model="$model_dir/Qwen3.8-27B-UD-Q4_K_M.gguf"
url="https://huggingface.co/unsloth/Qwen3.8-27B-GGUF/resolve/main/Qwen3.8-27B-UD-Q4_K_M.gguf"
ref="${LLAMA_CPP_REF:-8642541b2}"
mkdir -p "$model_dir"

dlpid=""
if [ -s "$model" ]; then
    # Pre-warm a snapshot-restored volume: eight readers over 1 GiB slices.
    size=$(stat -c %s "$model")
    slices=$(((size + 1073741823) / 1073741824))
    seq 0 $((slices - 1)) | xargs -P 8 -I{} \
        dd if="$model" of=/dev/null bs=16M iflag=skip_bytes,count_bytes skip={}G count=1G status=none
else
    nohup wget -q -c -O "$model" "$url" > "$HOME/dl.log" 2>&1 &
    dlpid=$!
fi

export PATH=/usr/local/cuda/bin:$PATH
if [ ! -x "$HOME/llama.cpp/build/bin/llama-server" ]; then
    [ -d "$HOME/llama.cpp" ] || git clone -q https://github.com/ggml-org/llama.cpp "$HOME/llama.cpp"
    git -C "$HOME/llama.cpp" fetch -q --tags || true
    git -C "$HOME/llama.cpp" checkout -q "$ref"
    cmake -S "$HOME/llama.cpp" -B "$HOME/llama.cpp/build" -DGGML_CUDA=ON \
        -DCMAKE_BUILD_TYPE=Release -DLLAMA_CURL=OFF > "$HOME/cmake.log" 2>&1
    cmake --build "$HOME/llama.cpp/build" -j"$(nproc)" --target llama-server >> "$HOME/cmake.log" 2>&1
fi

[ -n "$dlpid" ] && wait "$dlpid"

nohup "$HOME/llama.cpp/build/bin/llama-server" -m "$model" --host 127.0.0.1 --port 8080 \
    --jinja --no-reasoning-preserve --reasoning-budget 0 \
    -lm none -ngl 99 -c 32768 > "$HOME/server.log" 2>&1 &

# Idle watchdog: power off once server.log has been quiet for 30 minutes.
nohup bash -c '
    while sleep 300; do
        last=$(stat -c %Y "$HOME/server.log" 2>/dev/null || echo 0)
        if [ $(( $(date +%s) - last )) -gt 1800 ]; then
            sudo shutdown -h now
            exit 0
        fi
    done' > /tmp/idle-watchdog.log 2>&1 &
