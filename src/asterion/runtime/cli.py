import argparse
from pathlib import Path

from asterion.platform.config import Settings


def main():
    parser = argparse.ArgumentParser(prog="asterion")
    parser.add_argument(
        "role",
        choices=[
            "plugin-run",
            "init",
            "serve",
            "worker",
            "node",
            "desktop-bootstrap",
            "desktop-supervise",
            "desktop-stop",
            "desktop-backup",
            "desktop-snapshot",
            "desktop-restore",
            "desktop-info",
            "desktop-environment",
            "desktop-activate",
            "desktop-rollback",
        ],
    )
    parser.add_argument("--communication-context")
    parser.add_argument("--once", action="store_true")
    parser.add_argument("--port", type=int, default=8000)
    parser.add_argument("--state", type=Path)
    parser.add_argument("--pg-root", type=Path)
    parser.add_argument("--archive", type=Path)
    parser.add_argument("--target", type=Path)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    if args.role == "plugin-run":
        from asterion.runtime.extension import run

        if args.target is None:
            parser.error("--target is required")
        run(args.target)
        return
    if args.role.startswith("desktop-"):
        import json

        from asterion.runtime.desktop import bootstrap, stop, supervise

        if args.state is None:
            parser.error("--state is required")
        if args.role == "desktop-supervise":
            if args.pg_root is None:
                parser.error("--pg-root is required")
            supervise(args.state, args.pg_root)
            return
        from asterion_bindings.communication import activate, context, loads, reply, validate

        from asterion.runtime.backup import create_backup, managed_backup, restore_backup
        from asterion.runtime.environment import EnvironmentHost

        trace = (
            validate("Context", loads(args.communication_context))
            if args.communication_context
            else context(600)
        )
        host = args.state.resolve()
        if args.pg_root is None:
            parser.error("--pg-root is required")
        try:
            with (
                activate(trace),
                EnvironmentHost(
                    host, args.pg_root, wait=args.role in {"desktop-bootstrap", "desktop-info"}
                ) as environment,
            ):
                if args.role not in {"desktop-info", "desktop-environment", "desktop-snapshot"}:
                    environment.recover()
                state = environment.active()
                if args.role == "desktop-info":
                    result = environment.info()
                elif args.role == "desktop-environment":
                    result = environment.status()
                elif args.role == "desktop-activate":
                    if args.target is None:
                        parser.error("--target is required")
                    result = environment.switch(args.target)
                elif args.role == "desktop-rollback":
                    result = environment.switch()
                elif args.role == "desktop-snapshot":
                    if args.output is None:
                        parser.error("--output is required")
                    result = create_backup(state, args.output)
                elif args.role == "desktop-backup":
                    result = managed_backup(state, args.pg_root, args.output)
                elif args.role == "desktop-restore":
                    if args.archive is None or args.target is None:
                        parser.error("--archive and --target are required")
                    if args.target.resolve().is_relative_to(
                        state
                    ) or args.target.resolve().is_relative_to(host):
                        raise ValueError("恢复目录不能位于本机状态目录内")
                    result = restore_backup(args.archive, args.target, args.pg_root)
                elif args.role == "desktop-stop":
                    stop(state)
                    result = {"status": "stopped"}
                else:
                    result = bootstrap(state, args.pg_root)
                print(json.dumps(reply({"context": trace}, result), allow_nan=False))
        except Exception:  # noqa: BLE001 - database exceptions may contain credentials
            print(
                json.dumps(
                    reply(
                        {"context": trace},
                        error={
                            "code": "OPERATION_FAILED",
                            "message": "本机维护失败，请检查运行环境诊断",
                        },
                    )
                )
            )
            parser.exit(
                1,
                "本机维护失败。请检查维护任务、目录、空间及版本兼容；若切换中断，重新启动服务会重试恢复原环境。原数据目录仍保留。\n",
            )
        return
    settings = Settings()
    if args.role == "init":
        from asterion.platform.store import database
        from asterion.runtime.initialize import initialize

        settings.require_token()
        engine = database(settings.database_url)
        try:
            initialize(settings, engine)
        finally:
            engine.dispose()
        print("Catalog and local storage initialized")
    elif args.role == "serve":
        import uvicorn

        from asterion.api.app import create_app

        uvicorn.run(create_app(settings), host="127.0.0.1", port=args.port)
    elif args.role == "worker":
        from asterion.runtime.worker import run

        run(settings, args.once)
    else:
        parser.error("Trading node is not implemented. No broker connection or orders are enabled.")


if __name__ == "__main__":
    main()
