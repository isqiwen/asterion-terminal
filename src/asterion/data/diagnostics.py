from asterion.platform.diagnostics import ServiceState


def provider_services(sync, database_ready):
    services = []
    if database_ready:
        for provider in sync.providers():
            lifecycle = provider["lifecycle"]["state"]
            if lifecycle == "archived":
                continue
            if lifecycle == "disabled":
                services.append(
                    ServiceState(
                        id=f"provider:{provider['id']}",
                        name=provider["name"],
                        state="disabled",
                        detail="连接已停用，不接受新同步；历史任务和数据仍保留",
                    )
                )
                continue
            demo = provider["demo"]
            configured = provider["configured"]
            services.append(
                ServiceState(
                    id=f"provider:{provider['id']}",
                    name=provider["name"],
                    state="unavailable"
                    if provider.get("credential_error")
                    else (
                        "ready"
                        if demo and configured
                        else "configured"
                        if configured
                        else "unconfigured"
                    ),
                    detail="配置读取失败，请到数据源设置修复"
                    if provider.get("credential_error")
                    else (
                        "本地生成示例数据，无需网络"
                        if demo
                        else provider["verification"]["message"] + "；按需请求，非持续连接"
                        if configured
                        else "尚未配置，请到数据源设置填写连接信息"
                    ),
                )
            )
    else:
        services.extend(
            ServiceState(
                id=f"provider:{p.manifest.id}",
                name=p.manifest.name,
                state="unknown",
                detail="数据库不可用，无法读取数据源配置",
            )
            for p in sync.registry.all()
        )
    return services
