import ast
import re
from pathlib import Path

Import("env")

secrets_path = Path(env["PROJECT_DIR"]) / "include" / "secrets.h"
if not secrets_path.is_file():
    raise RuntimeError(
        "Missing include/secrets.h. Copy include/secrets.h.example and fill in your secrets."
    )

match = re.search(
    r'^#define\s+SPA_OTA_PASSWORD\s+(.+?)\s*$',
    secrets_path.read_text(encoding="utf-8"),
    re.MULTILINE,
)
if not match:
    raise RuntimeError("include/secrets.h does not define SPA_OTA_PASSWORD")

try:
    password = ast.literal_eval(match.group(1))
except (SyntaxError, ValueError) as error:
    raise RuntimeError("SPA_OTA_PASSWORD must be a quoted C string") from error

if not isinstance(password, str) or not password:
    raise RuntimeError("SPA_OTA_PASSWORD must not be empty")

env.Prepend(UPLOADERFLAGS=["-a", password])
