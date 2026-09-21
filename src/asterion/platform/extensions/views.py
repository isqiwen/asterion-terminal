"""Declarative table contribution. No HTML, script, native IPC or arbitrary API URLs."""

from pydantic import BaseModel, ConfigDict, Field, model_validator


class Column(BaseModel):
    model_config = ConfigDict(extra="forbid")
    key: str = Field(pattern=r"^[a-z][a-z0-9_]{0,40}$")
    label: str = Field(min_length=1, max_length=80)


class TableView(BaseModel):
    model_config = ConfigDict(extra="forbid")
    id: str = Field(pattern=r"^[a-z][a-z0-9_]{0,40}$")
    title: str = Field(min_length=1, max_length=80)
    columns: list[Column] = Field(min_length=1, max_length=30)

    @model_validator(mode="after")
    def unique_columns(self):
        if len({column.key for column in self.columns}) != len(self.columns):
            raise ValueError("Duplicate table column")
        return self


def validate_view(package, value):
    TableView.model_validate(value)


def validate_rows(view, rows):
    keys = {column.key for column in view.columns}
    if not isinstance(rows, list) or len(rows) > 1000:
        raise ValueError("插件表格超过 1000 行或格式错误")
    for row in rows:
        if not isinstance(row, dict) or set(row) != keys:
            raise ValueError("插件表格字段不符合声明")
        for value in row.values():
            if value is not None and type(value) not in (str, int, float, bool):
                raise ValueError("插件表格仅支持文本、数值和布尔值")
            if len(str(value)) > 2000:
                raise ValueError("插件表格单元格超过限制")
    return rows
