"""Built-in account policy, UI API and authentication lifecycle."""

from fastapi import Header
from fastapi.responses import JSONResponse

from asterion.identity.backup import check, restore
from asterion.identity.public import ACCOUNT_ACCESS, ACCOUNT_POLICY, IDENTITY_DIGEST, Access
from asterion.identity.routes import router
from asterion.identity.service import Identity, IdentityError
from asterion.platform.plugins import Activation, Context, Plugin
from asterion.platform.resources import DATA_ROOT, STORAGE
from asterion.platform.tasks.service import Conflict


def activate(context: Context):
    policy = context.resource(ACCOUNT_POLICY)
    identity = Identity(
        context.resource(STORAGE),
        context.resource(DATA_ROOT),
        context.resource(IDENTITY_DIGEST),
        mode=policy.verification,
    )

    async def identity_error(_, exc):
        return JSONResponse(status_code=exc.status, content={"detail": str(exc), "code": exc.code})

    def account(x_account_session: str = Header(default="")):
        if policy.require_account:
            identity.pin.require_unlocked(x_account_session)

    def owner(x_account_session: str = Header(default=""), expected_account: str = ""):
        if not policy.require_account:
            return "local-development"
        email = identity.me(x_account_session)["email"]
        if expected_account and email != expected_account:
            raise Conflict("账户已切换，请重新载入研究页")
        return email

    return Activation(
        close=context.resource(STORAGE).close,
        exports={ACCOUNT_ACCESS: Access(account, owner)},
        routers=(router(identity),),
        exception_handlers=((IdentityError, identity_error),),
        hooks={"access": (account,)},
    )


plugin = Plugin(
    "asterion.identity",
    (),
    activate,
    backup=check,
    restore=restore,
    provides=(ACCOUNT_ACCESS,),
    resources=(STORAGE, DATA_ROOT, IDENTITY_DIGEST, ACCOUNT_POLICY),
)
