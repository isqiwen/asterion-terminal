"""Fixed distribution bootstrap; Rust owns input/output framing and process lifetime."""

from asterion_bindings.task_process import serve
from pydantic_settings import SettingsConfigDict

from asterion.platform.config import Settings
from asterion.runtime.worker import execute_job


class ChildSettings(Settings):
    model_config = SettingsConfigDict(env_file=None)


def dispatch(request):
    return execute_job(ChildSettings.model_validate(request["settings"]), request["job"])


if __name__ == "__main__":
    serve(dispatch)
