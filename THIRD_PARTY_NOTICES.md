# Third-Party Notices for YTTranscript

YTTranscript downloads the following third-party components **at runtime**. They
are **not** bundled with, or redistributed by, the YTTranscript installer or
source repository — the application fetches them automatically on first run (or
on demand, for the optional GPU backends). Each component is governed by its own
license, summarized below.

---

### 1. yt-dlp
- **Project:** https://github.com/yt-dlp/yt-dlp
- **Used for:** downloading the audio stream of the YouTube URL you provide.
- **License:** The Unlicense (public domain). Note that the prebuilt Windows
  executable may bundle dependencies under their own (e.g. LGPL/GPL) terms.
- **SPDX:** `Unlicense`

### 2. FFmpeg — BtbN Windows build
- **Project:** https://github.com/BtbN/FFmpeg-Builds
- **Build downloaded:** `ffmpeg-master-latest-win64-gpl.zip`
- **Used for:** converting downloaded audio to 16 kHz mono WAV for the
  speech-to-text stage.
- **License:** **GPL** (v2 or later). The `-gpl` build includes GPL-only
  libraries (e.g. libx264/libx265), so the binary as a whole is distributed
  under the GPL. FFmpeg's own core is LGPL-2.1+; LGPL-only builds exist
  separately if needed.
- **SPDX:** `GPL-2.0-or-later`

### 3. whisper.cpp
- **Project:** https://github.com/ggml-org/whisper.cpp (release `v1.9.1`)
- **Used for:** the transcription engine (`whisper-cli.exe`), including the
  optional cuBLAS/CUDA and Vulkan GPU builds from the same project.
- **License:** MIT
- **SPDX:** `MIT`

### 4. Whisper `base.en` model
- **Source:** https://huggingface.co/ggerganov/whisper.cpp (`ggml-base.en.bin`)
- **Used for:** English speech-recognition model weights.
- **License:** MIT (OpenAI Whisper model weights, converted to GGML format).
- **SPDX:** `MIT`

### 5. llama.cpp
- **Project:** https://github.com/ggml-org/llama.cpp (build `b9811`)
- **Used for:** the local summarization engine (`llama-completion.exe`),
  including the optional CUDA and Vulkan GPU builds from the same project.
- **License:** MIT
- **SPDX:** `MIT`

### 6. Qwen2.5-3B-Instruct (GGUF)
- **Source:** https://huggingface.co/Qwen/Qwen2.5-3B-Instruct-GGUF
- **Used for:** the local model that summarizes the transcript.
- **License:** **Qwen Research License Agreement — NON-COMMERCIAL use only.**
  Commercial use requires a separate license from Alibaba Cloud. The license
  also requests attribution ("Built with Qwen"). Note that Qwen2.5 licensing
  varies by size — several sizes are Apache-2.0, but the 3B (and 72B) use this
  more restrictive license.
- **License text:** https://huggingface.co/Qwen/Qwen2.5-3B/blob/main/LICENSE
- **SPDX:** none (proprietary research license)

> ⚠️ Because the default summarization model is non-commercial, YTTranscript as
> shipped is intended for personal / non-commercial use. To use it commercially,
> swap the summarizer for an Apache-2.0 model (e.g. a different Qwen2.5 size or a
> Llama model).

### 7. NVIDIA CUDA runtime (optional)
- **Source:** bundled in llama.cpp's `cudart-llama-bin-win-cuda-12.4-x64.zip`,
  downloaded **only** if you enable the CUDA backend in Settings ▸ Encoder.
- **Used for:** GPU acceleration on NVIDIA cards.
- **License:** NVIDIA CUDA Redistributable (EULA). Redistribution of the runtime
  components is permitted under NVIDIA's terms, which include an attribution
  requirement ("This software contains source code provided by NVIDIA
  Corporation") and a prohibition on use in safety-critical systems.
- **License text:** https://docs.nvidia.com/cuda/eula/index.html
- **SPDX:** `LicenseRef-NVIDIA-CUDA`

---

**Summary:** all components above are downloaded on demand; none are pre-bundled.
The mix includes MIT (most permissive), GPL (FFmpeg `-gpl` build), and the Qwen
research license (non-commercial). Keep the GPL and Qwen terms in mind if you
redistribute this application or use its output commercially.

_Last updated: June 2026._
