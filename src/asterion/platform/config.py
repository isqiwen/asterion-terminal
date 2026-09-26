from pathlib import Path
from typing import Literal

from pydantic_settings import BaseSettings, SettingsConfigDict


class Settings(BaseSettings):
    model_config = SettingsConfigDict(env_prefix="ASTERION_", env_file=".env")
    database_url: str = "postgresql://asterion:asterion@127.0.0.1:5432/asterion"
    data_root: Path = Path(".state/data")
    api_url: str = "http://127.0.0.1:8000"
    token: str = ""
    lease_seconds: int = 60
    require_account: bool = False
    # Verification policy of the entry's account service; the supervisor passes it on.
    account_verification: Literal["local", "email"] = "local"

    def require_token(self) -> None:
        if len(self.token) < 24:
            raise ValueError("Set ASTERION_TOKEN to a random token of at least 24 characters")
