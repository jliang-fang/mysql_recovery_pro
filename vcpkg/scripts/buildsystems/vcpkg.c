{
  "id": "chatcmpl-r7c3vzqwp9fcx7xu6gpzd4",
  "object": "chat.completion",
  "created": 1772070083,
  "model": "qwen3-coder-next",
  "choices": [
    {
      "index": 0,
      "message": {
        "role": "assistant",
        "content": "```json\n{\"path\": \"vcpkg/scripts/buildsystems/vcpkg.c\", \"code\": \"#include <stdio.h>\\n#include <stdlib.h>\\n\\nint main(int argc, char *argv[]) {\\n    fprintf(stderr, \\\"vcpkg: error: This file is a placeholder and should not be executed directly.\\\\n\\\");\\n    return 1;\\n}\\n\"}\n```",
        "tool_calls": []
      },
      "logprobs": null,
      "finish_reason": "stop"
    }
  ],
  "usage": {
    "prompt_tokens": 248,
    "completion_tokens": 86,
    "total_tokens": 334
  },
  "stats": {},
  "system_fingerprint": "qwen3-coder-next"
}
