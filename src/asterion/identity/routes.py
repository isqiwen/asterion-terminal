from fastapi import APIRouter, Header
from pydantic import BaseModel, Field

from .service import Identity


class Email(BaseModel):
    email: str = Field(max_length=254)


class Login(Email):
    password: str = Field(min_length=1, max_length=128)


class Registration(Login):
    pin: str = Field(pattern=r"^[0-9]{6}$")
    country_code: str = Field(default="", max_length=6, pattern=r"^\+?[0-9]*$")
    phone: str = Field(default="", max_length=30, pattern=r"^[0-9 ()-]*$")
    first_name: str = Field(min_length=1, max_length=80)
    last_name: str = Field(min_length=1, max_length=80)


class Code(Email):
    code: str = Field(pattern=r"^\d{6}$")


class Reset(Code):
    password: str = Field(min_length=12, max_length=128)


class SecurityState(BaseModel):
    pin_required: bool
    locked: bool
    timeout_seconds: int
    remaining_seconds: float
    revision: int
    retry_after: int


class PinInput(BaseModel):
    pin: str = Field(pattern=r"^[0-9]{6}$")
    expected: int | None = None
    password: str | None = Field(default=None, max_length=128)


class LockTimeout(BaseModel):
    seconds: int


def router(identity: Identity):
    api = APIRouter(prefix="/api/v1/account")

    @api.get("/capabilities")
    def capabilities():
        return {"verification": identity.verification.mode, "code_length": 6}

    @api.post("/register")
    def register(body: Registration):
        return identity.register(**body.model_dump())

    @api.post("/login")
    def login(body: Login):
        return identity.login(**body.model_dump())

    @api.post("/verify")
    def verify(body: Code):
        return identity.verify(**body.model_dump())

    @api.post("/resend")
    def resend(body: Email):
        return identity.resend(body.email, "verify")

    @api.post("/forgot")
    def forgot(body: Email):
        return identity.resend(body.email, "reset")

    @api.post("/reset")
    def reset(body: Reset):
        return identity.verify(**body.model_dump())

    @api.get("/me")
    def me(x_account_session: str = Header(default="")):
        return identity.me(x_account_session)

    @api.post("/logout")
    def logout(x_account_session: str = Header(default="")):
        return identity.logout(x_account_session)

    @api.get("/security", response_model=SecurityState)
    def security(x_account_session: str = Header(default="")):
        return identity.pin.state(x_account_session)

    @api.post("/security/activity", response_model=SecurityState)
    def activity(x_account_session: str = Header(default="")):
        return identity.pin.state(x_account_session, "activity")

    @api.post("/security/lock", response_model=SecurityState)
    def lock(x_account_session: str = Header(default="")):
        return identity.pin.state(x_account_session, "lock")

    @api.post("/security/unlock", response_model=SecurityState)
    def unlock(body: PinInput, x_account_session: str = Header(default="")):
        return identity.pin.state(x_account_session, "unlock", pin=body.pin, expected=body.expected)

    @api.post("/security/setup", response_model=SecurityState)
    def setup(body: PinInput, x_account_session: str = Header(default="")):
        return identity.pin.state(x_account_session, "setup", pin=body.pin)

    @api.post("/security/change", response_model=SecurityState)
    def change(body: PinInput, x_account_session: str = Header(default="")):
        return identity.pin.state(x_account_session, "change", pin=body.pin, password=body.password)

    @api.post("/security/timeout", response_model=SecurityState)
    def timeout(body: LockTimeout, x_account_session: str = Header(default="")):
        return identity.pin.state(x_account_session, "timeout", timeout=body.seconds)

    return api
