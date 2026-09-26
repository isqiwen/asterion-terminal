"""In-process callback harness for HTTP orchestration tests only."""

from contextlib import nullcontext

from asterion.runtime import worker


def inline_computation(monkeypatch):
    """Exercise host HTTP orchestration with local test callbacks, never a production fallback."""

    class InlineArtifact:
        def __init__(self, content, metadata):
            self.content, self.metadata = content, metadata

        def __iter__(self):
            yield self.content

    class InlineProcess:
        def __init__(self, settings, job):
            self.settings, self.job = settings, job

        def __enter__(self):
            return self

        def __exit__(self, *_):
            pass

        def poll(self, timeout):
            self.result = worker.execute_job(self.settings, self.job)
            return True

        def take_result(self):
            return nullcontext(InlineArtifact(*self.result))

    monkeypatch.setattr(worker, "computation", InlineProcess)
