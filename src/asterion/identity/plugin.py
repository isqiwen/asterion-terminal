"""Account access of the operations this process still owns.

Accounts, sessions and the terminal lock belong to the Rust entry. It decides
the account state of every forwarded request and sends it with the request;
this plugin only reads that decision.
"""

from asterion_bindings.plugin_host import Activation, Context, Plugin
from asterion_bindings.task_repository import Conflict
from fastapi import Request
from fastapi.responses import JSONResponse

from asterion.identity.backup import check, restore
from asterion.identity.public import ACCOUNT_ACCESS, ACCOUNT_POLICY, Access

STATUS = "X-Asterion-Account-Status"
ACCOUNT = "X-Asterion-Account"


class AccountRefused(Exception):
    def __init__(self, message, status, code):
        super().__init__(message)
        self.status = status
        self.code = code


EXPIRED = AccountRefused("登录已过期，请重新登录", 401, "SESSION_EXPIRED")
REFUSALS = {
    "locked": AccountRefused("终端已锁定，请输入 PIN", 423, "TERMINAL_LOCKED"),
    "expired": EXPIRED,
    "unsupported": AccountRefused(
        "账号安全状态不受支持，缺少注册时设置的 PIN", 409, "UNSUPPORTED_ACCOUNT_SECURITY"
    ),
}
UNAVAILABLE = AccountRefused("账户服务暂不可用，请稍后重试", 503, "ACCOUNT_UNAVAILABLE")


def signed_in(request: Request, *, unlocked: bool) -> str:
    status = request.headers.get(STATUS, "")
    email = request.headers.get(ACCOUNT, "")
    if status == "unlocked" or (status == "locked" and not unlocked):
        if not email:
            raise UNAVAILABLE
        return email
    raise REFUSALS.get(status, UNAVAILABLE)


def activate(context: Context):
    policy = context.resource(ACCOUNT_POLICY)

    async def refused(_, exc):
        return JSONResponse(status_code=exc.status, content={"detail": str(exc), "code": exc.code})

    def account(request: Request):
        if policy.require_account:
            signed_in(request, unlocked=True)

    def owner(request: Request, expected_account: str = ""):
        if not policy.require_account:
            return "local-development"
        email = signed_in(request, unlocked=False)
        if expected_account and email != expected_account:
            raise Conflict("账户已切换，请重新载入研究页")
        return email

    return Activation(
        exports={ACCOUNT_ACCESS: Access(account, owner)},
        exception_handlers=((AccountRefused, refused),),
        hooks={"access": (account,)},
    )


plugin = Plugin(
    "asterion.identity",
    (),
    activate,
    backup=check,
    restore=restore,
    provides=(ACCOUNT_ACCESS,),
    resources=(ACCOUNT_POLICY,),
)
