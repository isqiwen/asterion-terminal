"""Current strategy parameter declarations and exact JSON value validation."""

import re
from decimal import Decimal
from typing import Annotated, Literal

from pydantic import BaseModel, ConfigDict, Field, StrictBool, StrictInt, StrictStr, model_validator

ParameterValue = StrictInt | StrictBool | StrictStr


class NamedParameter(BaseModel):
    model_config = ConfigDict(extra="forbid", frozen=True)
    key: str = Field(pattern=r"^[a-z][a-z0-9_]*$", max_length=64)
    label: str = Field(min_length=1, max_length=100)


class IntegerParameter(NamedParameter):
    type: Literal["integer"] = "integer"
    minimum: StrictInt = Field(ge=-(2**53 - 1), le=2**53 - 1)
    maximum: StrictInt = Field(ge=-(2**53 - 1), le=2**53 - 1)
    default: StrictInt

    @model_validator(mode="after")
    def bounds(self):
        if not self.minimum <= self.default <= self.maximum:
            raise ValueError("整数参数初始值或范围无效")
        return self


def decimal_value(value, scale):
    if (
        type(value) is not str
        or len(value) > 40
        or not re.fullmatch(r"-?(?:0|[1-9][0-9]*)(?:\.[0-9]+)?", value)
    ):
        raise ValueError("小数参数必须是有限十进制文本，不接受浮点数或科学计数法")
    if len(value.partition(".")[2]) > scale:
        raise ValueError("小数参数超过声明精度")
    return Decimal(value)


class DecimalParameter(NamedParameter):
    type: Literal["decimal"] = "decimal"
    minimum: StrictStr
    maximum: StrictStr
    scale: StrictInt = Field(ge=0, le=12)
    default: StrictStr

    @model_validator(mode="after")
    def bounds(self):
        if (
            not decimal_value(self.minimum, self.scale)
            <= decimal_value(self.default, self.scale)
            <= decimal_value(self.maximum, self.scale)
        ):
            raise ValueError("小数参数初始值或范围无效")
        return self


class BooleanParameter(NamedParameter):
    type: Literal["boolean"] = "boolean"
    default: StrictBool


class Choice(BaseModel):
    model_config = ConfigDict(extra="forbid", frozen=True)
    value: str = Field(min_length=1, max_length=100, strict=True)
    label: str = Field(min_length=1, max_length=100)


class EnumParameter(NamedParameter):
    type: Literal["enum"] = "enum"
    choices: tuple[Choice, ...] = Field(min_length=1, max_length=64)
    default: StrictStr

    @model_validator(mode="after")
    def options(self):
        values = [choice.value for choice in self.choices]
        if len(set(values)) != len(values) or self.default not in values:
            raise ValueError("枚举选项重复或初始值不在选项中")
        return self


Parameter = Annotated[
    IntegerParameter | DecimalParameter | BooleanParameter | EnumParameter,
    Field(discriminator="type"),
]


def validate_parameters(fields: tuple[Parameter, ...], values: dict):
    if set(values) != {p.key for p in fields}:
        raise ValueError("策略参数缺失或包含未知字段")
    for field in fields:
        value = values[field.key]
        if isinstance(field, IntegerParameter):
            valid = type(value) is int and field.minimum <= value <= field.maximum
        elif isinstance(field, DecimalParameter):
            number = decimal_value(value, field.scale)
            valid = Decimal(field.minimum) <= number <= Decimal(field.maximum)
        elif isinstance(field, BooleanParameter):
            valid = type(value) is bool
        else:
            valid = type(value) is str and value in {c.value for c in field.choices}
        if not valid:
            raise ValueError(f"策略参数类型或范围无效：{field.label}")
    return dict(values)
