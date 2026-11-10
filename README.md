# RobinhoodAutomation

Windows console app that sends a prompt to OpenAI or Anthropic Claude and connects to Robinhood's MCP trading service.

## Requirements

- Windows x64, the application executable, and possibly the Microsoft C++ runtime.
- An API key for the provider you select. You only need an OpenAI key or a Claude key, not both.
- A Robinhood account authorized for the Robinhood MCP service. On first run, the app opens a browser for Robinhood sign-in and authorization.
- Internet access. The first-run Robinhood sign-in uses local callback port `8400`.

## Choose a provider

Run the executable from PowerShell or Command Prompt. OpenAI is the default when no switch is supplied:

- `RobinhoodAutomation.exe` — use OpenAI.
- `RobinhoodAutomation.exe --provider openai` — use OpenAI explicitly.
- `RobinhoodAutomation.exe --provider claude` — use Claude.

The equivalent `--provider=claude` syntax is also accepted. Provider names must be lowercase `openai` or `claude`.

## API key text files

Put the key for the provider you selected on the first line of the matching text file in the app's current working directory:

- `openai_key.txt` — OpenAI API key; used by default and with `--provider openai`.
- `claude_key.txt` — Anthropic API key; used with `--provider claude`.

The app reads the text file first. If it is missing or empty, it checks the corresponding environment variable instead:

- OpenAI: `OPENAI_API_KEY`
- Claude: `ANTHROPIC_API_KEY`

Keep these files private. They are excluded by `.gitignore`; do not commit them, paste keys into source code, or include them in a public release. Each user supplies their own API key and is responsible for any provider charges.

## Robinhood authentication and local tokens

On first use, the program opens a browser for Robinhood authorization. After authorization, it stores OAuth tokens in `rh_tokens.json` in the current working directory and reuses them on later runs. Treat this file as a secret; do not share or commit it. The app listens for the OAuth redirect on `127.0.0.1:8400` during sign-in.

## Prompt and trading safety

The app reads `prompt.txt` from the current working directory. If that file is missing, the built-in fallback prompt asks the agent to execute autonomous trades if rules permit. The OpenAI MCP request sets `require_approval` to `never`, so tool calls may occur without a separate approval prompt. Review the prompt and account permissions before use. Test carefully, and do not run against a live account unless you understand and accept the trading risks.

The Claude request uses Anthropic's remote MCP connector. Availability may depend on Anthropic API support and account access. This project has not been validated against live provider or brokerage services.
