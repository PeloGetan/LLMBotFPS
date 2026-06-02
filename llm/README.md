# Local LLM runtime (optional)

Place a local OpenAI-compatible server and a model here, then enable it in
`config.json` (`"llm_enabled": true`). When bundling the final prototype, ship:

```
game.exe
llm/
  llama-server.exe   <- e.g. from llama.cpp
  model.gguf         <- any small instruct model in GGUF format
```

The game can launch `llama-server.exe` automatically
(`"llm_autostart_server": true`) and communicate over localhost HTTP at
`server_url`. No model is committed to the repo.

If this folder is empty / the server is unreachable, the game automatically uses
its built-in rule-based analyst (mock mode) — everything still works.
